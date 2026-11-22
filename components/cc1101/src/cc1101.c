#include "cc1101.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <limits.h>

static const char *TAG = "CC1101";

/* ============================================================
 * INTERNAL LOW-LEVEL SPI
 * ============================================================ */

static inline void cc1101_select(cc1101_handle_t *dev)
{
    gpio_set_level(dev->cs_pin, 0);
    esp_rom_delay_us(1);
}

static inline void cc1101_deselect(cc1101_handle_t *dev)
{
    esp_rom_delay_us(1);
    gpio_set_level(dev->cs_pin, 1);
}

static esp_err_t cc1101_wait_miso(cc1101_handle_t *dev)
{
    unsigned remaining = 2000;
    while (gpio_get_level(dev->miso_pin)) {
        if (--remaining == 0) return ESP_ERR_TIMEOUT;
        esp_rom_delay_us(1);
    }
    return ESP_OK;
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

/* Checked register/profile and packet transport. Unlike legacy
 * accessors it propagates MISO/SPI failures instead of manufacturing zero.
 * Header + up to one FIFO use one transaction/one CS assertion, on dev->spi.
 * This is used for TX too: no split header/data transfers or ignored errors. */
static esp_err_t cc1101_profile_transfer(cc1101_handle_t *dev, uint8_t command,
                                        const uint8_t *data, uint8_t *out, size_t len)
{
    if (dev == NULL || dev->spi == NULL || len > CC1101_FIFO_SIZE)
        return ESP_ERR_INVALID_ARG;
    uint8_t tx[CC1101_FIFO_SIZE + 1] = {command};
    uint8_t rx[CC1101_FIFO_SIZE + 1] = {0};
    if (data != NULL) memcpy(tx + 1, data, len);
    cc1101_select(dev);
    esp_err_t err = cc1101_wait_miso(dev);
    if (err == ESP_OK) {
        spi_transaction_t transaction = {
            .length = (len + 1) * 8,
            .tx_buffer = tx,
            .rx_buffer = rx
        };
        err = spi_device_polling_transmit(dev->spi, &transaction);
    }
    cc1101_deselect(dev);
    if (err == ESP_OK && out != NULL) memcpy(out, rx + 1, len);
    return err;
}

static esp_err_t cc1101_profile_read(cc1101_handle_t *dev, uint8_t reg, uint8_t *out)
{
    return cc1101_profile_transfer(dev, reg | CC1101_READ_SINGLE, NULL, out, 1);
}

static esp_err_t cc1101_write_reg(cc1101_handle_t *dev, uint8_t reg, uint8_t value)
{
    return cc1101_profile_transfer(dev, reg, &value, NULL, 1);
}

/* Compatibility accessors have no error return. Active init/profile/TX/RX
 * paths use checked APIs; never turn a failed read into a plausible zero. */
static uint8_t cc1101_read_reg(cc1101_handle_t *dev, uint8_t reg)
{
    uint8_t value = 0xFF;
    esp_err_t err = cc1101_profile_read(dev, reg, &value);
    if (err != ESP_OK) ESP_LOGE(TAG, "Register %02X read failed: %s", reg, esp_err_to_name(err));
    return value;
}

static uint8_t cc1101_read_status(cc1101_handle_t *dev, uint8_t reg)
{
    uint8_t value = 0xFF;
    esp_err_t err = cc1101_profile_transfer(dev, reg | CC1101_READ_BURST, NULL, &value, 1);
    if (err != ESP_OK) ESP_LOGE(TAG, "Status %02X read failed: %s", reg, esp_err_to_name(err));
    return value;
}

uint8_t cc1101_strobe(cc1101_handle_t *dev, uint8_t strobe)
{
    if (dev == NULL || dev->spi == NULL) return 0xFF;
    uint8_t rx = 0xFF;
    cc1101_select(dev);
    esp_err_t err = cc1101_wait_miso(dev);
    if (err == ESP_OK) {
        spi_transaction_t t = {.length = 8, .tx_buffer = &strobe, .rx_buffer = &rx};
        err = spi_device_polling_transmit(dev->spi, &t);
    }
    cc1101_deselect(dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Strobe %02X failed: %s", strobe, esp_err_to_name(err));
        return 0xFF;
    }
    return rx;
}

static void cc1101_rx_parser_reset(cc1101_handle_t *dev)
{
    dev->rx_length_pending = false;
    dev->rx_packet_length = 0;
    dev->rx_packet_deadline_us = 0;
}

/* TI SWRZ020E: dynamic multi-bit status must agree twice. Bound retries;
 * disagreement is not permission to consume or flush the FIFO. */
static esp_err_t cc1101_read_stable_status(cc1101_handle_t *dev, uint8_t reg,
                                          uint8_t *out)
{
    uint8_t previous;
    esp_err_t err = cc1101_profile_transfer(dev, reg | CC1101_READ_BURST,
                                           NULL, &previous, 1);
    if (err != ESP_OK) return err;
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t current;
        err = cc1101_profile_transfer(dev, reg | CC1101_READ_BURST, NULL, &current, 1);
        if (err != ESP_OK) return err;
        if (current == previous) { *out = current; return ESP_OK; }
        previous = current;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t cc1101_profile_wait_state(cc1101_handle_t *dev, uint8_t state)
{
    int64_t deadline = esp_timer_get_time() + 20000;
    do {
        uint8_t actual;
        esp_err_t err = cc1101_profile_transfer(dev, CC1101_MARCSTATE | CC1101_READ_BURST,
                                               NULL, &actual, 1);
        if (err != ESP_OK) return err;
        if ((actual & 0x1F) == state) return ESP_OK;
        vTaskDelay(1);
    } while (esp_timer_get_time() < deadline);
    return ESP_ERR_TIMEOUT;
}

esp_err_t cc1101_idle_locked(cc1101_handle_t *dev)
{
    if (dev == NULL) return ESP_ERR_INVALID_ARG;
    dev->tx_prepared = false;
    esp_err_t err = cc1101_profile_transfer(dev, CC1101_SIDLE, NULL, NULL, 0);
    return err == ESP_OK ? cc1101_profile_wait_state(dev, 0x01) : err;
}

esp_err_t cc1101_start_rx_locked(cc1101_handle_t *dev)
{
    esp_err_t err = cc1101_profile_transfer(dev, CC1101_SRX, NULL, NULL, 0);
    if (err == ESP_OK) err = cc1101_profile_wait_state(dev, 0x0D);
    if (err != ESP_OK && dev != NULL)
        (void)cc1101_idle_locked(dev); /* Best effort only; original error wins. */
    return err;
}

esp_err_t cc1101_profile_readback_locked(cc1101_handle_t *dev,
                                        cc1101_profile_status_t *status)
{
    if (dev == NULL || status == NULL) return ESP_ERR_INVALID_ARG;
    cc1101_profile_status_t value = {0};
    const uint8_t registers[] = {CC1101_PKTCTRL0, CC1101_MDMCFG2, CC1101_DEVIATN,
        CC1101_AGCCTRL2, CC1101_AGCCTRL1, CC1101_AGCCTRL0, CC1101_FOCCFG,
        CC1101_FREND0, CC1101_IOCFG0, CC1101_PKTCTRL1, CC1101_PKTLEN,
        CC1101_SYNC1, CC1101_SYNC0, CC1101_MCSM1, CC1101_MDMCFG4, CC1101_MDMCFG3,
        CC1101_FREQ2, CC1101_FREQ1, CC1101_FREQ0, CC1101_CHANNR, CC1101_FIFOTHR};
    uint8_t *fields[] = {&value.pktctrl0, &value.mdmcfg2, &value.deviatn,
        &value.agcctrl2, &value.agcctrl1, &value.agcctrl0, &value.foccfg,
        &value.frend0, &value.iocfg0, &value.pktctrl1, &value.pktlen,
        &value.sync1, &value.sync0, &value.mcsm1, &value.mdmcfg4, &value.mdmcfg3,
        &value.freq2, &value.freq1, &value.freq0, &value.channr, &value.fifothr};
    for (size_t i = 0; i < sizeof(registers); i++) {
        esp_err_t err = cc1101_profile_read(dev, registers[i], fields[i]);
        if (err != ESP_OK) return err;
    }
    esp_err_t err = cc1101_profile_transfer(dev, CC1101_MARCSTATE | CC1101_READ_BURST,
                                           NULL, &value.marcstate, 1);
    if (err != ESP_OK) return err;
    value.marcstate &= 0x1F;
    err = cc1101_profile_transfer(dev, CC1101_PATABLE | CC1101_READ_BURST,
                                 NULL, value.patable, sizeof(value.patable));
    if (err != ESP_OK) return err;
    const cc1101_profile_status_t empty = {0};
    /* Neither an all-zero bus image nor an undefined MARCSTATE is evidence
     * of a successful profile readback, even if the host SPI call succeeded. */
    if (value.marcstate > 0x16 || memcmp(&value, &empty, sizeof(value)) == 0)
        return ESP_FAIL;
    *status = value;
    return ESP_OK;
}

esp_err_t cc1101_apply_profile_locked(cc1101_handle_t *dev,
                                     cc1101_modulation_t modulation,
                                     cc1101_rx_format_t format)
{
    if (dev == NULL || (modulation != CC1101_PROFILE_OOK && modulation != CC1101_PROFILE_ASK &&
        modulation != CC1101_PROFILE_2FSK && modulation != CC1101_PROFILE_GFSK) ||
        (format != CC1101_RX_PACKET && format != CC1101_RX_RAW)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = cc1101_idle_locked(dev);
    if (err != ESP_OK) return err;
    err = cc1101_profile_transfer(dev, CC1101_SFRX, NULL, NULL, 0);
    if (err != ESP_OK) return err;
    cc1101_rx_parser_reset(dev);
    dev->rx_recovery_pending = false;
    err = cc1101_profile_transfer(dev, CC1101_SFTX, NULL, NULL, 0);
    if (err != ESP_OK) return err;

    uint8_t frend0, mdmcfg1;
    err = cc1101_profile_read(dev, CC1101_FREND0, &frend0);
    if (err != ESP_OK) return err;
    err = cc1101_profile_read(dev, CC1101_MDMCFG1, &mdmcfg1);
    if (err != ESP_OK) return err;
    bool amplitude = modulation == CC1101_PROFILE_OOK || modulation == CC1101_PROFILE_ASK;
    uint8_t modem = amplitude ? 0x30 : modulation == CC1101_PROFILE_GFSK ? 0x10 : 0x00;
    uint8_t table[8] = {0};
    if (amplitude) {
        table[0] = modulation == CC1101_PROFILE_ASK ? 0x34 : 0x00;
        table[1] = modulation == CC1101_PROFILE_OOK ?
            CC1101_OOK_PA_433_MHZ : CC1101_DEFAULT_PA_433_MHZ;
    } else table[0] = CC1101_DEFAULT_PA_433_MHZ;
    /* 26 MHz crystal, +/-20.629883 kHz FSK deviation. Amplitude profiles
     * ignore deviation; writing it still makes every transition deterministic. */
    uint32_t frequency = (uint32_t)(((uint64_t)433920000 << 16) / 26000000ULL);
    const struct { uint8_t reg, value; } writes[] = {
        /* Make the existing reset-default sync and framing deterministic on A/B.
         * 61 + length + RSSI/LQI fits the FIFO; long streaming is unsupported. */
        {CC1101_SYNC1, 0xD3}, {CC1101_SYNC0, 0x91},
        {CC1101_PKTLEN, CC1101_MAX_PAYLOAD_LEN},
        {CC1101_PKTCTRL1, CC1101_APPEND_STATUS | CC1101_ADR_CHK_NONE},
        {CC1101_MCSM1, 0x3F},
        {CC1101_PKTCTRL0, format == CC1101_RX_RAW ? 0x30 : 0x05},
        {CC1101_MDMCFG2, modem | (format == CC1101_RX_RAW ? 0 : 3)},
        {CC1101_DEVIATN, 0x35}, {CC1101_AGCCTRL2, 0x03},
        {CC1101_AGCCTRL1, amplitude ? 0x00 : 0x40}, {CC1101_AGCCTRL0, 0x91},
        {CC1101_FOCCFG, amplitude ? 0x34 : 0x36},
        {CC1101_FREND0, (frend0 & ~CC1101_FREND0_PA_POWER_MASK) | (amplitude ? 1 : 0)},
        {CC1101_IOCFG0, format == CC1101_RX_RAW ? 0x0D : 0x06},
        {CC1101_MDMCFG1, mdmcfg1 & 0x7F}, /* FEC off; preserve preamble/spacing. */
        {CC1101_FREQ2, (frequency >> 16) & 0xFF},
        {CC1101_FREQ1, (frequency >> 8) & 0xFF}, {CC1101_FREQ0, frequency & 0xFF},
        {CC1101_CHANNR, 0}, {CC1101_MDMCFG4, 0xCA}, {CC1101_MDMCFG3, 0x83}
    };
    for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); i++) {
        err = cc1101_profile_transfer(dev, writes[i].reg, &writes[i].value, NULL, 1);
        if (err != ESP_OK) return err;
    }
    err = cc1101_profile_transfer(dev, CC1101_PATABLE | CC1101_WRITE_BURST,
                                 table, NULL, sizeof(table));
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); i++) {
        uint8_t actual;
        err = cc1101_profile_read(dev, writes[i].reg, &actual);
        if (err != ESP_OK) return err;
        if (actual != writes[i].value) return ESP_FAIL;
    }
    cc1101_profile_status_t actual;
    err = cc1101_profile_readback_locked(dev, &actual);
    if (err != ESP_OK) return err;
    if (memcmp(actual.patable, table, sizeof(table)) != 0 || actual.marcstate != 0x01)
        return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t cc1101_reset_checked(cc1101_handle_t *dev)
{
    if (dev == NULL || dev->spi == NULL) return ESP_ERR_INVALID_ARG;
    cc1101_rx_parser_reset(dev);
    dev->rx_recovery_pending = false;
    dev->tx_prepared = false;
    cc1101_deselect(dev);
    esp_rom_delay_us(5);

    cc1101_select(dev);
    esp_rom_delay_us(10);
    cc1101_deselect(dev);
    esp_rom_delay_us(40);

    cc1101_select(dev);
    esp_err_t err = cc1101_wait_miso(dev);
    uint8_t reset = CC1101_SRES;
    if (err == ESP_OK) {
        spi_transaction_t t = {.length = 8, .tx_buffer = &reset};
        err = spi_device_polling_transmit(dev->spi, &t);
    }
    if (err == ESP_OK) err = cc1101_wait_miso(dev);
    cc1101_deselect(dev);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    return cc1101_profile_wait_state(dev, 0x01);
}

