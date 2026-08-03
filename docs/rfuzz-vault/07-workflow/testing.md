# Testing Workflows

> Unit tests, integration tests, and validation procedures.

---

## Test Categories

| Level | Target | Tools | Frequency |
|-------|--------|-------|-----------|
| **Unit** | Individual functions | Unity, custom | Per commit |
| **Integration** | Component interaction | Hardware + host | Per PR |
| **System** | Full TX/RX chain | ESP32 + HackRF | Release |
| **Regression** | Known bugs | Automated | CI |

---

## Unit Testing (Unity)

### Setup
```bash
# Unity already in ESP-IDF
# Test files: test/ or components/*/test/

# Run all tests
idf.py -p COM6 test
```

### Example Test (CC1101 Driver)
```c
// components/cc1101/test/test_cc1101.c
#include "unity.h"
#include "cc1101.h"
#include "hw_init.h"

static cc1101_handle_t radio;

TEST_CASE("CC1101 init and reset", "[cc1101]") {
    TEST_ESP_OK(init_hardware());
    TEST_ESP_OK(cc1101_init(&radio, hw_cc1101_spi, PIN_NUM_CS, PIN_NUM_MISO, PIN_NUM_GDO0));
    
    uint8_t partnum = cc1101_read_status_reg(&radio, CC1101_PARTNUM);
    uint8_t version = cc1101_read_status_reg(&radio, CC1101_VERSION);
    
    TEST_ASSERT_EQUAL_HEX8(0x00, partnum);  // CC1101 PARTNUM
    TEST_ASSERT_EQUAL_HEX8(0x14, version);  // Typical version
}

TEST_CASE("CC1101 configure async RX", "[cc1101]") {
    TEST_ESP_OK(cc1101_config_async_rx(&radio, 433920000, 2400, 0x27, 0x0C, 0x0D));
    
    // Verify key registers
    TEST_ASSERT_EQUAL_HEX8(0x0D, cc1101_read_reg(&radio, CC1101_IOCFG0));  // Async data
    TEST_ASSERT_EQUAL_HEX8(0x0D, cc1101_read_reg(&radio, CC1101_IOCFG2));  // Async data
    TEST_ASSERT_EQUAL_HEX8(0x30, cc1101_read_reg(&radio, CC1101_PKTCTRL0)); // Async format
}
```

### Run Specific Test
```bash
idf.py -p COM6 test --test-filter "cc1101*"
```

---

## Integration Testing

### 1. Loopback Test (TX → RX on Same Board)
```bash
# Hardware: Connect GDO0 to GDO2 (or use antenna loopback)
# Firmware: Custom test app using cc1101_tx_test_preserve_rx()

# Test sequence:
# 1. Configure RX (async or packet mode)
# 2. Call cc1101_tx_test_preserve_rx() → sends test packet
# 3. Verify RX receives packet
# 4. Check RSSI, CRC, payload
```

### 2. Two-Board Test (TX Board + RX Board)
```bash
# Board 1: TX beacon (RFuzz_TX.c or rfuzz_tx_2fsk.c)
idf.py flash monitor  # On board 1

# Board 2: RX packet (RFuzz_RX.c)
idf.py flash monitor  # On board 2

# Verify: Board 2 logs "RX OK" with correct payload
```

### 3. Host Script Integration
```bash
# Test capture.py
python scripts/capture.py --port COM7 --rate 24000 --samples 10000 --output test.sr
# Verify: test.sr exists, valid .sr format

# Test capture_sump.py
python scripts/capture_sump.py --port COM7 --rate 24000 --samples 10000 --format sr
# Verify: capture file opens in PulseView
```

---

## System Validation (ESP32 + HackRF)

### Test Matrix

| Test | ESP32 Config | HackRF Config | Expected |
|------|--------------|---------------|----------|
| **TX Frequency** | Beacon TX | Spectrum sweep | Peak at 433.92 MHz |
| **TX Bandwidth** | Beacon TX | 2 MS/s capture | BW matches deviation |
| **TX Power** | Beacon TX | RX with known gain | Power ≈ configured |
| **RX Sensitivity** | Packet RX | TX sweep power | Min power for RX OK |
| **Sync Detection** | Packet RX | TX with sync | RX only on sync match |
| **CRC Validation** | CRC enabled | TX with/without CRC | RX only valid CRC |

### Automated Validation Script
```bash
#!/bin/bash
# validate_tx.sh

set -e

FREQ=433920000
RATE=2000000
SAMPLES=4000000
OUTPUT="tx_validation.c8"

echo "=== TX Validation ==="

# 1. Start HackRF capture
echo "Starting HackRF capture..."
ssh dragon "hackrf_transfer -r $OUTPUT -f $FREQ -s $RATE -n $SAMPLES -l 16 -g 30" &
HACKRF_PID=$!

# 2. Wait for capture to start
sleep 1

# 3. Trigger ESP32 TX (assuming beacon runs every 3s)
echo "Waiting for ESP32 beacon..."
sleep 5

# 4. Wait for capture complete
wait $HACKRF_PID

# 5. Download
scp dragon:$OUTPUT .

# 6. Analyze (requires Python + numpy/scipy)
python3 analyze_tx.py $OUTPUT $FREQ $RATE

echo "=== Validation Complete ==="
```

