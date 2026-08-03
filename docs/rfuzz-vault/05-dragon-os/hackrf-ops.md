# HackRF Operations Reference

> **Source**: Dragon OS Focal on HackRF One
> **SSH**: `dragon@192.168.1.101`

---

## hackrf_transfer — Raw IQ Capture/Playback

### Capture (RX)
```bash
# Basic capture
hackrf_transfer -r output.c8 -f 433920000 -s 2000000 -n 4000000

# Options:
# -r FILE       : Receive to file (.c8 = 8-bit signed IQ)
# -f HZ         : Center frequency (Hz)
# -s SPS        : Sample rate (SPS, max 20M for HackRF One)
# -n SAMPLES    : Number of samples (I+Q pairs)
# -a AMP        : Enable amp (1=on, 0=off) - default 1
# -l LNA_GAIN   : LNA gain 0-40 dB (8 dB steps)
# -g VGA_GAIN   : VGA gain 0-62 dB (2 dB steps)
# -x TX_GAIN    : TX gain 0-47 dB (1 dB steps) - for TX only
```

**Examples**:
```bash
# 433 MHz, 2 MS/s, 2 seconds (4M samples)
hackrf_transfer -r cap_433.c8 -f 433920000 -s 2000000 -n 4000000

# 868 MHz, 10 MS/s, 5 seconds (50M samples)
hackrf_transfer -r cap_868.c8 -f 868300000 -s 10000000 -n 50000000

# High gain for weak signals
hackrf_transfer -r cap_weak.c8 -f 433920000 -s 2000000 -n 4000000 -l 32 -g 40

# Stream to stdout (pipe to local)
hackrf_transfer -r - -f 433920000 -s 2000000 -n 2000000 > local.c8
```

### Playback (TX)
```bash
# Basic playback
hackrf_transfer -t input.c8 -f 433920000 -s 2000000 -x 20

# Options:
# -t FILE       : Transmit from file
# -x GAIN       : TX VGA gain 0-47 dB
# -R            : Repeat file indefinitely
# -b            : Binary mode (raw IQ, no .c8 header)
```

**Examples**:
```bash
# Replay at +20 dB gain
hackrf_transfer -t cap_433.c8 -f 433920000 -s 2000000 -x 20

# Loop forever (beacon)
hackrf_transfer -t beacon.c8 -f 433920000 -s 2000000 -x 20 -R

# Maximum power (check regulations!)
hackrf_transfer -t signal.c8 -f 433920000 -s 2000000 -x 47
```

### Close-Range CC1101 Test Signal (verified settings)
When driving the ESP32-S3 CC1101 module on the bench (antennas a few cm apart):

```bash
# Single pass, no RF amp (-a 0), TX VGA 18-26 dB. NEVER use -a 1 here.
hackrf_transfer -t ~/rfuzz_2fsk.c8 -f 433920000 -s 2400000 -x 26 -a 0
```

**Rules (from bench testing):**
- **`-a 0` always** — do NOT enable the RF amp for TX; it overdrives the CC1101 LNA at cm distance (desensitizes/degrades the demod).
- **`-x` 18–26 max** — at `-x 20 -a 0` the CC1101 RSSI is ~-70…-83 dBm but the demodulator fails to lock (GDO2 shows no 1200 Hz preamble component). At `-x 26 -a 0` RSSI is ~-64…-70 dBm and the demod locks reliably. Use `-x 26` for reliable capture; 18–26 is the working band.
- **Single pass, not `-R`/loops** — transmit only for the duration of one capture. Do NOT run a continuous TX loop; a 5 s capture only needs the 10 s file played once.
- At `-x 30 -a 1` (max) the signal clips (avg power ~-3.1 dBfs measured at the DAC) and can overdrive the receiver.
- RSSI at the CC1101 console should read ≈-60…-70 dBm for reliable GDO2 demod; the noise floor is ≈-108…-112 dBm.

