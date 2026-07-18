#ifndef CC1101_H
#define CC1101_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CC1101_FIFO_SIZE        64
#define CC1101_MAX_PACKET_LEN   255
#define CC1101_MAX_PAYLOAD_LEN  (CC1101_FIFO_SIZE - 1)

/* ============================================================
 * CC1101 REGISTER MAP
 * ============================================================ */

#define CC1101_IOCFG2          0x00
#define CC1101_IOCFG1          0x01
#define CC1101_IOCFG0          0x02
#define CC1101_FIFOTHR         0x03
#define CC1101_SYNC1           0x04
#define CC1101_SYNC0           0x05
#define CC1101_PKTLEN          0x06
#define CC1101_PKTCTRL1        0x07
#define CC1101_PKTCTRL0        0x08
#define CC1101_ADDR            0x09
#define CC1101_CHANNR          0x0A
#define CC1101_FSCTRL1         0x0B
#define CC1101_FSCTRL0         0x0C
#define CC1101_FREQ2           0x0D
#define CC1101_FREQ1           0x0E
#define CC1101_FREQ0           0x0F
#define CC1101_MDMCFG4         0x10
#define CC1101_MDMCFG3         0x11
#define CC1101_MDMCFG2         0x12
#define CC1101_MDMCFG1         0x13
#define CC1101_MDMCFG0         0x14
#define CC1101_DEVIATN         0x15
#define CC1101_MCSM2           0x16
#define CC1101_MCSM1           0x17
#define CC1101_MCSM0           0x18
#define CC1101_FOCCFG          0x19
#define CC1101_BSCFG           0x1A
#define CC1101_AGCCTRL2        0x1B
#define CC1101_AGCCTRL1        0x1C
#define CC1101_AGCCTRL0        0x1D
#define CC1101_WOREVT1         0x1E
#define CC1101_WOREVT0         0x1F
#define CC1101_WORCTRL         0x20
#define CC1101_FREND1          0x21
#define CC1101_FREND0          0x22
#define CC1101_FSCAL3          0x23
#define CC1101_FSCAL2          0x24
#define CC1101_FSCAL1          0x25
#define CC1101_FSCAL0          0x26
#define CC1101_RCCTRL1         0x27
#define CC1101_RCCTRL0         0x28
#define CC1101_FSTEST          0x29
#define CC1101_PTEST           0x2A
#define CC1101_AGCTEST         0x2B
#define CC1101_TEST2           0x2C
#define CC1101_TEST1           0x2D
#define CC1101_TEST0           0x2E

/* ============================================================
 * COMMAND STROBES
 * ============================================================ */

#define CC1101_SRES            0x30
#define CC1101_SFSTXON         0x31
#define CC1101_SXOFF           0x32
#define CC1101_SCAL            0x33
#define CC1101_SRX             0x34
#define CC1101_STX             0x35
#define CC1101_SIDLE           0x36
#define CC1101_SAFC            0x37
#define CC1101_SWOR            0x38
#define CC1101_SPWD            0x39
#define CC1101_SFRX            0x3A
#define CC1101_SFTX            0x3B
#define CC1101_SWORRST         0x3C
#define CC1101_SNOP            0x3D

/* ============================================================
 * STATUS REGISTERS
 * ============================================================ */

#define CC1101_PARTNUM         0x30
#define CC1101_VERSION         0x31
#define CC1101_FREQEST         0x32
#define CC1101_LQI             0x33
#define CC1101_RSSI            0x34
#define CC1101_MARCSTATE       0x35
#define CC1101_WORTIME1        0x36
#define CC1101_WORTIME0        0x37
#define CC1101_PKTSTATUS       0x38
#define CC1101_VCO_VC_DAC      0x39
#define CC1101_TXBYTES         0x3A
#define CC1101_RXBYTES         0x3B
#define CC1101_RCCTRL1_STATUS  0x3C
#define CC1101_RCCTRL0_STATUS  0x3D

#define CC1101_PATABLE         0x3E
#define CC1101_FIFO_ADDR       0x3F

/* ============================================================
 * SPI ACCESS
 * ============================================================ */

#define CC1101_WRITE_BURST     0x40
#define CC1101_READ_SINGLE     0x80
#define CC1101_READ_BURST      0xC0

/* ============================================================
 * GDO MODES
 * ============================================================ */

#define CC1101_GDO_RXFIFO_THRESHOLD   0x00
#define CC1101_GDO_RXFIFO_OVERFLOW    0x01
#define CC1101_GDO_TXFIFO_THRESHOLD   0x02
#define CC1101_GDO_TXFIFO_UNDERFLOW   0x03
#define CC1101_GDO_SYNC_WORD          0x06
#define CC1101_GDO_CRC_OK             0x07
#define CC1101_GDO_CARRIER_SENSE      0x0E
#define CC1101_GDO_HIGH_Z             0x2E
#define CC1101_GDO_HW0                0x2F

/* ============================================================
 * PACKET CONTROL
 * ============================================================ */

#define CC1101_PKT_FIXED              0x00
#define CC1101_PKT_VARIABLE           0x01
#define CC1101_PKT_INFINITE           0x02
#define CC1101_CRC_ENABLE             (1 << 2)
#define CC1101_DATA_WHITENING         (1 << 6)

