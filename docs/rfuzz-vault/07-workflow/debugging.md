# Debugging Guide

> Common issues, diagnostic techniques, and tools.

---

## Debugging Tools

| Tool | Purpose | Access |
|------|---------|--------|
| **Serial Monitor** | Logs, assertions | `idf.py monitor` |
| **GDB + OpenOCD** | Breakpoints, stack trace | `idf.py openocd` + `idf.py gdb` |
| **SUMP Logic Analyzer** | GDO0/GDO2 timing | `capture_sump.py --format vcd` |
| **GTKWave** | VCD waveform view | Open `.vcd` |
| **PulseView** | Protocol decode | Open `.sr` |
| **HackRF + inspectrum** | RF spectrum/validation | `hackrf_transfer` + `inspectrum` |
| **ESP-IDF Monitor** | Panic decode, coredump | `idf.py monitor` |

---

## Common Issues & Solutions

### 1. CC1101 Not Responding

**Symptoms**:
```
E (123) CC1101: Reset failed: MISO didn't go low after CS
E (123) CC1101: Reset failed: MISO didn't go low after SRES
```

**Causes & Fixes**:
| Cause | Check | Fix |
|-------|-------|-----|
| Wrong MISO pin | `CONFIG_CC1101_PIN_MISO` | Verify wiring, correct GPIO |
| CS not connected | CS pin wiring | Check CS line, pull-up |
| SPI mode mismatch | SPI mode 0 (CPOL=0, CPHA=0) | Verify in `spi_init.c` |
| CC1101 not powered | VCC/GND | Measure 3.3V at module |
| Crystal not oscillating | XOSC pins | Check 26 MHz crystal |

**Debug Steps**:
```bash
# 1. Check MISO pin level before init
gpio_get_level(PIN_NUM_MISO)  # Should be 1 (idle high)

# 2. Scope SPI lines during init
# CS: should pulse low
# SCK: 1 MHz clock
# MOSI: SRES (0x30) sent
# MISO: should go low after CS, then after SRES

# 3. Read PARTNUM/VERSION after init
cc1101_read_status_reg(&radio, CC1101_PARTNUM)  # Should be 0x00
cc1101_read_status_reg(&radio, CC1101_VERSION)  # Should be 0x14 (typical)
```

### 2. No Data Captured (SUMP)

**Symptoms**:
- Host times out waiting for data
- Capture returns 0 bytes
- PulseView shows empty capture

**Causes & Fixes**:
| Cause | Check | Fix |
|-------|-------|-----|
| Wrong transport | USB vs UART | Match firmware `CONFIG_SUMP_TRANSPORT` |
| Wrong port | COM7 vs COM6 | Check Device Manager |
| Firmware not in SUMP mode | Running TX/RX app | Flash `main.c` (capture firmware) |
| GDO pins not configured | `CONFIG_CC1101_PIN_GDO0/2` | Match hardware |
| Timer not running | GPTIMER init | Check logs for "Timer: X Hz" |

**Debug Steps**:
```bash
# 1. Check boot marker (0xAA)
python -c "
import serial, time
s = serial.Serial('COM7', 115200, timeout=1)
time.sleep(1)
print('Boot:', s.read(10).hex())
"

# 2. Send ID command manually
python -c "
import serial, time
s = serial.Serial('COM7', 115200, timeout=1)
time.sleep(1)
s.write(bytes([0x00]))  # RESET
time.sleep(0.3)
s.write(bytes([0x02]))  # ID
time.sleep(0.3)
print('ID:', s.read(4))
"

# 3. Check firmware logs
idf.py monitor
# Look for: "Ready. [0x01][rate:4LE][count:4LE]=capture..."
```

### 3. TX Not Working

**Symptoms**:
- `cc1101_wait_tx_done()` returns timeout
- No signal on HackRF/spectrum
- GDO0 doesn't pulse

