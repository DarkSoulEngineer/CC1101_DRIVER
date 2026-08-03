# CC1101 Driver API Reference

> **Source**: `components/cc101/include/cc1101.h`, `components/cc1101/src/cc1101.c`

---

## Quick Reference

| Category | Functions |
|----------|-----------|
| **Init/Config** | `cc1101_init`, `cc1101_configure`, `cc1101_reset`, `CC1101_DEFAULT_CONFIG` |
| **Mode Control** | `cc1101_set_tx_mode`, `cc1101_set_rx_mode`, `cc1101_transmit`, `cc1101_receive_packet` |
| **Runtime Adjust** | `cc1101_set_frequency`, `cc1101_set_channel`, `cc1101_set_tx_power`, `cc1101_set_datarate`, `cc1101_set_tx_len` |
| **ISR/Timing** | `cc1101_isr_enable`, `cc1101_wait_tx_done` |
| **App Helpers** | `cc1101_config_async_rx`, `cc1101_tx_test`, `cc1101_tx_test_preserve_rx`, `cc1101_freq_sweep` |
| **Diagnostics** | `cc1101_log_registers`, `cc1101_dump_registers`, `cc1101_verify_config`, `cc1101_start_status_monitor` |
| **Low-Level** | `cc1101_write_reg`, `cc1101_read_reg`, `cc1101_strobe`, `cc1101_read_status_reg` |

---

## Init & Configuration

### `cc1101_init`
```c
esp_err_t cc1101_init(cc1101_handle_t *dev,
                      spi_device_handle_t spi_handle,
                      gpio_num_t cs,
                      gpio_num_t miso,
                      gpio_num_t gdo0);
```
- Binds handle to SPI device and GPIO pins
- Creates internal mutex, resets radio via SRES strobe
- Must be called before any other API

### `cc1101_configure`
```c
esp_err_t cc1101_configure(cc1101_handle_t *dev,
                           const cc1101_config_t *cfg);
```
- Full radio configuration from `cc1101_config_t` struct
- Programs all modem, packet, and radio registers
- Calculates datarate registers from `datarate_bps`
- Calibrates (SCAL) after configuration

### `CC1101_DEFAULT_CONFIG()` Macro
```c
cc1101_config_t cfg = CC1101_DEFAULT_CONFIG();
// Override individual fields
cfg.freq_hz = 868300000;
cfg.modem.datarate_bps = 38400;
cc1101_configure(&radio, &cfg);
```
- Populates struct from Kconfig values (see [[06-build-config/kconfig]])
- All fields overrideable at runtime

### `cc1101_reset`
```c
esp_err_t cc1101_reset(cc1101_handle_t *dev);
```
- Hardware reset sequence: CS toggle → wait MISO low → SRES strobe → wait MISO low
- Returns radio to power-on defaults

---

## Mode Control

### `cc1101_set_tx_mode`
```c
void cc1101_set_tx_mode(cc1101_handle_t *dev);
```
- SIDLE → SFTX (flush TX FIFO)

### `cc1101_set_rx_mode`
```c
void cc1101_set_rx_mode(cc1101_handle_t *dev);
```
- SIDLE → SFRX (flush RX FIFO) → SRX

### `cc1101_transmit`
```c
void cc1101_transmit(cc1101_handle_t *dev, uint8_t *data, size_t len);
```
- SIDLE → SFTX → write FIFO → STX
- If ISR enabled: sets `tx_pending`, stores caller task handle
- Non-blocking; use `cc1101_wait_tx_done()` to wait

### `cc1101_receive_packet`
```c
bool cc1101_receive_packet(cc1101_handle_t *dev,
                           uint8_t *buffer, size_t *len);
```
- Checks `RXBYTES` status register
- Reads packet length from FIFO, then payload
- If `append_status`: reads 2 status bytes (RSSI + CRC), returns CRC OK
- Always re-enters RX mode after read

---

## Runtime Adjustments

All take effect immediately (no re-calibration unless noted):

| Function | Register(s) Modified |
|----------|---------------------|
| `cc1101_set_frequency` | FREQ2, FREQ1, FREQ0 |
| `cc1101_set_channel` | CHANNR |
| `cc1101_set_tx_power` | PATABLE (burst) |
| `cc1101_set_datarate` | MDMCFG4 (drate_e), MDMCFG3 (drate_m) |
| `cc1101_set_tx_len` | PKTLEN |

---

## ISR & Timing

### `cc1101_isr_enable`
```c
esp_err_t cc1101_isr_enable(cc1101_handle_t *dev, bool enable);
```
- Installs/removes GPIO ISR on GDO0 (negative edge)
- ISR notifies `tx_caller_task` when TX done (GDO0 goes low in SYNC_WORD mode)
- Required for `cc1101_wait_tx_done()` to work

### `cc1101_wait_tx_done`
```c
bool cc1101_wait_tx_done(cc1101_handle_t *dev, uint32_t timeout_ms);
```
- If ISR enabled: waits for task notification (FreeRTOS)
- If ISR disabled: polls MARCSTATE until not TX (0x0B)
- Returns `true` if TX completed, `false` on timeout