**Packet-mode TX:** for clean CC1101 packet reception (no FIFO overflow) use the *gapped* variant — packets with silent gaps between them (50 ms):
```bash
# ~/rfuzz_2fsk_gap.c8 = 60 packets of [AA x4 + DEAF + 01 02 03 04], 50 ms silence between packets (5 s)
hackrf_transfer -t ~/rfuzz_2fsk_gap.c8 -f 433920000 -s 2400000 -x 26 -a 0
```
Generated with `scripts/gen_2fsk.py out.c8 60 50` (3rd arg = gap ms, silent = zero samples).

**Decode / regenerate tools:** `scripts/rfuzz_tools.py` turns a CC1101 GDO2 capture (.raw from `scripts/capture_custom.py`) into packets and back into a 2FSK I/Q signal for URH comparison:
```bash
python scripts/rfuzz_tools.py decode --raw capture6.raw --rate 48000   # packets, sync hits, payload errors
python scripts/rfuzz_tools.py regen  --raw capture6.raw --rate 48000 --out capture6_regen.c8
# writes capture6_regen.c8 (full I/Q) + capture6_regen_i.c8 (I-only, Q=0), amp=90 dev=50kHz, matches gen_2fsk.py format
```
Note: the firmware timer alarm is `1000000/rate` (integer), so requesting 48000 Hz actually captures at 50000 Hz; the tool derives the real samples-per-bit automatically (20.833).

**One-shot end-to-end:** `scripts/rfuzz_testcase.py` runs generate → deploy → packet-RX verify → capture → decode → regen. See [[04-host-scripts/rfuzz-testcase]].

### File Format (.c8)
- **8-bit signed IQ interleaved**: I0, Q0, I1, Q1, ...
- **Range**: -128 to +127 (signed char)
- **No header** in standard hackrf_transfer output
- **Sample rate** and **frequency** not stored — must track separately

---

## hackrf_sweep — Spectrum Analyzer

### Single Sweep
```bash
hackrf_sweep -f 400:500 -w 100000 -1
# -f START:STOP : Frequency range (MHz)
# -w WIDTH      : FFT bin width (Hz)
# -1            : Single sweep (exit after one)
# -a AMP        : Enable amp
# -l LNA        : LNA gain
# -g VGA        : VGA gain
```

**Output** (CSV to stdout):
```
date, time, hz_low, hz_high, hz_step, num_samples, dB, dB, dB, ...
2024-01-01, 12:00:00, 400000000, 400100000, 100000, 100, -50.2, -49.8, ...
```

### Continuous Sweep
```bash
hackrf_sweep -f 400:500 -w 100000
# Streams CSV continuously (Ctrl+C to stop)
```

### Common Sweep Configs
```bash
# 433 MHz ISM band (430-440 MHz, 100 kHz bins)
hackrf_sweep -f 430:440 -w 100000

# 868 MHz SRD band (860-870 MHz)
hackrf_sweep -f 860:870 -w 100000

# 915 MHz ISM band (902-928 MHz)
hackrf_sweep -f 902:928 -w 100000

# Wideband survey (300-930 MHz, 1 MHz bins)
hackrf_sweep -f 300:930 -w 1000000

# High resolution (10 kHz bins, slower)
hackrf_sweep -f 433:434 -w 10000
```

---

## hackrf_info — Device Information

```bash
hackrf_info
```
**Output**:
```
Found HackRF board.
Board ID: HackRF One
Firmware: 2024.01.1 (API: 1.02)
Part ID: 0x00574746 0x00574746
Serial: 0x00000000 0x00000000 0x00000000 0x00000000
```

---

## hackrf_debug — Register/SPI Debug

```bash
# Read register
hackrf_debug --reg-read REG_ADDR

# Write register
hackrf_debug --reg-write REG_ADDR VALUE

# SPI register read
hackrf_debug --spi-read REG_ADDR

# SPI register write
hackrf_debug --spi-write REG_ADDR VALUE

# CPLD version
hackrf_debug --cpld-version
```

---

## Signal Analysis Tools

### inspectrum (GUI)
```bash
# On Dragon OS (requires X11 forwarding or VNC)
ssh -X dragon "inspectrum capture.c8"
```
**Features**: Waterfall, spectrum, demodulators (AM/FM/SSB), symbol extraction.

### URH (Universal Radio Hacker) (GUI)
```bash
# On Dragon OS
ssh -X dragon "urh"
```
**Features**: Protocol analysis, demodulation, decoding, replay.

