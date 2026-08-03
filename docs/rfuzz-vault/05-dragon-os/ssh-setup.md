# Dragon OS / HackRF Integration

> **Target**: Dragon OS Focal on HackRF One
> **SSH**: `dragon@192.168.1.101`

---

## Overview

Dragon OS is a Ubuntu-based SDR distribution pre-installed on HackRF One (PortaPack H2/H4). It provides a complete RF analysis environment accessible via SSH.

**Use Cases with RFuzz**:
- **TX Validation**: Verify ESP32+CC1101 transmissions with HackRF RX
- **RX Cross-check**: Receive on ESP32, verify with HackRF spectrum
- **Spectrum Monitoring**: Wideband view while ESP32 captures narrowband
- **Coordinated Fuzzing**: ESP32 transmits, HackRF receives (or vice versa)
- **Signal Replay**: Capture with HackRF, replay with ESP32

---

## Prerequisites — VMware + USB Pass-Through (Required)

Dragon OS runs inside a **VMware VM**. SSH will not work until the VM is powered on, and **the HackRF is only usable after its USB device is passed through to the VM**:

1. **Start the VM**: VMware Workstation → *Dragon OS* → Power On.
2. **Pass the HackRF through**: VM → *Removable Devices* → *HackRF One SDR* → **Connect**.
   - If the entry is greyed out, the HackRF is not plugged into the host or is claimed by another program.
3. **Verify the radio (not just SSH)**:
   ```bash
   ssh dragon@192.168.1.101 "lsusb"        # expect: ID 1d50:6089 ... HackRF One SDR
   ssh dragon@192.168.1.101 "hackrf_info"  # expect: "Found HackRF"
   ```
   - `lsusb` without the HackRF entry → device still on host, redo pass-through.
   - `hackrf_info` reporting `No HackRF boards found.` → USB not attached to the VM (SSH is fine, radio is not).

> ⚠️ Order matters: start the VM **and** attach the HackRF USB **before** running any HackRF capture/sweep workflow. SSH reachability alone does not mean the radio is usable.

---

## SSH Connection

### First-Time Setup
```bash
# From Windows PowerShell / WSL / Linux
ssh dragon@192.168.1.101
# Password: dragon (default)
```

### SSH Key Setup (Recommended)
```bash
# On host machine
ssh-keygen -t ed25519 -C "rfuzz-host"
ssh-copy-id dragon@192.168.1.101

# Verify passwordless login
ssh dragon@192.168.1.101 "echo 'Connected to Dragon OS'"
```

### SSH Config (`~/.ssh/config`)
```ssh
Host dragon
    HostName 192.168.1.101
    User dragon
    Port 22
    IdentityFile ~/.ssh/id_ed25519
    ServerAliveInterval 30
    ServerAliveCountMax 3
```
Then: `ssh dragon`

---

## HackRF Tools Available

| Tool | Purpose |
|------|---------|
| `hackrf_transfer` | Raw RX/TX file capture/playback |
| `hackrf_sweep` | Frequency sweep (spectrum analyzer) |
| `hackrf_info` | Device info, firmware version |
| `hackrf_debug` | Register read/write, SPI test |
| `hackrf_cpldjtag` | CPLD programming |
| `gr-*` | GNU Radio companions |
| `inspectrum` | Signal analysis GUI |
| `URH` (Universal Radio Hacker) | Protocol analysis GUI |
| `gqrx` / `linrad` | SDR receivers |
| `kalibrate-hackrf` | GSM calibration |
| `rfcat` | RfCat integration |

---

## Common Operations

### 1. Device Info
```bash
ssh dragon "hackrf_info"
```
```
Found HackRF board.
Board ID: HackRF One
Firmware: 2024.01.1
...
```

### 2. RX Capture (Save to File)
```bash
# Capture 4M samples at 2 MS/s, center 433.92 MHz
ssh dragon "hackrf_transfer -r capture.c8 -f 433920000 -s 2000000 -n 4000000"
# -r: receive, -f: freq Hz, -s: sample rate, -n: num samples
# Output: 8-bit signed IQ (.c8 format)
```

### 3. RX Capture (Stream to stdout)
```bash
# Pipe to local analysis
ssh dragon "hackrf_transfer -r - -f 433920000 -s 2000000 -n 2000000" > local_capture.c8
```

### 4. TX Replay
```bash
# Replay captured file
ssh dragon "hackrf_transfer -t capture.c8 -f 433920000 -s 2000000 -x 20"
# -t: transmit, -x: gain (0-47 dB)
```

