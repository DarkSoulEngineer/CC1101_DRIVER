#include "cc1101.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <limits.h>

static const char *TAG = "CC1101";

/* ============================================================
 * INTERNAL LOW-LEVEL SPI (manual CS, polling transmit)
 * ============================================================ */

static inline void cc1101_select(cc1101_handle_t *dev)
{
    gpio_set_level(dev->cs_pin, 0);
    esp_rom_delay_us(5);
}

static inline void cc1101_deselect(cc1101_handle_t *dev)
{
    gpio_set_level(dev->cs_pin, 1);
    esp_rom_delay_us(5);
}

/* ============================================================
 * ISR - GDO0 falling edge (TX done: GDO0 goes LOW when packet
 * transmission is complete with GDO_SYNC_WORD mode)
 * ============================================================ */

static void IRAM_ATTR cc1101_gdo0_isr(void *arg)
{
    cc1101_handle_t *dev = (cc1101_handle_t *)arg;
    if (dev->tx_pending && dev->tx_caller_task) {
        BaseType_t wake = pdFALSE;
        vTaskNotifyGiveFromISR(dev->tx_caller_task, &wake);
        dev->tx_caller_task = NULL;
        portYIELD_FROM_ISR(wake);
    }
}

/* ============================================================
 * SPI PRIMITIVES
 * ============================================================ */

uint8_t cc1101_strobe(cc1101_handle_t *dev, uint8_t strobe)
{
    uint8_t tx_data[1] = { strobe };
    uint8_t rx_data[1] = { 0 };

    xSemaphoreTake(dev->spi_mutex, portMAX_DELAY);

    cc1101_select(dev);

    spi_transaction_t t = {0};
    t.length = 8;
    t.tx_buffer = tx_data;
    t.rx_buffer = rx_data;
    spi_device_polling_transmit(dev->spi, &t);

    cc1101_deselect(dev);

    xSemaphoreGive(dev->spi_mutex);
    return rx_data[0];
}

void cc1101_write_reg(cc1101_handle_t *dev, uint8_t reg, uint8_t value)
{
    uint8_t tx_data[2] = { reg, value };
    uint8_t rx_data[2] = { 0, 0 };

    xSemaphoreTake(dev->spi_mutex, portMAX_DELAY);

    cc1101_select(dev);

    spi_transaction_t t = {0};
    t.length = 16;
    t.tx_buffer = tx_data;
    t.rx_buffer = rx_data;
    spi_device_polling_transmit(dev->spi, &t);

    cc1101_deselect(dev);

    xSemaphoreGive(dev->spi_mutex);
}

uint8_t cc1101_read_reg(cc1101_handle_t *dev, uint8_t reg)
{
    uint8_t tx_data[2] = { reg | CC1101_READ_SINGLE, 0x00 };
    uint8_t rx_data[2] = { 0x00, 0x00 };

    xSemaphoreTake(dev->spi_mutex, portMAX_DELAY);

    cc1101_select(dev);

    spi_transaction_t t = {0};
    t.length = 16;
    t.tx_buffer = tx_data;
    t.rx_buffer = rx_data;
    spi_device_polling_transmit(dev->spi, &t);

    cc1101_deselect(dev);

    xSemaphoreGive(dev->spi_mutex);
    return rx_data[1];
}

static uint8_t cc1101_read_status(cc1101_handle_t *dev, uint8_t reg)
{
    /* Status registers are single-read (0x80) per the CC1101 datasheet. */
    uint8_t tx_data[2] = { reg | CC1101_READ_SINGLE, 0x00 };
    uint8_t rx_data[2] = { 0x00, 0x00 };

    xSemaphoreTake(dev->spi_mutex, portMAX_DELAY);

    cc1101_select(dev);

    spi_transaction_t t = {0};
    t.length = 16;
    t.tx_buffer = tx_data;
    t.rx_buffer = rx_data;
    spi_device_polling_transmit(dev->spi, &t);

    cc1101_deselect(dev);

    xSemaphoreGive(dev->spi_mutex);
    return rx_data[1];
}

static void cc1101_write_burst(cc1101_handle_t *dev,
                               uint8_t reg,
                               const uint8_t *data,
                               uint8_t len)
{
    uint8_t tx_buf[1 + CC1101_FIFO_SIZE];
    uint8_t rx_buf[1 + CC1101_FIFO_SIZE];
    tx_buf[0] = reg | CC1101_WRITE_BURST;
    memcpy(&tx_buf[1], data, len);
    memset(rx_buf, 0, sizeof(rx_buf));

    xSemaphoreTake(dev->spi_mutex, portMAX_DELAY);

    cc1101_select(dev);

    spi_transaction_t t = {0};
    t.length = (1 + len) * 8;
    t.tx_buffer = tx_buf;
    t.rx_buffer = rx_buf;
    spi_device_polling_transmit(dev->spi, &t);

    cc1101_deselect(dev);

    xSemaphoreGive(dev->spi_mutex);
}

