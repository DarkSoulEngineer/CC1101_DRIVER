# capture_sump.py — Detailed Reference

> **Source**: `scripts/capture_sump.py`

---

## Overview

The primary host tool for SUMP/OLS capture. Implements full OLS protocol subset for PulseView compatibility. Supports triggered capture, continuous streaming, frequency sweeps, and multiple output formats.

---

## Installation

```bash
pip install pyserial
# Optional:
pip install numpy  # Used in some analysis helpers
```

---

## Command Line Reference

```bash
python capture_sump.py [OPTIONS]

Options:
  -p, --port PORT         Serial port (default: COM7)
  -b, --baud BAUD         Baud rate (default: 115200)
  -r, --rate RATE         Sample rate in Hz (default: 24000)
  -n, --samples SAMPLES   Number of samples (default: 100000)
  -c, --channels CHANNELS Number of channels: 1 or 2 (default: 2)
  --clock CLOCK           Reference clock Hz (default: 240000000)
  -o, --output FILE       Output file (default: auto timestamp)
  -f, --format FORMAT     Output format: vcd, bin, hex, sr (default: sr)
  --selftest              Trigger CC1101 self-test (TX 0x55 pattern)
  -h, --help              Show help
```

---

## Transport Modes

### USB Serial/JTAG Mode (Recommended)
```bash
# Firmware: CONFIG_SUMP_TRANSPORT_USB=y
# Console: CONFIG_ESP_CONSOLE_UART_DEFAULT=y (UART0 = COM6)
# SUMP: USB JTAG = COM7

python capture_sump.py --port COM7 --rate 24000 --samples 100000
```
**Advantages**: No DTR reset, stable PulseView connection.

### UART0 Mode (Legacy)
```bash
# Firmware: CONFIG_SUMP_TRANSPORT_UART=y
# Console: CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y (USB JTAG = COM7)
# SUMP: UART0 = COM6 (baud must match CONFIG_SUMP_UART_BAUD)

python capture_sump.py --port COM6 --baud 921600 --rate 24000 --samples 100000
```
**Issues**: PulseView toggles DTR → ESP32 reset → boot log corrupts handshake.

---

## Capture Modes

### 1. Triggered Capture (Default)
```bash
python capture_sump.py --port COM7 --rate 24000 --samples 100000
```
- Sends `SET_DIV`, `SET_COUNT`, `SET_FLAGS`, trigger config
- Sends `RUN` (0x01)
- Waits for all samples, then streams back
- **Timeout**: Auto-scaled based on `samples/rate`

### 2. Continuous Stream
```bash
# Not directly supported in capture_sump.py
# Use sump_stream_capture.py or capture.py --stream
```

### 3. Frequency Sweep
```bash
# Requires firmware with sweep callback registered
# capture_set_freq_sweep_cb(cc1101_freq_sweep)
python capture_sump.py --port COM7 --selftest
# Actually triggers self-test (TX 0x55), not sweep
```
**Note**: Sweep command (0x05) not exposed in CLI. Use `cc1101_freq_sweep` directly from firmware or custom script.

### 4. Self-Test
```bash
python capture_sump.py --selftest
```
- Sends `SELFTEST` (0x03) with pattern `0x55`
- CC1101 transmits 0x55 pattern on GDO0
- Captures resulting signal

---

## Output Formats

### 1. Sigrok `.sr` (Default, Recommended)
```bash
python capture_sump.py --format sr --output capture.sr
```
**Structure**:
```
capture.sr (zip)
├── version          # "2"
├── metadata         # INI: [global], [device 1], probes, samplerate
└── logic-1-1        # Packed: 1 byte/timestep, bit0=ch0, bit1=ch1
```
**Open in PulseView**: File → Open → select `.sr` file.

### 2. VCD (GTKWave)
```bash
python capture_sump.py --format vcd --output capture.vcd
```
**Signals**: `GDO0` (!), `GDO2` (@)
**Open in GTKWave**: File → Open → select `.vcd` file.

### 3. Raw Binary `.bin`
```bash
python capture_sump.py --format bin --output capture.bin
```
**Format**: Ch0 bits packed (8 samples/byte), then Ch1 bits packed (if 2ch).

### 4. Hex Text `.hex`
```bash
python capture_sump.py --format hex --output capture.hex
```
**Format**: Human-readable, 4 samples per hex nibble, 32 nibbles per line.