---

## App-Level Helpers

### `cc1101_config_async_rx`
```c
esp_err_t cc1101_config_async_rx(cc1101_handle_t *dev,
                                 uint32_t freq_hz,
                                 uint32_t datarate_bps,
                                 uint8_t deviation,
                                 uint8_t chanbw,
                                 uint8_t gdo2_mode);
```
**Configures transparent async serial RX:**
- 2FSK, no sync word, infinite packet length
- No CRC, no whitening, no status bytes
- GDO0 = async data out (0x0D)
- GDO2 = `gdo2_mode` (e.g., 0x0D for same data, 0x2E for disabled)
- Enters RX mode
- **Caches params** on handle for `cc1101_tx_test()` restore

### `cc1101_tx_test`
```c
esp_err_t cc1101_tx_test(cc1101_handle_t *dev);
```
- Sends fixed test packet: sync=0xDEAF, payload=0x01, +10 dBm
- Uses 2FSK, 16/16 sync, 4-byte preamble
- **Restores async RX config** from handle cache after TX
- Use for loopback/self-test

### `cc1101_tx_test_preserve_rx`
```c
esp_err_t cc1101_tx_test_preserve_rx(cc1101_handle_t *dev);
```
- Same test packet but **saves/restores all registers** (0x00-0x3E)
- Use when RX is in packet mode (not async) and must not be disturbed

### `cc1101_freq_sweep`
```c
void cc1101_freq_sweep(cc1101_handle_t *dev,
                       uint32_t start_hz, uint32_t end_hz, uint32_t step_hz,
                       cc1101_sweep_output_fn out);
```
- `out` callback: `void (*fn)(const uint8_t *buf, size_t len)` — one RSSI byte per step
- Steps frequency, enters RX, waits 15ms (autocal + settle), reads RSSI status
- Streams raw RSSI bytes via callback
- Restores async RX config on completion

---

## Diagnostics

### `cc1101_log_registers`
```c
void cc1101_log_registers(cc1101_handle_t *dev);
```
Logs key config + status registers once (boot diagnostics):
- IOCFG2, IOCFG0, PKTCTRL1, PKTCTRL0, SYNC1/0, PKTLEN
- MDMCFG4-2, MCSM1, FREQ2/1/0, MARCSTATE, PKTSTATUS, RSSI

### `cc1101_dump_registers`
```c
void cc1101_dump_registers(cc1101_handle_t *dev);
```
Full register dump (0x00-0x2E) + PARTNUM, VERSION, MARCSTATE

### `cc1101_verify_config`
```c
void cc1101_verify_config(cc1101_handle_t *dev, const cc1101_config_t *cfg);
```
Reads back key registers, compares to expected values from `cfg`, logs PASS/FAIL

### `cc1101_start_status_monitor`
```c
void cc1101_start_status_monitor(cc1101_handle_t *dev, uint32_t period_ms);
```
Spawns FreeRTOS task logging every `period_ms`:
- MARCSTATE, PKTSTATUS, RSSI (dBm), RXBYTES

---

## Low-Level SPI Primitives

| Function | Description |
|----------|-------------|
| `cc1101_write_reg(dev, reg, val)` | Single register write |
| `cc1101_read_reg(dev, reg)` | Single register read |
| `cc1101_strobe(dev, strobe)` | Command strobe (SRES, STX, SRX, etc.) |
| `cc1101_read_status_reg(dev, reg)` | Status register read (burst with dummy) |
| `cc1101_write_burst(dev, reg, data, len)` | Burst write (FIFO, PATABLE) |
| `cc1101_read_burst(dev, reg, data, len)` | Burst read (FIFO, status) |

All protected by `dev->spi_mutex` (FreeRTOS mutex).

---

## Configuration Structs

```c
// Modem configuration
typedef struct {
    cc1101_modulation_t modulation;    // 2FSK, GFSK, ASK, 4FSK, MSK
    cc1101_sync_mode_t  sync_mode;     // None, 15/16, 16/16, 30/32, carrier
    bool                dc_filter_off;
    bool                manchester;
    bool                fec_enable;
    uint8_t             preamble_bytes;   // 0,2,4,8,12,16,20,24,28,32
    uint32_t            datarate_bps;     // Symbol rate
    uint8_t             deviation;        // DEVIATN register value (0-7)
    uint8_t             chanbw;           // MDMCFG4 channel BW (manual)
    uint32_t            channel_spacing;  // MDMCFG0 value
} cc1101_modem_config_t;

// Packet configuration
typedef struct {
    cc1101_pkt_mode_t mode;           // Fixed, Variable, Infinite
    bool              crc_enable;
    bool              whitening;
    bool              append_status;  // RSSI + CRC in FIFO
    uint8_t           max_length;     // PKTLEN (0=255 for variable)
    uint8_t           addr_check;     // Address filtering
    uint8_t           sync1, sync0;   // 16-bit sync word (MSB first)
} cc1101_packet_config_t;

// Radio control
typedef struct {
    uint8_t  autocal;      // NEVER, IDLE_TO_RXTX, RXTX_TO_IDLE, ALWAYS
    uint8_t  pin_mode;     // MCSM1 after TX: 0x00=IDLE, 0x10=RX, 0x3F=FSTXON
    bool     pin_output;   // GDOx output enable
    uint8_t  gdo0_mode;    // IOCFG0 value
    uint8_t  gdo2_mode;    // IOCFG2 value
} cc1101_radio_config_t;

// Full config
typedef struct {
    cc1101_modem_config_t  modem;
    cc1101_packet_config_t packet;
    cc1101_radio_config_t  radio;
    uint32_t               freq_hz;
    uint8_t                channel;
    uint8_t                pa_value;      // PATABLE[0] value
    bool                   isr_enabled;   // GDO0 interrupt
} cc1101_config_t;
```

