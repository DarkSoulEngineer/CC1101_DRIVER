# Kconfig Reference

> **Source**: `sdkconfig`, component `Kconfig` files

---

## Configuration Hierarchy

```
menuconfig (idf.py menuconfig)
├── CC1101 Radio Configuration (components/cc1101/Kconfig)
├── CC1101 Hardware / Board Support (components/hw_init/Kconfig)
├── SUMP Capture (components/sump_capture/Kconfig)
├── USB Interface (components/hw_init/Kconfig)
└── ESP-IDF System Config (sdkconfig)
```

---

## CC1101 Radio Configuration

### Frequency Band
```kconfig
choice CC1101_FREQ_BAND
    default CC1101_FREQ_433
    config CC1101_FREQ_433    # 433 MHz ISM
    config CC1101_FREQ_868    # 868 MHz SRD
    config CC1101_FREQ_915    # 915 MHz ISM
endchoice

config CC1101_FREQ_HZ
    int "Frequency override (Hz, 0 = use band default)"
    default 0
```
| Setting | Value | Description |
|---------|-------|-------------|
| `CONFIG_CC1101_FREQ_433` | y | 433.92 MHz default |
| `CONFIG_CC1101_FREQ_868` | y | 868.30 MHz default |
| `CONFIG_CC1101_FREQ_915` | y | 915.00 MHz default |
| `CONFIG_CC1101_FREQ_HZ` | 0 | Override (e.g., 433920000) |

### Modulation
```kconfig
choice CC1101_MODULATION
    default CC1101_MOD_2FSK
    config CC1101_MOD_2FSK
    config CC1101_MOD_GFSK
    config CC1101_MOD_ASK_OOK
    config CC1101_MOD_4FSK
    config CC1101_MOD_MSK
endchoice
```
| Setting | Value | Mode |
|---------|-------|------|
| `CONFIG_CC1101_MOD_2FSK` | y | 2-FSK |
| `CONFIG_CC1101_MOD_GFSK` | y | GFSK |
| `CONFIG_CC1101_MOD_ASK_OOK` | y | ASK/OOK |
| `CONFIG_CC1101_MOD_4FSK` | y | 4-FSK |
| `CONFIG_CC1101_MOD_MSK` | y | MSK |

### Datarate & Deviation
```kconfig
config CC1101_DATARATE
    int "Datarate (bps)"
    default 2400
    range 600 500000

config CC1101_DEVIATION
    int "Frequency deviation (register 0-7)"
    default 4
    range 0 7
```
| Setting | Default | Range | Notes |
|---------|---------|-------|-------|
| `CONFIG_CC1101_DATARATE` | 2400 | 600-500000 | Symbol rate |
| `CONFIG_CC1101_DEVIATION` | 4 | 0-7 | DEVIATN register |

**Common Deviation Values**:
| Reg | 2FSK Dev | GFSK Dev |
|-----|----------|----------|
| 0x03 | ~24 kHz | ~24 kHz |
| 0x04 | ~24 kHz | ~24 kHz |
| 0x47 | ~12 kHz | ~12 kHz |
| 0x27 | ~11.9 kHz | ~11.9 kHz |

### Channel Bandwidth
```kconfig
config CC1101_CHANNEL_BW
    int "Channel bandwidth (MDMCFG4 BW bits, 0-60)"
    default 12
    range 0 60
```
**Value = (CHANBW_E << 4) | CHANBW_M**

| Value | BW (kHz) | CHANBW_E | CHANBW_M |
|-------|----------|----------|----------|
| 0x00 | 58 | 0 | 0 |
| 0x0C | 203 | 0 | 3 |
| 0x03 | 812 | 0 | 3 |
| 0x30 | 81 | 2 | 0 |
| 0x40 | 101 | 3 | 0 |
| 0x80 | 128 | 4 | 0 |
| 0x90 | 162 | 4 | 1 |
| 0xB0 | 203 | 5 | 0 |
| 0xC0 | 258 | 6 | 0 |

