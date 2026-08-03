# Firmware Architecture

> **Source**: `main/`, `components/cc1101/`, `components/hw_init/`, `components/sump_capture/`
      -- You can change "p" to a custom keymap like "<leader>pr" if you prefer.
      -- You can change "p" to a custom keymap like "<leader>pr" if you prefer.

---

## Component Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              ESP32-S3 Application                           │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐                  │
│  │   main.c     │    │  RFuzz_TX.c  │    │  RFuzz_RX.c  │                  │
│  │  (default)   │    │  (beacon)    │    │  (packet RX) │                  │
│  └──────┬───────┘    └──────┬───────┘    └──────┬───────┘                  │
│         │                   │                   │                          │
│         ▼                   ▼                   ▼                          │
│  ┌──────────────────────────────────────────────────────────────────┐     │
│  │                     hw_init (Board Support)                       │     │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐             │     │
│  │  │  spi_init    │  │  usb_iface   │  │  gpio_config │             │     │
│  │  │  (SPI3_HOST) │  │  (USB JTAG)  │  │  (GDO0/GDO2) │             │     │
│  │  └──────────────┘  └──────────────┘  └──────────────┘             │     │
│  └────────────────────────────┬────────────────────────────────────────┘     │
│                               │                                            │
│         ┌─────────────────────┼─────────────────────┐                      │
│         ▼                     ▼                     ▼                      │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐                  │
│  │   cc1101     │    │ sump_capture │    │   (app-specific)              │
│  │  (driver)    │    │  (logic ana) │    │                            │
│  └──────┬───────┘    └──────┬───────┘    └──────────────┘                  │
│         │                   │                                             │
│         ▼                   ▼                                             │
│  ┌──────────────────────────────────────────────────────────────────┐     │
│  │                        Hardware Layer                             │     │
│  │  CC1101 (SPI)  ◄──────────────────────────►  ESP32-S3 SPI3       │     │
│  │  GDO0/GDO2    ◄──────────────────────────►  GPIO 3/4             │     │
│  │  USB JTAG     ◄──────────────────────────►  Host PC              │     │
│  └──────────────────────────────────────────────────────────────────┘     │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Component Responsibilities

### 1. `hw_init` — Board Support Package
**Files**: `components/hw_init/src/{hw_init.c, spi_init.c, usb_interface.c}`

| Function | Purpose |
|----------|---------|
| `init_hardware()` | Initialize SPI bus + CC1101 device + USB Serial/JTAG |
| `hw_spi_init()` | Configure SPI3_HOST, DMA, 1 MHz clock, manual CS |
| `hw_usb_init()` | Install USB Serial/JTAG driver (for SUMP transport) |
| `hw_init_gdo0_input()` | Configure GDO0 as input with pull-down |
| `hw_init_gdo2_input()` | Configure GDO2 as input with pull-down |

**Exports**: `hw_cc1101_spi` (global `spi_device_handle_t`)

---

### 2. `cc1101` — Radio Driver
**Files**: `components/cc1101/src/cc1101.c`, `components/cc1101/include/cc1101.h`

**Architecture**: Handle-based, thread-safe, dual-layer API

| Layer | Functions | Purpose |
|-------|-----------|---------|
| **Low-level** | `cc1101_write_reg`, `cc1101_read_reg`, `cc1101_strobe`, `cc1101_write_burst`, `cc1101_read_burst` | Direct register access, SPI mutex protected |
| **Mid-level** | `cc1101_init`, `cc1101_configure`, `cc1101_reset`, `cc1101_set_frequency`, `cc1101_set_datarate`, `cc1101_set_tx_power` | Parameter validation, register calculation |
| **High-level** | `cc1101_transmit`, `cc1101_receive_packet`, `cc1101_set_tx_mode`, `cc1101_set_rx_mode` | State machine management |
| **App-level** | `cc1101_config_async_rx`, `cc1101_tx_test`, `cc1101_freq_sweep`, `cc1101_start_status_monitor` | Common use-case abstractions |

