# Host Scripts Overview

> **Source**: `scripts/`

---

## Scripts Summary

| Script | Purpose | Protocol | Output |
|--------|---------|----------|--------|
| `capture.py` | Simple raw streaming capture | Custom (0x01/0x03) | `.sr` (Sigrok) |
| `capture_sump.py` | Full SUMP/OLS capture | SUMP/OLS | `.sr`, `.vcd`, `.bin`, `.hex` |
| `capture_custom.py` | GDO0/GDO2 capture + FSK analog ch3 | Custom (0x01) | `.raw`, `.sr`, `.vcd` |
| `gen_2fsk.py` | 2FSK test-signal generator (gaps) | — | `.c8` (int8 I/Q) |
| `rfuzz_tools.py` | Decode / regenerate GDO2 captures | — | packets, `.c8` |
| `rfuzz_testcase.py` | End-to-end 2FSK test case (1-shot) | Custom + ssh | all of the above |
| `sump_stream_capture.py` | Continuous stream test | Custom stream | Console debug |
| `tx_verify.py` | Dual-port TX verification | Custom | Console log |
| `rssi_mon.py` | RSSI logging from UART | UART console | Text log |
| `capture_gdo.py` | Quick GDO stream test | Custom stream | Console debug |
| `switch_transport.py` | (Module) Transport abstraction | — | Library |

---

## Common Requirements

```bash
pip install pyserial
# Optional for capture_sump.py:
pip install numpy  # For some analysis features
```

**Default Ports** (Windows):
- **COM7**: USB Serial/JTAG (SUMP transport)
- **COM6**: UART0 (ESP-IDF console)

**Default Baud**: 115200 (all scripts)

---

## capture.py — Raw Streaming Capture

**Source**: `scripts/capture.py`

### Purpose
Simple triggered capture using custom protocol (`0x01` start, `0x03` stream). Outputs Sigrok `.sr` session files.

### Usage
```bash
python capture.py                          # COM7, 24kHz, 100k samples
python capture.py --port COM7
python capture.py --rate 100000 -n 500000
python capture.py --channels 1
python capture.py --stream --seconds 10    # Continuous stream
```

### Arguments
| Arg | Default | Description |
|-----|---------|-------------|
| `--port`, `-p` | `COM7` | Serial port |
| `--rate`, `-r` | `24000` | Sample rate (Hz) |
| `--samples`, `-n` | `100000` | Sample count |
| `--channels`, `-c` | `2` | 1 or 2 channels |
| `--output`, `-o` | Auto | Output `.sr` file |
| `--timeout`, `-t` | Auto | Capture timeout (sec) |
| `--stream` | False | Continuous stream mode |
| `--seconds`, `-s` | `10.0` | Stream duration (sec) |

### Protocol
```
Host → ESP32: [0x01][rate:4 LE][count:4 LE] (9 bytes)
ESP32 → Host: count raw bytes (1 byte/sample, bit0=GDO0, bit1=GDO2)
```

### Stream Mode
```
Host → ESP32: [0x03][rate:4 LE] (5 bytes)
ESP32 → Host: Continuous raw bytes
Host → ESP32: [0x04] (stop)
```

### Output Format (`.sr` — Sigrok Session)
```
capture_20240101_120000.sr/
├── version          # "2"
├── metadata         # INI: capturefile logic-1, probes GDO0/GDO2, analog3 FSK
├── logic-1-1        # Packed logic (1 byte = 1 timestep: bit0 GDO0, bit1 GDO2)
└── analog-1-3       # FSK sinusoid, float32 (1 sample = 1 timestep)
```

### Statistics Printed
```
Ch0: 50123/100000 high (50.1%), 2400 transitions
```

---

## capture_sump.py — SUMP/OLS Protocol Capture

**Source**: `scripts/capture_sump.py`

### Purpose
Full SUMP/OLS protocol implementation for PulseView/Sigrok compatibility. Supports device metadata, trigger config, multiple output formats.

### Usage
```bash
python capture_sump.py                         # COM7, 24kHz, 2ch
python capture_sump.py --port COM7             # Custom port (USB mode)
python capture_sump.py --samples 500000        # 500k samples
python capture_sump.py --rate 100000           # 100 kHz sample rate
python capture_sump.py --channels 1            # Single channel
python capture_sump.py --format vcd            # Save as VCD (GTKWave)
python capture_sump.py --format sr             # Save as Sigrok .sr
python capture_sump.py --output capture.bin    # Save raw bits
python capture_sump.py --selftest              # CC1101 self-test (TX 0x55)
```

