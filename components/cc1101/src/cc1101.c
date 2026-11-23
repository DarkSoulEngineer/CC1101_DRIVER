/**
 * @file cc1101.c
 * @brief Checked CC1101 driver: SPI transport, register profiles, manual TX
 *        and packet RX with bounded FIFO recovery.
 *
 * Every register/profile/FIFO access propagates MISO/SPI failures instead of
 * manufacturing a plausible zero. Header plus payload (up to one FIFO) travel
 * in a single transaction under one CS assertion on dev->spi, for TX as well
 * as RX: no split header/data transfers and no ignored errors.
 *
 * All *_locked entry points require the caller to already own the bus
 * serialization; none acquire a mutex. Legacy void wrappers exist for
 * compatibility and only log failures.
 */
#include "cc1101.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <limits.h>

static const char *TAG = "CC1101";

/** Register/value pair used by the deterministic write tables below. */
typedef struct { uint8_t reg, value; } cc1101_reg_write_t;

/**
 * @brief Assert chip select and let it settle before clocking.
 */
static inline void cc1101_select(cc1101_handle_t *dev)
{
    gpio_set_level(dev->cs_pin, 0);
    esp_rom_delay_us(1);
}

/**
 * @brief Let the last bit settle, then release chip select.
 */
static inline void cc1101_deselect(cc1101_handle_t *dev)
{
    esp_rom_delay_us(1);
    gpio_set_level(dev->cs_pin, 1);
}

/**
 * @brief Wait for the radio to release MISO (chip ready), bounded to ~2 ms.
 */
static esp_err_t cc1101_wait_miso(cc1101_handle_t *dev)
{
    unsigned remaining = 2000;
    while (gpio_get_level(dev->miso_pin)) {
        if (--remaining == 0) return ESP_ERR_TIMEOUT;
        esp_rom_delay_us(1);
    }
    return ESP_OK;
}

/**
 * @brief One checked header+payload SPI transaction under a single CS pulse.
 *
 * Transfers of at most four bytes use the SPI driver's inline data registers
 * (no external buffer setup); longer transfers copy into bounded local
 * buffers. Only the command byte needs initialization: the chip ignores the
 * remaining TX bytes on reads, so zero-filling whole buffers is unnecessary.
 *
 * @param command Header byte (register address plus access bits, or strobe).
 * @param data    Payload to write, or NULL for reads/strobes.
 * @param out     Destination for read bytes, or NULL for writes/strobes.
 * @param len     Payload length in bytes, at most CC1101_FIFO_SIZE.
 */
static esp_err_t cc1101_profile_transfer(cc1101_handle_t *dev, uint8_t command,
                                        const uint8_t *data, uint8_t *out, size_t len)
{
    if (dev == NULL || dev->spi == NULL || len > CC1101_FIFO_SIZE)
        return ESP_ERR_INVALID_ARG;
    spi_transaction_t transaction = {.length = (len + 1) * 8};
    uint8_t tx[CC1101_FIFO_SIZE + 1];
    uint8_t rx[CC1101_FIFO_SIZE + 1];
    if (len < sizeof(transaction.tx_data)) {
        transaction.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
        transaction.tx_data[0] = command;
        if (data != NULL) memcpy(transaction.tx_data + 1, data, len);
    } else {
        tx[0] = command;
        if (data != NULL) memcpy(tx + 1, data, len);
        transaction.tx_buffer = tx;
        transaction.rx_buffer = rx;
    }
    cc1101_select(dev);
    esp_err_t err = cc1101_wait_miso(dev);
    if (err == ESP_OK)
        err = spi_device_polling_transmit(dev->spi, &transaction);
    cc1101_deselect(dev);
    if (err == ESP_OK && out != NULL) {
        const uint8_t *received =
            (transaction.flags & SPI_TRANS_USE_RXDATA) ? transaction.rx_data : rx;
        memcpy(out, received + 1, len);
    }
    return err;
}

/**
 * @brief Read one configuration register with error propagation.
 */
static esp_err_t cc1101_profile_read(cc1101_handle_t *dev, uint8_t reg, uint8_t *out)
{
    return cc1101_profile_transfer(dev, reg | CC1101_READ_SINGLE, NULL, out, 1);
}