static void cc1101_read_burst(cc1101_handle_t *dev,
                              uint8_t reg,
                              uint8_t *data,
                              uint8_t len)
{
    uint8_t tx_buf[1 + CC1101_FIFO_SIZE];
    uint8_t rx_buf[1 + CC1101_FIFO_SIZE];
    memset(tx_buf, 0, sizeof(tx_buf));
    memset(rx_buf, 0, sizeof(rx_buf));

    tx_buf[0] = reg | CC1101_READ_BURST;

    xSemaphoreTake(dev->spi_mutex, portMAX_DELAY);

    cc1101_select(dev);

    spi_transaction_t t = {0};
    t.length = (1 + len) * 8;
    t.tx_buffer = tx_buf;
    t.rx_buffer = rx_buf;
    spi_device_polling_transmit(dev->spi, &t);

    cc1101_deselect(dev);

    xSemaphoreGive(dev->spi_mutex);

    memcpy(data, &rx_buf[1], len);
}

/* ============================================================
 * DATARATE CALCULATION
 * ============================================================ */

static void calc_datarate(uint32_t baud, uint8_t *drate_e, uint8_t *drate_m)
{
    *drate_e = 0;
    *drate_m = 0;
    for (uint8_t e = 0; e < 16; e++) {
        uint64_t val = ((uint64_t)baud << 28) / (26000000ULL << e);
        if (val >= 256 && val <= 511) {
            *drate_e = e;
            *drate_m = (uint8_t)(val - 256);
            return;
        }
    }
    ESP_LOGW(TAG, "Datarate %lu bps not achievable, using slowest", (unsigned long)baud);
}

/* ============================================================
 * PUBLIC API - INIT
 * ============================================================ */

esp_err_t cc1101_init(cc1101_handle_t *dev,
                      spi_device_handle_t spi_handle,
                      gpio_num_t cs,
                      gpio_num_t miso,
                      gpio_num_t gdo0)
{
    dev->spi        = spi_handle;
    dev->cs_pin     = cs;
    dev->miso_pin   = miso;
    dev->gdo0_pin   = gdo0;
    dev->isr_enabled   = false;
    dev->append_status = false;
    dev->tx_pending    = false;
    dev->tx_caller_task = NULL;

    dev->spi_mutex = xSemaphoreCreateMutex();
    if (!dev->spi_mutex) {
        ESP_LOGE(TAG, "Failed to create SPI mutex");
        return ESP_ERR_NO_MEM;
    }

    dev->async_freq_hz       = 0;
    dev->async_datarate_bps  = 0;
    dev->async_deviation     = 0;
    dev->async_chanbw        = 0;
    dev->async_gdo2_mode     = CC1101_GDO_HIGH_Z;
    dev->status_period_ms    = 0;

    cc1101_reset(dev);
    return ESP_OK;
}