### Sync Mode
```kconfig
config CC1101_SYNC_MODE
    int "Sync word mode (0-7)"
    default 2
    range 0 7
```
| Value | Mode |
|-------|------|
| 0 | No sync |
| 1 | 15/16 bits |
| 2 | 16/16 bits |
| 3 | 30/32 bits |
| 5 | Carrier + 15/16 |
| 6 | Carrier + 16/16 |
| 7 | Carrier + 30/32 |

### Sync Word
```kconfig
config CC1101_SYNC_WORD
    int "Sync word (16-bit decimal)"
    default 57007  # 0xDEAF
```
| Default | Hex | Use Case |
|---------|-----|----------|
| 57007 | 0xDEAF | General |
| 57007 | 0xDEAF | RFuzz TX/RX |
| 57007 | 0xDEAF | Async RX (unused) |

### Preamble
```kconfig
config CC1101_PREAMBLE_BYTES
    int "Preamble bytes"
    default 4
    range 0 32
```
Valid: 0, 2, 4, 8, 12, 16, 20, 24, 28, 32

### Packet Mode
```kconfig
choice CC1101_PKT_MODE
    default CC1101_PKT_FIXED
    config CC1101_PKT_FIXED
    config CC1101_PKT_VARIABLE
    config CC1101_PKT_INFINITE
endchoice
```
| Setting | Mode | Use Case |
|---------|------|----------|
| `CONFIG_CC1101_PKT_FIXED` | Fixed length | Beacon, known payload size |
| `CONFIG_CC1101_PKT_VARIABLE` | Variable length | General comms |
| `CONFIG_CC1101_PKT_INFINITE` | Infinite (async) | Sniffing, raw demod |

### Packet Options
```kconfig
config CC1101_CRC_ENABLE
    bool "Enable CRC"
    default n

config CC1101_WHITENING
    bool "Enable whitening"
    default n

config CC1101_APPEND_STATUS
    bool "Append status bytes (RSSI + CRC)"
    default n
```

### TX Power
```kconfig
choice CC1101_PA_PRESET
    default CC1101_PA_10dBm
    config CC1101_PA_neg30dBm
    config CC1101_PA_neg20dBm
    config CC1101_PA_neg15dBm
    config CC1101_PA_neg10dBm
    config CC1101_PA_0dBm
    config CC1101_PA_5dBm
    config CC1101_PA_7dBm
    config CC1101_PA_10dBm
    config CC1101_PA_12dBm
endchoice

config CC1101_PA_VALUE
    int "PA table value (raw, 0 = use preset)"
    default 0
    range 0 199
```
| Preset | Power | PATABLE[0] |
|--------|-------|------------|
| `CC1101_PA_neg30dBm` | -30 dBm | 0x00 |
| `CC1101_PA_neg20dBm` | -20 dBm | 0x01 |
| `CC1101_PA_neg15dBm` | -15 dBm | 0x02 |
| `CC1101_PA_neg10dBm` | -10 dBm | 0x03 |
| `CC1101_PA_0dBm` | 0 dBm | 0x50 |
| `CC1101_PA_5dBm` | +5 dBm | 0x84 |
| `CC1101_PA_7dBm` | +7 dBm | 0x85 |
| `CC1101_PA_10dBm` | +10 dBm | 0xC5 |
| `CC1101_PA_12dBm` | +12 dBm | 0xC7 |

### ISR
```kconfig
config CC1101_ISR_ENABLE
    bool "Enable GDO0 interrupt (TX done)"
    default n
```

---

## CC1101 Hardware / Board Support

### Pinout
```kconfig
config CC1101_PIN_GDO0
    int "GDO0 pin"
    default 3
    range 0 48

config CC1101_PIN_GDO2
    int "GDO2 pin"
    default 4
    range 0 48

config CC1101_PIN_CS
    int "CS (chip select) pin"
    default 5
    range 0 48

config CC1101_PIN_SCK
    int "SCK (SPI clock) pin"
    default 15
    range 0 48

config CC1101_PIN_MOSI
    int "MOSI pin"
    default 7
    range 0 48

config CC1101_PIN_MISO
    int "MISO pin"
    default 6
    range 0 48

config CC1101_SPI_HZ
    int "SPI clock speed (Hz)"
    default 1000000
    range 100000 8000000
```