#define CC1101_APPEND_STATUS          (1 << 2)
#define CC1101_ADR_CHK_NONE           0x00
#define CC1101_ADR_CHK_NO_BCAST       0x01
#define CC1101_ADR_CHK_0_BCAST        0x02
#define CC1101_ADR_CHK_0_255_BCAST    0x03

/* ============================================================
 * MODULATION FORMATS
 * ============================================================ */

#define CC1101_MOD_2FSK               0
#define CC1101_MOD_GFSK               1
#define CC1101_MOD_ASK_OOK            3
#define CC1101_MOD_4FSK               4
#define CC1101_MOD_MSK                7

/* ============================================================
 * SYNC MODES
 * ============================================================ */

#define CC1101_SYNC_NONE              0
#define CC1101_SYNC_15_16             1
#define CC1101_SYNC_16_16             2
#define CC1101_SYNC_30_32             3
#define CC1101_SYNC_CARRIER_15_16     5
#define CC1101_SYNC_CARRIER_16_16     6
#define CC1101_SYNC_CARRIER_30_32     7

/* ============================================================
 * MCSM
 * ============================================================ */

#define CC1101_AUTOCAL_NEVER          0x00
#define CC1101_AUTOCAL_IDLE_TO_RXTX   0x10
#define CC1101_AUTOCAL_RXTX_TO_IDLE   0x20
#define CC1101_AUTOCAL_ALWAYS         0x30

/* ============================================================
 * MODEM CONFIG BUILDERS
 * ============================================================ */

#define CC1101_MDMCFG4_VALUE(chanbw_e, chanbw_m, drate_e) \
    ((((chanbw_e) & 0x3) << 6) | (((chanbw_m) & 0x3) << 4) | ((drate_e) & 0xF))

#define CC1101_MDMCFG3_VALUE(drate_m) \
    ((drate_m) & 0xFF)

#define CC1101_MDMCFG2_VALUE(dc_filter_off, mod_format, manchester, sync_mode) \
    ((((dc_filter_off) & 0x1) << 7) | \
     (((mod_format) & 0x7) << 4) | \
     (((manchester) & 0x1) << 3) | \
     ((sync_mode) & 0x7))

#define CC1101_MDMCFG1_VALUE(fec_en, num_preamble, chanspc_e) \
    ((((fec_en) & 0x1) << 7) | \
     (((num_preamble) & 0x7) << 4) | \
     ((chanspc_e) & 0x3))

#define CC1101_MDMCFG0_VALUE(chanspc_m) \
    ((chanspc_m) & 0xFF)

/* ============================================================
 * COMMON FREQUENCY PRESETS
 * ============================================================ */

#define CC1101_FREQ_433_92_2          0x10
#define CC1101_FREQ_433_92_1          0xA7
#define CC1101_FREQ_433_92_0          0x62

#define CC1101_FREQ_868_3_2           0x21
#define CC1101_FREQ_868_3_1           0x65
#define CC1101_FREQ_868_3_0           0x6A

#define CC1101_FREQ_915_0_2           0x23
#define CC1101_FREQ_915_0_1           0x31
#define CC1101_FREQ_915_0_0           0x3B

/* ============================================================
 * PA TABLE PRESETS (CC1101 datasheet Table 35)
 * ============================================================ */

#define CC1101_PA_NEG30dBm   0x00
#define CC1101_PA_NEG20dBm   0x01
#define CC1101_PA_NEG15dBm   0x02
#define CC1101_PA_NEG10dBm   0x03
#define CC1101_PA_0dBm       0x50
#define CC1101_PA_POS5dBm    0x84
#define CC1101_PA_POS7dBm    0x85
#define CC1101_PA_POS10dBm   0xC5
#define CC1101_PA_POS12dBm   0xC7

/* ============================================================
 * CONFIGURATION STRUCTS
 * ============================================================ */

typedef enum {
    CC1101_MOD_2FSK_E  = CC1101_MOD_2FSK,
    CC1101_MOD_GFSK_E  = CC1101_MOD_GFSK,
    CC1101_MOD_ASK_E   = CC1101_MOD_ASK_OOK,
    CC1101_MOD_4FSK_E  = CC1101_MOD_4FSK,
    CC1101_MOD_MSK_E   = CC1101_MOD_MSK,
} cc1101_modulation_t;

typedef enum {
    CC1101_SYNC_NONE_E          = CC1101_SYNC_NONE,
    CC1101_SYNC_15_16_E         = CC1101_SYNC_15_16,
    CC1101_SYNC_16_16_E         = CC1101_SYNC_16_16,
    CC1101_SYNC_30_32_E         = CC1101_SYNC_30_32,
    CC1101_SYNC_CARRIER_15_16_E = CC1101_SYNC_CARRIER_15_16,
    CC1101_SYNC_CARRIER_16_16_E = CC1101_SYNC_CARRIER_16_16,
    CC1101_SYNC_CARRIER_30_32_E = CC1101_SYNC_CARRIER_30_32,
} cc1101_sync_mode_t;