---

## PulseView Integration

### Open Capture
1. Launch PulseView
2. File → Open → select `capture_YYYYMMDD_HHMMSS.sr`
3. Add protocol decoder:
   - Right-click signal → Add Decoder
   - UART, Manchester, NRZ, etc.
   - Set baud rate (e.g., 2400)

### Common Decoders for CC1101
| Signal Type | Decoder | Settings |
|-------------|---------|----------|
| Async 2FSK | UART | Baud=2400, Data bits=8, Parity=None |
| Manchester | Manchester | Bit rate=2400 |
| Packet (sync) | Custom | Use sync word 0xDEAF |

---

## Troubleshooting

| Issue | Cause | Fix |
|-------|-------|-----|
| "No SUMP device found" | Firmware not in SUMP mode | Flash main.c (capture firmware), not TX/RX |
| "Cannot open COM7" | Wrong port | Check Device Manager, try COM6 |
| Garbage ID response | UART mode + DTR reset | Use USB mode, or add 10uF on EN-GND |
| Capture timeout | Rate too low for sample count | Increase timeout or reduce samples |
| "Bad stream rate" | Rate > 500000 | Max is 500 kHz |
| PulseView shows no signals | Wrong format/decoder | Check `.sr` metadata, add correct decoder |

---

## Advanced Usage

### Custom Sample Rate Calculation
```python
# Divider = clock_freq / sample_rate - 1
# OLS protocol: divider is 24-bit (3 bytes LE)
# Firmware multiplies read_count by 4
# Max read_count = 0xFFFF → 262144 samples max per capture
```

### Large Captures (>262k samples)
```bash
# Not supported in single OLS capture
# Options:
# 1. Multiple captures, stitch in post
# 2. Use continuous stream mode (capture.py --stream)
# 3. Increase CONFIG_SUMP_MAX_SAMPLES (requires PSRAM)
```

### Automated Capture Script
```python
#!/usr/bin/env python3
import subprocess, sys

def capture(port, rate, samples, output):
    cmd = [
        sys.executable, "capture_sump.py",
        "--port", port,
        "--rate", str(rate),
        "--samples", str(samples),
        "--format", "sr",
        "--output", output
    ]
    subprocess.run(cmd, check=True)

if __name__ == "__main__":
    capture("COM7", 24000, 100000, "test.sr")
```

---

## AI-Readable Spec

```yaml
capture_sump_py:
  protocol: "SUMP/OLS subset"
  transports: ["USB Serial/JTAG", "UART0"]
  max_rate_hz: 500000
  max_samples_per_capture: 262144  # OLS protocol limit
  commands:
    - {byte: 0x00, name: "RESET"}
    - {byte: 0x01, name: "RUN"}
    - {byte: 0x02, name: "ID"}
    - {byte: 0x03, name: "SELFTEST"}
    - {byte: 0x80, name: "SET_DIV", args: "3 bytes LE"}
    - {byte: 0x81, name: "SET_COUNT", args: "2 bytes read_count + 2 bytes delay"}
    - {byte: 0x82, name: "SET_FLAGS", args: "1 byte"}
    - {byte: 0xC0, name: "TRIG_MASK0"}
    - {byte: 0xC1, name: "TRIG_VALUE0"}
    - {byte: 0xC2, name: "TRIG_CONFIG0"}
  metadata_tlv:
    0x01: "device_name"
    0x02: "version"
    0x21: "memory_bytes (u32 BE)"
    0x22: "max_sample_rate (u32 BE)"
    0x23: "protocol_version"
    0x40: "num_channels"
  output_formats:
    sr: "Sigrok session (zip: version, metadata, logic-1-1)"
    vcd: "Value Change Dump (GTKWave)"
    bin: "Packed bits (ch0 then ch1)"
    hex: "Text hex dump (4 samples/nibble)"
  default_port: "COM7"
  default_baud: 115200
  default_rate: 24000
  default_samples: 100000
  default_channels: 2
  default_format: "sr"
```

---

## Related

- [[04-host-scripts/index|Host Scripts Overview]]
- [[02-firmware/sump-capture|SUMP Capture Internals]]
- [[03-applications/main-capture|Default Capture Firmware]]
- [[05-dragon-os/hackrf-ops|HackRF Operations]]
- [[06-build-config/kconfig|Kconfig: SUMP_TRANSPORT]]