**Key Data Structures**:
```c
// Opaque handle - supports multiple radios on different buses
typedef struct cc1101_dev {
    spi_device_handle_t spi;
    gpio_num_t cs_pin, miso_pin, gdo0_pin;
    SemaphoreHandle_t spi_mutex;
    bool isr_enabled, append_status;
    volatile bool tx_pending;
    TaskHandle_t tx_caller_task;
    // Async RX config cache for tx_test restore
    uint32_t async_freq_hz, async_datarate_bps;
    uint8_t async_deviation, async_chanbw, async_gdo2_mode;
    uint32_t status_period_ms;
} cc1101_handle_t;

// Full configuration from Kconfig or runtime
typedef struct cc1101_config_t {
    cc1101_modem_config_t  modem;    // Modulation, rate, deviation, BW
    cc1101_packet_config_t packet;   // Sync, CRC, whitening, length
    cc1101_radio_config_t  radio;    // Autocal, GDO modes, pin config
    uint32_t freq_hz;
    uint8_t channel;
    uint8_t pa_value;
    bool isr_enabled;
} cc1101_config_t;
```

---

### 3. `sump_capture` — On-Chip Logic Analyzer
**Files**: `components/sump_capture/src/sump_capture.c`, `components/sump_capture/include/sump_capture.h`

**Architecture**: Timer ISR sampling → ring buffer → transport task

| Component | Purpose |
|-----------|---------|
| **Timer ISR** | `gptimer` at sample rate, packs GDO0/GDO2 into 1 byte/sample |
| **Capture Buffer** | `CONFIG_SUMP_MAX_SAMPLES` bytes (default 100k), IRAM/PSRAM |
| **Stream Buffer** | FreeRTOS `StreamBuffer` (2 KB) for continuous streaming |
| **Transport Task** | `stream_task` on CPU0: handles SUMP/OLS protocol commands |
| **Transports** | USB Serial/JTAG (default) or UART0 |

**Command Protocol** (Host → ESP32):
| Cmd | Bytes | Description |
|-----|-------|-------------|
| `0x01` | `[rate:4 LE][count:4 LE]` | Triggered capture (9 bytes) |
| `0x02` | — | TX trigger callback |
| `0x03` | `[rate:4 LE]` | Continuous stream start |
| `0x04` | — | Stop stream |
| `0x05` | `[start:4][end:4][step:4]` | Frequency sweep |

**Response Protocol** (ESP32 → Host):
- Capture: Raw bytes (1 byte = 1 sample, bit0=GDO0, bit1=GDO2)
- Stream: Same format, continuous until `0x04`
- Boot marker: `0xAA` on startup

---

## Data Flow

### Async RX Capture (main.c)
```
Host                    ESP32-S3
  │                        │
  ├─ [0x01][rate][count]──►│  capture_init()
  │                        │  timer ISR starts
  │                        │  samples GDO0/GDO2
  │                        │  fills buffer
  │                        │  timer ISR completes
  │◄─ raw bytes ───────────│  transport_write()
  │                        │
```

### Continuous Stream (capture_sump.py)
```
Host                    ESP32-S3
  │                        │
  ├─ [0x03][rate]─────────►│  stream_start()
  │                        │  timer ISR → StreamBuffer
  │◄─ raw bytes ───────────│  stream_task drains → transport
  │        ...             │  (loop until timeout/stop)
  ├─ [0x04]───────────────►│  stream_stop()
  │                        │
```

### Frequency Sweep
```
Host                    ESP32-S3
  │                        │
  ├─ [0x05][start][end][step]►│ sweep callback
  │                        │  cc1101_freq_sweep()
  │                        │  loops freq, reads RSSI
  │◄─ RSSI bytes ──────────│  capture_send()
  │                        │
```

---

## Threading Model

