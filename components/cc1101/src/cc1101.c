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
    status->pktctrl0 = cc1101_read_reg(dev, CC1101_PKTCTRL0);
    status->mdmcfg2 = cc1101_read_reg(dev, CC1101_MDMCFG2);
    status->iocfg0 = cc1101_read_reg(dev, CC1101_IOCFG0);
    status->marcstate = cc1101_read_status(dev, CC1101_MARCSTATE) & 0x1F;
}

static esp_err_t cc1101_wait_state(cc1101_handle_t *dev, uint8_t state)
{
    int64_t deadline = esp_timer_get_time() + 20000;
    do {
        if ((cc1101_read_status(dev, CC1101_MARCSTATE) & 0x1F) == state)
            return ESP_OK;
        vTaskDelay(1);
    } while (esp_timer_get_time() < deadline);
    return ESP_ERR_TIMEOUT;
}

esp_err_t cc1101_set_rx_format(cc1101_handle_t *dev, cc1101_rx_format_t format)
{
    if (format != CC1101_RX_PACKET && format != CC1101_RX_RAW_OOK)
        return ESP_ERR_INVALID_ARG;

    cc1101_strobe(dev, CC1101_SIDLE);
    esp_err_t err = cc1101_wait_state(dev, 0x01);
    if (err != ESP_OK)
        return err; /* Do not flush a potentially live receiver. */
    cc1101_strobe(dev, CC1101_SFRX);

    bool raw = format == CC1101_RX_RAW_OOK;
    uint8_t pktctrl0 = raw ? 0x30 : 0x05;
    uint8_t mdmcfg2 = raw ? 0x30 : 0x33;
    uint8_t iocfg0 = raw ? 0x0D : CC1101_GDO_SYNC_WORD;
    cc1101_write_reg(dev, CC1101_PKTCTRL0, pktctrl0);
    cc1101_write_reg(dev, CC1101_MDMCFG2, mdmcfg2);
    cc1101_write_reg(dev, CC1101_IOCFG0, iocfg0);

    cc1101_rx_format_status_t actual;
    cc1101_get_rx_format_status(dev, &actual);
    if (actual.pktctrl0 != pktctrl0 || actual.mdmcfg2 != mdmcfg2 ||
        actual.iocfg0 != iocfg0)
        return ESP_FAIL; /* Remain IDLE, application marks applied mode invalid. */

    cc1101_strobe(dev, CC1101_SRX);
    err = cc1101_wait_state(dev, 0x0D);
    if (err != ESP_OK)
        cc1101_strobe(dev, CC1101_SIDLE);
    return err;
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
    uint8_t modulation = cc1101_read_reg(dev, CC1101_MDMCFG2) &
                         CC1101_MDMCFG2_MOD_FORMAT_MASK;
    uint8_t frend0 = cc1101_read_reg(dev, CC1101_FREND0) &
                     ~CC1101_FREND0_PA_POWER_MASK;

    if (modulation == (CC1101_MOD_ASK_OOK << 4))
    {
        /* One ordered burst: index 0 is off, index 1 is the OOK on level. */
        const uint8_t pa_table[] = {0x00, pa_value};
        cc1101_write_burst(dev, CC1101_PATABLE, pa_table, sizeof(pa_table));
        frend0 |= 1;
    }
    else
    {
        cc1101_write_burst(dev, CC1101_PATABLE, &pa_value, 1);
    }

    /* Preserve the front-end configuration outside the PA table index. */
    cc1101_write_reg(dev, CC1101_FREND0, frend0);
    return ESP_OK;
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