esp_err_t cc1101_reset(cc1101_handle_t *dev)
{
    cc1101_deselect(dev);
    esp_rom_delay_us(5);

    cc1101_select(dev);
    esp_rom_delay_us(5);
    cc1101_deselect(dev);

    esp_rom_delay_us(45);

    cc1101_select(dev);
    esp_err_t ret = ESP_OK;
    int timeout = 100000;
    while (gpio_get_level(dev->miso_pin)) {
        if (--timeout <= 0) {
            ESP_LOGE(TAG, "Reset failed: MISO didn't go low after CS");
            cc1101_deselect(dev);
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(1);
    }

    /* 4. Send SRES (0x30) — raw SPI byte, CS stays low */
    uint8_t tx_data[1] = { CC1101_SRES };
    uint8_t rx_data[1] = { 0 };
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };
    spi_device_polling_transmit(dev->spi, &t);

    timeout = 100000;
    while (gpio_get_level(dev->miso_pin)) {
        if (--timeout <= 0) {
            ESP_LOGE(TAG, "Reset failed: MISO didn't go low after SRES");
            cc1101_deselect(dev);
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(1);
    }

    cc1101_deselect(dev);

    vTaskDelay(pdMS_TO_TICKS(10));
    return ret;
}

/* ============================================================
 * PUBLIC API - CONFIGURATION
 * ============================================================ */

esp_err_t cc1101_configure(cc1101_handle_t *dev,
                           const cc1101_config_t *cfg)
{
    if (!dev || !cfg) return ESP_ERR_INVALID_ARG;

    /* Frequency */
    cc1101_set_frequency(dev, cfg->freq_hz);

    /* Channel */
    cc1101_set_channel(dev, cfg->channel);

    /* PA power */
    if (cfg->modem.modulation == CC1101_MOD_ASK_OOK) {
        /* OOK: PATABLE[0]=OFF (bit=0), PATABLE[7]=PA (bit=1) */
        uint8_t pa_table[8] = {0};
        pa_table[0] = 0x00;
        pa_table[7] = cfg->pa_value;
        cc1101_write_burst(dev, CC1101_PATABLE, pa_table, 8);
        cc1101_write_reg(dev, CC1101_FREND0, 0x11);
    } else {
        /* FSK/GFSK/MSK/4FSK: PATABLE[0]=PA, FREND0 LPA=00 */
        cc1101_set_tx_power(dev, cfg->pa_value);
        uint8_t frend0 = cc1101_read_reg(dev, CC1101_FREND0);
        frend0 = (frend0 & 0xFC) | 0x00;
        cc1101_write_reg(dev, CC1101_FREND0, frend0);
    }

    /* Modem */
    uint8_t drate_e = 0, drate_m = 0;
    calc_datarate(cfg->modem.datarate_bps, &drate_e, &drate_m);

    /* chanbw is the packed MDMCFG4 high nibble (CC1101_CHANBW_*_KHZ);
     * only the datarate exponent is OR'd into the low nibble. */
    uint8_t mdmcfg4 = (cfg->modem.chanbw & 0xF0) | (drate_e & 0x0F);
    cc1101_write_reg(dev, CC1101_MDMCFG4, mdmcfg4);
    cc1101_write_reg(dev, CC1101_MDMCFG3, drate_m);
    cc1101_write_reg(dev, CC1101_MDMCFG2,
                     CC1101_MDMCFG2_VALUE(cfg->modem.dc_filter_off ? 1 : 0,
                                          cfg->modem.modulation,
                                          cfg->modem.manchester ? 1 : 0,
                                          cfg->modem.sync_mode));
    cc1101_write_reg(dev, CC1101_MDMCFG1,
                     CC1101_MDMCFG1_VALUE(cfg->modem.fec_enable ? 1 : 0,
                                          cfg->modem.preamble_bytes,
                                          0));
    cc1101_write_reg(dev, CC1101_MDMCFG0, cfg->modem.channel_spacing);
    cc1101_write_reg(dev, CC1101_DEVIATN, cfg->modem.deviation);

    /* Packet */
    cc1101_write_reg(dev, CC1101_SYNC1, cfg->packet.sync1);
    cc1101_write_reg(dev, CC1101_SYNC0, cfg->packet.sync0);
    cc1101_write_reg(dev, CC1101_PKTLEN, cfg->packet.max_length);
    cc1101_write_reg(dev, CC1101_PKTCTRL1,
                     (cfg->packet.append_status ? CC1101_APPEND_STATUS : 0) |
                     (cfg->packet.addr_check & CC1101_ADR_CHK_0_255_BCAST));

    /* Build PKTCTRL0: bits[4:3]=PKT_FORMAT, bits[2:1]=LENGTH_CONFIG, bit[0]=CRC_EN */
    uint8_t pkt_format = CC1101_PKT_FORMAT_NORMAL;
    uint8_t pkt_len;
    switch (cfg->packet.mode) {
        case CC1101_PKT_VARIABLE_E: pkt_len = CC1101_PKTLEN_VARIABLE; break;
        case CC1101_PKT_INFINITE_E: pkt_len = CC1101_PKTLEN_INFINITE; break;
        default:                    pkt_len = CC1101_PKTLEN_FIXED;    break;
    }
    cc1101_write_reg(dev, CC1101_PKTCTRL0,
                     pkt_format | pkt_len |
                     (cfg->packet.crc_enable ? CC1101_CRC_ENABLE : 0) |
                     (cfg->packet.whitening ? CC1101_DATA_WHITENING : 0));

    /* Radio control */
    cc1101_write_reg(dev, CC1101_MCSM0, cfg->radio.autocal);
    cc1101_write_reg(dev, CC1101_MCSM1, cfg->radio.pin_mode);
    cc1101_write_reg(dev, CC1101_IOCFG0, cfg->radio.gdo0_mode);
    cc1101_write_reg(dev, CC1101_IOCFG2, cfg->radio.gdo2_mode);

    /* Store append_status for receive_packet */
    dev->append_status = cfg->packet.append_status;

    /* ISR */
    if (cfg->isr_enabled) {
        cc1101_isr_enable(dev, true);
    }

    /* Calibrate */
    cc1101_strobe(dev, CC1101_SCAL);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "Configured: freq=%luHz rate=%lu bps pa=0x%02X mod=%d sync=%d",
             (unsigned long)cfg->freq_hz,
             (unsigned long)cfg->modem.datarate_bps,
             cfg->pa_value,
             cfg->modem.modulation,
             cfg->modem.sync_mode);
    return ESP_OK;
}

