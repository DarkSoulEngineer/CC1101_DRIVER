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
    esp_rom_delay_us(1);  /* t_CSS: CS low to first SCK */
}

static inline void cc1101_deselect(cc1101_handle_t *dev)
{
    gpio_set_level(dev->cs_pin, 1);
    esp_rom_delay_us(1);  /* t_CSH: CS high between transactions */
}

static esp_err_t cc1101_wait_miso(cc1101_handle_t *dev)
{
    int timeout = 100000;
    while (gpio_get_level(dev->miso_pin)) {
        if (--timeout <= 0) {
            ESP_LOGE(TAG, "MISO timeout");
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(1);
    }
    return ESP_OK;
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
    uint8_t status = 0;
    xSemaphoreTake(dev->spi_mutex, portMAX_DELAY);

    cc1101_select(dev);
    if (cc1101_wait_miso(dev) == ESP_OK) {
        spi_transaction_t t = {0};
        t.length = 8;
        t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
        t.tx_data[0] = strobe;
        if (spi_device_polling_transmit(dev->spi, &t) == ESP_OK)
            status = t.rx_data[0];
    }
    cc1101_deselect(dev);

    xSemaphoreGive(dev->spi_mutex);
    return status;
}

void cc1101_write_reg(cc1101_handle_t *dev, uint8_t reg, uint8_t value)
{
    xSemaphoreTake(dev->spi_mutex, portMAX_DELAY);

    cc1101_select(dev);
    if (cc1101_wait_miso(dev) == ESP_OK) {
        spi_transaction_t t = {0};
        t.length = 16;
        t.flags = SPI_TRANS_USE_TXDATA;
        t.tx_data[0] = reg;
        t.tx_data[1] = value;
        spi_device_polling_transmit(dev->spi, &t);
    }
    cc1101_deselect(dev);

    xSemaphoreGive(dev->spi_mutex);
}

uint8_t cc1101_read_reg(cc1101_handle_t *dev, uint8_t reg)
{
    uint8_t value = 0;
    xSemaphoreTake(dev->spi_mutex, portMAX_DELAY);

    cc1101_select(dev);
    if (cc1101_wait_miso(dev) == ESP_OK) {
        spi_transaction_t t = {0};
        t.length = 16;
        t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
        t.tx_data[0] = reg | CC1101_READ_SINGLE;
        t.tx_data[1] = 0;
        spi_device_polling_transmit(dev->spi, &t);
        value = t.rx_data[1];
    }
    cc1101_deselect(dev);

    xSemaphoreGive(dev->spi_mutex);
    return value;
}

static uint8_t cc1101_read_status(cc1101_handle_t *dev, uint8_t reg)
{
    uint8_t value = 0;
    xSemaphoreTake(dev->spi_mutex, portMAX_DELAY);

    cc1101_select(dev);
    if (cc1101_wait_miso(dev) == ESP_OK) {
        spi_transaction_t t = {0};
        t.length = 16;
        t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
        t.tx_data[0] = reg | CC1101_READ_BURST;
        t.tx_data[1] = 0;
        spi_device_polling_transmit(dev->spi, &t);
        value = t.rx_data[1];
    }
    cc1101_deselect(dev);

    xSemaphoreGive(dev->spi_mutex);
    return value;
}

static void cc1101_write_burst(cc1101_handle_t *dev,
                               uint8_t reg,
                               const uint8_t *data,
                               uint8_t len)
{
    xSemaphoreTake(dev->spi_mutex, portMAX_DELAY);

    cc1101_select(dev);
    if (cc1101_wait_miso(dev) == ESP_OK) {
        uint8_t buf[1 + CC1101_FIFO_SIZE];
        buf[0] = reg | CC1101_WRITE_BURST;
        memcpy(&buf[1], data, len);

        spi_transaction_t t = {0};
        t.length = (1 + len) * 8;
        t.tx_buffer = buf;
        spi_device_polling_transmit(dev->spi, &t);
    }
    cc1101_deselect(dev);

    xSemaphoreGive(dev->spi_mutex);
}

static void cc1101_read_burst(cc1101_handle_t *dev,
                              uint8_t reg,
                              uint8_t *data,
                              uint8_t len)
{
    xSemaphoreTake(dev->spi_mutex, portMAX_DELAY);

    cc1101_select(dev);
    if (cc1101_wait_miso(dev) == ESP_OK) {
        uint8_t tx_buf[1 + CC1101_FIFO_SIZE];
        uint8_t rx_buf[1 + CC1101_FIFO_SIZE];
        memset(tx_buf, 0, sizeof(tx_buf));
        memset(rx_buf, 0, sizeof(rx_buf));

        tx_buf[0] = reg | CC1101_READ_BURST;

        spi_transaction_t t = {0};
        t.length = (1 + len) * 8;
        t.tx_buffer = tx_buf;
        t.rx_buffer = rx_buf;
        spi_device_polling_transmit(dev->spi, &t);

        memcpy(data, &rx_buf[1], len);
    }
    cc1101_deselect(dev);

    xSemaphoreGive(dev->spi_mutex);
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

    cc1101_reset(dev);
    return ESP_OK;
}

esp_err_t cc1101_reset(cc1101_handle_t *dev)
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
    return ESP_OK;
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

    /* PA power — OOK needs PATABLE[0]=OFF, PATABLE[7]=ON */
    if (cfg->modem.modulation == CC1101_MOD_ASK_OOK) {
        uint8_t pa_table[8] = {0};
        pa_table[0] = 0x00;        /* PA OFF for bit=0 */
        pa_table[7] = cfg->pa_value; /* PA ON for bit=1 */
        cc1101_write_burst(dev, CC1101_PATABLE, pa_table, 8);
        /* FREND0[1:0]=01 → TX uses PATABLE[7] (the ON value) */
        cc1101_write_reg(dev, CC1101_FREND0, 0x11);
    } else {
        cc1101_set_tx_power(dev, cfg->pa_value);
    }

    /* Modem */
    uint8_t drate_e = 0, drate_m = 0;
    calc_datarate(cfg->modem.datarate_bps, &drate_e, &drate_m);

    uint8_t mdmcfg4 = CC1101_MDMCFG4_VALUE(cfg->modem.chanbw >> 6,
                                             cfg->modem.chanbw & 0x3,
                                             drate_e);
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
    cc1101_write_reg(dev, CC1101_PKTCTRL0,
                     (cfg->packet.crc_enable ? CC1101_CRC_ENABLE : 0) |
                     (cfg->packet.whitening ? CC1101_DATA_WHITENING : 0) |
                     (cfg->packet.mode & 0x03));

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
    /* Default config for backward compat with sniffer */
    cc1101_write_reg(dev, CC1101_IOCFG2, CC1101_GDO_SYNC_WORD);
    cc1101_write_reg(dev, CC1101_IOCFG0, CC1101_GDO_SYNC_WORD);
    cc1101_write_reg(dev, CC1101_PKTLEN, 255);
    cc1101_write_reg(dev, CC1101_PKTCTRL1, CC1101_APPEND_STATUS | CC1101_ADR_CHK_NONE);
    cc1101_write_reg(dev, CC1101_PKTCTRL0, CC1101_CRC_ENABLE | CC1101_PKT_VARIABLE);
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
    uint8_t rx_bytes = cc1101_read_status(dev, CC1101_RXBYTES);

    if (rx_bytes & 0x80) {
        ESP_LOGW(TAG, "RX overflow");
        cc1101_set_rx_mode(dev);
        return false;
    }
    if ((rx_bytes & 0x7F) == 0)
        return false;

    if (!buffer || !len) {
        ESP_LOGE(TAG, "Invalid RX args");
        return false;
    }

    uint8_t pkt_len = cc1101_read_reg(dev, CC1101_FIFO_ADDR);
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

    /* Compute expected values */
    uint32_t freq_reg = (uint32_t)(((uint64_t)cfg->freq_hz << 16) / 26000000ULL);
    uint8_t exp_freq2 = (freq_reg >> 16) & 0xFF;

    uint8_t drate_e = 0, drate_m = 0;
    calc_datarate(cfg->modem.datarate_bps, &drate_e, &drate_m);
    uint8_t exp_mdmcfg4 = CC1101_MDMCFG4_VALUE(cfg->modem.chanbw >> 6,
                                                 cfg->modem.chanbw & 0x3,
                                                 drate_e);
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
                                       (cfg->packet.mode & 0x03) },
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