---

## Common Register Values

### Modulation (`MDMCFG2[6:4]`)
| Value | Mode |
|-------|------|
| 0 | 2-FSK |
| 1 | GFSK |
| 3 | ASK/OOK |
| 4 | 4-FSK |
| 7 | MSK |

### Sync Mode (`MDMCFG2[2:0]`)
| Value | Mode |
|-------|------|
| 0 | No sync |
| 1 | 15/16 bits |
| 2 | 16/16 bits |
| 3 | 30/32 bits |
| 5-7 | Carrier + sync |

### Packet Mode (`PKTCTRL0[4:3]` format, `[2:1]` length)
| Mode | PKTCTRL0 Value |
|------|----------------|
| Fixed | `0x00` (normal, fixed len) |
| Variable | `0x04` (normal, variable len) |
| Infinite | `0x08` (normal, infinite) |
| Async | `0x30` (async format, infinite) |

### GDO Modes (IOCFG0/IOCFG2)
| Value | Mode |
|-------|------|
| 0x00 | RX FIFO threshold |
| 0x06 | Sync word detect |
| 0x07 | CRC OK |
| 0x0B | RSSI valid |
| 0x0D | Async serial data |
| 0x2E | High-Z (disabled) |
| 0x2F | HW to 0 |

### Autocal (MCSM0)
| Value | Mode |
|-------|------|
| 0x00 | Never |
| 0x10 | Idle → RX/TX |
| 0x20 | RX/TX → Idle |
| 0x30 | Always |

### PA Table (Common Values)
| Value | Power |
|-------|-------|
| 0x00 | -30 dBm |
| 0x50 | 0 dBm |
| 0x84 | +5 dBm |
| 0x85 | +7 dBm |
| 0xC5 | +10 dBm |
| 0xC7 | +12 dBm |

---

## Frequency Calculation

```c
// Register value from Hz
freq_reg = (freq_hz << 16) / 26000000
FREQ2 = (freq_reg >> 16) & 0xFF
FREQ1 = (freq_reg >> 8) & 0xFF
FREQ0 = freq_reg & 0xFF

// Common presets (from cc1101.h)
433.92 MHz:  0x10, 0xA7, 0x62
868.30 MHz:  0x21, 0x65, 0x6A
915.00 MHz:  0x23, 0x31, 0x3B
```

---

## Datarate Calculation

```c
// From cc1101.c:calc_datarate()
for (e = 0; e < 16; e++) {
    val = (baud << 28) / (26000000 << e);
    if (val >= 256 && val <= 511) {
        drate_e = e;
        drate_m = val - 256;
        break;
    }
}
MDMCFG4 = (chanbw_e << 6) | (chanbw_m << 4) | drate_e
MDMCFG3 = drate_m
```

---

## AI-Readable API Index

```yaml
api:
  init:
    - cc1101_init
    - cc1101_configure
    - cc1101_reset
    - CC1101_DEFAULT_CONFIG
  mode:
    - cc1101_set_tx_mode
    - cc1101_set_rx_mode
    - cc1101_transmit
    - cc1101_receive_packet
  runtime:
    - cc1101_set_frequency
    - cc1101_set_channel
    - cc1101_set_tx_power
    - cc1101_set_datarate
    - cc1101_set_tx_len
  isr:
    - cc1101_isr_enable
    - cc1101_wait_tx_done
  app_helpers:
    - cc1101_config_async_rx
    - cc1101_tx_test
    - cc1101_tx_test_preserve_rx
    - cc1101_freq_sweep
  diagnostics:
    - cc1101_log_registers
    - cc1101_dump_registers
    - cc1101_verify_config
    - cc1101_start_status_monitor
  low_level:
    - cc1101_write_reg
    - cc1101_read_reg
    - cc1101_strobe
    - cc1101_read_status_reg
    - cc1101_write_burst
    - cc1101_read_burst

config_structs:
  - cc1101_modem_config_t
  - cc1101_packet_config_t
  - cc1101_radio_config_t
  - cc1101_config_t
  - cc1101_handle_t
```

---

## Related

- [[02-firmware/architecture|Firmware Architecture]]
- [[02-firmware/sump-capture|SUMP Capture Internals]]
- [[03-applications/main-capture|Async RX Capture Usage]]
- [[03-applications/tx-beacon|TX Beacon Usage]]
- [[06-build-config/kconfig|Kconfig Options]]