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
}

static inline void cc1101_deselect(cc1101_handle_t *dev)
{
    gpio_set_level(dev->cs_pin, 1);
}

static esp_err_t cc1101_wait_miso(cc1101_handle_t *dev)
{
    int timeout = 100000;

    while (gpio_get_level(dev->miso_pin))
    {
        if (--timeout <= 0)
        {
            ESP_LOGE(TAG, "MISO timeout");
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(1);
    }

    return ESP_OK;
}

uint8_t cc1101_strobe(cc1101_handle_t *dev, uint8_t strobe)
{
    uint8_t status = 0;
    cc1101_select(dev);

    if (cc1101_wait_miso(dev) == ESP_OK)
    {
        spi_transaction_t t = {0};
        t.length = 8;
        t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
        t.tx_data[0] = strobe;

        if (spi_device_polling_transmit(dev->spi, &t) == ESP_OK)
            status = t.rx_data[0];
    }

    cc1101_deselect(dev);
    return status;
}

static void cc1101_write_reg(cc1101_handle_t *dev, uint8_t reg, uint8_t value)
{
    cc1101_select(dev);

    if (cc1101_wait_miso(dev) == ESP_OK)
    {
        spi_transaction_t t = {0};
        t.length = 16;
        t.flags = SPI_TRANS_USE_TXDATA;
        t.tx_data[0] = reg;
        t.tx_data[1] = value;
        spi_device_polling_transmit(dev->spi, &t);
    }

    cc1101_deselect(dev);
}

static uint8_t cc1101_read_reg(cc1101_handle_t *dev, uint8_t reg)
{
    uint8_t value = 0;
    cc1101_select(dev);

    if (cc1101_wait_miso(dev) == ESP_OK)
    {
        spi_transaction_t t = {0};
        t.length = 16;
        t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
        t.tx_data[0] = reg | CC1101_READ_SINGLE;
        t.tx_data[1] = 0;

        spi_device_polling_transmit(dev->spi, &t);
        value = t.rx_data[1];
    }

    cc1101_deselect(dev);
    return value;
}

static uint8_t cc1101_read_status(cc1101_handle_t *dev, uint8_t reg)
{
    uint8_t value = 0;
    cc1101_select(dev);

    if (cc1101_wait_miso(dev) == ESP_OK)
    {
        spi_transaction_t t = {0};
        t.length = 16;
        t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
        t.tx_data[0] = reg | CC1101_READ_BURST;
        t.tx_data[1] = 0;

        spi_device_polling_transmit(dev->spi, &t);
        value = t.rx_data[1];
    }

    cc1101_deselect(dev);
    return value;
}

static void cc1101_write_burst(cc1101_handle_t *dev,
                               uint8_t reg,
                               const uint8_t *data,
                               uint8_t len)
{
    cc1101_select(dev);

    if (cc1101_wait_miso(dev) == ESP_OK)
    {
        spi_transaction_t t1 = {0};
        t1.length = 8;
        t1.flags = SPI_TRANS_USE_TXDATA;
        t1.tx_data[0] = reg | CC1101_WRITE_BURST;
        spi_device_polling_transmit(dev->spi, &t1);

        spi_transaction_t t2 = {0};
        t2.length = len * 8;
        t2.tx_buffer = data;
        spi_device_polling_transmit(dev->spi, &t2);
    }

    cc1101_deselect(dev);
}

static void cc1101_read_burst(cc1101_handle_t *dev,
                              uint8_t reg,
                              uint8_t *data,
                              uint8_t len)
{
    cc1101_select(dev);

    if (cc1101_wait_miso(dev) == ESP_OK)
    {
        spi_transaction_t t1 = {0};
        t1.length = 8;
        t1.flags = SPI_TRANS_USE_TXDATA;
        t1.tx_data[0] = reg | CC1101_READ_BURST;
        spi_device_polling_transmit(dev->spi, &t1);

        spi_transaction_t t2 = {0};
        t2.length = len * 8;
        t2.rx_buffer = data;
        spi_device_polling_transmit(dev->spi, &t2);
    }

    cc1101_deselect(dev);
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

/* Focused checked transport for profile transactions only. Unlike legacy
 * accessors it propagates MISO/SPI failures instead of manufacturing zero.
 * Header + up to eight bytes use one transaction/one CS assertion. */
static esp_err_t cc1101_profile_transfer(cc1101_handle_t *dev, uint8_t command,
                                        const uint8_t *data, uint8_t *out, size_t len)
{
    if (dev == NULL || len > 8) return ESP_ERR_INVALID_ARG;
    uint8_t tx[9] = {command};
    uint8_t rx[9] = {0};
    if (data != NULL) memcpy(tx + 1, data, len);
    cc1101_select(dev);
    esp_err_t err = ESP_OK;
    unsigned remaining = 2000;
    while (gpio_get_level(dev->miso_pin)) {
        if (--remaining == 0) { err = ESP_ERR_TIMEOUT; break; }
        esp_rom_delay_us(1);
    }
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
        CC1101_FREND0, CC1101_IOCFG0};
    uint8_t *fields[] = {&value.pktctrl0, &value.mdmcfg2, &value.deviatn,
        &value.agcctrl2, &value.agcctrl1, &value.agcctrl0, &value.foccfg,
        &value.frend0, &value.iocfg0};
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
        table[1] = CC1101_DEFAULT_PA_433_MHZ;
    } else table[0] = CC1101_DEFAULT_PA_433_MHZ;
    /* 26 MHz crystal, +/-20.629883 kHz FSK deviation. Amplitude profiles
     * ignore deviation; writing it still makes every transition deterministic. */
    uint32_t frequency = (uint32_t)(((uint64_t)433920000 << 16) / 26000000ULL);
    const struct { uint8_t reg, value; } writes[] = {
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

esp_err_t cc1101_init(cc1101_handle_t *dev,
                      spi_device_handle_t spi_handle,
                      gpio_num_t cs,
                      gpio_num_t miso,
                      gpio_num_t gdo0)
{
    dev->spi = spi_handle;
    dev->cs_pin = cs;
    dev->miso_pin = miso;
    dev->gdo0_pin = gdo0;

    cc1101_reset(dev);
    return ESP_OK;
}

void cc1101_reset(cc1101_handle_t *dev)
{
    cc1101_deselect(dev);
    esp_rom_delay_us(5);

    cc1101_select(dev);
    esp_rom_delay_us(10);
    cc1101_deselect(dev);
    esp_rom_delay_us(40);

    cc1101_select(dev);
    cc1101_wait_miso(dev);

    spi_transaction_t t = {0};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = CC1101_SRES;
    spi_device_polling_transmit(dev->spi, &t);

    cc1101_wait_miso(dev);
    cc1101_deselect(dev);

    vTaskDelay(pdMS_TO_TICKS(10));
}

void cc1101_config(cc1101_handle_t *dev)
{
    cc1101_write_reg(dev, CC1101_IOCFG2, CC1101_GDO_SYNC_WORD);
    cc1101_write_reg(dev, CC1101_IOCFG0, CC1101_GDO_SYNC_WORD);

    cc1101_write_reg(dev, CC1101_PKTLEN, 255);

    cc1101_write_reg(dev, CC1101_PKTCTRL1,
                     CC1101_APPEND_STATUS | CC1101_ADR_CHK_NONE);

    cc1101_write_reg(dev, CC1101_PKTCTRL0,
                     CC1101_CRC_ENABLE | CC1101_PKT_VARIABLE);

    cc1101_set_frequency(dev, 433920000);

    cc1101_write_reg(dev, CC1101_MDMCFG4,
                     CC1101_MDMCFG4_VALUE(3, 0, 10));
    cc1101_write_reg(dev, CC1101_MDMCFG3,
                     CC1101_MDMCFG3_VALUE(131));
    cc1101_write_reg(dev, CC1101_MDMCFG2,
                     CC1101_MDMCFG2_VALUE(
                         0,
                          CC1101_MOD_ASK_OOK,
                         0,
                         CC1101_SYNC_30_32));
    cc1101_write_reg(dev, CC1101_MDMCFG1,
                     CC1101_MDMCFG1_VALUE(0, 2, 2));
    cc1101_write_reg(dev, CC1101_MDMCFG0,
                     CC1101_MDMCFG0_VALUE(248));

    /* OOK requires FOC_LIMIT=0; retain all other frequency-offset settings. */
    uint8_t foccfg = cc1101_read_reg(dev, CC1101_FOCCFG);
    cc1101_write_reg(dev, CC1101_FOCCFG,
                     foccfg & ~CC1101_FOCCFG_FOC_LIMIT_MASK);

    /* TI DN022 OOK starting profile: hardware tuning is still required.
     * Keep the existing ~38.4 kbps / 101.5625 kHz rate/bandwidth settings.
     * FREND1 retains reset 0xB6, suitable for bandwidths above 101 kHz. */
    cc1101_write_reg(dev, CC1101_AGCCTRL2, 0x03);
    cc1101_write_reg(dev, CC1101_AGCCTRL1, 0x00);
    cc1101_write_reg(dev, CC1101_AGCCTRL0, 0x91);

    /* Configure both radios at nominal +7 dBm; actual ERP needs measurement. */
    /* This default configuration explicitly selects OOK, including when a
     * caller reapplies it after true ASK. The power helper itself preserves ASK. */
    const uint8_t off_table[8] = {0};
    cc1101_write_burst(dev, CC1101_PATABLE, off_table, sizeof(off_table));
    cc1101_set_tx_power(dev, CC1101_DEFAULT_PA_433_MHZ);

    cc1101_write_reg(dev, CC1101_MCSM0,
                     CC1101_AUTOCAL_IDLE_TO_RXTX);

    cc1101_write_reg(dev, CC1101_MCSM1, 0x3F);

    cc1101_strobe(dev, CC1101_SCAL);
    vTaskDelay(pdMS_TO_TICKS(10));
}

esp_err_t cc1101_set_tx_len(cc1101_handle_t *dev, uint8_t len) {
    cc1101_write_reg(dev, CC1101_PKTLEN, len);
    return ESP_OK;
}

void cc1101_set_tx_mode(cc1101_handle_t *dev)
{
    cc1101_strobe(dev, CC1101_SIDLE);
    cc1101_strobe(dev, CC1101_SFTX);
}

void cc1101_transmit(cc1101_handle_t *dev, uint8_t *data, size_t len)
{
    if (dev == NULL || data == NULL)
    {
        ESP_LOGE(TAG, "Invalid transmit arguments");
        return;
    }

    if (len == 0)
    {
        ESP_LOGW(TAG, "Empty payload");
        return;
    }

    if (len > CC1101_MAX_PAYLOAD_LEN)
    {
        ESP_LOGW(TAG,
                 "Payload too large (%u), truncating to %u",
                 (unsigned)len,
                 (unsigned)CC1101_MAX_PAYLOAD_LEN);

        len = CC1101_MAX_PAYLOAD_LEN;
    }

    uint8_t packet[CC1101_FIFO_SIZE + 1];
    packet[0] = (uint8_t)len;

    memcpy(&packet[1], data, len);

    cc1101_strobe(dev, CC1101_SIDLE);
    cc1101_strobe(dev, CC1101_SFTX);

    cc1101_write_burst(
        dev,
        CC1101_FIFO_ADDR,
        packet,
        (uint8_t)(len + 1));

    uint8_t status = cc1101_strobe(dev, CC1101_STX);

    ESP_LOGI(TAG,
             "TX started, payload=%u bytes, status=0x%02X",
             (unsigned)len,
             status);
}

void cc1101_set_rx_mode(cc1101_handle_t *dev)
{
    cc1101_strobe(dev, CC1101_SIDLE);
    cc1101_strobe(dev, CC1101_SFRX);
    cc1101_strobe(dev, CC1101_SRX);
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

bool cc1101_poll_packet(cc1101_handle_t *dev,
                        uint8_t *buffer,
                        size_t *len,
                        bool *crc_ok)
{
    uint8_t rx_bytes = cc1101_read_status(dev, CC1101_RXBYTES);

    if (rx_bytes & 0x80)
    {
        ESP_LOGW(TAG, "RX overflow");
        cc1101_set_rx_mode(dev);
        return false;
    }

    if ((rx_bytes & 0x7F) == 0)
        return false;

    if (buffer == NULL || len == NULL || crc_ok == NULL)
    {
        ESP_LOGE(TAG, "Invalid RX args");
        return false;
    }

    uint8_t pkt_len = cc1101_read_reg(dev, CC1101_FIFO_ADDR);

    if (pkt_len > *len)
    {
        ESP_LOGE(TAG,
        "Packet too large: packet=%u buffer=%u",
        pkt_len,
        (unsigned)*len);

        cc1101_set_rx_mode(dev);
        return false;
    }

    cc1101_read_burst(dev, CC1101_FIFO_ADDR, buffer, pkt_len);
    *len = pkt_len;

    uint8_t status[2];
    cc1101_read_burst(dev, CC1101_FIFO_ADDR, status, 2);

    cc1101_set_rx_mode(dev);

    *crc_ok = (status[1] & 0x80) != 0;
    return true;
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

    cc1101_write_reg(dev, CC1101_FREQ2, (freq_reg >> 16) & 0xFF);
    cc1101_write_reg(dev, CC1101_FREQ1, (freq_reg >> 8) & 0xFF);
    cc1101_write_reg(dev, CC1101_FREQ0, freq_reg & 0xFF);

    return ESP_OK;
}

esp_err_t cc1101_set_channel(cc1101_handle_t *dev, uint8_t channel)
{
    cc1101_write_reg(dev, CC1101_CHANNR, channel);
    return ESP_OK;
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

    uint8_t mdmcfg4 = cc1101_read_reg(dev, CC1101_MDMCFG4);
    mdmcfg4 &= 0xF0;
    mdmcfg4 |= best_e;

    cc1101_write_reg(dev, CC1101_MDMCFG4, mdmcfg4);
    cc1101_write_reg(dev, CC1101_MDMCFG3, best_m);

    return ESP_OK;
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