### GNU Radio Companion (GRC)
```bash
# On Dragon OS
ssh -X dragon "gnuradio-companion"
```

---

## Gain Staging

| Stage | Range | Step | Purpose |
|-------|-------|------|---------|
| **LNA** | 0-40 dB | 8 dB | RF amplifier (near antenna) |
| **VGA** | 0-62 dB | 2 dB | Baseband gain (after mixer) |
| **TX VGA** | 0-47 dB | 1 dB | Transmit gain |

**Typical Settings**:
| Scenario | LNA | VGA | TX |
|----------|-----|-----|----|
| Strong local signal | 8-16 | 20-30 | — |
| Weak signal | 32-40 | 40-62 | — |
| TX (near) | — | — | 10-20 |
| TX (far) | — | — | 30-47 |

**Total RX Gain** = LNA + VGA (max ~102 dB)

---

## Sample Rate Limits

| Rate | Use Case | Notes |
|------|----------|-------|
| 2 MS/s | Narrowband (433/868/915) | Default, good for most |
| 4 MS/s | Wider bandwidth |  |
| 8 MS/s | Wideband |  |
| 10 MS/s | Max for HackRF One | USB 2.0 limit |
| 20 MS/s | HackRF One (theoretical) | Often unstable |

**Recommended**: 2 MS/s for sub-GHz work (covers ~2 MHz bandwidth)

---

## Antenna Considerations

| Frequency | Antenna Type |
|-----------|--------------|
| 433 MHz | Quarter-wave monopole (~17 cm), helical |
| 868 MHz | Quarter-wave (~8.6 cm), SMA duck |
| 915 MHz | Quarter-wave (~8.2 cm), SMA duck |
| Wideband | Discone, log-periodic, biconical |

**HackRF One Ports**:
- **RX**: SMA female (receive)
- **TX**: SMA female (transmit)
- **CLK**: SMA female (clock in/out)

---

## Power & Thermal

- **USB Power**: 5V, ~500 mA max (USB 2.0)
- **TX Duty Cycle**: Limit to <50% to avoid overheating
- **Heat Sink**: Recommended for extended TX
- **PortaPack**: Battery powered, lower TX power

---

## AI-Readable Spec

```yaml
hackrf_one:
  max_sample_rate: 20000000
  recommended_sample_rate: 2000000
  freq_range: "1 MHz - 6 GHz"
  tx_power_max: "47 dB (register value)"
  rx_gain_lna: "0-40 dB (8 dB steps)"
  rx_gain_vga: "0-62 dB (2 dB steps)"
  tx_gain_vga: "0-47 dB (1 dB steps)"
  ports:
    rx: "SMA female"
    tx: "SMA female"
    clk: "SMA female"
  file_format: "IQ interleaved, 8-bit signed (.c8)"
  usb: "USB 2.0 High Speed"

tools:
  hackrf_transfer:
    rx: "-r FILE -f FREQ -s RATE -n SAMPLES [-l LNA -g VGA]"
    tx: "-t FILE -f FREQ -s RATE -x GAIN [-R]"
  hackrf_sweep:
    single: "-f START:STOP -w BIN_WIDTH -1"
    continuous: "-f START:STOP -w BIN_WIDTH"
  hackrf_info: "Device info"
  hackrf_debug: "Register/SPI access"

gain_staging:
  strong_signal: {lna: 16, vga: 30}
  weak_signal: {lna: 40, vga: 60}
  tx_near: {tx: 20}
  tx_far: {tx: 47}

common_freqs:
  433_mhz: {band: "430:440", bin: 100000}
  868_mhz: {band: "860:870", bin: 100000}
  915_mhz: {band: "902:928", bin: 100000}
```

---

## Related

- [[05-dragon-os/ssh-setup|Dragon OS SSH Setup]]
- [[05-dragon-os/coordinated|Coordinated Workflows]]
- [[03-applications/tx-beacon|TX Beacon Firmware]]
- [[03-applications/rx-packet|RX Packet Firmware]]
- [[04-host-scripts/tx_verify.py|TX Verify Script]]