/**
 * @brief Write one configuration register with error propagation.
 */
static esp_err_t cc1101_write_reg(cc1101_handle_t *dev, uint8_t reg, uint8_t value)
{
    return cc1101_profile_transfer(dev, reg, &value, NULL, 1);
}

/**
 * @brief Compatibility read accessor with no error return.
 *
 * Active init/profile/TX/RX paths use the checked APIs instead; this wrapper
 * only logs failures and never turns a failed read into a plausible zero
 * silently (the sentinel 0xFF marks the failure).
 */
static uint8_t cc1101_read_reg(cc1101_handle_t *dev, uint8_t reg)
{
    uint8_t value = 0xFF;
    esp_err_t err = cc1101_profile_read(dev, reg, &value);
    if (err != ESP_OK) ESP_LOGE(TAG, "Register %02X read failed: %s", reg, esp_err_to_name(err));
    return value;
}

/**
 * @brief Compatibility status-register read; logs failures, 0xFF sentinel.
 */
static uint8_t cc1101_read_status(cc1101_handle_t *dev, uint8_t reg)
{
    uint8_t value = 0xFF;
    esp_err_t err = cc1101_profile_transfer(dev, reg | CC1101_READ_BURST, NULL, &value, 1);
    if (err != ESP_OK) ESP_LOGE(TAG, "Status %02X read failed: %s", reg, esp_err_to_name(err));
    return value;
}

/**
 * @brief Issue a command strobe and return the chip status byte.
 *
 * Logs transport failures and returns the 0xFF sentinel instead of a
 * plausible status; callers on active paths use checked transfers.
 */
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

/**
 * @brief Forget any partial packet framing and its stall deadline.
 */
static void cc1101_rx_parser_reset(cc1101_handle_t *dev)
{
    dev->rx_length_pending = false;
    dev->rx_packet_length = 0;
    dev->rx_packet_deadline_us = 0;
}

/**
 * @brief Read a dynamic multi-bit status register until two reads agree.
 *
 * TI SWRZ020E: dynamic status must agree twice. Retries are bounded;
 * disagreement is not permission to consume or flush the FIFO.
 */
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

/**
 * @brief Poll MARCSTATE until the requested state is confirmed (~20 ms bound).
 */
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

/**
 * @brief Strobe SIDLE and confirm the IDLE state; clears TX preparation.
 */
esp_err_t cc1101_idle_locked(cc1101_handle_t *dev)
{
    if (dev == NULL) return ESP_ERR_INVALID_ARG;
    dev->tx_prepared = false;
    esp_err_t err = cc1101_profile_transfer(dev, CC1101_SIDLE, NULL, NULL, 0);
    return err == ESP_OK ? cc1101_profile_wait_state(dev, 0x01) : err;
}

/**
 * @brief Strobe SRX and confirm the RX state.
 *
 * On failure a best-effort IDLE is attempted; the original error wins.
 */
esp_err_t cc1101_start_rx_locked(cc1101_handle_t *dev)
{
    esp_err_t err = cc1101_profile_transfer(dev, CC1101_SRX, NULL, NULL, 0);
    if (err == ESP_OK) err = cc1101_profile_wait_state(dev, 0x0D);
    if (err != ESP_OK && dev != NULL)
        (void)cc1101_idle_locked(dev);
    return err;
}

/** Configuration registers sampled by cc1101_profile_readback_locked(). */
static const uint8_t s_readback_regs[] = {
    CC1101_PKTCTRL0, CC1101_MDMCFG2, CC1101_DEVIATN,
    CC1101_AGCCTRL2, CC1101_AGCCTRL1, CC1101_AGCCTRL0, CC1101_FOCCFG,
    CC1101_FREND0, CC1101_IOCFG0, CC1101_PKTCTRL1, CC1101_PKTLEN,
    CC1101_SYNC1, CC1101_SYNC0, CC1101_MCSM1, CC1101_MDMCFG4, CC1101_MDMCFG3,
    CC1101_FREQ2, CC1101_FREQ1, CC1101_FREQ0, CC1101_CHANNR, CC1101_FIFOTHR
};

/**
 * @brief Byte offsets of the readback registers inside cc1101_profile_status_t.
 *
 * Generated from s_readback_regs so the two tables can never drift apart.
 * PATABLE and MARCSTATE are read separately as burst/status accesses.
 */