### 5. Frequency Sweep (Spectrum)
```bash
# Sweep 400-500 MHz, 1 MHz steps
ssh dragon "hackrf_sweep -f 400:500 -w 1000000 -1" > sweep.csv
# -f: start:stop, -w: bin width, -1: single sweep
# Output: CSV (date, time, hz_low, hz_high, hz_step, samples, dB...)
```

### 6. Continuous Sweep (Streaming)
```bash
# Real-time spectrum for GNU Radio / custom tools
ssh dragon "hackrf_sweep -f 400:500 -w 1000000" | python process_sweep.py
```

---

## Coordinated Workflows

### Workflow 1: ESP32 TX → HackRF RX (Validate Transmission)
```bash
# Terminal 1: Start HackRF capture
ssh dragon "hackrf_transfer -r esp32_tx.c8 -f 433920000 -s 2000000 -n 2000000"

# Terminal 2: Trigger ESP32 TX (via capture.py 0x02 command)
python scripts/tx_verify.py
# Or use capture_sump.py with selftest
python scripts/capture_sump.py --port COM7 --selftest

# Analyze
# Copy file back: scp dragon:esp32_tx.c8 .
# Open in inspectrum/URH
```

### Workflow 2: HackRF TX → ESP32 RX (Validate Reception)
```bash
# Terminal 1: ESP32 in RX mode (main.c or RFuzz_RX.c)
idf.py flash monitor

# Terminal 2: HackRF transmits test signal
ssh dragon "hackrf_transfer -t test_signal.c8 -f 433920000 -s 2000000 -x 20"

# Terminal 1: Check ESP32 logs for RX OK
```

### Workflow 3: Simultaneous Spectrum + Narrowband
```bash
# Terminal 1: HackRF wideband sweep (background)
ssh dragon "hackrf_sweep -f 430:440 -w 100000" > spectrum.csv &

# Terminal 2: ESP32 narrowband capture at 433.92 MHz
python scripts/capture_sump.py --port COM7 --rate 24000 --samples 100000

# Correlate: Check spectrum.csv for activity during ESP32 capture
```

### Workflow 4: Signal Replay (HackRF → ESP32 → HackRF)
```bash
# 1. Capture with HackRF
ssh dragon "hackrf_transfer -r original.c8 -f 433920000 -s 2000000 -n 4000000"

# 2. Analyze in URH/inspectrum → extract packet bits

# 3. Program ESP32 TX with extracted payload
# Edit RFuzz_TX.c payload array

# 4. Replay with ESP32
idf.py flash monitor

# 5. Verify replay with HackRF
ssh dragon "hackrf_transfer -r replay.c8 -f 433920000 -s 2000000 -n 4000000"

# 6. Compare original.c8 vs replay.c8 in inspectrum
```

---

## File Transfer

### Download from Dragon OS
```bash
# Single file
scp dragon:esp32_tx.c8 .

# Multiple files
scp dragon:'*.c8' .

# With progress
scp -P 22 dragon:large_capture.c8 .  # Shows progress
```

### Upload to Dragon OS
```bash
scp test_signal.c8 dragon:~/
scp -r my_project/ dragon:~/rfuzz/
```

### Sync Directories
```bash
# Using rsync over SSH
rsync -avz -e ssh dragon:~/captures/ ./captures/
rsync -avz -e ssh ./firmware/ dragon:~/rfuzz/firmware/
```

---

## Remote Script Execution

### Run Python on Dragon OS
```bash
# Execute local script remotely
ssh dragon "python3 -c \"import numpy; print('numpy ok')\""

# Run script file
scp analyze_capture.py dragon:~/
ssh dragon "python3 analyze_capture.py esp32_tx.c8"
```

### Background Capture
```bash
# Start capture in background, get PID
ssh dragon "nohup hackrf_transfer -r long_capture.c8 -f 433920000 -s 2000000 -n 20000000 > capture.log 2>&1 & echo \$!"
# Later: check log, kill if needed
ssh dragon "tail -f capture.log"
ssh dragon "pkill hackrf_transfer"
```

---

## GNU Radio on Dragon OS

### Flowgraph via SSH (Headless)
```bash
# Generate flowgraph on host, run on Dragon
cat > sweep_flowgraph.py << 'EOF'
from gnuradio import gr, blocks, fft
from gnuradio.fft import window
import hackrf

class SweepFlowgraph(gr.top_block):
    def __init__(self):
        gr.top_block.__init__(self)
        self.src = hackrf.source()
        self.src.set_center_freq(433.92e6)
        self.src.set_sample_rate(2e6)
        self.src.set_gain(20)
        self.sink = blocks.file_sink(gr.sizeof_gr_complex, "/tmp/gr_capture.c8")
        self.connect(self.src, self.sink)

if __name__ == "__main__":
    tb = SweepFlowgraph()
    tb.run()
EOF

scp sweep_flowgraph.py dragon:~/
ssh dragon "python3 sweep_flowgraph.py"
```