typedef enum {
    CC1101_PKT_FIXED_E    = 0,
    CC1101_PKT_VARIABLE_E = 1,
    CC1101_PKT_INFINITE_E = 2,
} cc1101_pkt_mode_t;

typedef struct {
    cc1101_modulation_t modulation;
    cc1101_sync_mode_t  sync_mode;
    bool                dc_filter_off;
    bool                manchester;
    bool                fec_enable;
    uint8_t             preamble_bytes;  /* 0,2,4,8,12,16,20,24,28,32 */
    uint32_t            datarate_bps;
    uint8_t             deviation;       /* register value (0-7) */
    uint8_t             chanbw;          /* MDMCFG4 channel bandwidth (manual) */
    uint32_t            channel_spacing; /* MDMCFG0 register value */
} cc1101_modem_config_t;

typedef struct {
    cc1101_pkt_mode_t mode;
    bool              crc_enable;
    bool              whitening;
    bool              append_status;
    uint8_t           max_length;
    uint8_t           addr_check;
    uint8_t           sync1;           /* MSB sync word */
    uint8_t           sync0;           /* LSB sync word */
} cc1101_packet_config_t;

typedef struct {
    uint8_t  autocal;          /* CC1101_AUTOCAL_* */
    uint8_t  pin_mode;         /* MCSM1 after TX: 0x00=IDLE, 0x10=RX, 0x3F=FSTXON */
    bool     pin_output;       /* GDOx output enable */
    uint8_t  gdo0_mode;        /* IOCFG0 value */
    uint8_t  gdo2_mode;        /* IOCFG2 value */
} cc1101_radio_config_t;

typedef struct {
    cc1101_modem_config_t  modem;
    cc1101_packet_config_t packet;
    cc1101_radio_config_t  radio;
    uint32_t               freq_hz;
    uint8_t                channel;
    uint8_t                pa_value;      /* raw PA register (use CC1101_PA_* macros) */
    bool                   isr_enabled;   /* GDO0 interrupt for TX done */
} cc1101_config_t;

/* ============================================================
 * HANDLE (opaque)
 * ============================================================ */

typedef struct cc1101_dev {
    spi_device_handle_t spi;
    gpio_num_t          cs_pin;
    gpio_num_t          miso_pin;
    gpio_num_t          gdo0_pin;
    SemaphoreHandle_t   spi_mutex;
    bool                isr_enabled;
    bool                append_status;
    volatile bool       tx_pending;
    TaskHandle_t        tx_caller_task;
} cc1101_handle_t;

/* ============================================================
 * PUBLIC API - INIT / CONFIG
 * ============================================================ */

esp_err_t cc1101_init(cc1101_handle_t *dev,
                      spi_device_handle_t spi_handle,
                      gpio_num_t cs,
                      gpio_num_t miso,
                      gpio_num_t gdo0);

esp_err_t cc1101_configure(cc1101_handle_t *dev,
                           const cc1101_config_t *cfg);

esp_err_t cc1101_reset(cc1101_handle_t *dev);

/* ============================================================
 * PUBLIC API - MODE CONTROL
 * ============================================================ */

void cc1101_transmit(cc1101_handle_t *dev, uint8_t *data, size_t len);
void cc1101_set_tx_mode(cc1101_handle_t *dev);
void cc1101_set_rx_mode(cc1101_handle_t *dev);
bool cc1101_receive_packet(cc1101_handle_t *dev, uint8_t *buffer, size_t *len);
void cc1101_config(cc1101_handle_t *dev);

/* ============================================================
 * PUBLIC API - RUNTIME ADJUSTMENTS
 * ============================================================ */

esp_err_t cc1101_set_frequency(cc1101_handle_t *dev, uint32_t freq_hz);
esp_err_t cc1101_set_channel(cc1101_handle_t *dev, uint8_t channel);
esp_err_t cc1101_set_tx_power(cc1101_handle_t *dev, uint8_t pa_value);
esp_err_t cc1101_set_datarate(cc1101_handle_t *dev, uint32_t baud);
esp_err_t cc1101_set_tx_len(cc1101_handle_t *dev, uint8_t len);

/* ============================================================
 * PUBLIC API - ISR CONTROL
 * ============================================================ */

esp_err_t cc1101_isr_enable(cc1101_handle_t *dev, bool enable);
bool      cc1101_wait_tx_done(cc1101_handle_t *dev, uint32_t timeout_ms);

/* ============================================================
 * PUBLIC API - LOW LEVEL (used by sniffer)
 * ============================================================ */

void    cc1101_write_reg(cc1101_handle_t *dev, uint8_t reg, uint8_t value);
uint8_t cc1101_read_reg(cc1101_handle_t *dev, uint8_t reg);
uint8_t cc1101_strobe(cc1101_handle_t *dev, uint8_t strobe);
uint8_t cc1101_read_status_reg(cc1101_handle_t *dev, uint8_t reg);
void    cc1101_dump_registers(cc1101_handle_t *dev);
void    cc1101_verify_config(cc1101_handle_t *dev, const cc1101_config_t *cfg);

#endif /* CC1101_H */
