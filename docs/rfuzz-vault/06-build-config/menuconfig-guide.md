# Menuconfig Guide

> Step-by-step configuration for common use cases.

---

## Launch Menuconfig

```bash
cd CC1101_DRIVER
idf.py menuconfig
```

**Navigation**:
- `↑/↓` - Move
- `Enter` - Select/Enter submenu
- `Esc` - Back/Exit
- `Space` - Toggle (bool) / Select (choice)
- `Y`/`N` / `M` - Yes/No/Module
- `?` - Help
- `/` - Search

---

## Use Case 1: Async RX Capture (Default main.c)

**Goal**: Raw demodulated bit streaming for sniffing/analysis.

### Settings
```
→ CC1101 Radio Configuration
  → Frequency band: 433 MHz ISM band
  → Frequency override (Hz): 0
  → Modulation format: 2-FSK
  → Datarate (bps): 2400
  → Frequency deviation: 39 (0x27 ≈ 12 kHz)
  → Channel bandwidth: 12 (≈ 203 kHz)
  → Sync word mode: 0 (None)
  → Sync word: 57007 (unused)
  → Preamble bytes: 0
  → Packet mode: Infinite length (async)
  → Enable CRC: No
  → Enable whitening: No
  → Append status bytes: No
  → TX power: 0 dBm
  → Enable GDO0 interrupt: No

→ CC1101 Hardware (Board Support)
  → CC1101 Pinout (use defaults or customize)
  → USB Interface (defaults OK)

→ SUMP Capture
  → OLS transport: USB Serial JTAG (Recommended)
  → Maximum samples per channel: 100000
  → Number of SUMP channels: 2
  → GDO2 output mode: 13 (0x0D = Async data)
  → GDO2 GPIO pin: 4
```

### Save & Build
```
Save configuration → Enter
Exit → Enter
```
```bash
idf.py build flash monitor
```

---

## Use Case 2: 2FSK Beacon TX (rfuzz_tx_2fsk.c)

**Goal**: Simple beacon for validation, easy URH decode.

### Settings
```
→ CC1101 Radio Configuration
  → Frequency band: 433 MHz ISM band
  → Frequency override: 0
  → Modulation format: 2-FSK
  → Datarate (bps): 2400
  → Frequency deviation: 71 (0x47 ≈ 24 kHz)
  → Channel bandwidth: 3 (≈ 812 kHz)
  → Sync word mode: 2 (16/16 bits)
  → Sync word: 57007 (0xDEAF)
  → Preamble bytes: 4
  → Packet mode: Fixed length
  → Enable CRC: No
  → Enable whitening: No
  → Append status bytes: No
  → TX power: +10 dBm
  → Enable GDO0 interrupt: No

→ SUMP Capture (if using capture firmware)
  → OLS transport: USB Serial JTAG
  → GDO2 output mode: 46 (0x2E = High-Z, 1-channel)
```

### Build
```bash
cp examples/rfuzz_tx_2fsk.c main/main.c
idf.py build flash monitor
```

---

## Use Case 3: GFSK Beacon TX (RFuzz_TX.c)

**Goal**: Higher rate beacon with CRC for validation.

### Settings
```
→ CC1101 Radio Configuration
  → Frequency band: 433 MHz ISM band
  → Modulation format: GFSK
  → Datarate (bps): 38400
  → Frequency deviation: 3
  → Channel bandwidth: 3
  → Sync word mode: 3 (30/32 bits)
  → Sync word: 57007
  → Preamble bytes: 2
  → Packet mode: Variable length
  → Enable CRC: Yes
  → Enable whitening: No
  → Append status bytes: Yes
  → TX power: 0 dBm
  → Enable GDO0 interrupt: Yes
```

---

## Use Case 4: Packet RX (RFuzz_RX.c)

**Goal**: Receive packets matching 2FSK beacon.

### Settings
```
→ CC1101 Radio Configuration
  → Frequency band: 433 MHz ISM band
  → Modulation format: 2-FSK
  → Datarate (bps): 2400
  → Frequency deviation: 71 (0x47)
  → Channel bandwidth: 3
  → Sync word mode: 2 (16/16 bits)
  → Sync word: 57007 (0xDEAF)
  → Preamble bytes: 4
  → Packet mode: Variable length
  → Enable CRC: No (matches TX)
  → Enable whitening: No
  → Append status bytes: Yes (for RSSI)
  → TX power: 0 dBm (unused in RX)
  → Enable GDO0 interrupt: No
```

---

## Use Case 5: 868 MHz / 915 MHz Operation

### 868 MHz (EU)
```
→ Frequency band: 868 MHz SRD band
→ (or) Frequency override: 868300000
```
**Notes**: Check local regulations (duty cycle, power limits).

### 915 MHz (US)
```
→ Frequency band: 915 MHz ISM band
→ (or) Frequency override: 915000000
```

---

## Use Case 6: Custom Pinout

