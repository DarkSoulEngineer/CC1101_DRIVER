# Host Scripts Overview

> **Source**: `scripts/`

---

## Scripts Summary

| Script | Purpose | Protocol | Output |
|--------|---------|----------|--------|
| `rfuzz_tools.py` | 2FSK signal gen + GDO2 decode/regen/clean/manual | — | `.c8`, packets |
| `capture_custom.py` | GDO0/GDO2 capture + FSK analog ch3 | Custom (0x01) | `.raw`, `.sr`, `.vcd` |
| `rfuzz_testcase.py` | End-to-end 2FSK test case (hardware or `--offline`) | Custom + ssh | all of the above |
| `sniff.py` | Analyze/receive/stream captures, offline selftest | Custom (0x01/0x03) | decoded packets, `.sr` |

---

## Common Requirements

```bash
pip install pyserial
# Optional for analysis features:
pip install numpy
```

**Default Ports** (Windows):
- **COM7**: USB Serial/JTAG (capture transport)
- **COM6**: UART0 (ESP-IDF console)

**Default Baud**: 115200 (all scripts)

---

## rfuzz_tools.py — Signal Gen, Decode & Regen

**Source**: `scripts/rfuzz_tools.py`

### Purpose
`gen`: generates the on-antenna 2FSK test signal (absorbed from the deleted `gen_2fsk.py`). `decode`: turns a `.raw` GDO2 capture into cleaned, quantized bits and locates packets (preamble 0xAA×4 + sync 0xDEAF + 4-byte payload, MSB-first). `regen`: re-modulates the decoded packets to 2FSK I/Q `.c8` (full + I-only) for URH comparison. `manual`: decodes a packet bit-by-bit directly from raw samples (majority vote per cell) with full sample-range table for hand verification against the GDO2 waveform. `clean`: diagnostic error-correction pipeline (glitch filter, edge resync, majority vote, noise gate).

### Usage
```bash
# Generate 2FSK test signal (absorbed from gen_2fsk.py)
python rfuzz_tools.py gen OUT.c8 [--repeat N] [--gap-ms MS] [--preamble B] [--payload HEX] [--baud B] [--dev D]
python rfuzz_tools.py gen rfuzz_2fsk_gap.c8 --repeat 60 --gap-ms 50
python rfuzz_tools.py gen rfuzz_2fsk_gap.c8 --repeat 100  # continuous

# Decode a capture
python rfuzz_tools.py decode --raw cap.raw --rate 250000

# Regenerate decoded packets to 2FSK I/Q for URH
python rfuzz_tools.py regen --raw cap.raw --rate 250000 --out cap_regen.c8

# Manual bit-by-bit decode for hand verification
python rfuzz_tools.py manual --raw cap.raw --rate 250000

# Clean/diagnostics
python rfuzz_tools.py clean --raw cap.raw --rate 250000
```

### gen Arguments
| Arg | Default | Description |
|-----|---------|-------------|
| `OUT.c8` | (required) | output file |
| `--repeat N` | `100` | number of packets |
| `--gap-ms MS` | `0` | idle silence between packets (ms) |
| `--preamble B` | `4` | preamble bytes (0xAA each) |
| `--payload HEX` | `01020304` | payload bytes (hex) |
| `--baud B` | `2400` | symbol rate (bps) |
| `--dev D` | `50000` | FSK deviation (Hz) |

### decode/regen/manual/clean Arguments
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

## rfuzz_testcase.py — End-to-End 2FSK Test Case

**Source**: `scripts/rfuzz_testcase.py`

### Purpose
Reproduces the whole bench validation in one command: generate the gapped 2FSK signal, deploy it to the Dragon OS VM (HackRF One), verify CC1101 packet RX on COM7, capture GDO0/GDO2, decode the capture, and regenerate the signal for URH. See [[04-host-scripts/rfuzz-testcase]].

### Usage
```bash
python rfuzz_testcase.py                       # defaults: 60 pkts/50ms gap, -x 26 -a 0
python rfuzz_testcase.py --out-dir C:\tmp\t
python rfuzz_testcase.py --packets 100 --gap-ms 0
python rfuzz_testcase.py --skip packets        # skip PASS 1
python rfuzz_testcase.py --offline             # synthetic captures, no hardware
python rfuzz_testcase.py --demo                # synthetic captures, no hardware
```

### What it does
```
GENERATE -> DEPLOY (scp) -> PASS 1 packet RX verify -> PASS 2 capture
        -> DECODE (rfuzz_tools) -> REGEN (full I/Q + I-only)
```