esp_err_t cc1101_init(cc1101_handle_t *dev, spi_device_handle_t spi_handle,
                      gpio_num_t cs, gpio_num_t miso, gpio_num_t gdo0)
{
    if (dev == NULL || spi_handle == NULL || cs < 0 || miso < 0 || gdo0 < 0)
        return ESP_ERR_INVALID_ARG;
    *dev = (cc1101_handle_t){.spi = spi_handle, .cs_pin = cs,
        .miso_pin = miso, .gdo0_pin = gdo0};
    esp_err_t err = cc1101_reset_checked(dev);
    uint8_t partnum, version;
    if (err == ESP_OK)
        err = cc1101_profile_transfer(dev, CC1101_PARTNUM | CC1101_READ_BURST, NULL, &partnum, 1);
    if (err == ESP_OK)
        err = cc1101_profile_transfer(dev, CC1101_VERSION | CC1101_READ_BURST, NULL, &version, 1);
    if (err != ESP_OK) return err;
    return partnum == 0 && version != 0 && version != 0xFF ? ESP_OK : ESP_FAIL;
}

void cc1101_reset(cc1101_handle_t *dev)
{
    esp_err_t err = cc1101_reset_checked(dev);
    if (err != ESP_OK) ESP_LOGE(TAG, "Reset failed: %s", esp_err_to_name(err));
}