```
→ CC1101 Hardware (Board Support)
  → CC1101 Pinout
    → GDO0 pin: 10
    → GDO2 pin: 11
    → CS pin: 12
    → SCK pin: 13
    → MOSI pin: 14
    → MISO pin: 9
    → SPI clock speed: 2000000
```

---

## Use Case 7: UART Transport (Legacy)

```
→ SUMP Capture
  → OLS transport: UART0 (DTR reset risk)
  → UART console / fallback baud rate: 921600
  → DTR/RTS reset guard: Yes
  → Guard drain time: 3000

→ ESP System Settings
  → Console: USB Serial/JTAG
```

**⚠️ Requires**: 10µF capacitor on EN-GND or reliable DTR guard.

---

## Use Case 8: Large Capture Buffer

```
→ SUMP Capture
  → Maximum samples per channel: 500000
  → (Requires PSRAM for > ~300k)
```

**Note**: Enable PSRAM in ESP-IDF:
```
→ Component config → ESP PSRAM
  → Support for external, SPI-connected RAM: Yes
  → SPI RAM config: Auto detect
```

---

## Saving Presets

### Create Preset File
```bash
# After configuring in menuconfig
cp sdkconfig sdkconfig.async_rx
cp sdkconfig sdkconfig.tx_2fsk
cp sdkconfig sdkconfig.rx_packet
```

### Apply Preset
```bash
cp sdkconfig.async_rx sdkconfig
idf.py build
```

### Version Control
```bash
# Add to git
git add sdkconfig.async_rx sdkconfig.tx_2fsk sdkconfig.rx_packet
git commit -m "Add Kconfig presets"
```

---

## Search Tips

Press `/` in menuconfig, then type:

| Search | Finds |
|--------|-------|
| `CC1101_FREQ` | Frequency settings |
| `CC1101_MOD` | Modulation |
| `CC1101_DATARATE` | Datarate |
| `CC1101_SYNC` | Sync word |
| `CC1101_PKT` | Packet mode |
| `CC1101_PA` | TX power |
| `CC1101_PIN` | Pinout |
| `SUMP_TRANSPORT` | SUMP transport |
| `SUMP_MAX` | Buffer size |
| `USB_INTERFACE` | USB buffers |
| `ESP_CONSOLE` | Console selection |

---

## Common Pitfalls

| Issue | Cause | Fix |
|-------|-------|-----|
| "sdkconfig not found" | First build | Run `idf.py menuconfig` once |
| Settings revert | Editing sdkconfig directly | Use `menuconfig` or `sdkconfig.defaults` |
| UART transport fails | Console on same port | Set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` |
| USB transport fails | Console on USB | Set `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` |
| Large buffer OOM | No PSRAM | Enable PSRAM or reduce `SUMP_MAX_SAMPLES` |
| Wrong frequency | Band vs override | Set `FREQ_HZ=0` to use band default |

---

## AI-Readable Quick Reference

```yaml
menuconfig_paths:
  cc1101_radio: "Component config → CC1101 Radio Configuration"
  hw_init: "Component config → CC1101 Hardware (Board Support)"
  sump: "Component config → SUMP Capture"
  usb: "Component config → CC1101 Hardware → USB Interface"
  console: "Component config → ESP System Settings → ESP Console"
  psram: "Component config → ESP PSRAM"

presets:
  async_rx:
    name: "Async RX Capture (main.c)"
    freq: "433 MHz"
    mod: "2FSK"
    rate: 2400
    sync: "None"
    pkt: "INFINITE"
    transport: "USB"
  
  tx_2fsk:
    name: "2FSK Beacon (rfuzz_tx_2fsk.c)"
    freq: "433 MHz"
    mod: "2FSK"
    rate: 2400
    sync: "16/16 (0xDEAF)"
    pkt: "FIXED"
    power: "+10 dBm"
  
  tx_gfsk:
    name: "GFSK Beacon (RFuzz_TX.c)"
    freq: "433 MHz"
    mod: "GFSK"
    rate: 38400
    sync: "30/32 (0x2DD4...)"
    pkt: "VARIABLE"
    crc: true
    power: "0 dBm"
  
  rx_packet:
    name: "Packet RX (RFuzz_RX.c)"
    freq: "433 MHz"
    mod: "2FSK"
    rate: 2400
    sync: "16/16 (0xDEAF)"
    pkt: "VARIABLE"
    append_status: true

common_searches:
  - "CC1101_FREQ"
  - "CC1101_MOD"
  - "CC1101_DATARATE"
  - "CC1101_SYNC"
  - "CC1101_PKT"
  - "CC1101_PA"
  - "CC1101_PIN"
  - "SUMP_TRANSPORT"
  - "ESP_CONSOLE"
```

---

## Related

- [[06-build-config/kconfig|Kconfig Reference]]
- [[01-hardware/board-support|Board Support]]
- [[03-applications/main-capture|Async RX Capture]]
- [[03-applications/tx-beacon|TX Beacon]]
- [[03-applications/rx-packet|RX Packet]]