### Arguments
| Arg | Default | Description |
|-----|---------|-------------|
| `--port`, `-p` | `COM7` | Serial port |
| `--baud`, `-b` | `115200` | Baud rate |
| `--rate`, `-r` | `24000` | Sample rate (Hz) |
| `--samples`, `-n` | `100000` | Sample count |
| `--channels`, `-c` | `2` | 1 or 2 |
| `--clock` | `240000000` | Ref clock (Hz) |
| `--output`, `-o` | Auto | Output file |
| `--format`, `-f` | `sr` | `vcd`, `bin`, `hex`, `sr` |
| `--selftest` | False | Trigger CC1101 TX 0x55 pattern |

### SUMP/OLS Protocol
```
Commands (Host → Device):
  0x00: RESET
  0x01: RUN (start capture)
  0x02: ID (identify)
  0x03: SELFTEST
  0x80: SET_DIV (3 bytes, little-endian)
  0x81: SET_COUNT (2 bytes read count + 2 bytes delay)
  0x82: SET_FLAGS
  0xC0-0xC2: Trigger config

Device → Host:
  ID: 4 bytes "1ALS" (OLS) or "SUMP"
  Metadata: TLV format (name, version, channels, memory, max_rate, protocol)
  Capture data: 4-byte blocks (ch0[8], ch1[8], ch2[8], ch3[8] bits)
```

### Output Formats

| Format | Extension | Use Case |
|--------|-----------|----------|
| `.sr` (Sigrok) | `.sr` | PulseView, Sigrok-cli |
| `.vcd` (VCD) | `.vcd` | GTKWave, Verilog simulators |
| `.bin` | `.bin` | Raw bit-packed, custom analysis |
| `.hex` | `.hex` | Human-readable hex dump |

### Device Metadata (Auto-detected)
```
[+] Device identified: '1ALS'
[+] Device name: CC1101 SUMP Logic Analyzer
[+] Version: 1.0
[+] Channels: 2
[+] Memory: 100000 bytes
[+] Max sample rate: 500000 Hz
[+] Protocol version: 1
```

### Capture Statistics
```
[*] Capture statistics:
    Samples:     100000
    Duration:    4166.67 ms
    Sample rate: 24000 Hz
    Ch0 (GDO0):  high=50123 (50.1%) low=49877 (49.9%)
    Transitions: 2400
    Avg period:  1736.1 us (576 Hz)
    Ch1 (GDO2):  high=123 (0.1%) low=99877 (99.9%)
    Ch1 trans:   2
```

---

## sump_stream_capture.py — Continuous Stream Test

**Source**: `scripts/sump_stream_capture.py`

### Purpose
Simple continuous streaming test using `0x03` stream command. Prints first 64 samples as binary.

### Usage
```bash
python sump_stream_capture.py
# Streams for 10 seconds at 24 kHz
```

### Output
```
Starting stream at 24000 Hz: 03405e0000
Streaming for 10 seconds... (Ctrl+C to stop)
  0x00 = 00000000
  0x01 = 00000001
  ...
Total samples captured: 240000
```

---

## tx_verify.py — Dual-Port TX Verification

**Source**: `scripts/tx_verify.py`

### Purpose
Verifies TX by using two serial ports:
- **COM7**: Command port (sends TX trigger `0x02`)
- **COM6**: Console port (reads ESP32 logs)

### Usage
```bash
python tx_verify.py
```

### Output
```
>>> sending TX command
[LOG OUTPUT FROM ESP32 CONSOLE]
```

### Requirements
- ESP32 firmware must have `capture_set_tx_cb()` registered
- COM7 = USB Serial/JTAG (command)
- COM6 = UART0 (console)

---

## rssi_mon.py — RSSI Logging

**Source**: `scripts/rssi_mon.py`

### Purpose
Logs RSSI output from ESP32 console (UART0) to file with timestamps.

### Usage
```bash
python rssi_mon.py 20 rssi_log.txt    # 20 seconds, output to rssi_log.txt
python rssi_mon.py                    # 20 seconds, default rssi_log.txt
```

### Output (`rssi_log.txt`)
```
12:34:56.123456 MARCSTATE=0x0D PKTSTATUS=0x00 RSSI=-74 dBm RXBYTES=0
12:34:56.623456 MARCSTATE=0x0D PKTSTATUS=0x00 RSSI=-73 dBm RXBYTES=0
...
```

---

## capture_gdo.py — Quick GDO Stream Test

**Source**: `scripts/capture_gdo.py`

### Purpose
Quick test of GDO streaming — captures 15 seconds at 24 kHz, prints first 64 samples with transition count.