static const size_t s_readback_offsets[] = {
#define CC1101_READBACK_OFFSET(field) offsetof(cc1101_profile_status_t, field)
    CC1101_READBACK_OFFSET(pktctrl0), CC1101_READBACK_OFFSET(mdmcfg2),
    CC1101_READBACK_OFFSET(deviatn), CC1101_READBACK_OFFSET(agcctrl2),
    CC1101_READBACK_OFFSET(agcctrl1), CC1101_READBACK_OFFSET(agcctrl0),
    CC1101_READBACK_OFFSET(foccfg), CC1101_READBACK_OFFSET(frend0),
    CC1101_READBACK_OFFSET(iocfg0), CC1101_READBACK_OFFSET(pktctrl1),
    CC1101_READBACK_OFFSET(pktlen), CC1101_READBACK_OFFSET(sync1),
    CC1101_READBACK_OFFSET(sync0), CC1101_READBACK_OFFSET(mcsm1),
    CC1101_READBACK_OFFSET(mdmcfg4), CC1101_READBACK_OFFSET(mdmcfg3),
    CC1101_READBACK_OFFSET(freq2), CC1101_READBACK_OFFSET(freq1),
    CC1101_READBACK_OFFSET(freq0), CC1101_READBACK_OFFSET(channr),
    CC1101_READBACK_OFFSET(fifothr)
#undef CC1101_READBACK_OFFSET
};

/**
 * @brief Read back the full profile image from hardware, checked.
 *
 * Publishes *status only after every checked read succeeds. Neither an
 * all-zero bus image nor an undefined MARCSTATE counts as a successful
 * readback, even if each host SPI call succeeded.
 */