### USB Interface
```kconfig
config USB_INTERFACE_USJ_TX_BUFFER_SIZE
    int "USB Serial/JTAG TX ring buffer size (bytes)"
    default 4096
    range 256 16384

config USB_INTERFACE_USJ_RX_BUFFER_SIZE
    int "USB Serial/JTAG RX ring buffer size (bytes)"
    default 256
    range 64 4096
```

---

## SUMP Capture

### Transport
```kconfig
choice SUMP_TRANSPORT
    default SUMP_TRANSPORT_USB
    config SUMP_TRANSPORT_USB
    config SUMP_TRANSPORT_UART
endchoice

config SUMP_UART_BAUD
    int "UART console / fallback baud rate"
    default 15200
    range 9600 3000000

config SUMP_CLOCK_FREQ
    int "SUMP reference clock frequency (Hz)"
    default 240000000
```
| Setting | Value | Description |
|---------|-------|-------------|
| `CONFIG_SUMP_TRANSPORT_USB` | y | USB Serial/JTAG (recommended) |
| `CONFIG_SUMP_TRANSPORT_UART` | y | UART0 (legacy, DTR reset risk) |
| `CONFIG_SUMP_UART_BAUD` | 15200 | UART baud (console in USB mode) |

### Capture Config
```kconfig
config SUMP_MAX_SAMPLES
    int "Maximum samples per channel"
    default 100000
    range 1000 4000000

config SUMP_NUM_CHANNELS
    int "Number of SUMP channels (1 or 2)"
    default 2
    range 1 2

config SUMP_GDO2_MODE
    int "GDO2 output mode (IOCFG2 register value)"
    default 13  # 0x0D = Async Data
    range 0 47
    # The GDO2 GPIO pin is NOT a SUMP option: capture_init() receives
    # PIN_NUM_GDO2 from CONFIG_CC1101_PIN_GDO2 (hw_init.h).
```

### DTR Reset Guard (UART Mode)
```kconfig
config SUMP_DTR_RESET_GUARD
    bool "DTR/RTS reset guard (software)"
    default y

config SUMP_DTR_GUARD_MS
    int "Guard drain time (ms)"
    default 3000
    range 500 10000
    depends on SUMP_DTR_RESET_GUARD
```

---

## ESP-IDF System Config (Key Settings)

### Target
```kconfig
CONFIG_IDF_TARGET="esp32s3"
CONFIG_IDF_TARGET_ESP32S3=y
```

### Console
```kconfig
# USB Mode (recommended):
CONFIG_ESP_CONSOLE_UART_DEFAULT=y           # Console on UART0 (COM6)
# CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG is not set

# UART Mode:
# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y        # Console on USB JTAG (COM7)
```

### Flash
```kconfig
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHMODE="dio"
CONFIG_ESPTOOLPY_FLASHFREQ="80m"
```

### Partition Table
```kconfig
CONFIG_PARTITION_TABLE_SINGLE_APP=y
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"
```

### Compiler
```kconfig
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
```

---

## Common Configuration Presets

### Preset 1: Async RX Capture (Default main.c)
```ini
CONFIG_CC1101_FREQ_433=y
CONFIG_CC1101_FREQ_HZ=0
CONFIG_CC1101_MOD_2FSK=y
CONFIG_CC1101_DATARATE=2400
CONFIG_CC1101_DEVIATION=39   # 0x27
CONFIG_CC1101_CHANNEL_BW=12
CONFIG_CC1101_SYNC_MODE=0    # None
CONFIG_CC1101_SYNC_WORD=57007
CONFIG_CC1101_PREAMBLE_BYTES=0
CONFIG_CC1101_PKT_INFINITE=y
CONFIG_CC1101_CRC_ENABLE=n
CONFIG_CC1101_WHITENING=n
CONFIG_CC1101_APPEND_STATUS=n
CONFIG_CC1101_PA_0dBm=y
CONFIG_CC1101_ISR_ENABLE=n

CONFIG_SUMP_TRANSPORT_USB=y
CONFIG_SUMP_MAX_SAMPLES=100000
CONFIG_SUMP_NUM_CHANNELS=2
CONFIG_SUMP_GDO2_MODE=13
```