### Usage
```bash
python capture_gdo.py
```

### Output
```
Starting stream: 03405e0000
Capturing for 15 seconds...
  3456 samples... (last: 0x01=00000001)
  ...
=== CAPTURE COMPLETE ===
Total samples: 360000
First 64 samples:
  [  0] 0x00 GDO0=0 GDO2=0
  [  1] 0x01 GDO0=1 GDO2=0
  ...
Transitions in first 64: 12
```

---

## switch_transport.py — Transport Abstraction

**Source**: `scripts/switch_transport.py`

### Purpose
Internal module for transport switching (not directly used). Contains transport abstraction for future USB/UART switching.

---

## rfuzz_testcase.py — End-to-End 2FSK Test Case (1-shot)

**Source**: `scripts/rfuzz_testcase.py`

### Purpose
Reproduces the whole bench validation in one command: generate the gapped 2FSK signal, deploy it to the Dragon OS VM (HackRF One), verify CC1101 packet RX on COM7, capture GDO0/GDO2, decode the capture, and regenerate the signal for URH. See [[04-host-scripts/rfuzz-testcase]].

### Usage
```bash
python rfuzz_testcase.py                       # defaults: 60 pkts/50ms gap, -x 26 -a 0
python rfuzz_testcase.py --out-dir C:\tmp\t
python rfuzz_testcase.py --packets 100 --gap-ms 0
python rfuzz_testcase.py --skip packets        # skip PASS 1
```

### What it does
```
GENERATE -> DEPLOY (scp) -> PASS 1 packet RX verify -> PASS 2 capture
        -> DECODE (rfuzz_tools) -> REGEN (full I/Q + I-only)
```

### Output
`rfuzz_2fsk_gap.c8`, `rfuzz_testcase.raw/.sr/.vcd`, `rfuzz_testcase_regen.c8`, `rfuzz_testcase_regen_i.c8`

---

## capture_custom.py — GDO0/GDO2 Capture + FSK Channel 3

**Source**: `scripts/capture_custom.py`

### Purpose
Finite capture (`0x01`) over the custom protocol. Outputs `.raw` plus a Sigrok `.sr` for PulseView with **3 channels**: `GDO0` + `GDO2` (logic), analog ch3 (synthesized) — a phase-continuous modulation sinusoid rendered from the decoded bits. Default `--mod 2fsk` shows the **true ±50 kHz deviation** as two distinct positive tones (`IF ± dev` with `IF` = 22.5% of the real rate when the deviation fits the band, else the old scaled fit `rate/10 ± rate/20`); `--mod 2psk` / `--mod ask` render carrier phase/amplitude instead. Overrides via `--fsk-if` / `--fsk-dev`, amplitude via `--analog-amp`. Also writes `.vcd` (analog as real channel).

### Usage
```bash
python capture_custom.py                                # COM7, 250kHz, 262144 samples
python capture_custom.py --rate 24000 --samples 100000 --out cap
python capture_custom.py --fsk-if 56250 --fsk-dev 50000 --analog-amp 90
python capture_custom.py --mod 2psk --fsk-if 100000     # PSK carrier render
```

---

## gen_2fsk.py — 2FSK Test-Signal Generator

**Source**: `scripts/gen_2fsk.py`

### Purpose
Generates the on-antenna test signal: `0xAA x4 + 0xDEAF + 01 02 03 04`, MSB-first, 2FSK ±50 kHz, phase-continuous, int8 I/Q (2.4 MS/s). 3rd argument inserts idle silence between packets (keeps the CC1101 FIFO from overflowing).

### Usage
```bash
python gen_2fsk.py out.c8 60 50     # 60 packets, 50 ms gap
python gen_2fsk.py out.c8 100       # 100 packets, continuous
```

---

## rfuzz_tools.py — Decode / Regenerate Captures

**Source**: `scripts/rfuzz_tools.py`

### Purpose
`decode`: turns a `.raw` GDO2 capture into cleaned, quantized bits and locates packets (preamble 0xAA×4 + sync 0xDEAF + 4-byte payload, MSB-first). `regen`: re-modulates the decoded packets to 2FSK I/Q `.c8` (full + I-only) for URH comparison. `manual`: decodes a packet bit-by-bit directly from raw samples (majority vote per cell) with full sample-range table for hand verification against the GDO2 waveform.

### Usage
```bash
python rfuzz_tools.py decode --raw cap.raw --rate 250000          # auto-decode packets
python rfuzz_tools.py regen  --raw cap.raw --rate 250000 --out cap_regen.c8
python rfuzz_tools.py manual --raw cap.raw --rate 250000         # per-bit table for hand verification
```