esp_err_t cc1101_profile_readback_locked(cc1101_handle_t *dev,
                                        cc1101_profile_status_t *status)
{
    if (dev == NULL || status == NULL) return ESP_ERR_INVALID_ARG;
    cc1101_profile_status_t value = {0};
    uint8_t *base = (uint8_t *)&value;
    for (size_t i = 0; i < sizeof(s_readback_regs); i++) {
        esp_err_t err = cc1101_profile_read(dev, s_readback_regs[i],
                                            base + s_readback_offsets[i]);
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
    if (value.marcstate > 0x16 || memcmp(&value, &empty, sizeof(value)) == 0)
        return ESP_FAIL;
    *status = value;
    return ESP_OK;
}

/**
 * @brief Apply one coordinated modulation/format profile, verified end to end.
 *
 * Validates the enums before any I/O, confirms IDLE before flushing both
 * FIFOs, writes the deterministic register delta, verifies every written
 * register plus the PA table, and returns the chip to IDLE. It never starts
 * TX or RX. Partial failure is not rolled back; the caller must invalidate
 * its applied-state bookkeeping.
 *
 * Frozen 26 MHz engineering values: 433.92 MHz, MDMCFG4/3 ~38.383 kbps,
 * DEVIATN 0x35 (~+/-20.63 kHz). Amplitude profiles ignore deviation; writing
 * it still makes every transition deterministic. OOK uses the configured
 * CC1101_OOK_PA_433_MHZ high level; true ASK uses the configured low/high
 * pair, and FSK/GFSK use the configured constant-envelope level.
 */
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
        table[0] = modulation == CC1101_PROFILE_ASK ?
            CC1101_ASK_LOW_PA_433_MHZ : 0x00;
        table[1] = modulation == CC1101_PROFILE_OOK ?
            CC1101_OOK_PA_433_MHZ : CC1101_ASK_HIGH_PA_433_MHZ;
    } else table[0] = CC1101_FSK_PA_433_MHZ;
    uint32_t frequency = (uint32_t)(((uint64_t)433920000 << 16) / 26000000ULL);
    /*
     * Deterministic sync/framing for A and B: 61-byte payload plus length and
     * RSSI/LQI status fits the FIFO; long streaming is unsupported. The
     * MDMCFG1 write clears FEC while preserving preamble/channel spacing.
     */
    const cc1101_reg_write_t writes[] = {
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
        {CC1101_MDMCFG1, mdmcfg1 & 0x7F},
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

/**
 * @brief Full hardware reset sequence (CS pulse train + SRES), checked.
 *
 * Clears all software parser/preparation state, waits for MISO readiness
 * around the reset strobe, then confirms IDLE after the datasheet delay.
 */
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

/**
 * @brief Bind the handle to its SPI device/pins, reset, and verify chip ID.
 *
 * Succeeds only when PARTNUM reads 0 and VERSION is neither 0 nor 0xFF.
 */
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

/**
 * @brief Compatibility reset wrapper; logs failures instead of returning them.
 */
void cc1101_reset(cc1101_handle_t *dev)
{
    esp_err_t err = cc1101_reset_checked(dev);
    if (err != ESP_OK) ESP_LOGE(TAG, "Reset failed: %s", esp_err_to_name(err));
}

/**
 * @brief Baseline packet configuration after reset, checked.
 *
 * Writes the deterministic OOK/packet starting profile (433.92 MHz, variable
 * length + CRC + appended status, RX-after-RX), disables the FOC limit,
 * zeroes then loads the PA table, and finishes with a manual calibration
 * confirmed in IDLE. Never starts TX or RX.
 */
esp_err_t cc1101_config_checked(cc1101_handle_t *dev)
{
    if (dev == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = cc1101_idle_locked(dev);
    if (err != ESP_OK) return err;
    const cc1101_reg_write_t writes[] = {
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

/**
 * @brief Compatibility configuration wrapper; logs failures.
 */
void cc1101_config(cc1101_handle_t *dev)
{
    esp_err_t err = cc1101_config_checked(dev);
    if (err != ESP_OK) ESP_LOGE(TAG, "Configuration failed: %s", esp_err_to_name(err));
}

/**
 * @brief Set the maximum packet length register (payload bound enforced).
 */
esp_err_t cc1101_set_tx_len(cc1101_handle_t *dev, uint8_t len)
{
    if (len > CC1101_MAX_PAYLOAD_LEN) return ESP_ERR_INVALID_ARG;
    return cc1101_write_reg(dev, CC1101_PKTLEN, len);
}

/**
 * @brief Compatibility TX-mode helper: IDLE plus TX FIFO flush, logs errors.
 */
void cc1101_set_tx_mode(cc1101_handle_t *dev)
{
    esp_err_t err = cc1101_idle_locked(dev);
    if (err == ESP_OK) err = cc1101_profile_transfer(dev, CC1101_SFTX, NULL, NULL, 0);
    if (err != ESP_OK) ESP_LOGE(TAG, "TX FIFO reset failed: %s", esp_err_to_name(err));
}

/**
 * @brief Load one length-prefixed packet into the TX FIFO, checked, no TX.
 *
 * Verifies IDLE, flushes the TX FIFO, confirms empty occupancy with a stable
 * status read, checks the packet framing registers, writes length+payload in
 * one contiguous burst, and re-verifies exact FIFO occupancy. Preparation is
 * consumed by cc1101_tx_start_locked(); it never transmits by itself.
 */
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

/**
 * @brief Issue the single STX strobe for a prepared packet, exactly once.
 *
 * Preparation is consumed even on error: the command may have reached the
 * chip before a transport failure, so a potentially started TX is never
 * retried by this driver.
 */
esp_err_t cc1101_tx_start_locked(cc1101_handle_t *dev)
{
    if (dev == NULL || !dev->tx_prepared) return ESP_ERR_INVALID_STATE;
    dev->tx_prepared = false;
    return cc1101_profile_transfer(dev, CC1101_STX, NULL, NULL, 0);
}

/**
 * @brief Sample TXBYTES/MARCSTATE (stable reads) plus the GDO0 level.
 *
 * TXBYTES bit 7 flags FIFO underflow; MARCSTATE is masked to five bits.
 */
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

/**
 * @brief Compatibility one-shot transmit (prepare + start); logs failures.
 */
void cc1101_transmit(cc1101_handle_t *dev, uint8_t *data, size_t len)
{
    esp_err_t err = cc1101_tx_prepare_locked(dev, data, len);
    if (err == ESP_OK) err = cc1101_tx_start_locked(dev);
    if (err != ESP_OK) ESP_LOGE(TAG, "TX start failed: %s", esp_err_to_name(err));
}

/**
 * @brief Compatibility RX restart: parser reset, IDLE, flush, SRX; logs errors.
 */
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

/**
 * @brief Legacy void format-status snapshot; 0xFF MARCSTATE means unavailable.
 */
void cc1101_get_rx_format_status(cc1101_handle_t *dev, cc1101_rx_format_status_t *status)
{
    if (status == NULL) return;
    cc1101_profile_status_t actual;
    memset(status, 0, sizeof(*status));
    status->marcstate = 0xFF;
    if (cc1101_profile_readback_locked(dev, &actual) != ESP_OK) return;
    status->pktctrl0 = actual.pktctrl0;
    status->mdmcfg2 = actual.mdmcfg2;
    status->iocfg0 = actual.iocfg0;
    status->marcstate = actual.marcstate;
}

/**
 * @brief Switch RX format while preserving the modulation read back from HW.
 *
 * Requires the caller to own the bus lock and disable the associated GDO
 * interrupt. Failure does not arm RX; nothing is flushed unless IDLE was
 * confirmed first.
 */
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
        if (actual.patable[0] == CC1101_ASK_LOW_PA_433_MHZ &&
            actual.patable[1] == CC1101_ASK_HIGH_PA_433_MHZ)
            modulation = CC1101_PROFILE_ASK;
        else if (actual.patable[0] == 0x00) modulation = CC1101_PROFILE_OOK;
        else return ESP_ERR_INVALID_STATE;
        break;
    default: return ESP_ERR_INVALID_STATE;
    }
    err = cc1101_apply_profile_locked(dev, modulation, format);
    return err == ESP_OK ? cc1101_start_rx_locked(dev) : err;
}

/**
 * @brief Confirmed IDLE -> SFRX -> RX recovery for malformed/stalled RX.
 *
 * Marks recovery pending until the sequence succeeds so the next health
 * check retries it. Never flushes before IDLE is confirmed.
 */
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

/**
 * @brief Check RX FIFO/state health, recovering overflow or stalls.
 *
 * Retries a pending recovery first, then reads stable RXBYTES/MARCSTATE.
 * Overflow can stick in RX without setting the overflow flag (SWRZ020E), and
 * a count beyond the physical FIFO is also a recovery condition. On success
 * *count holds the masked FIFO byte count; an idle radio is rearmed into RX.
 */
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

/**
 * @brief Gate a manual TX on Radio B being idle-listening with an empty FIFO.
 *
 * Health checks run first but do not consume pending data or discard a
 * successfully queued packet. Any queued/partial frame, non-RX state, or
 * asserted GDO0 rejects the transmit request.
 */
esp_err_t cc1101_packet_rx_ready_locked(cc1101_handle_t *dev)
{
    if (dev == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t count, state;
    esp_err_t err = cc1101_packet_rx_health(dev, &count);
    if (err != ESP_OK) return err;
    err = cc1101_read_stable_status(dev, CC1101_MARCSTATE, &state);
    if (err != ESP_OK) return err;
    if ((state & 0x1F) != 0x0D || count || dev->rx_length_pending ||
        gpio_get_level(dev->gdo0_pin))
        return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

/**
 * @brief Arm the incomplete-frame deadline from the actual rate registers.
 *
 * Two maximum FIFO-frame airtimes plus a 20 ms scheduling margin, derived
 * from MDMCFG4/3 rather than assuming 38.4 kbps.
 */
static esp_err_t cc1101_packet_deadline(cc1101_handle_t *dev)
{
    uint8_t e, m;
    esp_err_t err = cc1101_profile_read(dev, CC1101_MDMCFG4, &e);
    if (err == ESP_OK) err = cc1101_profile_read(dev, CC1101_MDMCFG3, &m);
    if (err != ESP_OK) return err;
    uint64_t numerator = (uint64_t)CC1101_FIFO_SIZE * 8 * 1000000 * (1ULL << 28);
    uint64_t denominator = 26000000ULL * (256U + m) * (1ULL << (e & 0x0F));
    dev->rx_packet_deadline_us = esp_timer_get_time() +
        (int64_t)(2 * (numerator / denominator + 1) + 20000);
    return ESP_OK;
}

/**
 * @brief Consume one complete packet if present, reporting the FCS outcome.
 *
 * Unlike cc1101_receive_packet(), a frame that failed the hardware CRC check
 * is reported (true + *crc_ok=false) instead of collapsing to "nothing".
 * Only complete frames are consumed; other queued frames remain in the FIFO.
 * Partial frames are retained between calls, keeping at least one FIFO byte
 * until payload plus the two appended status bytes are present (SWRZ020E RX
 * FIFO pointer erratum). Malformed/stalled/overflowed reception is discarded
 * via checked IDLE -> SFRX -> RX recovery.
 *
 * Caller owns serialization. Requires variable length, APPEND_STATUS, CRC,
 * no CRC autoflush, and MCSM1.RXOFF_MODE=RX (as configured by this driver);
 * after success the receiver stays in RX with no SIDLE/SFRX, so queued
 * packets survive and no calibration/deaf interval is created.
 */
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
        /* A lone length byte is not ready: keep it queued until more arrives. */
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

/**
 * @brief Packet poll without the optional RSSI/LQI status outputs.
 */
bool cc1101_poll_packet(cc1101_handle_t *dev,
                        uint8_t *buffer,
                        size_t *len,
                        bool *crc_ok)
{
    return cc1101_poll_packet_with_status(dev, buffer, len, crc_ok, NULL, NULL);
}

/**
 * @brief Legacy receive: true only for a complete frame with a good CRC.
 */
bool cc1101_receive_packet(cc1101_handle_t *dev,
                           uint8_t *buffer,
                           size_t *len)
{
    bool crc_ok = false;
    return cc1101_poll_packet(dev, buffer, len, &crc_ok) && crc_ok;
}

/**
 * @brief Public status-register read (compatibility sentinel semantics).
 */
uint8_t cc1101_read_status_reg(cc1101_handle_t *dev, uint8_t reg)
{
    return cc1101_read_status(dev, reg);
}

/**
 * @brief Program the 24-bit carrier frequency word for a 26 MHz crystal.
 */
esp_err_t cc1101_set_frequency(cc1101_handle_t *dev, uint32_t freq_hz)
{
    uint32_t freq_reg =
        (uint32_t)(((uint64_t)freq_hz << 16) / 26000000ULL);

    esp_err_t err = cc1101_write_reg(dev, CC1101_FREQ2, (freq_reg >> 16) & 0xFF);
    if (err == ESP_OK) err = cc1101_write_reg(dev, CC1101_FREQ1, (freq_reg >> 8) & 0xFF);
    if (err == ESP_OK) err = cc1101_write_reg(dev, CC1101_FREQ0, freq_reg & 0xFF);
    return err;
}

/**
 * @brief Select the channel number register.
 */
esp_err_t cc1101_set_channel(cc1101_handle_t *dev, uint8_t channel)
{
    return cc1101_write_reg(dev, CC1101_CHANNR, channel);
}

/**
 * @brief Set the PA output level while preserving the modulation's PA layout.
 *
 * ASK/OOK share modulation code 3: keep true ASK's configured low level,
 * otherwise use off=0; the high level is pa_value with PA_POWER=1.
 * Reset/unrecognized tables mean OOK. Other modulations use entry/index 0.
 * Caller owns bus serialization.
 */
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
        table[0] = old_table[0] == CC1101_ASK_LOW_PA_433_MHZ ?
            CC1101_ASK_LOW_PA_433_MHZ : 0x00;
        table[1] = pa_value;
        frend0 |= 1;
    } else table[0] = pa_value;
    err = cc1101_profile_transfer(dev, CC1101_PATABLE | CC1101_WRITE_BURST,
                                 table, NULL, sizeof(table));
    if (err != ESP_OK) return err;
    return cc1101_profile_transfer(dev, CC1101_FREND0, &frend0, NULL, 1);
}

/**
 * @brief Quantize a requested baud rate into the DRATE_E/DRATE_M registers.
 *
 * Picks the first exponent whose mantissa lands in [256, 511] for the 26 MHz
 * crystal, preserving the RX bandwidth bits of MDMCFG4. The written value is
 * the quantized rate, not necessarily the exact request.
 */
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

/**
 * @brief Log every configuration and status register (debug aid).
 */
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