### Preset 2: 2FSK Beacon TX (rfuzz_tx_2fsk.c)
```ini
CONFIG_CC1101_FREQ_433=y
CONFIG_CC1101_FREQ_HZ=0
CONFIG_CC1101_MOD_2FSK=y
CONFIG_CC1101_DATARATE=2400
CONFIG_CC1101_DEVIATION=71   # 0x47
CONFIG_CC1101_CHANNEL_BW=3
CONFIG_CC1101_SYNC_MODE=2    # 16/16
CONFIG_CC1101_SYNC_WORD=57007
CONFIG_CC1101_PREAMBLE_BYTES=4
CONFIG_CC1101_PKT_FIXED=y
CONFIG_CC1101_CRC_ENABLE=n
CONFIG_CC1101_WHITENING=n
CONFIG_CC1101_APPEND_STATUS=n
CONFIG_CC1101_PA_10dBm=y
CONFIG_CC1101_ISR_ENABLE=n
```

### Preset 3: GFSK Beacon TX (RFuzz_TX.c)
```ini
CONFIG_CC1101_FREQ_433=y
CONFIG_CC1101_FREQ_HZ=0
CONFIG_CC1101_MOD_GFSK=y
CONFIG_CC1101_DATARATE=38400
CONFIG_CC1101_DEVIATION=3
CONFIG_CC1101_CHANNEL_BW=3
CONFIG_CC1101_SYNC_MODE=3    # 30/32
CONFIG_CC1101_SYNC_WORD=57007
CONFIG_CC1101_PREAMBLE_BYTES=2
CONFIG_CC1101_PKT_VARIABLE=y
CONFIG_CC1101_CRC_ENABLE=y
CONFIG_CC1101_WHITENING=n
CONFIG_CC1101_APPEND_STATUS=y
CONFIG_CC1101_PA_0dBm=y
CONFIG_CC1101_ISR_ENABLE=y
```

### Preset 4: Packet RX (RFuzz_RX.c)
```ini
CONFIG_CC1101_FREQ_433=y
CONFIG_CC1101_FREQ_HZ=0
CONFIG_CC1101_MOD_2FSK=y
CONFIG_CC1101_DATARATE=2400
CONFIG_CC1101_DEVIATION=71   # 0x47
CONFIG_CC1101_CHANNEL_BW=3
CONFIG_CC1101_SYNC_MODE=2    # 16/16
CONFIG_CC1101_SYNC_WORD=57007
CONFIG_CC1101_PREAMBLE_BYTES=4
CONFIG_CC1101_PKT_VARIABLE=y
CONFIG_CC1101_CRC_ENABLE=n
CONFIG_CC1101_WHITENING=n
CONFIG_CC1101_APPEND_STATUS=y
CONFIG_CC1101_PA_0dBm=y
CONFIG_CC1101_ISR_ENABLE=n
```

---

## Applying Configurations

### Via menuconfig (Interactive)
```bash
idf.py menuconfig
# Navigate to component menus, change values
# Save → Exit
idf.py build
```

### Via sdkconfig.defaults (Reproducible)
```bash
# Create preset file
cat > sdkconfig.rfuzz_tx << 'EOF'
CONFIG_CC1101_FREQ_433=y
CONFIG_CC1101_MOD_2FSK=y
CONFIG_CC1101_DATARATE=2400
# ... all settings
EOF

# Apply
cp sdkconfig.rfuzz_tx sdkconfig
idf.py build
```