void cc1101_config(cc1101_handle_t *dev)
{
    cc1101_write_reg(dev, CC1101_IOCFG2, CC1101_GDO_ASYNC_DATA);
    cc1101_write_reg(dev, CC1101_IOCFG0, CC1101_GDO_ASYNC_DATA);
    cc1101_write_reg(dev, CC1101_PKTLEN, 255);
    cc1101_write_reg(dev, CC1101_PKTCTRL1, CC1101_ADR_CHK_NONE);
    cc1101_write_reg(dev, CC1101_PKTCTRL0, CC1101_PKT_FORMAT_ASYNC | CC1101_PKTLEN_VARIABLE);
    cc1101_set_frequency(dev, 433920000);
    cc1101_write_reg(dev, CC1101_MDMCFG4, CC1101_MDMCFG4_VALUE(3, 0, 10));
    cc1101_write_reg(dev, CC1101_MDMCFG3, CC1101_MDMCFG3_VALUE(131));
    cc1101_write_reg(dev, CC1101_MDMCFG2,
                     CC1101_MDMCFG2_VALUE(0, CC1101_MOD_GFSK, 0, CC1101_SYNC_30_32));
    cc1101_write_reg(dev, CC1101_MDMCFG1, CC1101_MDMCFG1_VALUE(0, 2, 2));
    cc1101_write_reg(dev, CC1101_MDMCFG0, CC1101_MDMCFG0_VALUE(248));
    cc1101_write_reg(dev, CC1101_MCSM0, CC1101_AUTOCAL_IDLE_TO_RXTX);
    cc1101_write_reg(dev, CC1101_MCSM1, 0x3F);
    cc1101_strobe(dev, CC1101_SCAL);
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* ============================================================
 * PUBLIC API - MODE CONTROL
 * ============================================================ */

esp_err_t cc1101_set_tx_len(cc1101_handle_t *dev, uint8_t len)
{
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
    if (!dev) {
        ESP_LOGE(TAG, "Invalid transmit arguments");
        return;
    }

    cc1101_strobe(dev, CC1101_SIDLE);
    cc1101_strobe(dev, CC1101_SFTX);

    if (len > 0 && data) {
        if (len > CC1101_MAX_PAYLOAD_LEN) {
            ESP_LOGW(TAG, "Payload too large (%u), truncating to %u",
                     (unsigned)len, (unsigned)CC1101_MAX_PAYLOAD_LEN);
            len = CC1101_MAX_PAYLOAD_LEN;
        }
        cc1101_write_burst(dev, CC1101_FIFO_ADDR, data, (uint8_t)len);
    }

    if (dev->isr_enabled) {
        dev->tx_pending = true;
        dev->tx_caller_task = xTaskGetCurrentTaskHandle();
    }

    ESP_LOGI(TAG, "TX pre-strobe: GDO0=%d isr=%d tx_pending=%d",
             gpio_get_level(dev->gdo0_pin),
             dev->isr_enabled, dev->tx_pending);

    uint8_t status = cc1101_strobe(dev, CC1101_STX);
    ESP_LOGI(TAG, "TX started, payload=%u bytes, status=0x%02X",
             (unsigned)len, status);
}

void cc1101_set_rx_mode(cc1101_handle_t *dev)
{
    cc1101_strobe(dev, CC1101_SIDLE);
    cc1101_strobe(dev, CC1101_SFRX);
    cc1101_strobe(dev, CC1101_SRX);
}

bool cc1101_receive_packet(cc1101_handle_t *dev,
                           uint8_t *buffer,
                           size_t *len)
{
    if (!buffer || !len) {
        ESP_LOGE(TAG, "Invalid RX args");
        return false;
    }

    uint8_t rx_bytes = cc1101_read_status(dev, CC1101_RXBYTES);

    if (rx_bytes & 0x80) {
        ESP_LOGW(TAG, "RX overflow");
        cc1101_set_rx_mode(dev);
        return false;
    }
    if ((rx_bytes & 0x7F) == 0)
        return false;

    /* Determine expected packet length from the live config.
     * Fixed  : PKTLEN bytes, FIFO holds raw data (no length byte).
     * Variable: first FIFO byte is the length byte.
     * Infinite: no framing, read whatever is available. */
    uint8_t length_cfg = (cc1101_read_reg(dev, CC1101_PKTCTRL0) >> 2) & 0x03;
    uint8_t pkt_len;

    if (length_cfg == CC1101_PKTLEN_VARIABLE) {
        pkt_len = cc1101_read_reg(dev, CC1101_FIFO_ADDR);
    } else if (length_cfg == CC1101_PKTLEN_INFINITE) {
        pkt_len = rx_bytes & 0x7F;
    } else {
        /* Fixed length: wait for the complete packet in the FIFO. */
        pkt_len = cc1101_read_reg(dev, CC1101_PKTLEN);
        if ((rx_bytes & 0x7F) < pkt_len)
            return false;
    }

    if (pkt_len > *len) {
        ESP_LOGE(TAG, "Packet too large: packet=%u buffer=%u", pkt_len, (unsigned)*len);
        cc1101_set_rx_mode(dev);
        return false;
    }

    cc1101_read_burst(dev, CC1101_FIFO_ADDR, buffer, pkt_len);
    *len = pkt_len;

    if (dev->append_status) {
        uint8_t status[2];
        cc1101_read_burst(dev, CC1101_FIFO_ADDR, status, 2);
        cc1101_set_rx_mode(dev);
        return (status[1] & 0x80) != 0;
    }

    cc1101_set_rx_mode(dev);
    return true;
}

/* ============================================================
 * PUBLIC API - RUNTIME ADJUSTMENTS
 * ============================================================ */

esp_err_t cc1101_set_frequency(cc1101_handle_t *dev, uint32_t freq_hz)
{
    uint32_t freq_reg = (uint32_t)(((uint64_t)freq_hz << 16) / 26000000ULL);
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
    cc1101_write_burst(dev, CC1101_PATABLE, &pa_value, 1);
    return ESP_OK;
}

esp_err_t cc1101_set_datarate(cc1101_handle_t *dev, uint32_t baud)
{
    uint8_t drate_e = 0, drate_m = 0;
    calc_datarate(baud, &drate_e, &drate_m);

    uint8_t mdmcfg4 = cc1101_read_reg(dev, CC1101_MDMCFG4);
    mdmcfg4 = (mdmcfg4 & 0xF0) | (drate_e & 0x0F);
    cc1101_write_reg(dev, CC1101_MDMCFG4, mdmcfg4);
    cc1101_write_reg(dev, CC1101_MDMCFG3, drate_m);
    return ESP_OK;
}

/* ============================================================
 * PUBLIC API - ISR CONTROL
 * ============================================================ */

esp_err_t cc1101_isr_enable(cc1101_handle_t *dev, bool enable)
{
    if (!dev || dev->gdo0_pin < 0)
        return ESP_ERR_INVALID_STATE;

    if (enable && !dev->isr_enabled) {
        /* Install GPIO ISR service if not already installed */
        esp_err_t ret = gpio_install_isr_service(0);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "ISR service install failed: %s", esp_err_to_name(ret));
            return ret;
        }

        gpio_set_direction(dev->gdo0_pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(dev->gdo0_pin, GPIO_PULLDOWN_ONLY);

        ret = gpio_set_intr_type(dev->gdo0_pin, GPIO_INTR_NEGEDGE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "set_intr_type failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = gpio_isr_handler_add(dev->gdo0_pin, cc1101_gdo0_isr, dev);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "isr_handler_add failed: %s", esp_err_to_name(ret));
            return ret;
        }

        dev->isr_enabled = true;
        ESP_LOGI(TAG, "ISR enabled on GDO0 (GPIO %d) level=%d",
                 dev->gdo0_pin, gpio_get_level(dev->gdo0_pin));
    } else if (!enable && dev->isr_enabled) {
        gpio_isr_handler_remove(dev->gdo0_pin);
        dev->isr_enabled = false;
        ESP_LOGI(TAG, "ISR disabled");
    }

    return ESP_OK;
}