esp_err_t cc1101_config_checked(cc1101_handle_t *dev)
{
    if (dev == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = cc1101_idle_locked(dev);
    if (err != ESP_OK) return err;
    const struct { uint8_t reg, value; } writes[] = {
        {CC1101_IOCFG2, CC1101_GDO_SYNC_WORD}, {CC1101_IOCFG0, CC1101_GDO_SYNC_WORD},
        {CC1101_SYNC1, 0xD3}, {CC1101_SYNC0, 0x91},
        {CC1101_PKTLEN, CC1101_MAX_PAYLOAD_LEN},
        {CC1101_PKTCTRL1, CC1101_APPEND_STATUS | CC1101_ADR_CHK_NONE},
        {CC1101_PKTCTRL0, CC1101_CRC_ENABLE | CC1101_PKT_VARIABLE},
        {CC1101_MDMCFG4, 0xCA}, {CC1101_MDMCFG3, 0x83}, {CC1101_MDMCFG2, 0x33},
        {CC1101_MDMCFG1, CC1101_MDMCFG1_VALUE(0, 2, 2)}, {CC1101_MDMCFG0, 248},
        {CC1101_AGCCTRL2, 0x03}, {CC1101_AGCCTRL1, 0x00}, {CC1101_AGCCTRL0, 0x91},
        {CC1101_MCSM0, CC1101_AUTOCAL_IDLE_TO_RXTX}, {CC1101_MCSM1, 0x3F}
    };
    for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); ++i) {
        err = cc1101_write_reg(dev, writes[i].reg, writes[i].value);
        if (err != ESP_OK) return err;
    }
    err = cc1101_set_frequency(dev, 433920000);
    uint8_t foccfg;
    if (err == ESP_OK) err = cc1101_profile_read(dev, CC1101_FOCCFG, &foccfg);
    if (err == ESP_OK)
        err = cc1101_write_reg(dev, CC1101_FOCCFG, foccfg & ~CC1101_FOCCFG_FOC_LIMIT_MASK);
    if (err != ESP_OK) return err;
    const uint8_t off_table[8] = {0};
    err = cc1101_profile_transfer(dev, CC1101_PATABLE | CC1101_WRITE_BURST,
                                  off_table, NULL, sizeof(off_table));
    if (err == ESP_OK) err = cc1101_set_tx_power(dev, CC1101_OOK_PA_433_MHZ);
    if (err == ESP_OK) err = cc1101_profile_transfer(dev, CC1101_SCAL, NULL, NULL, 0);
    if (err == ESP_OK) err = cc1101_profile_wait_state(dev, 0x01);
    return err;
}