| Task | Core | Priority | Purpose |
|------|------|----------|---------|
| `main` / `app_main` | CPU0 | 1 | Application entry, inits capture (timer ISR on CPU0) |
| `stream_task` | **CPU0** | 2 | SUMP command loop, transport |
| `rx_loop_task` | **CPU1** | 4 | CC1101 packet RX/FIFO drain loop |
| `cc1101_status_monitor` | CPU0/1 | 5 | Periodic RSSI/MARCSTATE logging |
| `timer ISR` | CPU0 | N/A | Hardware timer, IRAM (follows `capture_init` core) |
| `GPIO ISR` (GDO0) | N/A | N/A | TX done notification |

**Design Notes**:
- `stream_task` pinned to CPU0 and the capture timer ISR allocated on CPU0
  (interrupts follow the calling core, so `capture_init` runs in `app_main` on
  CPU0), keeping the high-rate sampling ISR off the CPU1 RX loop.
- `rx_loop_task` pinned to CPU1 so CC1101 FIFO draining continues while a
  250 kHz capture ISR saturates CPU0 (watchdog: alarm must be ≥ 4 µs).
- SPI mutex protects all CC1101 register access
- Timer ISR runs in IRAM for deterministic sampling

---

## Memory Map

| Region | Size | Purpose |
|--------|------|---------|
| Capture Buffer | `CONFIG_SUMP_MAX_SAMPLES` (default 100 KB) | Raw samples, IRAM preferred |
| Stream Buffer | 2 KB (`CAPTURE_STREAM_BUF_SIZE`) | Continuous stream ring buffer |
| SPI DMA | 256 bytes (`max_transfer_sz`) | Burst transfers |
| USB JTAG TX | 4 KB (`CONFIG_USB_INTERFACE_USJ_TX_BUFFER_SIZE`) | Host → device |
| USB JTAG RX | 256 B (`CONFIG_USB_INTERFACE_USJ_RX_BUFFER_SIZE`) | Device → host |

---

## AI-Readable Architecture

```yaml
components:
  - name: "hw_init"
    type: "BSP"
    provides: ["spi_handle", "usb_driver", "gpio_config"]
    files: ["hw_init.c", "spi_init.c", "usb_interface.c"]
  
  - name: "cc1101"
    type: "driver"
    api_layers: ["low", "mid", "high", "app"]
    handle_based: true
    thread_safe: true
    features: ["burst_spi", "gdo0_isr", "async_rx", "freq_sweep", "status_monitor"]
    files: ["cc1101.c", "cc1101.h"]
  
  - name: "sump_capture"
    type: "logic_analyzer"
    max_rate_hz: 500000
    channels: 2
    sample_format: "bit0=GDO0, bit1=GDO2"
    transports: ["USB_Serial_JTAG", "UART0"]
    protocol: "SUMP/OLS subset"
    files: ["sump_capture.c", "sump_capture.h"]

data_flow:
  async_capture: "Host -> [0x01][rate][count] -> ESP32 -> timer ISR -> buffer -> transport -> Host"
  continuous_stream: "Host -> [0x03][rate] -> ESP32 -> timer ISR -> StreamBuffer -> stream_task -> transport -> Host"
  freq_sweep: "Host -> [0x05][start][end][step] -> ESP32 -> cc1101_freq_sweep -> RSSI -> transport -> Host"

threading:
  app_main: "CPU0, prio 1"
  stream_task: "CPU0, prio 2"
  rx_loop_task: "CPU1, prio 4"
  status_monitor: "CPU0/1, prio 5"
  timer_isr: "IRAM, hardware, CPU0"
  gpio_isr: "hardware"

memory:
  capture_buffer: "CONFIG_SUMP_MAX_SAMPLES bytes (default 100KB)"
  stream_buffer: "2KB"
  spi_dma: "256 bytes"
  usb_tx: "4KB"
  usb_rx: "256B"
```

---

## Related

- [[02-firmware/cc1101-driver|CC1101 Driver API Reference]]
- [[02-firmware/sump-capture|SUMP Capture Internals]]
- [[03-applications/main-capture|Default Capture Firmware]]
- [[03-applications/tx-beacon|TX Beacon Firmware]]
- [[03-applications/rx-packet|RX Packet Firmware]]