bool cc1101_wait_tx_done(cc1101_handle_t *dev, uint32_t timeout_ms)
{
    if (!dev) return false;

    if (dev->isr_enabled) {
        uint32_t notif = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));
        return notif > 0;
    }

    /* Poll MARCSTATE: wait for TX state (0x0B) to end */
    uint32_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        uint8_t marcstate = cc1101_read_status_reg(dev, CC1101_MARCSTATE) & 0x1F;
        if (marcstate != 0x0B) { /* 0x0B = TX */
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGW(TAG, "TX done timeout (MARCSTATE=0x%02X)",
             cc1101_read_status_reg(dev, CC1101_MARCSTATE) & 0x1F);
    return false;
}

/* ============================================================
 * PUBLIC API - STATUS / DEBUG
 * ============================================================ */

uint8_t cc1101_read_status_reg(cc1101_handle_t *dev, uint8_t reg)
{
    return cc1101_read_status(dev, reg);
}

void cc1101_dump_registers(cc1101_handle_t *dev)
{
    ESP_LOGI(TAG, "==== REGISTER DUMP ====");
    for (uint8_t reg = 0; reg <= CC1101_TEST0; reg++) {
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

void cc1101_verify_config(cc1101_handle_t *dev, const cc1101_config_t *cfg)
{
    ESP_LOGI(TAG, "==== REGISTER VERIFICATION ====");

    uint32_t freq_reg = (uint32_t)(((uint64_t)cfg->freq_hz << 16) / 26000000ULL);
    uint8_t exp_freq2 = (freq_reg >> 16) & 0xFF;

    uint8_t drate_e = 0, drate_m = 0;
    calc_datarate(cfg->modem.datarate_bps, &drate_e, &drate_m);
    /* chanbw is the packed MDMCFG4 high nibble (CC1101_CHANBW_*_KHZ) */
    uint8_t exp_mdmcfg4 = (cfg->modem.chanbw & 0xF0) | (drate_e & 0x0F);
    uint8_t exp_mdmcfg2 = CC1101_MDMCFG2_VALUE(cfg->modem.dc_filter_off ? 1 : 0,
                                                cfg->modem.modulation,
                                                cfg->modem.manchester ? 1 : 0,
                                                cfg->modem.sync_mode);
    uint8_t exp_mdmcfg1 = CC1101_MDMCFG1_VALUE(cfg->modem.fec_enable ? 1 : 0,
                                                cfg->modem.preamble_bytes, 0);
    uint8_t exp_iocfg0 = cfg->radio.gdo0_mode;
    uint8_t exp_iocfg2 = cfg->radio.gdo2_mode;

    struct { uint8_t addr; const char *name; uint8_t expected; } checks[] = {
        { CC1101_IOCFG2,  "IOCFG2 ", exp_iocfg2 },
        { CC1101_IOCFG0,  "IOCFG0 ", exp_iocfg0 },
        { CC1101_SYNC1,   "SYNC1  ", cfg->packet.sync1 },
        { CC1101_SYNC0,   "SYNC0  ", cfg->packet.sync0 },
        { CC1101_PKTLEN,  "PKTLEN ", cfg->packet.max_length },
        { CC1101_PKTCTRL0,"PKTCTRL0", (cfg->packet.crc_enable ? CC1101_CRC_ENABLE : 0) |
                                       (cfg->packet.whitening ? CC1101_DATA_WHITENING : 0) |
                                       ((cfg->packet.mode == CC1101_PKT_VARIABLE_E) ? CC1101_PKTLEN_VARIABLE :
                                        (cfg->packet.mode == CC1101_PKT_INFINITE_E) ? CC1101_PKTLEN_INFINITE :
                                        CC1101_PKTLEN_FIXED) },
        { CC1101_MDMCFG4, "MDMCFG4", exp_mdmcfg4 },
        { CC1101_MDMCFG3, "MDMCFG3", drate_m },
        { CC1101_MDMCFG2, "MDMCFG2", exp_mdmcfg2 },
        { CC1101_MDMCFG1, "MDMCFG1", exp_mdmcfg1 },
        { CC1101_MDMCFG0, "MDMCFG0", cfg->modem.channel_spacing },
        { CC1101_DEVIATN, "DEVIATN", cfg->modem.deviation },
        { CC1101_MCSM0,   "MCSM0  ", cfg->radio.autocal },
        { CC1101_MCSM1,   "MCSM1  ", cfg->radio.pin_mode },
        { CC1101_FREQ2,   "FREQ2  ", exp_freq2 },
        { CC1101_FREND0,  "FREND0 ", (cfg->modem.modulation == CC1101_MOD_ASK_OOK) ? 0x11 : 0x10 },
    };

    int pass = 0, fail = 0;
    for (int i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        uint8_t actual = cc1101_read_reg(dev, checks[i].addr);
        bool ok = (actual == checks[i].expected);
        ESP_LOGI(TAG, "  %s = 0x%02X  %s 0x%02X  [%s]",
                 checks[i].name, actual, ok ? "==" : "!=", checks[i].expected,
                 ok ? "OK" : "FAIL");
        if (ok) pass++; else fail++;
    }

    /* Status registers */
    uint8_t marcstate = cc1101_read_status_reg(dev, CC1101_MARCSTATE) & 0x1F;
    uint8_t txbytes   = cc1101_read_status_reg(dev, CC1101_TXBYTES) & 0x7F;
    ESP_LOGI(TAG, "  MARCSTATE = 0x%02X  TXBYTES = 0x%02X", marcstate, txbytes);
    ESP_LOGI(TAG, "  RESULT: %d pass, %d fail", pass, fail);
}

/* ============================================================
 * APP-LEVEL CONFIG / DIAGNOSTICS
 * ============================================================ */

esp_err_t cc1101_config_async_rx(cc1101_handle_t *dev, uint32_t freq_hz,
                                 uint32_t datarate_bps, uint8_t deviation,
                                 uint8_t chanbw, uint8_t gdo2_mode)
{
    if (!dev) return ESP_ERR_INVALID_ARG;

    dev->async_freq_hz      = freq_hz;
    dev->async_datarate_bps = datarate_bps;
    dev->async_deviation    = deviation;
    dev->async_chanbw       = chanbw;
    dev->async_gdo2_mode    = gdo2_mode;

    cc1101_config_t cfg = CC1101_DEFAULT_CONFIG();
    cfg.isr_enabled = false;
    cfg.freq_hz = freq_hz;
    cfg.modem.modulation    = CC1101_MOD_2FSK_E;
    cfg.modem.sync_mode     = CC1101_SYNC_NONE_E;
    cfg.modem.datarate_bps  = datarate_bps;
    cfg.modem.deviation     = deviation;
    cfg.modem.chanbw        = chanbw;
    cfg.packet.mode         = CC1101_PKT_INFINITE_E;
    cfg.packet.crc_enable   = false;
    cfg.packet.whitening    = false;
    cfg.packet.append_status = false;
    cfg.radio.gdo0_mode     = CC1101_GDO_ASYNC_DATA;
    cfg.radio.gdo2_mode     = gdo2_mode;
    cfg.radio.autocal       = CC1101_AUTOCAL_ALWAYS;
    cfg.radio.pin_mode      = 0x3F;

    esp_err_t ret = cc1101_configure(dev, &cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Transparent async-serial packet format, infinite length, no CRC.
     * (cc1101_configure does not emit PKT_FORMAT=ASYNC, so force it.) */
    cc1101_write_reg(dev, CC1101_PKTCTRL0,
                     CC1101_PKT_FORMAT_ASYNC | CC1101_PKTLEN_INFINITE);
    cc1101_write_reg(dev, CC1101_PKTCTRL1, 0x04);
    cc1101_write_reg(dev, CC1101_MCSM1, 0x3F);
    cc1101_write_reg(dev, CC1101_IOCFG0, CC1101_GDO_ASYNC_DATA);
    cc1101_write_reg(dev, CC1101_IOCFG2, gdo2_mode);
    cc1101_set_rx_mode(dev);

    ESP_LOGI(TAG, "Async RX: %lu Hz, %lu bps, dev=0x%02X, chanbw=0x%02X",
             (unsigned long)freq_hz, (unsigned long)datarate_bps,
             deviation, chanbw);
    return ESP_OK;
}

esp_err_t cc1101_tx_test(cc1101_handle_t *dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;

    uint32_t freq_hz     = dev->async_freq_hz     ? dev->async_freq_hz     : 433920000;
    uint32_t datarate    = dev->async_datarate_bps ? dev->async_datarate_bps : 2400;
    uint8_t  deviation   = dev->async_deviation   ? dev->async_deviation   : 0x47;

    ESP_LOGI(TAG, "TX test: 2FSK %lu bps sync=DEAF payload=0x01",
             (unsigned long)datarate);

    cc1101_config_t tx = CC1101_DEFAULT_CONFIG();
    tx.freq_hz = freq_hz;
    tx.modem.modulation    = CC1101_MOD_2FSK_E;
    tx.modem.datarate_bps  = datarate;
    tx.modem.sync_mode     = CC1101_SYNC_16_16_E;
    tx.modem.preamble_bytes = 4;
    tx.modem.deviation     = deviation;
    tx.modem.chanbw        = CC1101_CHANBW_464_KHZ;
    tx.packet.mode         = CC1101_PKT_FIXED_E;
    tx.packet.max_length   = 1;
    tx.packet.crc_enable   = false;
    tx.packet.whitening    = false;
    tx.packet.append_status = false;
    tx.packet.sync1        = 0xDE;
    tx.packet.sync0        = 0xAF;
    tx.pa_value            = CC1101_PA_POS10dBm;
    tx.radio.gdo0_mode     = CC1101_GDO_SYNC_WORD;
    tx.radio.gdo2_mode     = CC1101_GDO_HIGH_Z;
    tx.radio.autocal       = CC1101_AUTOCAL_ALWAYS;
    tx.radio.pin_mode      = 0x00;

    cc1101_configure(dev, &tx);
    uint8_t pkt = 0x01;
    cc1101_transmit(dev, &pkt, 1);
    bool ok = cc1101_wait_tx_done(dev, 1000);
    ESP_LOGI(TAG, "TX result: %s", ok ? "OK" : "timeout");

    cc1101_config_async_rx(dev, dev->async_freq_hz, dev->async_datarate_bps,
                           dev->async_deviation, dev->async_chanbw,
                           dev->async_gdo2_mode);
    return ok ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* TX test that preserves current RX config (for loopback testing) */
esp_err_t cc1101_tx_test_preserve_rx(cc1101_handle_t *dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;

    uint32_t freq_hz = dev->async_freq_hz ? dev->async_freq_hz : 433920000;
    uint32_t datarate = dev->async_datarate_bps ? dev->async_datarate_bps : 2400;
    uint8_t deviation = dev->async_deviation ? dev->async_deviation : 0x47;

    ESP_LOGI(TAG, "TX test (preserve RX): 2FSK %lu bps sync=DEAF payload=0x01",
             (unsigned long)datarate);

    /* Save current register state for restore */
    uint8_t regs_backup[0x3E + 1];
    for (int i = 0; i <= 0x3E; i++) {
        regs_backup[i] = cc1101_read_reg(dev, i);
    }

    cc1101_config_t tx = CC1101_DEFAULT_CONFIG();
    tx.freq_hz = freq_hz;
    tx.modem.modulation    = CC1101_MOD_2FSK_E;
    tx.modem.datarate_bps  = datarate;
    tx.modem.sync_mode     = CC1101_SYNC_16_16_E;
    tx.modem.preamble_bytes = 4;
    tx.modem.deviation     = deviation;
    tx.modem.chanbw        = CC1101_CHANBW_464_KHZ;
    tx.packet.mode         = CC1101_PKT_FIXED_E;
    tx.packet.max_length   = 1;
    tx.packet.crc_enable   = false;
    tx.packet.whitening    = false;
    tx.packet.append_status = false;
    tx.packet.sync1        = 0xDE;
    tx.packet.sync0        = 0xAF;
    tx.pa_value            = CC1101_PA_POS10dBm;
    tx.radio.gdo0_mode     = CC1101_GDO_SYNC_WORD;
    tx.radio.gdo2_mode     = CC1101_GDO_HIGH_Z;
    tx.radio.autocal       = CC1101_AUTOCAL_ALWAYS;
    tx.radio.pin_mode      = 0x00;

    cc1101_configure(dev, &tx);
    uint8_t pkt = 0x01;
    cc1101_transmit(dev, &pkt, 1);
    bool ok = cc1101_wait_tx_done(dev, 1000);
    ESP_LOGI(TAG, "TX result: %s", ok ? "OK" : "timeout");

    /* Restore registers */
    for (int i = 0; i <= 0x3E; i++) {
        cc1101_write_reg(dev, i, regs_backup[i]);
    }
    cc1101_set_rx_mode(dev);
    return ok ? ESP_OK : ESP_ERR_TIMEOUT;
}

void cc1101_freq_sweep(cc1101_handle_t *dev, uint32_t start_hz,
                       uint32_t end_hz, uint32_t step_hz,
                       cc1101_sweep_output_fn out)
{
    if (!dev || !out || step_hz == 0 || start_hz > end_hz) {
        return;
    }

    ESP_LOGI(TAG, "Sweep: %lu..%lu Hz, step %lu Hz",
             (unsigned long)start_hz, (unsigned long)end_hz,
             (unsigned long)step_hz);

    cc1101_strobe(dev, CC1101_SIDLE);

    uint8_t rssi[256];
    size_t n = 0;
    for (uint64_t f = start_hz; f <= end_hz; f += step_hz) {
        cc1101_set_frequency(dev, (uint32_t)f);
        cc1101_strobe(dev, CC1101_SRX);
        vTaskDelay(pdMS_TO_TICKS(15));   /* autocal + synth settle */
        rssi[n++] = (uint8_t)cc1101_read_status_reg(dev, CC1101_RSSI);
        if (n == sizeof(rssi)) {
            out(rssi, n);
            n = 0;
        }
    }
    if (n) {
        out(rssi, n);
    }

    ESP_LOGI(TAG, "Sweep done");

    /* Return to the operational async-RX configuration. */
    cc1101_config_async_rx(dev, dev->async_freq_hz, dev->async_datarate_bps,
                           dev->async_deviation, dev->async_chanbw,
                           dev->async_gdo2_mode);
}

void cc1101_log_registers(cc1101_handle_t *dev)
{
    if (!dev) return;

    ESP_LOGI(TAG, "=== CC1101 Register Dump ===");
    ESP_LOGI(TAG, "  IOCFG2  = 0x%02X",  cc1101_read_reg(dev, CC1101_IOCFG2));
    ESP_LOGI(TAG, "  IOCFG0  = 0x%02X",  cc1101_read_reg(dev, CC1101_IOCFG0));
    ESP_LOGI(TAG, "  PKTCTRL1= 0x%02X",  cc1101_read_reg(dev, CC1101_PKTCTRL1));
    ESP_LOGI(TAG, "  PKTCTRL0= 0x%02X",  cc1101_read_reg(dev, CC1101_PKTCTRL0));
    ESP_LOGI(TAG, "  SYNC1   = 0x%02X",  cc1101_read_reg(dev, CC1101_SYNC1));
    ESP_LOGI(TAG, "  SYNC0   = 0x%02X",  cc1101_read_reg(dev, CC1101_SYNC0));
    ESP_LOGI(TAG, "  PKTLEN  = 0x%02X",  cc1101_read_reg(dev, CC1101_PKTLEN));
    ESP_LOGI(TAG, "  MDMCFG4 = 0x%02X",  cc1101_read_reg(dev, CC1101_MDMCFG4));
    ESP_LOGI(TAG, "  MDMCFG3 = 0x%02X",  cc1101_read_reg(dev, CC1101_MDMCFG3));
    ESP_LOGI(TAG, "  MDMCFG2 = 0x%02X",  cc1101_read_reg(dev, CC1101_MDMCFG2));
    ESP_LOGI(TAG, "  MCSM1   = 0x%02X",  cc1101_read_reg(dev, CC1101_MCSM1));
    ESP_LOGI(TAG, "  FREQ    = 0x%02X/0x%02X/0x%02X",
             cc1101_read_reg(dev, CC1101_FREQ2),
             cc1101_read_reg(dev, CC1101_FREQ1),
             cc1101_read_reg(dev, CC1101_FREQ0));
    ESP_LOGI(TAG, "  MARCSTATE= 0x%02X", cc1101_read_status_reg(dev, CC1101_MARCSTATE) & 0x1F);
    ESP_LOGI(TAG, "  PKTSTATUS= 0x%02X", cc1101_read_status_reg(dev, CC1101_PKTSTATUS));
    ESP_LOGI(TAG, "  RSSI    = %d dBm",  (int8_t)cc1101_read_status_reg(dev, CC1101_RSSI) / 2 - 74);
    ESP_LOGI(TAG, "============================");
}

static void cc1101_status_monitor_task(void *arg)
{
    cc1101_handle_t *dev = (cc1101_handle_t *)arg;
    for (;;) {
        int8_t rssi = (int8_t)cc1101_read_status_reg(dev, CC1101_RSSI);
        uint8_t marc = cc1101_read_status_reg(dev, CC1101_MARCSTATE) & 0x1F;
        uint8_t pkt  = cc1101_read_status_reg(dev, CC1101_PKTSTATUS);
        uint8_t rx   = cc1101_read_status_reg(dev, CC1101_RXBYTES);
        ESP_LOGI(TAG, "MARCSTATE=0x%02X PKTSTATUS=0x%02X RSSI=%d dBm RXBYTES=%d",
                 marc, pkt, rssi / 2 - 74, rx);
        vTaskDelay(pdMS_TO_TICKS(dev->status_period_ms));
    }
}

void cc1101_start_status_monitor(cc1101_handle_t *dev, uint32_t period_ms)
{
    if (!dev || period_ms == 0) return;
    dev->status_period_ms = period_ms;
    xTaskCreatePinnedToCore(cc1101_status_monitor_task, "rfstatus", 4096, dev, 5, NULL, 1);
}