void cc1101_config(cc1101_handle_t *dev)
{
    esp_err_t err = cc1101_config_checked(dev);
    if (err != ESP_OK) ESP_LOGE(TAG, "Configuration failed: %s", esp_err_to_name(err));
}

esp_err_t cc1101_set_tx_len(cc1101_handle_t *dev, uint8_t len) {
    if (len > CC1101_MAX_PAYLOAD_LEN) return ESP_ERR_INVALID_ARG;
    return cc1101_write_reg(dev, CC1101_PKTLEN, len);
}

void cc1101_set_tx_mode(cc1101_handle_t *dev)
{
    esp_err_t err = cc1101_idle_locked(dev);
    if (err == ESP_OK) err = cc1101_profile_transfer(dev, CC1101_SFTX, NULL, NULL, 0);
    if (err != ESP_OK) ESP_LOGE(TAG, "TX FIFO reset failed: %s", esp_err_to_name(err));
}

esp_err_t cc1101_tx_prepare_locked(cc1101_handle_t *dev, const uint8_t *data, size_t len)
{
    if (dev == NULL) return ESP_ERR_INVALID_ARG;
    dev->tx_prepared = false;
    if (data == NULL || len == 0 || len > CC1101_MAX_PAYLOAD_LEN)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = cc1101_idle_locked(dev);
    if (err == ESP_OK) err = cc1101_profile_transfer(dev, CC1101_SFTX, NULL, NULL, 0);
    uint8_t txbytes, pktctrl0, pktlen, iocfg0;
    if (err == ESP_OK) err = cc1101_read_stable_status(dev, CC1101_TXBYTES, &txbytes);
    if (err == ESP_OK && txbytes != 0) return ESP_FAIL;
    if (err == ESP_OK) err = cc1101_profile_read(dev, CC1101_PKTCTRL0, &pktctrl0);
    if (err == ESP_OK) err = cc1101_profile_read(dev, CC1101_PKTLEN, &pktlen);
    if (err == ESP_OK) err = cc1101_profile_read(dev, CC1101_IOCFG0, &iocfg0);
    if (err != ESP_OK) return err;
    if (pktctrl0 != (CC1101_CRC_ENABLE | CC1101_PKT_VARIABLE) ||
        iocfg0 != CC1101_GDO_SYNC_WORD || len > pktlen)
        return ESP_ERR_INVALID_STATE;
    uint8_t packet[CC1101_FIFO_SIZE];
    packet[0] = (uint8_t)len;
    memcpy(&packet[1], data, len);
    err = cc1101_profile_transfer(dev, CC1101_FIFO_ADDR | CC1101_WRITE_BURST,
                                  packet, NULL, len + 1);
    if (err == ESP_OK) err = cc1101_read_stable_status(dev, CC1101_TXBYTES, &txbytes);
    if (err != ESP_OK) return err;
    if (txbytes != len + 1) return ESP_FAIL;
    dev->tx_prepared = true;
    return ESP_OK;
}

