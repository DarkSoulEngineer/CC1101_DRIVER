# RFuzz Obsidian Vault — Documentation Index

> This vault contains comprehensive documentation for the **RFuzz** project: a CC1101 Sub-GHz RF analysis framework for ESP32-S3.

---

## 📚 Vault Structure

```
docs/rfuzz-vault/
├── 00-overview/
│   └── index.md                 # Project overview, goals, quick start
├── 01-hardware/
│   ├── pinout.md                # ESP32-S3 ↔ CC1101 pin mapping
│   └── board-support.md         # Supported boards, pin configs, USB setup
├── 02-firmware/
│   ├── architecture.md          # Component diagram, data flow, threading
│   ├── cc1101-driver.md         # Full API reference, config structs
│   └── sump-capture.md          # Logic analyzer internals, SUMP protocol
├── 03-applications/
│   ├── main-capture.md          # Default async RX firmware (main.c)
│   ├── tx-beacon.md             # TX beacon firmwares (RFuzz_TX.c, rfuzz_tx_2fsk.c)
│   ├── rx-packet.md             # Packet RX firmware (RFuzz_RX.c)
│   └── examples.md              # Example projects (usb_interface_test)
├── 04-host-scripts/
│   ├── index.md                 # All Python scripts overview

├── 05-dragon-os/
│   ├── ssh-setup.md             # SSH keys, connection to dragon@192.168.1.101
│   ├── hackrf-ops.md            # hackrf_transfer, sweep, gain staging
│   └── coordinated.md           # Joint ESP32+HackRF workflows
├── 06-build-config/
│   ├── kconfig.md               # Complete Kconfig reference
│   └── menuconfig-guide.md      # Step-by-step config for each use case
├── 07-workflow/
│   ├── development.md           # Edit-build-flash-debug cycle
│   ├── testing.md               # Unit, integration, system tests
│   └── debugging.md             # Common issues, diagnostic techniques
├── 08-reference/
│   ├── register-map.md          # CC1101 register quick reference
│   ├── protocol.md              # Host↔ESP32 protocols (raw, SUMP, console)
│   ├── troubleshooting.md       # Symptoms, causes, fixes
│   └── ai-index.md              # Machine-readable project summary
├── _templates/                  # Obsidian templates (empty)
└── _attachments/                # Images, captures (empty)
```

---

## 🛠 Initial Setup & Connectivity Checklist

Verify the physical setup below **before** running any capture, TX/RX test, or HackRF workflow.

### ESP32-S3 — Both USB Interfaces Required

The ESP32-S3 exposes **two independent USB interfaces**. Connect **both** cables:

| Port | Interface | Purpose | Used by |
|------|-----------|---------|---------|
| **COM6** | CH343 UART bridge (UART0, GPIO 43/44) | ESP-IDF console, flashing, programming | `idf.py flash monitor`, `idf.py -p COM6 test`, esptool |
| **COM7** | Native USB Serial/JTAG (GPIO 19/20) | SUMP/OLS data dump | `capture_custom.py`, `sniff.py`, PulseView / GTKWave |

> ⚠️ The two ports are **separate devices** — capturing on COM7 never touches the UART console on COM6 and vice versa. If a script or `esptool` reports "port busy / not found", make sure **both** cables are plugged in and re-check the numbers in Device Manager. COM numbers are machine-dependent (see `01-hardware/board-support.md`); list them with:
> ```powershell
> Get-PnpDevice -Class Ports
> ```

### Dragon OS / HackRF — VMware VM Must Be Running + USB Pass-Through

The HackRF host runs in a **VMware VM**. Before any HackRF command:

1. **Power on the Dragon OS VM** (VMware → Dragon OS).
2. **Attach the HackRF One to the VM**: VM → *Removable Devices* → *HackRF One* → **Connect**.
3. Verify inside the VM that the radio is visible:
   ```bash
   ssh dragon@192.168.1.101 "lsusb | grep -i hackrf"   # must show ID 1d50:6089
   ssh dragon@192.168.1.101 "hackrf_info"              # must print "Found HackRF"
   ```
   If `lsusb` shows no HackRF, the device is still assigned to the host — redo the pass-through.

> 📌 SSH (`dragon@192.168.1.101`, password `dragon`) works as soon as the VM is running, **even if the HackRF USB is not attached yet**. SSH reachability does NOT imply the radio is usable — always confirm with `hackrf_info`. A "No HackRF boards found." error means the USB device is not passed through to the VM.

### Network

Host and VM must be on the same LAN. Reference topology: host `192.168.1.136`, Dragon OS VM `192.168.1.101`, gateway `192.168.1.1`.

---

## 🎯 Quick Navigation by Role

### Hardware Engineer
- [[01-hardware/pinout|Pinout & Wiring]]
- [[01-hardware/board-support|Board Variants]]
- [[05-dragon-os/hackrf-ops|HackRF Operations]]

### Firmware Developer
- [[02-firmware/architecture|Architecture Overview]]
- [[02-firmware/cc1101-driver|CC1101 Driver API]]
- [[02-firmware/sump-capture|SUMP Capture Internals]]
- [[07-workflow/development|Development Workflow]]
- [[07-workflow/debugging|Debugging Guide]]

