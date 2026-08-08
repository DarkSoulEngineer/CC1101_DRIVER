# RFuzz — CC1101 Sub-GHz RF Analysis Framework

> **Tagline**: Professional-grade ESP32-S3 + CC1101 driver and analysis toolkit for sub-1 GHz RF research, fuzzing, and protocol reverse engineering.

---

## 🎯 Project Goals

| Goal | Description |
|------|-------------|
| **Zero-dependency driver** | Native ESP-IDF CC1101 driver with full register-level control |
| **Logic analyzer on-chip** | SUMP/OLS compatible 2-channel capture (GDO0 + GDO2) up to 500 kS/s |
| **Async raw demodulation** | Transparent 2FSK/GFSK bit streaming without packet layer overhead |
| **Cross-platform host tools** | Python scripts for capture, analysis, PulseView/Sigrok integration |
| **Dragon OS / HackRF integration** | Coordinated TX/RX with HackRF One via SSH for validation |

---

## 📦 Repository Structure

```text
CC1101_DRIVER/
├── main/                    # ESP-IDF main application
│   ├── main.c              # Default: Async RX capture firmware
│   ├── RFuzz_TX.c          # Beacon TX example (2FSK/GFSK)
│   └── RFuzz_RX.c          # Packet RX example (2FSK 2400 bps)
├── components/
│   ├── cc1101/             # CC1101 driver (SPI, config, ISR)
│   ├── hw_init/            # Board support (SPI, GPIO, USB)
│   └── sump_capture/       # SUMP/OLS logic analyzer firmware
├── examples/
│   ├── rfuzz_tx_2fsk.c     # 2FSK TX example
│   └── usb_interface_test/ # USB Serial/JTAG test
├── scripts/                # Python host tools
│   ├── capture.py          # Raw streaming capture → .sr
│   ├── capture_sump.py     # SUMP/OLS protocol → .sr/.vcd/.bin
│   ├── sump_stream_capture.py
│   ├── tx_verify.py        # Dual-port TX verification
│   ├── rssi_mon.py         # RSSI logging
│   └── capture_gdo.py      # Quick GDO stream test
├── docs/obsidian/          # ← THIS VAULT
└── sdkconfig               # ESP-IDF Kconfig (menuconfig)
```

---

## 🔑 Key Features

### Firmware (ESP32-S3)
- **CC1101 Driver**: Full register map, burst SPI, GDO0 ISR, async RX mode
- **SUMP Capture**: 2-channel logic analyzer (GDO0/GDO2), timer ISR sampling up to 250 kHz sustained
- **Transport**: USB Serial/JTAG (recommended) or UART0 for OLS protocol
- **Commands**: `0x01` capture, `0x02` TX trigger, `0x03` stream, `0x04` stop, `0x05` freq sweep

### Host Tools (Python)
- **capture.py**: Simple raw streaming → Sigrok `.sr` session files
- **capture_sump.py**: Full SUMP/OLS protocol → PulseView compatible `.sr`, `.vcd`, `.bin`, `.hex`
- **RSSI sweep**: Frequency domain analysis via CC1101 RSSI register

### Dragon OS / HackRF Integration
- **SSH**: `dragon@192.168.1.101` (Dragon OS Focal on HackRF One)
- **Use cases**: TX validation, RX cross-check, spectrum monitoring, coordinated fuzzing

---

## 🚀 Quick Start

### 1. Hardware
```
ESP32-S3 DevKit + CC1101 Module (433/868/915 MHz)
Connections: See [[01-hardware/pinout]]
```

### 2. Build & Flash
```bash
cd CC1101_DRIVER
idf.py set-target esp32s3
idf.py menuconfig          # Configure pins, freq, modulation
idf.py build flash monitor
```

### 3. Capture (Host)
```bash
# Simple raw capture
python scripts/capture.py --port COM7 --rate 24000 --samples 100000

# SUMP/OLS (PulseView compatible)
python scripts/capture_sump.py --port COM7 --rate 24000 --samples 100000 --format sr
```

### 4. Dragon OS / HackRF
```bash
ssh dragon@192.168.1.101
# hackrf_transfer, hackrf_sweep, gr-inspector, etc.
```

---

## 📚 Documentation Map

| Section | File | Purpose |
|---------|------|---------|
| **Overview** | `00-overview/index.md` | This file |
| **Hardware** | `01-hardware/pinout.md` | ESP32-S3 ↔ CC1101 wiring |
| | `01-hardware/board-support.md` | Supported boards, pin mappings |
| **Firmware** | `02-firmware/architecture.md` | Component diagram, data flow |
| | `02-firmware/cc1101-driver.md` | API reference, config structs |
| | `02-firmware/sump-capture.md` | Logic analyzer internals |
| **Applications** | `03-applications/main-capture.md` | Default async RX firmware |
| | `03-applications/tx-beacon.md` | RFuzz_TX.c beacon transmitter |
| | `03-applications/rx-packet.md` | RFuzz_RX.c packet receiver |
| | `03-applications/examples.md` | Example projects |
| **Host Scripts** | `04-host-scripts/capture.py.md` | Raw streaming capture |
| | `04-host-scripts/capture_sump.py.md` | SUMP/OLS protocol capture |
| | `04-host-scripts/other-scripts.md` | tx_verify, rssi_mon, etc. |
| **Dragon OS** | `05-dragon-os/ssh-setup.md` | SSH keys, connection |
| | `05-dragon-os/hackrf-ops.md` | hackrf_transfer, sweep, RX/TX |
| | `05-dragon-os/coordinated.md` | Joint ESP32+HackRF workflows |
| **Build/Config** | `06-build-config/kconfig.md` | All Kconfig options |
| | `06-build-config/menuconfig-guide.md` | Step-by-step config |
| **Workflow** | `07-workflow/development.md` | Edit-build-flash-debug cycle |
| | `07-workflow/testing.md` | Unit test, integration test |
| | `07-workflow/debugging.md` | Logs, GDB, logic analyzer |
| **Reference** | `08-reference/register-map.md` | CC1101 register quick-ref |
| | `08-reference/protocol.md` | Host↔ESP32 command protocol |
| | `08-reference/troubleshooting.md` | Common issues |

---

## 🤖 AI-Readable Summary

```yaml
project: "RFuzz"
target: "ESP32-S3"
radio: "CC1101"
bands: ["433 MHz", "868 MHz", "915 MHz"]
modulations: ["2FSK", "GFSK", "ASK/OOK", "4FSK", "MSK"]
max_baud: 600000
capture_channels: 2
capture_max_rate: 500000
transport: ["USB Serial/JTAG", "UART0"]
host_scripts: ["capture.py", "capture_sump.py", "tx_verify.py", "rssi_mon.py"]
dragon_os: "dragon@192.168.1.101"
hackrf: "HackRF One"
firmware_apps: ["main (async RX)", "RFuzz_TX (beacon)", "RFuzz_RX (packet)"]
output_formats: [".sr (Sigrok)", ".vcd (GTKWave)", ".bin", ".hex"]
```

---

## 🔗 Related Links

- [[01-hardware/pinout|Hardware Pinout]]
- [[02-firmware/architecture|Firmware Architecture]]
- [[03-applications/main-capture|Default Capture Firmware]]
- [[04-host-scripts/capture_sump.py|SUMP Capture Script]]
- [[05-dragon-os/ssh-setup|Dragon OS SSH Setup]]