**Causes & Fixes**:
| Cause | Check | Fix |
|-------|-------|-----|
| GDO0 ISR not enabled | `CONFIG_CC1101_ISR_ENABLE=y` | Enable for `wait_tx_done` |
| GDO0 mode wrong | `IOCFG0 = 0x06` (SYNC_WORD) | Check `radio.gdo0_mode` |
| PA not configured | `PA_VALUE` / `PATABLE` | Check `cc1101_set_tx_power` |
| Not in TX mode | `cc1101_set_tx_mode()` called | Verify state machine |
| Crystal frequency | 26 MHz reference | Check `calc_datarate` |

**Debug Steps**:
```c
// Add debug prints in cc1101_transmit()
ESP_LOGI(TAG, "Pre-TX: MARCSTATE=0x%02X TXBYTES=0x%02X GDO0=%d",
         cc1101_read_status_reg(dev, CC1101_MARCSTATE) & 0x1F,
         cc1101_read_status_reg(dev, CC1101_TXBYTES) & 0x7F,
         gpio_get_level(dev->gdo0_pin));

// Check after STX
uint8_t status = cc1101_strobe(dev, CC1101_STX);
ESP_LOGI(TAG, "STX status=0x%02X", status);

// Poll MARCSTATE
for (int i=0; i<100; i++) {
    uint8_t marc = cc1101_read_status_reg(dev, CC1101_MARCSTATE) & 0x1F;
    ESP_LOGI(TAG, "MARCSTATE=0x%02X", marc);
    if (marc != 0x0B) break;  // 0x0B = TX
    vTaskDelay(1);
}
```

### 4. RX Not Receiving

**Symptoms**:
- `cc1101_receive_packet()` always returns false
- `RXBYTES` always 0
- `MARCSTATE` not staying in RX (0x0D)

**Causes & Fixes**:
| Cause | Check | Fix |
|-------|-------|-----|
| Frequency mismatch | TX vs RX freq | Match exactly |
| Sync word mismatch | TX vs RX sync | Match 0xDEAF |
| Modulation mismatch | 2FSK vs GFSK | Match modulation |
| Datarate mismatch | TX vs RX rate | Match datarate |
| Deviation mismatch | TX vs RX deviation | Match deviation reg |
| Preamble too short | TX preamble ≥ RX | Increase RX preamble |
| CRC enabled one side | Both CRC on/off | Match CRC setting |
| Not in RX mode | `cc1101_set_rx_mode()` | Call in loop |

**Debug Steps**:
```c
// Monitor MARCSTATE in loop
uint8_t marc = cc1101_read_status_reg(&radio, CC1101_MARCSTATE) & 0x1F;
ESP_LOGI(TAG, "MARCSTATE=0x%02X", marc);
// Should be 0x0D (RX) most of the time

// Check PKTSTATUS
uint8_t pkt = cc1101_read_status_reg(&radio, CC1101_PKTSTATUS);
ESP_LOGI(TAG, "PKTSTATUS=0x%02X", pkt);
// Bit 0: CRC_OK, Bit 1: CS, Bit 2: PQT

// Check RXBYTES
uint8_t rxb = cc1101_read_status_reg(&radio, CC1101_RXBYTES);
ESP_LOGI(TAG, "RXBYTES=0x%02X", rxb);
// Bit 7: overflow, Bits 6-0: bytes in FIFO

// Verify config registers
cc1101_verify_config(&radio, &rx_cfg);
```

### 5. USB Serial/JTAG Issues

**Symptoms**:
- Host can't connect to COM7
- Capture works once, then fails
- "Device not found" in PulseView

**Causes & Fixes**:
| Cause | Check | Fix |
|-------|-------|-----|
| Driver not installed | Windows Device Manager | Install ESP32 USB JTAG driver |
| Port busy | Another app using COM7 | Close PulseView, monitor |
| Buffer overflow | `USB_INTERFACE_USJ_TX_BUFFER_SIZE` | Increase to 8192 |
| Firmware crash | Check monitor | Fix crash, rebuild |

