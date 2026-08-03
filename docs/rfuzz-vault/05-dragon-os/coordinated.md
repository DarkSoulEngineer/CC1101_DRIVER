# Coordinated ESP32 + HackRF Workflows

> **Goal**: Combine ESP32+CC1101 (narrowband, precise timing) with HackRF One (wideband, high dynamic range) for comprehensive RF analysis.

---

## Workflow Categories

| Category | ESP32 Role | HackRF Role | Use Case |
|----------|------------|-------------|----------|
| **Validation** | TX beacon | RX capture | Verify TX params, spectrum purity |
| **Validation** | RX packet | TX signal | Verify RX sensitivity, decoding |
| **Spectrum** | Narrowband capture | Wideband sweep | Context + detail |
| **Replay** | TX replay | Capture → analyze | Signal reproduction |
| **Fuzzing** | Mutating TX | RX monitor | Protocol fuzzing |
| **Geolocation** | Multiple RX | Reference TX | TDOA/DoA |

---

## Workflow 1: TX Validation (ESP32 TX → HackRF RX)

### Objective
Verify ESP32+CC1101 transmission: frequency, modulation, deviation, packet structure.

### Setup
```
ESP32+CC1101 (TX)          HackRF One (RX)
    │                            │
    │  433.92 MHz                │  433.92 MHz ± 1 MHz
    │  2FSK 2.4 kbps             │  2 MS/s capture
    │  Sync 0xDEAF               │  4M samples (2 sec)
    │  +10 dBm                   │  LNA 16, VGA 30
    ▼                            ▼
```

### Steps
```bash
# 1. Start HackRF capture (Terminal 1)
ssh dragon "hackrf_transfer -r esp32_tx_val.c8 -f 433920000 -s 2000000 -n 4000000 -l 16 -g 30"

# 2. Trigger ESP32 TX (Terminal 2)
# Option A: RFuzz_TX.c beacon (every 3s)
python scripts/tx_verify.py

# Option B: rfuzz_tx_2fsk.c (every 10s) - just wait
# Option C: Custom trigger via capture_sump.py --selftest
python scripts/capture_sump.py --port COM7 --selftest

# 3. Wait for capture to complete (~2 sec)
# 4. Download and analyze
scp dragon:esp32_tx_val.c8 .
# Open in inspectrum/URH
```

### Analysis Checklist
- [ ] Center frequency = 433.92 MHz ± 10 kHz
- [ ] Bandwidth matches deviation setting
- [ ] Sync word visible (0xDEAF = 11011110 10101111)
- [ ] Preamble visible (0xAA = 10101010...)
- [ ] Packet spacing = 3s or 10s
- [ ] No spurious emissions > -30 dBc
- [ ] Power level ≈ +10 dBm (check HackRF gain calibration)

---

## Workflow 2: RX Validation (HackRF TX → ESP32 RX)

### Objective
Verify ESP32+CC1101 reception: sensitivity, sync detection, CRC, packet parsing.

### Setup
```
HackRF One (TX)            ESP32+CC1101 (RX)
    │                            │
    │  433.92 MHz                │  433.92 MHz
    │  2FSK 2.4 kbps             │  2FSK 2.4 kbps
    │  Sync 0xDEAF               │  Sync 0xDEAF
    │  Variable power            │  CRC disabled
    ▼                            ▼
```

### Steps
```bash
# 1. Prepare test signal on HackRF
# Create known packet: preamble + sync + payload
python3 -c "
import numpy as np
# Generate 2FSK signal... (or use pre-recorded)
" > test_packet.c8

# 2. Start ESP32 RX (Terminal 1)
idf.py flash monitor
# Should show: "Entering RX mode, waiting for packets..."

# 3. Transmit from HackRF (Terminal 2)
ssh dragon "hackrf_transfer -t test_packet.c8 -f 433920000 -s 2000000 -x 20"

# 4. Check ESP32 logs
# Expected: "RX OK (9 bytes): SRC=0xDE DST=0xAD TYPE=0xBE SEQ=0xEF01 DATA=..."
```

### Power Sweep (Sensitivity Test)
```bash
# Test at decreasing power levels
for gain in 20 15 10 5 0 -5 -10 -15 -20; do
    echo "Testing TX gain: ${gain} dB"
    ssh dragon "hackrf_transfer -t test_packet.c8 -f 433920000 -s 2000000 -x ${gain}"
    sleep 2
done
# Observe ESP32: at what gain does RX fail?
```

---

## Workflow 3: Simultaneous Spectrum + Narrowband

### Objective
Wideband context (HackRF sweep) + narrowband detail (ESP32 capture).