### Output
`rfuzz_2fsk_gap.c8`, `rfuzz_testcase.raw/.sr/.vcd`, `rfuzz_testcase_regen.c8`, `rfuzz_testcase_regen_i.c8`

---

## sniff.py — Capture, Analyze & Self-Test

**Source**: `scripts/sniff.py`

### Purpose
Unified capture and analysis tool (absorbed from the deleted `sniff_decoder.py` and `sniff_test.py`). `analyze` decodes a saved `.raw` or `.sr` capture. `receive` captures from firmware on COM7 then analyzes. `stream` continuously streams from firmware then analyzes. `selftest` runs an offline regression self-test.

### Usage
```bash
# Analyze a saved capture
python sniff.py analyze FILE [--rate R] [--baud B] [--sync HEX] [--max-bytes N]

# Capture from firmware then analyze
python sniff.py receive --port COM7 [--rate R] [--samples N] [--baud B] [--sync HEX] [--out BASE] [--no-strict]

# Continuous stream then analyze
python sniff.py stream --port COM7 [--rate R] [--seconds S] [--baud B] [--sync HEX] [--out BASE]

# Offline regression self-test
python sniff.py selftest
```

### Arguments
| Arg | Default | Description |
|-----|---------|-------------|
| `--port` | `COM7` | Serial port (receive/stream) |
| `--rate R` | `250000` | Sample rate (Hz) |
| `--baud B` | `2400` | Symbol rate (bps) |
| `--sync HEX` | `DEAF` | Sync word (hex) |
| `--samples N` | `262144` | Sample count (receive) |
| `--seconds S` | `10` | Stream duration (stream) |
| `--out BASE` | Auto | Output file base name |
| `--max-bytes N` | `64` | Max payload bytes to decode |
| `--no-strict` | False | Relax decode validation |

---

## Choosing the Right Script

| Need | Script |
|------|--------|
| Reproduce the full 2FSK bench test (hardware) | `rfuzz_testcase.py` |
| Reproduce the full 2FSK bench test (no hardware) | `rfuzz_testcase.py --offline` |
| Generate the 2FSK test signal | `rfuzz_tools.py gen` |
| Decode/regen a captured signal | `rfuzz_tools.py` |
| Capture GDO0/GDO2 + FSK analog | `capture_custom.py` |
| Analyze a saved .raw or .sr | `sniff.py analyze` |
| Capture from firmware then analyze | `sniff.py receive` |
| Continuous stream then analyze | `sniff.py stream` |
| Offline regression self-test | `sniff.py selftest` |

---

## AI-Readable Script Index

```yaml
host_scripts:
  - name: "rfuzz_tools.py"
    subcommands: ["gen", "decode", "clean", "regen", "manual"]
    outputs: [".c8", "packets"]
    features: ["2fsk_gen", "phase_continuous", "packet_gaps", "glitch_filter", "run_length_decode", "urh_regen"]
    defaults: {repeat: 100, gap_ms: 0, preamble: 4, payload: "01020304", baud: 2400, dev: 50000}

  - name: "capture_custom.py"
    protocol: "custom_raw"
    commands: [0x01]
    outputs: [".raw", ".sr", ".vcd"]
    features: ["finite_capture", "analog_ch3", "mod_2fsk", "mod_2psk", "mod_ask", "stats"]
    default_port: "COM7"
    default_rate: 250000
    default_samples: 262144

  - name: "rfuzz_testcase.py"
    protocol: "custom + ssh"
    phases: ["generate", "deploy", "packet_rx_verify", "capture", "decode", "regen"]
    outputs: [".c8", ".raw", ".sr", ".vcd"]
    features: ["offline_mode", "demo_mode", "hardware_mode"]
    default_host: "dragon@192.168.1.101"
    default_port: "COM7"
    default_rate: 250000
    default_samples: 262144

  - name: "sniff.py"
    subcommands: ["analyze", "receive", "stream", "selftest"]
    outputs: ["packets", ".sr"]
    features: ["offline_selftest", "firmware_capture", "continuous_stream", "decode"]
    default_port: "COM7"
    default_rate: 250000
    default_baud: 2400
```

---

## Related

- [[03-applications/main-capture|Default Capture Firmware]]
- [[02-firmware/sump-capture|SUMP Capture Internals]]
- [[04-host-scripts/rfuzz-testcase|2FSK End-to-End Test Case]]
- [[05-dragon-os/ssh-setup|Dragon OS SSH Setup]]