```python
# analyze_tx.py
import numpy as np
import sys

def analyze_capture(file, freq, rate):
    # Load IQ data
    iq = np.fromfile(file, dtype=np.int8).astype(np.float32)
    i = iq[0::2] / 127.5
    q = iq[1::2] / 127.5
    signal = i + 1j * q
    
    # FFT for spectrum
    fft = np.fft.fftshift(np.fft.fft(signal))
    freqs = np.fft.fftshift(np.fft.fftfreq(len(signal), 1/rate))
    power_db = 20 * np.log10(np.abs(fft) + 1e-10)
    
    # Find peak
    peak_idx = np.argmax(power_db)
    peak_freq = freqs[peak_idx]
    peak_power = power_db[peak_idx]
    
    print(f"Peak frequency: {peak_freq/1e6:.3f} MHz (target: {freq/1e6:.3f} MHz)")
    print(f"Peak power: {peak_power:.1f} dB")
    print(f"Frequency error: {(peak_freq-freq)/1e3:.1f} kHz")
    
    # Check bandwidth (3dB)
    half_max = peak_power - 3
    above = np.where(power_db > half_max)[0]
    if len(above) > 1:
        bw = freqs[above[-1]] - freqs[above[0]]
        print(f"3dB bandwidth: {bw/1e3:.1f} kHz")
    
    # Check for spurs (>30dBc)
    spurs = np.where((power_db > peak_power - 30) & (np.abs(freqs - peak_freq) > 100e3))[0]
    if len(spurs) > 0:
        print(f"WARNING: {len(spurs)} potential spurs detected")
        for s in spurs[:5]:
            print(f"  Spur at {freqs[s]/1e6:.3f} MHz: {power_db[s]:.1f} dB")
    else:
        print("No significant spurs (>30dBc)")

if __name__ == "__main__":
    analyze_capture(sys.argv[1], float(sys.argv[2]), float(sys.argv[3]))
```

---

## Regression Testing

### Known Issues Checklist
```markdown
# Before release, verify:
- [ ] CC1101 reset sequence works (MISO low detection)
- [ ] SPI mutex prevents concurrent access
- [ ] GDO0 ISR fires on TX done (SYNC_WORD mode)
- [ ] Async RX mode outputs clean bits on GDO0
- [ ] SUMP capture samples at correct rate
- [ ] USB transport doesn't reset on host connect
- [ ] UART transport DTR guard works
- [ ] Frequency sweep restores async RX config
- [ ] Status monitor task doesn't crash
- [ ] TX test preserves/restores RX config
```

### Automated Regression (CI)
```yaml
# .github/workflows/regression.yml
jobs:
  regression:
    runs-on: self-hosted  # Needs ESP32 + HackRF hardware
    steps:
      - checkout
      - build_all_presets
      - run_loopback_test
      - run_two_board_test
      - run_hackrf_validation
      - check_known_issues
```

---

## Performance Benchmarks

### CC1101 Driver Benchmarks
```c
// Benchmark: Register read/write speed
void benchmark_spi() {
    uint32_t start = xTaskGetTickCount();
    for (int i = 0; i < 1000; i++) {
        cc1101_write_reg(&radio, CC1101_SYNC1, 0xDE);
        cc1101_read_reg(&radio, CC1101_SYNC1);
    }
    uint32_t elapsed = xTaskGetTickCount() - start;
    ESP_LOGI(TAG, "1000 reg read/write: %lu ms", elapsed);
}
```

### SUMP Capture Benchmarks
```bash
# Max sustainable sample rate
for rate in 100000 200000 300000 400000 500000; do
    python capture_sump.py --port COM7 --rate $rate --samples 100000 --format bin
    # Check for dropped samples / timeout
done
```

---

## AI-Readable Test Spec

```yaml
testing:
  unit:
    framework: "Unity (ESP-IDF)"
    location: "components/*/test/"
    run: "idf.py test"
    examples:
      - cc1101_init_reset
      - cc1101_configure_async_rx
      - cc1101_register_verify
      - sump_capture_buffer
      - sump_transport_protocol
  
  integration:
    loopback:
      hardware: "Single ESP32 (GDO0→GDO2 or antenna)"
      firmware: "cc1101_tx_test_preserve_rx()"
      verify: "RX receives test packet"
    two_board:
      hardware: "2x ESP32+CC1101"
      tx: "RFuzz_TX.c or rfuzz_tx_2fsk.c"
      rx: "RFuzz_RX.c"
      verify: "RX logs match TX payload"
    host_scripts:
      - capture_py: "python capture.py --output test.sr"
      - capture_sump_py: "python capture_sump.py --format sr"
      - tx_verify: "python tx_verify.py"
  
  system:
    hackrf_validation:
      tx_freq: "Beacon TX + HackRF sweep → peak at target"
      tx_bw: "Beacon TX + HackRF capture → BW matches deviation"
      tx_power: "Beacon TX + HackRF RX → power ≈ config"
      rx_sensitivity: "Packet RX + HackRF TX sweep → min power for RX OK"
      sync_detection: "Packet RX + HackRF TX sync variants → RX only on match"
      crc_validation: "CRC enabled RX + HackRF TX CRC variants → RX only valid"
  
  regression:
    checklist:
      - cc1101_reset_miso
      - spi_mutex
      - gdo0_isr_tx_done
      - async_rx_gdo0_bits
      - sump_sample_rate
      - usb_no_reset
      - uart_dtr_guard
      - sweep_restores_config
      - status_monitor_stable
      - tx_test_preserves_rx
  
  benchmarks:
    spi_rw_1000: "< 50 ms typical"
    sump_max_rate: "500 kHz sustained"
    tx_latency: "< 1 ms (STX to GDO0 sync)"
    rx_latency: "< 1 ms (sync to RXBYTES)"
```

---

## Related

- [[07-workflow/development|Development Workflow]]
- [[07-workflow/debugging|Debugging Guide]]
- [[05-dragon-os/coordinated|Coordinated Workflows]]
- [[02-firmware/cc1101-driver|CC1101 Driver API]]
- [[02-firmware/sump-capture|SUMP Capture Internals]]
- [[04-host-scripts/rfuzz-testcase|2FSK End-to-End Test Case]]