esp_err_t cc1101_tx_start_locked(cc1101_handle_t *dev)
{
    if (dev == NULL || !dev->tx_prepared) return ESP_ERR_INVALID_STATE;
    /* Consume preparation even on error: the command may have reached the
     * chip before transport failure. Never retry a potentially started TX. */
    dev->tx_prepared = false;
    return cc1101_profile_transfer(dev, CC1101_STX, NULL, NULL, 0);
}

esp_err_t cc1101_tx_status_locked(cc1101_handle_t *dev, cc1101_tx_status_t *out)
{
    if (dev == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    cc1101_tx_status_t value = {0};
    esp_err_t err = cc1101_read_stable_status(dev, CC1101_TXBYTES, &value.txbytes);
    if (err == ESP_OK) err = cc1101_read_stable_status(dev, CC1101_MARCSTATE, &value.marcstate);
    if (err != ESP_OK) return err;
    value.marcstate &= 0x1F;
    value.gdo0_high = gpio_get_level(dev->gdo0_pin) != 0;
    *out = value;
    return ESP_OK;
}

void cc1101_transmit(cc1101_handle_t *dev, uint8_t *data, size_t len)
{
    esp_err_t err = cc1101_tx_prepare_locked(dev, data, len);
    if (err == ESP_OK) err = cc1101_tx_start_locked(dev);
    if (err != ESP_OK) ESP_LOGE(TAG, "TX start failed: %s", esp_err_to_name(err));
}

void cc1101_set_rx_mode(cc1101_handle_t *dev)
{
    if (dev == NULL) return;
    cc1101_rx_parser_reset(dev);
    dev->rx_recovery_pending = false;
    esp_err_t err = cc1101_idle_locked(dev);
    if (err == ESP_OK) err = cc1101_profile_transfer(dev, CC1101_SFRX, NULL, NULL, 0);
    if (err == ESP_OK) err = cc1101_start_rx_locked(dev);
    dev->rx_recovery_pending = err != ESP_OK;
    if (err != ESP_OK) ESP_LOGE(TAG, "RX start failed: %s", esp_err_to_name(err));
}

void cc1101_get_rx_format_status(cc1101_handle_t *dev, cc1101_rx_format_status_t *status)
{
    if (status == NULL) return;
    cc1101_profile_status_t actual;
    memset(status, 0, sizeof(*status));
    status->marcstate = 0xFF; /* Legacy void API: explicit unavailable sentinel. */
    if (cc1101_profile_readback_locked(dev, &actual) != ESP_OK) return;
    status->pktctrl0 = actual.pktctrl0;
    status->mdmcfg2 = actual.mdmcfg2;
    status->iocfg0 = actual.iocfg0;
    status->marcstate = actual.marcstate;
}

esp_err_t cc1101_set_rx_format(cc1101_handle_t *dev, cc1101_rx_format_t format)
{
    if (dev == NULL || (format != CC1101_RX_PACKET && format != CC1101_RX_RAW))
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = cc1101_idle_locked(dev);
    if (err != ESP_OK) return err;
    cc1101_profile_status_t actual;
    err = cc1101_profile_readback_locked(dev, &actual);
    if (err != ESP_OK) return err;
    cc1101_modulation_t modulation;
    switch (actual.mdmcfg2 & CC1101_MDMCFG2_MOD_FORMAT_MASK) {
    case (CC1101_MOD_2FSK << 4): modulation = CC1101_PROFILE_2FSK; break;
    case (CC1101_MOD_GFSK << 4): modulation = CC1101_PROFILE_GFSK; break;
    case (CC1101_MOD_ASK_OOK << 4):
        if (actual.patable[0] == 0x34) modulation = CC1101_PROFILE_ASK;
        else if (actual.patable[0] == 0x00) modulation = CC1101_PROFILE_OOK;
        else return ESP_ERR_INVALID_STATE;
        break;
    default: return ESP_ERR_INVALID_STATE;
    }
    err = cc1101_apply_profile_locked(dev, modulation, format);
    return err == ESP_OK ? cc1101_start_rx_locked(dev) : err;
}

static esp_err_t cc1101_recover_packet_rx(cc1101_handle_t *dev)
{
    cc1101_rx_parser_reset(dev);
    dev->rx_recovery_pending = true;
    esp_err_t err = cc1101_idle_locked(dev);
    if (err == ESP_OK)
        err = cc1101_profile_transfer(dev, CC1101_SFRX, NULL, NULL, 0);
    if (err == ESP_OK) err = cc1101_start_rx_locked(dev);
    dev->rx_recovery_pending = err != ESP_OK;
    if (err != ESP_OK)
        ESP_LOGW(TAG, "Packet RX recovery failed: %s (will retry)", esp_err_to_name(err));
    return err;
}

static esp_err_t cc1101_packet_rx_health(cc1101_handle_t *dev, uint8_t *count)
{
    if (dev->rx_recovery_pending) {
        esp_err_t err = cc1101_recover_packet_rx(dev);
        if (err != ESP_OK) return err;
    }
    uint8_t bytes, state;
    esp_err_t err = cc1101_read_stable_status(dev, CC1101_RXBYTES, &bytes);
    if (err != ESP_OK) return err;
    err = cc1101_read_stable_status(dev, CC1101_MARCSTATE, &state);
    if (err != ESP_OK) return err;
    state &= 0x1F;
    /* Overflow can stick in RX without setting the overflow flag (SWRZ020E).
     * A count beyond the physical FIFO is also a recovery condition. */
    if ((bytes & 0x80) || (bytes & 0x7F) > CC1101_FIFO_SIZE || state == 0x11) {
        ESP_LOGW(TAG, "Packet RX overflow/stall: RXBYTES=%02X MARCSTATE=%02X; discarding FIFO",
                 bytes, state);
        err = cc1101_recover_packet_rx(dev);
        *count = 0;
        return err;
    }
    *count = bytes & 0x7F;
    if (state == 0x01) return cc1101_start_rx_locked(dev);
    if (state != 0x0D && state != 0x0E && state != 0x0F)
        return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t cc1101_packet_rx_ready_locked(cc1101_handle_t *dev)
{
    if (dev == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t count, state;
    esp_err_t err = cc1101_packet_rx_health(dev, &count);
    if (err != ESP_OK) return err;
    err = cc1101_read_stable_status(dev, CC1101_MARCSTATE, &state);
    if (err != ESP_OK) return err;
    /* Do not transmit over a frame already arriving at B. Health checks do not
     * consume pending data or discard a successfully queued packet. */
    if ((state & 0x1F) != 0x0D || count || dev->rx_length_pending ||
        gpio_get_level(dev->gdo0_pin))
        return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

static esp_err_t cc1101_packet_deadline(cc1101_handle_t *dev)
{
    uint8_t e, m;
    esp_err_t err = cc1101_profile_read(dev, CC1101_MDMCFG4, &e);
    if (err == ESP_OK) err = cc1101_profile_read(dev, CC1101_MDMCFG3, &m);
    if (err != ESP_OK) return err;
    /* Two maximum FIFO-frame airtimes plus 20 ms scheduling margin, derived
     * from the actual rate registers rather than assuming 38.4 kbps. */
    uint64_t numerator = (uint64_t)CC1101_FIFO_SIZE * 8 * 1000000 * (1ULL << 28);
    uint64_t denominator = 26000000ULL * (256U + m) * (1ULL << (e & 0x0F));
    dev->rx_packet_deadline_us = esp_timer_get_time() +
        (int64_t)(2 * (numerator / denominator + 1) + 20000);
    return ESP_OK;
}

bool cc1101_poll_packet_with_status(cc1101_handle_t *dev,
                                    uint8_t *buffer,
                                    size_t *len,
                                    bool *crc_ok,
                                    uint8_t *rssi_status,
                                    uint8_t *lqi_crc_status)
{
    if (dev == NULL || buffer == NULL || len == NULL || crc_ok == NULL)
    {
        ESP_LOGE(TAG, "Invalid RX args");
        return false;
    }

    uint8_t count;
    if (cc1101_packet_rx_health(dev, &count) != ESP_OK) return false;
    bool receiving = gpio_get_level(dev->gdo0_pin) != 0;
    if (!dev->rx_packet_deadline_us && (count || receiving)) {
        if (cc1101_packet_deadline(dev) != ESP_OK) return false;
    }
    if (!dev->rx_length_pending) {
        /* Leave at least one byte in the FIFO while the packet is incomplete
         * (SWRZ020E RX FIFO pointer erratum). A lone length byte is not ready. */
        if (count >= 2) {
            uint8_t length;
            if (cc1101_profile_transfer(dev, CC1101_FIFO_ADDR | CC1101_READ_BURST,
                                       NULL, &length, 1) != ESP_OK) {
                (void)cc1101_recover_packet_rx(dev);
                return false;
            }
            dev->rx_packet_length = length;
            dev->rx_length_pending = true;
            --count;
        } else if (!count && !receiving) {
            dev->rx_packet_deadline_us = 0;
            return false;
        }
    }
    if (dev->rx_length_pending) {
        uint8_t length = dev->rx_packet_length;
        if (length > CC1101_MAX_PAYLOAD_LEN || length > *len) {
            ESP_LOGW(TAG, "Invalid/oversized RX length=%u capacity=%u; discarding FIFO",
                     length, (unsigned)*len);
            (void)cc1101_recover_packet_rx(dev);
            return false;
        }
        if (count >= (unsigned)length + 2) {
            uint8_t packet[CC1101_FIFO_SIZE];
            if (cc1101_profile_transfer(dev, CC1101_FIFO_ADDR | CC1101_READ_BURST,
                                       NULL, packet, length + 2) != ESP_OK) {
                (void)cc1101_recover_packet_rx(dev);
                return false;
            }
            memcpy(buffer, packet, length);
            *len = length;
            *crc_ok = (packet[length + 1] & 0x80) != 0;
            if (rssi_status != NULL) *rssi_status = packet[length];
            if (lqi_crc_status != NULL) *lqi_crc_status = packet[length + 1];
            cc1101_rx_parser_reset(dev);
            /* RXOFF_MODE=RX: do not SIDLE/SFRX after success. That used to
             * discard queued packets and create a calibration/deaf interval. */
            return true;
        }
    }
    if (dev->rx_packet_deadline_us &&
        esp_timer_get_time() >= dev->rx_packet_deadline_us) {
        ESP_LOGW(TAG, "Incomplete packet RX timed out; discarding FIFO and rearming");
        (void)cc1101_recover_packet_rx(dev);
    }
    return false;
}

bool cc1101_poll_packet(cc1101_handle_t *dev,
                        uint8_t *buffer,
                        size_t *len,
                        bool *crc_ok)
{
    return cc1101_poll_packet_with_status(dev, buffer, len, crc_ok, NULL, NULL);
}

bool cc1101_receive_packet(cc1101_handle_t *dev,
                           uint8_t *buffer,
                           size_t *len)
{
    bool crc_ok = false;

    if (cc1101_poll_packet(dev, buffer, len, &crc_ok))
        return crc_ok;

    return false;
}

uint8_t cc1101_read_status_reg(cc1101_handle_t *dev, uint8_t reg)
{
    return cc1101_read_status(dev, reg);
}

/* ============================================================
 * EXTRA CONFIG APIs
 * ============================================================ */

esp_err_t cc1101_set_frequency(cc1101_handle_t *dev, uint32_t freq_hz)
{
    uint32_t freq_reg =
        (uint32_t)(((uint64_t)freq_hz << 16) / 26000000ULL);

    esp_err_t err = cc1101_write_reg(dev, CC1101_FREQ2, (freq_reg >> 16) & 0xFF);
    if (err == ESP_OK) err = cc1101_write_reg(dev, CC1101_FREQ1, (freq_reg >> 8) & 0xFF);
    if (err == ESP_OK) err = cc1101_write_reg(dev, CC1101_FREQ0, freq_reg & 0xFF);
    return err;
}

esp_err_t cc1101_set_channel(cc1101_handle_t *dev, uint8_t channel)
{
    return cc1101_write_reg(dev, CC1101_CHANNR, channel);
}

esp_err_t cc1101_set_tx_power(cc1101_handle_t *dev, uint8_t pa_value)
{
    if (dev == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t modulation, frend0;
    esp_err_t err = cc1101_profile_read(dev, CC1101_MDMCFG2, &modulation);
    if (err != ESP_OK) return err;
    err = cc1101_profile_read(dev, CC1101_FREND0, &frend0);
    if (err != ESP_OK) return err;
    frend0 &= ~CC1101_FREND0_PA_POWER_MASK;
    uint8_t table[8] = {0};
    if ((modulation & CC1101_MDMCFG2_MOD_FORMAT_MASK) == (CC1101_MOD_ASK_OOK << 4)) {
        uint8_t old_table[8];
        err = cc1101_profile_transfer(dev, CC1101_PATABLE | CC1101_READ_BURST,
                                     NULL, old_table, sizeof(old_table));
        if (err != ESP_OK) return err;
        /* Modulation code 3 serves both profiles. Do not erase true ASK's
         * configured -10 dBm low level; reset/unrecognized tables mean OOK. */
        table[0] = old_table[0] == 0x34 ? 0x34 : 0x00;
        table[1] = pa_value;
        frend0 |= 1;
    } else table[0] = pa_value;
    err = cc1101_profile_transfer(dev, CC1101_PATABLE | CC1101_WRITE_BURST,
                                 table, NULL, sizeof(table));
    if (err != ESP_OK) return err;
    return cc1101_profile_transfer(dev, CC1101_FREND0, &frend0, NULL, 1);
}

esp_err_t cc1101_set_datarate(cc1101_handle_t *dev, uint32_t baud)
{
    uint8_t best_e = 0;
    uint8_t best_m = 0;

    for (uint8_t e = 0; e < 16; e++)
    {
        uint64_t val = ((uint64_t)baud << 28) / (26000000ULL << e);

        if (val >= 256 && val <= 511)
        {
            best_e = e;
            best_m = (uint8_t)(val - 256);
            break;
        }
    }

    uint8_t mdmcfg4;
    esp_err_t err = cc1101_profile_read(dev, CC1101_MDMCFG4, &mdmcfg4);
    if (err != ESP_OK) return err;
    mdmcfg4 &= 0xF0;
    mdmcfg4 |= best_e;

    err = cc1101_write_reg(dev, CC1101_MDMCFG4, mdmcfg4);
    if (err == ESP_OK) err = cc1101_write_reg(dev, CC1101_MDMCFG3, best_m);
    return err;
}

void cc1101_dump_registers(cc1101_handle_t *dev)
{
    ESP_LOGI(TAG, "==== REGISTER DUMP ====");

    for (uint8_t reg = 0; reg <= CC1101_TEST0; reg++)
    {
        uint8_t value = cc1101_read_reg(dev, reg);
        ESP_LOGI(TAG, "REG[0x%02X] = 0x%02X", reg, value);
    }

    ESP_LOGI(TAG, "PARTNUM   : 0x%02X",
             cc1101_read_status_reg(dev, CC1101_PARTNUM));
    ESP_LOGI(TAG, "VERSION   : 0x%02X",
             cc1101_read_status_reg(dev, CC1101_VERSION));
    ESP_LOGI(TAG, "MARCSTATE : 0x%02X",
             cc1101_read_status_reg(dev, CC1101_MARCSTATE));
}