---

## Useful Aliases (Add to `~/.bashrc` on Dragon)

```bash
# HackRF shortcuts
alias hx='hackrf_transfer'
alias hs='hackrf_sweep'
alias hi='hackrf_info'

# Common captures
alias cap433='hackrf_transfer -r cap433.c8 -f 433920000 -s 2000000 -n 4000000'
alias cap868='hackrf_transfer -r cap868.c8 -f 868300000 -s 2000000 -n 4000000'
alias cap915='hackrf_transfer -r cap915.c8 -f 915000000 -s 2000000 -n 4000000'

# Spectrum sweeps
alias sweep433='hackrf_sweep -f 430:440 -w 100000'
alias sweep868='hackrf_sweep -f 860:870 -w 100000'
alias sweep915='hackrf_sweep -f 902:928 -w 100000'

# TX with gain
alias tx433='hackrf_transfer -t cap433.c8 -f 433920000 -s 2000000 -x 20'
```

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| "No HackRF found" | Check USB connection, `lsusb`, try `hackrf_info` |
| "Permission denied" | Add user to `plugdev`: `sudo usermod -a -G plugdev dragon` |
| TX fails | Check antenna, gain (-x), sample rate matches file |
| Sweep slow | Reduce bin width (-w), use single sweep (-1) |
| SSH timeout | Increase `ServerAliveInterval`, check WiFi/power |
| File transfer slow | Use `rsync -z` for compression, or `scp -C` |

---

## AI-Readable Spec

```yaml
dragon_os:
  host: "192.168.1.101"
  user: "dragon"
  default_password: "dragon"
  ssh_key_recommended: true
  os: "Dragon OS Focal (Ubuntu-based)"
  hardware: "HackRF One (PortaPack H2/H4)"

hackrf_tools:
  - hackrf_transfer: "Raw IQ capture/playback"
  - hackrf_sweep: "Frequency sweep/spectrum"
  - hackrf_info: "Device info"
  - hackrf_debug: "Register/SPI debug"
  - gr-*: "GNU Radio blocks"
  - inspectrum: "Signal analysis GUI"
  - URH: "Protocol analysis GUI"
  - gqrx: "SDR receiver GUI"

common_operations:
  rx_capture: "hackrf_transfer -r file.c8 -f FREQ -s RATE -n SAMPLES"
  tx_replay: "hackrf_transfer -t file.c8 -f FREQ -s RATE -x GAIN"
  sweep: "hackrf_sweep -f START:STOP -w BIN_WIDTH"
  sweep_single: "hackrf_sweep -f START:STOP -w BIN_WIDTH -1"

coordinated_workflows:
  - name: "ESP32_TX_HackRF_RX"
    desc: "Validate ESP32 transmission with HackRF"
    steps: ["hackrf_transfer -r", "trigger ESP32 TX", "analyze"]
  - name: "HackRF_TX_ESP32_RX"
    desc: "Validate ESP32 reception with HackRF"
    steps: ["ESP32 RX mode", "hackrf_transfer -t", "check ESP32 logs"]
  - name: "Simultaneous_Spectrum_Narrowband"
    desc: "Wideband sweep + narrowband capture"
    steps: ["hackrf_sweep background", "ESP32 capture", "correlate"]
  - name: "Signal_Replay"
    desc: "Capture → analyze → replay → verify"
    steps: ["HackRF capture", "URH analysis", "ESP32 TX", "HackRF verify"]

file_transfer:
  download: "scp dragon:file.c8 ."
  upload: "scp file.c8 dragon:~/"
  sync: "rsync -avz -e ssh dragon:~/dir/ ./dir/"

remote_execution:
  python: "ssh dragon 'python3 script.py'"
  background: "ssh dragon 'nohup cmd > log 2>&1 &'"
  gnuradio: "scp flowgraph.py dragon:~/ && ssh dragon 'python3 flowgraph.py'"
```

---

## Related

- [[03-applications/tx-beacon|TX Beacon Firmware]]
- [[03-applications/rx-packet|RX Packet Firmware]]
- [[04-host-scripts/tx_verify.py|TX Verify Script]]
- [[04-host-scripts/rssi_mon.py|RSSI Monitor]]
- [[07-workflow/testing|Testing Workflows]]