### Setup
```
                    HackRF One
                       │
            ┌──────────┴──────────┐
            ▼                     ▼
       Sweep 430-440 MHz    ESP32+CC1101
       100 kHz bins         Capture 433.92 MHz
       Continuous           24 kS/s, 100k samples
```

### Steps
```bash
# Terminal 1: HackRF continuous sweep (background)
ssh dragon "hackrf_sweep -f 430:440 -w 100000 > sweep_433.csv" &
SWEEP_PID=$!

# Terminal 2: ESP32 triggered capture
python scripts/capture_sump.py --port COM7 --rate 24000 --samples 100000 --format sr

# Terminal 1: Stop sweep
ssh dragon "kill $SWEEP_PID"

# Analyze
# 1. Check sweep_433.csv for activity during capture window
# 2. Correlate timestamps
# 3. Identify interferers, adjacent channels
```

### Correlation Script
```python
# correlate.py
import pandas as pd
import zipfile

# Parse sweep CSV
sweep = pd.read_csv('sweep_433.csv', header=None)
sweep.columns = ['date','time','hz_low','hz_high','hz_step','samples'] + [f'dB_{i}' for i in range(100)]

# Parse ESP32 capture timestamps (from metadata)
with zipfile.ZipFile('capture_20240101_120000.sr') as zf:
    meta = zf.read('metadata').decode()
    # Extract samplerate, calculate capture time window

# Find sweep rows matching capture window
# Plot sweep waterfall with capture window highlighted
```

---

## Workflow 4: Signal Capture → Analysis → Replay

### Objective
Capture unknown signal with HackRF, analyze in URH, replay with ESP32.

### Phase 1: Capture (HackRF)
```bash
# Wide capture around target freq
ssh dragon "hackrf_transfer -r unknown.c8 -f 433920000 -s 2000000 -n 20000000"
scp dragon:unknown.c8 .
```

### Phase 2: Analysis (Host - URH/inspectrum)
```bash
# Open in URH
urh unknown.c8
# 1. Find packet bursts
# 2. Measure baud rate, modulation
# 3. Extract sync word, preamble
# 4. Decode payload bits
# 5. Export as .c8 or raw bits
```

### Phase 3: Program ESP32 TX
```c
// Edit main/RFuzz_TX.c or examples/rfuzz_tx_2fsk.c
// Update config to match discovered params:
static const cc1101_config_t tx_cfg = {
    .freq_hz = 433920000,           // From analysis
    .modem = {
        .modulation = CC1101_MOD_2FSK_E,  // Or GFSK
        .sync_mode = CC1101_SYNC_16_16_E,
        .datarate_bps = 2400,             // Measured
        .deviation = 0x47,                // Measured
        .preamble_bytes = 4,              // Measured
        ...
    },
    .packet = {
        .sync1 = 0xXX,  // Discovered sync
        .sync0 = 0xXX,
        ...
    },
    // Payload from analysis
    .payload = {0xXX, 0xXX, ...}
};
```

### Phase 4: Replay (ESP32)
```bash
idf.py flash monitor
# ESP32 transmits replayed signal
```

### Phase 5: Verify (HackRF)
```bash
ssh dragon "hackrf_transfer -r replay_verify.c8 -f 433920000 -s 2000000 -n 4000000"
scp dragon:replay_verify.c8 .

# Compare in inspectrum
inspectrum unknown.c8 replay_verify.c8
# Check: frequency, timing, modulation, packet structure match
```

---

## Workflow 5: Protocol Fuzzing

### Objective
ESP32 mutates packets, HackRF monitors for anomalies/crashes.

### Setup
```
ESP32 (Fuzzer TX)          HackRF (Monitor RX)
    │                            │
    │  Mutated packets           │  Continuous capture
    │  433.92 MHz                │  433.92 MHz ± 2 MHz
    ▼                            ▼
```

### ESP32 Fuzzer Code (Conceptual)
```c
// In main loop of custom fuzzer app
while (1) {
    // Generate mutated packet
    uint8_t pkt[64];
    generate_fuzz_packet(pkt, sizeof(pkt));
    
    // Transmit
    cc1101_transmit(&radio, pkt, sizeof(pkt));
    cc1101_wait_tx_done(&radio, 500);
    
    // Log mutation for correlation
    ESP_LOGI(TAG, "Fuzz: %s", mutation_description);
    
    vTaskDelay(pdMS_TO_TICKS(100));  // 10 Hz fuzz rate
}
```

### HackRF Monitor
```bash
# Continuous capture for post-analysis
ssh dragon "hackrf_transfer -r fuzz_monitor.c8 -f 433920000 -s 4000000 -n 80000000" &
# 20 sec at 4 MS/s = 80M samples
```