**Debug Steps**:
```bash
# 1. Check USB device
lsusb  # Linux
# Or Device Manager → Universal Serial Bus devices

# 2. Test basic communication
python -c "
import serial
s = serial.Serial('COM7', 115200, timeout=1, dsrdtr=True, rtscts=True)
s.reset_input_buffer()
s.write(bytes([0x02]))  # ID
import time; time.sleep(0.3)
print(s.read(4))
"

# 3. Check firmware USB init logs
# Should see: "Transport: USB Serial JTAG"
```

### 6. UART Transport Issues (DTR Reset)

**Symptoms**:
- PulseView connects, then "No device"
- Garbage ID response
- Firmware restarts when PulseView opens

**Fixes**:
```bash
# 1. Add 10µF capacitor on EN-GND (hardware fix)
# 2. Enable DTR guard (software)
CONFIG_SUMP_DTR_RESET_GUARD=y
CONFIG_SUMP_DTR_GUARD_MS=3000

# 3. Use USB transport instead (recommended)
CONFIG_SUMP_TRANSPORT_USB=y
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
```

---

## Diagnostic Commands

### Register Dump
```c
// In code
cc1101_dump_registers(&radio);

// Key registers to check:
cc1101_log_registers(&radio);
// Outputs: IOCFG2, IOCFG0, PKTCTRL1, PKTCTRL0, SYNC1/0, PKTLEN, MDMCFG4-2, MCSM1, FREQ2/1/0, MARCSTATE, PKTSTATUS, RSSI
```

### Config Verification
```c
cc1101_verify_config(&radio, &cfg);
// Compares: IOCFG2, IOCFG0, SYNC1/0, PKTLEN, PKTCTRL0, MDMCFG4-1, DEVIATN, MCSM0/1, FREQ2, FREND0
// Logs PASS/FAIL for each
```

### Status Monitor
```c
cc1101_start_status_monitor(&radio, 500);
// Logs every 500ms:
// MARCSTATE=0x0D PKTSTATUS=0x00 RSSI=-74 dBm RXBYTES=0
```

### Frequency Sweep
```c
// Capture RSSI vs frequency
cc1101_freq_sweep(&radio, 430000000, 440000000, 100000, output_callback);
// Output: 1 RSSI byte per 100 kHz step
```

---

## Panic / Crash Analysis

### Guru Meditation Error
```
Guru Meditation Error: Core 0 panic'ed (LoadProhibited). Exception was unhandled.
Core 0 register dump:
PC      : 0x4008xxxx  PS      : 0x00060x30  A0      : 0x8008xxxx  A1      : 0x3ffbxxxx
...
Backtrace: 0x4008xxxx:0x3ffbxxxx 0x4008xxxx:0x3ffbxxxx ...
```

**Analysis**:
```bash
# 1. Get backtrace addresses
# 2. Decode with addr2line
addr2line -pfiaC -e build/RFuzz.elf 0x4008xxxx 0x4008xxxx ...

# 3. Or use IDF monitor (auto-decodes)
idf.py monitor
# Shows decoded backtrace with function names
```

### Common Crash Causes
| Crash | Cause | Fix |
|-------|-------|-----|
| `LoadProhibited` | Null pointer dereference | Check pointer before use |
| `StoreProhibited` | Write to invalid address | Check buffer bounds |
| `IntegerDivideByZero` | Division by zero | Check divisor |
| `IllegalInstruction` | Corrupted flash/IRAM | Reflash, check power |
| `DoubleException` | Exception in exception handler | Fix primary exception |

---

## Performance Debugging

### SPI Timing
```bash
# Scope SPI lines
# CS low time, SCK frequency, MOSI/MISO setup/hold
# CC1101 requires: SCK ≤ 10 MHz, Mode 0
```