### Via Command Line (Single Values)
```bash
# Set single value
idf.py -DCC1101_DATARATE=9600 build

# Or use cmake directly
cmake -DCC1101_DATARATE=9600 -B build
```

---

## AI-Readable Kconfig Index

```yaml
kconfig_sections:
  cc1101_radio:
    prefix: "CONFIG_CC1101_"
    options:
      - {name: "FREQ_BAND", type: "choice", values: ["433", "868", "915"], default: "433"}
      - {name: "FREQ_HZ", type: "int", default: 0, desc: "Override frequency"}
      - {name: "MODULATION", type: "choice", values: ["2FSK", "GFSK", "ASK_OOK", "4FSK", "MSK"], default: "2FSK"}
      - {name: "DATARATE", type: "int", default: 2400, range: "600-500000"}
      - {name: "DEVIATION", type: "int", default: 4, range: "0-7", desc: "DEVIATN register"}
      - {name: "CHANNEL_BW", type: "int", default: 12, range: "0-60", desc: "MDMCFG4 BW bits"}
      - {name: "SYNC_MODE", type: "int", default: 2, range: "0-7"}
      - {name: "SYNC_WORD", type: "int", default: 57007, desc: "16-bit decimal (0xDEAF)"}
      - {name: "PREAMBLE_BYTES", type: "int", default: 4, range: "0-32"}
      - {name: "PKT_MODE", type: "choice", values: ["FIXED", "VARIABLE", "INFINITE"], default: "FIXED"}
      - {name: "CRC_ENABLE", type: "bool", default: false}
      - {name: "WHITENING", type: "bool", default: false}
      - {name: "APPEND_STATUS", type: "bool", default: false}
      - {name: "PA_PRESET", type: "choice", values: ["neg30dBm","neg20dBm","neg15dBm","neg10dBm","0dBm","5dBm","7dBm","10dBm","12dBm"], default: "10dBm"}
      - {name: "PA_VALUE", type: "int", default: 0, range: "0-199", desc: "Raw PA override"}
      - {name: "ISR_ENABLE", type: "bool", default: false}
  
  hw_init:
    prefix: "CONFIG_CC1101_PIN_"
    options:
      - {name: "GDO0", type: "int", default: 3}
      - {name: "GDO2", type: "int", default: 4}
      - {name: "CS", type: "int", default: 5}
      - {name: "SCK", type: "int", default: 15}
      - {name: "MOSI", type: "int", default: 7}
      - {name: "MISO", type: "int", default: 6}
      - {name: "SPI_HZ", type: "int", default: 1000000, range: "100000-8000000"}
  
  usb_interface:
    prefix: "CONFIG_USB_INTERFACE_USJ_"
    options:
      - {name: "TX_BUFFER_SIZE", type: "int", default: 4096}
      - {name: "RX_BUFFER_SIZE", type: "int", default: 256}
  
  sump_capture:
    prefix: "CONFIG_SUMP_"
    options:
      - {name: "TRANSPORT", type: "choice", values: ["USB", "UART"], default: "USB"}
      - {name: "UART_BAUD", type: "int", default: 15200}
      - {name: "CLOCK_FREQ", type: "int", default: 240000000}
      - {name: "MAX_SAMPLES", type: "int", default: 100000, range: "1000-4000000"}
      - {name: "NUM_CHANNELS", type: "int", default: 2, range: "1-2"}
      - {name: "GDO2_MODE", type: "int", default: 13, range: "0-47"}
      - {name: "GDO2_PIN", type: "int", default: 4}
      - {name: "DTR_RESET_GUARD", type: "bool", default: true}
      - {name: "DTR_GUARD_MS", type: "int", default: 3000, range: "500-10000"}
```

---

## Related

- [[06-build-config/menuconfig-guide|Menuconfig Guide]]
- [[01-hardware/board-support|Board Support]]
- [[03-applications/main-capture|Async RX Capture]]
- [[03-applications/tx-beacon|TX Beacon]]
- [[03-applications/rx-packet|RX Packet]]