### RF Researcher / Reverse Engineer
- [[03-applications/main-capture|Async RX Capture (Sniffing)]]
- [[04-host-scripts/index|Host Scripts]]
- [[05-dragon-os/coordinated|Coordinated ESP32+HackRF Workflows]]
- [[05-dragon-os/ssh-setup|Dragon OS SSH Setup]]

### DevOps / CI/CD
- [[06-build-config/kconfig|Kconfig Reference]]
- [[06-build-config/menuconfig-guide|Menuconfig Presets]]
- [[07-workflow/testing|Testing Workflows]]

### AI Agent / Automated Tool
- [[08-reference/ai-index|AI-Readable Project Index]]
- [[08-reference/protocol|Protocol Specifications]]
- [[08-reference/register-map|Register Map]]

---

## 🚀 Quick Start Links

| Task | Command |
|------|---------|
| **Build & Flash Default (Async RX)** | `idf.py build flash monitor` |
| **Build 2FSK Beacon TX** | `idf.py build -DSUMP_APP=RFuzz_TX flash monitor` |
| **Build Packet RX** | `idf.py build -DSUMP_APP=RFuzz_RX flash monitor` |
| **Build TX Example** | `cp examples/rfuzz_tx_2fsk.c main/main.c && idf.py build flash monitor` |
| **Capture (raw + .sr + .vcd)** | `python scripts/capture_custom.py -p COM7 -r 24000 -n 100000` |
| **Capture @ 100 kHz (GTKWave)** | `python scripts/capture_custom.py -p COM7 -r 100000 -n 100000` (open the `.vcd`) |
| **Live Stream + Decode** | `python scripts/sniff.py stream --port COM7 --rate 250000 --seconds 5` |
| **Receive + Decode** | `python scripts/sniff.py receive --port COM7 --rate 250000 --samples 262144` |
| **Analyze a saved capture** | `python scripts/sniff.py analyze sniff_capture.sr --probe gdo2` |
| **Dragon OS SSH** | `ssh dragon@192.168.1.101` |
| **HackRF RX** | `ssh dragon 'hackrf_transfer -r cap.c8 -f 433920000 -s 2000000 -n 4000000'` |
| **HackRF Sweep** | `ssh dragon 'hackrf_sweep -f 430:440 -w 100000 -1' > sweep.csv` |

---

## 🔑 Key Concepts

| Concept | Description |
|---------|-------------|
| **Async RX** | Transparent demodulation: no sync, no packet layer, raw bits on GDO0 |
| **SUMP Capture** | On-chip logic analyzer sampling GDO0/GDO2 at up to 250 kS/s |
| **USB Transport** | USB Serial/JTAG for SUMP (COM7), UART0 for console (COM6) — no DTR reset |
| **Dragon OS** | Ubuntu-based SDR distro on HackRF One at `dragon@192.168.1.101` |
| **Coordinated Workflows** | ESP32 narrowband + HackRF wideband for validation, replay, fuzzing |

---

## 📖 Reading Order Suggestions

### New to Project
1. [[00-overview/index|Project Overview]]
2. [[01-hardware/pinout|Hardware Pinout]]
3. [[03-applications/main-capture|Default Capture Firmware]]
4. [[04-host-scripts/index|Host Scripts]]
5. [[06-build-config/menuconfig-guide|Menuconfig Guide]]

### Developing Firmware
1. [[02-firmware/architecture|Architecture]]
2. [[02-firmware/cc1101-driver|CC1101 Driver]]
3. [[07-workflow/development|Development Workflow]]
4. [[07-workflow/debugging|Debugging]]

### RF Analysis
1. [[03-applications/main-capture|Async Capture]]
2. [[04-host-scripts/index|Host Scripts]]
3. [[05-dragon-os/ssh-setup|Dragon OS]]
4. [[05-dragon-os/coordinated|Coordinated Workflows]]

---

## 🏷️ Tags for Filtering

| Tag | Pages |
|-----|-------|
| `#hardware` | pinout, board-support |
| `#firmware` | architecture, cc1101-driver, sump-capture |
| `#tx` | tx-beacon, examples |
| `#rx` | main-capture, rx-packet |
| `#host` | index (host-scripts) |
| `#dragon` | ssh-setup, hackrf-ops, coordinated |
| `#build` | kconfig, menuconfig-guide |
| `#workflow` | development, testing, debugging |
| `#reference` | register-map, protocol, troubleshooting, ai-index |

---

## 🔗 External Resources

- **ESP-IDF Programming Guide**: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/
- **CC1101 Datasheet**: https://www.ti.com/product/CC1101
- **SUMP Protocol**: http://www.sump.org/projects/analyzer/protocol/
- **Sigrok PulseView**: https://sigrok.org/wiki/PulseView
- **Dragon OS**: https://dragonos.io/
- **HackRF One**: https://greatscottgadgets.com/hackrfone/

---

## 📝 Contributing to Docs

1. Edit `.md` files in `docs/rfuzz-vault/`
2. Follow existing structure and formatting
3. Update this index if adding new pages
4. Keep AI-readable YAML blocks at end of each file

---

*Last updated: 2026-08-02 | Vault version: 1.0.0*