### ISR Latency
```c
// Measure GDO0 ISR latency
static uint32_t isr_start, isr_end;

void IRAM_ATTR gdo0_isr(void *arg) {
    isr_start = xTaskGetTickCountFromISR();
    // ... ISR work ...
    isr_end = xTaskGetTickCountFromISR();
}

// In task
ESP_LOGI(TAG, "ISR latency: %lu ticks (%lu us)",
         isr_end - isr_start, (isr_end - isr_start) * portTICK_PERIOD_MS * 1000);
```

### Buffer Overflows
```c
// Check capture buffer
if (s_write_idx >= s_total_samples) {
    ESP_LOGW(TAG, "Capture buffer overflow!");
}

// Check stream buffer
size_t free = xStreamBufferSpacesAvailable(s_stream_buf);
if (free < 64) {
    ESP_LOGW(TAG, "Stream buffer low: %zu bytes free", free);
}
```

---

## AI-Readable Debug Index

```yaml
debugging:
  tools:
    - {name: "Serial Monitor", cmd: "idf.py monitor", purpose: "Logs, assertions"}
    - {name: "GDB+OpenOCD", cmd: "idf.py openocd + gdb", purpose: "Breakpoints, stack"}
    - {name: "SUMP Logic Analyzer", cmd: "capture_sump.py --format vcd", purpose: "GDO timing"}
    - {name: "GTKWave", purpose: "VCD waveform view"}
    - {name: "PulseView", purpose: "Protocol decode (.sr)"}
    - {name: "HackRF+inspectrum", purpose: "RF spectrum/validation"}
  
  common_issues:
    cc1101_no_response:
      symptoms: ["MISO didn't go low", "PARTNUM=0xFF"]
      checks: ["MISO pin", "CS wiring", "SPI mode", "Power", "Crystal"]
      debug: "scope SPI lines during init"
    
    no_capture:
      symptoms: ["Timeout", "0 bytes", "Empty .sr"]
      checks: ["Transport mode", "Port", "Firmware is main.c", "GDO pins", "Timer"]
      debug: "check boot marker 0xAA, send ID command"
    
    tx_fails:
      symptoms: ["wait_tx_done timeout", "No RF signal"]
      checks: ["ISR enabled", "GDO0 mode=0x06", "PA configured", "TX mode set"]
      debug: "poll MARCSTATE, check STX status"
    
    rx_fails:
      symptoms: ["receive_packet false", "RXBYTES=0"]
      checks: ["Freq match", "Sync match", "Mod match", "Rate match", "Dev match", "CRC match", "Preamble", "RX mode"]
      debug: "monitor MARCSTATE/PKTSTATUS/RXBYTES, verify_config"
    
    usb_issues:
      symptoms: ["Can't open COM7", "Disconnects"]
      checks: ["Driver", "Port busy", "TX buffer size", "Firmware crash"]
      debug: "test basic serial comm, check USB init logs"
    
    uart_dtr_reset:
      symptoms: ["PulseView resets board", "Garbage ID"]
      fixes: ["10uF EN-GND", "DTR guard", "Use USB transport"]
  
  diagnostic_commands:
    - cc1101_dump_registers
    - cc1101_log_registers
    - cc1101_verify_config
    - cc1101_start_status_monitor
    - cc1101_freq_sweep
  
  crash_analysis:
    panic_decode: "idf.py monitor (auto) or addr2line -e build/RFuzz.elf"
    common_causes:
      - LoadProhibited: "null pointer"
      - StoreProhibited: "buffer overflow"
      - IntegerDivideByZero: "div by zero"
      - IllegalInstruction: "corrupted flash"
  
  performance:
    spi_timing: "scope CS/SCK/MOSI/MISO"
    isr_latency: "measure in IRAM ISR"
    buffer_overflow: "check s_write_idx vs s_total_samples"
```

---

## Related

- [[07-workflow/development|Development Workflow]]
- [[07-workflow/testing|Testing Workflows]]
- [[02-firmware/cc1101-driver|CC1101 Driver API]]
- [[02-firmware/sump-capture|SUMP Capture Internals]]
- [[05-dragon-os/hackrf-ops|HackRF Operations]]