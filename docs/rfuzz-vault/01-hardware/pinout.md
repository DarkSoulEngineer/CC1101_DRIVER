# Hardware Pinout — ESP32-S3 ↔ CC1101

> **Source**: `components/hw_init/include/hw_init.h` and `components/hw_init/Kconfig`

---

## Default Pin Mapping (ESP32-S3 DevKitC-1)

| CC1101 Pin | Signal | ESP32-S3 GPIO | Kconfig Option | Notes |
|------------|--------|---------------|----------------|-------|
| **CSN** | Chip Select (active low) | **GPIO 5** | `CONFIG_CC1101_PIN_CS` | Output, managed by driver |
| **SCK** | SPI Clock | **GPIO 15** | `CONFIG_CC1101_PIN_SCK` | SPI3_HOST default |
| **MOSI** | Master Out Slave In | **GPIO 7** | `CONFIG_CC1101_PIN_MOSI` | SPI3_HOST default |
| **MISO** | Master In Slave Out | **GPIO 6** | `CONFIG_CC1101_PIN_MISO` | SPI3_HOST default, **critical for reset detection** |
| **GDO0** | Digital Output 0 | **GPIO 3** | `CONFIG_CC1101_PIN_GDO0` | Interrupt / sync word / async data |
| **GDO2** | Digital Output 2 | **GPIO 4** | `CONFIG_CC1101_PIN_GDO2` | Optional 2nd capture channel |

---

## SPI Bus Configuration

```c
// From components/hw_init/src/spi_init.c
spi_bus_config_t buscfg = {
    .mosi_io_num = PIN_NUM_MOSI,   // GPIO 7
    .miso_io_num = PIN_NUM_MISO,   // GPIO 6
    .sclk_io_num = PIN_NUM_CLK,    // GPIO 15
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 256
};

spi_device_interface_config_t devcfg = {
    .clock_speed_hz = CC1101_SPI_SPEED_HZ,  // 1 MHz default
    .mode = 0,                               // SPI Mode 0 (CPOL=0, CPHA=0)
    .spics_io_num = -1,                      // Manual CS via GPIO
    .queue_size = 1
};
```

- **SPI Host**: `SPI3_HOST` (VSPI on ESP32-S3)
- **DMA**: `SPI_DMA_CH_AUTO` (automatic channel allocation)
- **CS Control**: Manual via `gpio_set_level(PIN_NUM_CS, 0/1)` in driver
- **Speed**: 1 MHz default (CC1101 supports up to 10 MHz, 1 MHz recommended for reliability)

---

## GDO Pin Configuration

### GDO0 (Primary — Required)
```c
// Input with pull-down, interrupt capable
gpio_config_t iocfg = {
    .pin_bit_mask = (1ULL << PIN_NUM_GDO0),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_ENABLE,
    .intr_type = GPIO_INTR_DISABLE,  // ISR installed by cc1101_isr_enable()
};
```
**Modes** (via `IOCFG0` register):
| Mode | Value | Use Case |
|------|-------|----------|
| Sync Word Detect | `0x06` | Packet timing reference |
| Async Serial Data | `0x0D` | Raw demodulated bits (sniffing) |
| TX FIFO Threshold | `0x02` | TX flow control |
| High-Z | `0x2E` | Disabled |

### GDO2 (Secondary — Optional)
```c
// Same config as GDO0, different pin
// PIN_NUM_GDO2 = GPIO 4 default
```
**Modes** (via `IOCFG2` register):
| Mode | Value | Use Case |
|------|-------|----------|
| Sync Word Detect | `0x06` | 2nd channel: packet timing |
| CRC OK | `0x07` | Data quality indicator |
| PLL Lock | `0x08` | Carrier detect |
| RSSI Valid | `0x0B` | Signal presence |
| Async Data | `0x0D` | Raw demodulated bits (same as GDO0) |
| High-Z | `0x2E` | Disabled (1-channel capture) |

**Default for SUMP 2-ch capture**: `CONFIG_SUMP_GDO2_MODE = 13` (`0x0D` = Async Data)

---

## Power & RF Connections

```
CC1101 Module          ESP32-S3 DevKit
──────────────────────────────────────────
VCC       ───────────── 3V3 (or 5V → LDO)
GND       ───────────── GND
ANT       ───────────── SMA / u.FL / PCB antenna
```

⚠️ **Critical**: CC1101 is 3.3V logic. ESP32-S3 is 3.3V tolerant. **No level shifter needed.**

---

## Supported Boards

| Board | Notes |
|-------|-------|
| **ESP32-S3-DevKitC-1** | Primary target, all pins available |
| **ESP32-S3-DevKitM-1** | Same pinout, module variant |
| **Custom** | Override via `menuconfig` → `CC1101 Hardware (Board Support)` → `CC1101 Pinout` |

---

## Pinout Diagram (ASCII)

```
                    ESP32-S3 DevKitC-1
    ┌─────────────────────────────────────────┐
    │  USB-C                    BOOT  EN      │
    │                                         │
    │  GPIO 15 (SCK)  ──────┐                 │
    │  GPIO 7  (MOSI)  ─────┤  SPI3_HOST      │
    │  GPIO 6  (MISO)  ─────┤  (VSPI)         │
    │  GPIO 5  (CS)   ──────┘                 │
    │                                         │
    │  GPIO 3  (GDO0)  ──────┐  CC1101 GDO0   │
    │  GPIO 4  (GDO2)  ──────┘  CC1101 GDO2   │
    │                                         │
    │  3V3  ────────────────── CC1101 VCC     │
    │  GND  ────────────────── CC1101 GND     │
    └─────────────────────────────────────────┘
```

---

## AI-Readable Pin Map

```yaml
spi_bus: "SPI3_HOST"
spi_mode: 0
spi_speed_hz: 1000000
pins:
  cs: 5
  sck: 15
  mosi: 7
  miso: 6
  gdo0: 3
  gdo2: 4
gdo0_modes:
  sync_word: 0x06
  async_data: 0x0D
  tx_fifo_thresh: 0x02
  high_z: 0x2E
gdo2_modes:
  sync_word: 0x06
  crc_ok: 0x07
  pll_lock: 0x08
  rssi_valid: 0x0B
  async_data: 0x0D
  high_z: 0x2E
default_sump_gdo2_mode: 0x0D
voltage: "3.3V"
level_shifter_required: false
```

---

## Related

- [[01-hardware/board-support|Board Support & Variants]]
- [[02-firmware/cc1101-driver|CC1101 Driver API]]
- [[02-firmware/sump-capture|SUMP Capture Internals]]
- [[06-build-config/kconfig|Kconfig Reference]]