### Analysis
- Correlate ESP32 mutation log with HackRF capture
- Look for target device responses (if attacking)
- Identify crash-causing mutations

---

## Workflow 6: TDOA / Direction Finding (Advanced)

### Objective
Use multiple ESP32+CC1101 nodes + HackRF reference for Time Difference of Arrival.

### Setup
```
Reference: HackRF One (TX beacon)
           │
    ┌──────┼──────┐
    ▼      ▼      ▼
 ESP32#1 ESP32#2 ESP32#3
 (RX)    (RX)    (RX)
    │      │      │
    └──────┼──────┘
           ▼
      Host: Correlate timestamps
```

### Requirements
- All ESP32s: GPS-disciplined clocks OR wired sync
- HackRF: Transmits precise timestamped beacon
- ESP32s: Capture with SUMP at high rate (100+ kS/s)
- Host: Cross-correlate received waveforms

### Synchronization
```bash
# HackRF transmits beacon with known pattern every 100ms
# ESP32s capture continuously
# Host aligns captures using beacon pattern
# TDOA = time_diff * c = distance_diff
```

---

## Automation Scripts

### Master Control Script
```bash
#!/bin/bash
# run_coordinated.sh

MODE=$1  # validate_tx, validate_rx, spectrum, replay, fuzz

case $MODE in
    validate_tx)
        ssh dragon "hackrf_transfer -r esp32_tx.c8 -f 433920000 -s 2000000 -n 4000000" &
        python scripts/tx_verify.py
        wait
        scp dragon:esp32_tx.c8 .
        ;;
    validate_rx)
        idf.py flash monitor &
        sleep 5
        ssh dragon "hackrf_transfer -t test_packet.c8 -f 433920000 -s 2000000 -x 20"
        ;;
    spectrum)
        ssh dragon "hackrf_sweep -f 430:440 -w 100000 > sweep.csv" &
        python scripts/capture_sump.py --port COM7 --rate 24000 --samples 100000
        ssh dragon "pkill hackrf_sweep"
        ;;
    *)
        echo "Usage: $0 {validate_tx|validate_rx|spectrum|replay|fuzz}"
        ;;
esac
```

---

## AI-Readable Workflow Spec

```yaml
coordinated_workflows:
  - name: "validate_tx"
    desc: "ESP32 TX → HackRF RX validation"
    esp32_role: "TX beacon"
    hackrf_role: "RX capture"
    steps:
      - "ssh dragon hackrf_transfer -r capture.c8 -f 433920000 -s 2000000 -n 4000000"
      - "python tx_verify.py (or wait for beacon)"
      - "scp dragon:capture.c8 ."
      - "analyze in inspectrum/URH"
    checks: ["freq accuracy", "deviation", "sync word", "power", "spurs"]
  
  - name: "validate_rx"
    desc: "HackRF TX → ESP32 RX validation"
    esp32_role: "RX packet"
    hackrf_role: "TX test signal"
    steps:
      - "idf.py flash monitor (RFuzz_RX.c)"
      - "ssh dragon hackrf_transfer -t test.c8 -f 433920000 -s 2000000 -x GAIN"
      - "check ESP32 logs for RX OK"
    checks: ["sensitivity", "sync detection", "CRC", "parsing"]
  
  - name: "spectrum_context"
    desc: "Wideband sweep + narrowband capture"
    esp32_role: "Narrowband capture"
    hackrf_role: "Wideband sweep"
    steps:
      - "ssh dragon hackrf_sweep -f 430:440 -w 100000 > sweep.csv &"
      - "python capture_sump.py --port COM7 --rate 24000 --samples 100000"
      - "kill sweep"
      - "correlate timestamps"
  
  - name: "signal_replay"
    desc: "Capture → analyze → replay → verify"
    steps:
      - "HackRF capture unknown signal"
      - "URH/inspectrum analysis"
      - "Program ESP32 TX with discovered params"
      - "ESP32 replay"
      - "HackRF verify replay"
  
  - name: "fuzzing"
    desc: "ESP32 mutates, HackRF monitors"
    esp32_role: "Fuzzer TX"
    hackrf_role: "Monitor RX"
    steps:
      - "ESP32 runs fuzzer loop"
      - "HackRF continuous capture"
      - "Correlate mutations with capture"
```

---

## Related

- [[05-dragon-os/ssh-setup|Dragon OS SSH Setup]]
- [[05-dragon-os/hackrf-ops|HackRF Operations]]
- [[03-applications/tx-beacon|TX Beacon Firmware]]
- [[03-applications/rx-packet|RX Packet Firmware]]
- [[04-host-scripts/tx_verify.py|TX Verify Script]]
- [[07-workflow/testing|Testing Workflows]]