### Arguments
| Arg | Default | Description |
|-----|---------|-------------|
| `--raw` | (required) | capture `.raw` file |
| `--rate`, `-r` | `250000` | capture sample rate |
| `--spb` | auto | samples per bit (default: `rate / 2400`) |
| `--glitch` | `10` | glitch run threshold in samples |
| `--out` | — | output `.c8` (regen only) |
| `--amp` | `90` | regeneration amplitude |
| `--dev` | `50000` | FSK deviation for regen |

---

## Choosing the Right Script

| Need | Script |
|------|--------|
| Reproduce the full 2FSK bench test | `rfuzz_testcase.py` |
| Decode/regen a captured signal | `rfuzz_tools.py` |
| Capture with FSK analog channel 3 | `capture_custom.py` |
| Generate the 2FSK test signal | `gen_2fsk.py` |
| Quick capture, Sigrok format | `capture.py` |
| PulseView compatible, full features | `capture_sump.py` |
| GTKWave (VCD) | `capture_sump.py --format vcd` |
| Raw binary for custom analysis | `capture_sump.py --format bin` |
| Continuous stream debug | `sump_stream_capture.py` or `capture_gdo.py` |
| TX verification with console logs | `tx_verify.py` |
| Long-term RSSI monitoring | `rssi_mon.py` |

---

## AI-Readable Script Index

```yaml
host_scripts:
  - name: "rfuzz_testcase.py"
    protocol: "custom + ssh"
    phases: ["generate", "deploy", "packet_rx_verify", "capture", "decode", "regen"]
    outputs: [".c8", ".raw", ".sr", ".vcd"]
    default_host: "dragon@192.168.1.101"
    default_port: "COM7"
    default_rate: 250000
    default_samples: 262144

  - name: "capture_custom.py"
    protocol: "custom_raw"
    commands: [0x01]
    outputs: [".raw", ".sr", ".vcd"]
    features: ["finite_capture", "analog_ch3", "mod_2fsk", "mod_2psk", "mod_ask", "stats"]
    default_port: "COM7"
    default_rate: 250000
    default_samples: 262144

  - name: "gen_2fsk.py"
    protocol: "signal_generator"
    outputs: [".c8"]
    features: ["2fsk", "phase_continuous", "packet_gaps"]
    default: "60 packets, 50 ms gap"

  - name: "rfuzz_tools.py"
    protocol: "offline_analysis"
    subcommands: ["decode", "regen"]
    outputs: ["packets", ".c8"]
    features: ["glitch_filter", "run_length_decode", "urh_regen"]

  - name: "capture.py"
    protocol: "custom_raw"
    commands: [0x01, 0x03, 0x04]
    outputs: [".sr"]
    features: ["triggered_capture", "continuous_stream", "auto_timeout"]
    default_port: "COM7"
    default_rate: 24000
    default_samples: 100000
  
  - name: "capture_sump.py"
    protocol: "SUMP/OLS"
    commands: [0x00, 0x01, 0x02, 0x03, 0x80, 0x81, 0x82, 0xC0-0xC2]
    outputs: [".sr", ".vcd", ".bin", ".hex"]
    features: ["metadata", "trigger_config", "selftest", "multi_format", "stats"]
    default_port: "COM7"
    default_baud: 115200
    default_rate: 24000
    default_samples: 100000
  
  - name: "sump_stream_capture.py"
    protocol: "custom_stream"
    commands: [0x03, 0x04]
    outputs: ["console"]
    features: ["live_binary_print"]
  
  - name: "tx_verify.py"
    protocol: "dual_port"
    ports: ["COM7(cmd)", "COM6(console)"]
    commands: [0x02]
    features: ["tx_trigger", "console_capture"]
  
  - name: "rssi_mon.py"
    protocol: "uart_console"
    port: "COM6"
    outputs: ["text_log"]
    features: ["timestamped_rssi"]
  
  - name: "capture_gdo.py"
    protocol: "custom_stream"
    commands: [0x03, 0x04]
    outputs: ["console", "transition_count"]
    features: ["quick_test", "binary_print"]
```

---

## Related

- [[03-applications/main-capture|Default Capture Firmware]]
- [[02-firmware/sump-capture|SUMP Capture Internals]]
- [[04-host-scripts/capture_sump.py|SUMP Capture Script Detail]]
- [[04-host-scripts/rfuzz-testcase|2FSK End-to-End Test Case]]
- [[05-dragon-os/ssh-setup|Dragon OS SSH Setup]]