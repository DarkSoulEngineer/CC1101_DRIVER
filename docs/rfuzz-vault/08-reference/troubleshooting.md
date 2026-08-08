# Troubleshooting Guide

> Common issues, symptoms, causes, and solutions.

---

## Quick Diagnostic Checklist

Before diving deep, verify:
- [ ] ESP32 powered, LED on
- [ ] CC1101 module connected (check all 6 wires)
- [ ] Antenna attached to CC1101
- [ ] Correct COM port selected (Device Manager)
- [ ] Firmware matches use case (main.c vs TX/RX)
- [ ] Kconfig matches hardware (pins, frequency)

---

## Hardware Issues

### CC1101 Not Detected
**Symptoms**:
```
E CC1101: Reset failed: MISO didn't go low after CS
E CC1101: Reset failed: MISO didn't go low after SRES
PARTNUM=0xFF VERSION=0xFF
```

| Cause | Check | Fix |
|-------|-------|-----|
| MISO pin wrong | `CONFIG_CC1101_PIN_MISO` vs wiring | Verify GPIO 6 (default) |
| CS pin wrong | `CONFIG_CC1101_PIN_CS` vs wiring | Verify GPIO 5 (default) |
| SPI pins wrong | SCK/MOSI/MISO mapping | Check `spi_init.c` |
| CC1101 not powered | Measure VCC at module | 3.3V required |
| CC1101 damaged | Try another module | Replace module |
| Crystal not running | Scope XOSC pins (26 MHz) | Check crystal/soldering |

**Debug**: Scope SPI during init — CS pulses low, SCK 1 MHz, MOSI sends 0x30 (SRES), MISO should go low.

---

### No RF Signal / Weak Signal
**Symptoms**: HackRF shows no peak, RSSI very low (-100 dBm)

| Cause | Check | Fix |
|-------|-------|-----|
| No antenna | Visual inspect | Attach antenna |
| Wrong antenna | Frequency match? | 433 MHz antenna for 433 MHz |
| Antenna disconnected | SMA/u.FL connection | Reseat connector |
| PA not configured | `PA_VALUE` / `PATABLE` | Check `cc1101_set_tx_power` |
| TX not actually running | `cc1101_wait_tx_done` timeout | Debug MARCSTATE |
| Frequency mismatch | TX vs RX freq | Match exactly |

---

### GDO Pins Not Working
**Symptoms**: No capture data, GDO0/GDO2 always 0

| Cause | Check | Fix |
|-------|-------|-----|
| Wrong GPIO | `CONFIG_CC1101_PIN_GDO0/2` | Match wiring (default 3/4) |
| Pin not configured as input | `hw_init_gdo0_input()` | Check logs |
| GDO mode wrong | `IOCFG0/IOCFG2` values | 0x0D for async, 0x06 for sync |
| CC1101 not in RX | `MARCSTATE != 0x0D` | Call `cc1101_set_rx_mode()` |

---

## Firmware Issues

### Build Fails
| Error | Cause | Fix |
|-------|-------|-----|
| `sdkconfig not found` | First build | Run `idf.py menuconfig` |
| `Component 'X' not found` | Missing CMakeLists.txt | Check `components/X/CMakeLists.txt` |
| `Multiple definitions` | Duplicate symbols | Check `REQUIRES` vs `PRIV_REQUIRES` |
| `Undefined reference` | Missing `REQUIRES` | Add component to `REQUIRES` |

---

### Runtime Crashes
**Guru Meditation Error**:
```
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
```

| Exception | Common Cause | Fix |
|-----------|--------------|-----|
| `LoadProhibited` | Null pointer dereference | Check pointers before use |
| `StoreProhibited` | Buffer overflow | Check array bounds |
| `IntegerDivideByZero` | Division by zero | Guard divisors |
| `IllegalInstruction` | Corrupted flash | Erase flash, reflash |
| `DoubleException` | Exception in handler | Fix primary exception |

**Debug**:
```bash
idf.py monitor  # Auto-decodes backtrace
# Or:
addr2line -pfiaC -e build/RFuzz.elf <addresses>
```

---

### Watchdog Reset
**Symptoms**: Random resets, "RTCWDT" or "TASKWDT" in logs

| Cause | Fix |
|-------|-----|
| Task starved (no yield) | Add `vTaskDelay()` in loops |
| ISR too long | Move work to task, keep ISR minimal |
| Infinite loop | Check loop conditions |
| `printf` in ISR | Use `ESP_EARLY_LOG` or queue to task |

---

## SUMP Capture Issues

### "No SUMP Device Found"
**Symptoms**: "No SUMP device found"

| Cause | Fix |
|-------|-----|
| Firmware not capture mode | Flash `main.c`, not TX/RX examples |
| Wrong transport | Match `CONFIG_SUMP_TRANSPORT` to script port |
| Wrong port | Try COM7 (USB) or COM6 (UART) |
| UART DTR reset | Use USB transport, or add 10µF EN-GND |
| Boot log not drained | Increase `CONFIG_SUMP_DTR_GUARD_MS` |

**Debug**:
```bash
# Check boot marker
python -c "
import serial, time
s = serial.Serial('COM7', 115200, timeout=1)
time.sleep(1)
print('Boot:', s.read(20).hex())
"

# Manual ID command
python -c "
import serial, time
s = serial.Serial('COM7', 115200, timeout=1)
time.sleep(1)
for _ in range(5):
    s.write(bytes([0x00]))  # RESET
    time.sleep(0.3)
    s.write(bytes([0x02]))  # ID
    time.sleep(0.3)
    print('ID:', s.read(4))
"
```

---

### Capture Timeout
**Symptoms**: "Timeout after 30s, got 0/100000 bytes"

| Cause | Fix |
|-------|-----|
| Sample rate too low for count | Increase timeout: `--timeout 60` |
| Firmware not sampling | Check GDO pins, timer init |
| Transport stalled | USB buffer full, increase `USB_INTERFACE_USJ_TX_BUFFER_SIZE` |

**Timeout Formula**:
```python
timeout = max(30.0, (count / rate) * 2.5 + 5.0)
```

---

### Garbage / Corrupted Data
**Symptoms**: Random bits, no recognizable signal

| Cause | Fix |
|-------|-----|
| Wrong sample rate | Match rate to signal (10x baud minimum) |
| Async vs sync mismatch | Firmware async RX needs async capture |
| GDO2 mode wrong | 0x0D for async, 0x2E for disabled |
| UART baud mismatch | Match `CONFIG_SUMP_UART_BAUD` to PulseView |

---

## TX/RX Communication Issues

### TX Works but RX Fails
**Symptoms**: HackRF sees TX, ESP32 RX gets nothing

| Cause | Fix |
|-------|-----|
| Frequency mismatch | Match `freq_hz` exactly |
| Sync word mismatch | Match `sync1`/`sync0` (0xDEAF) |
| Modulation mismatch | Both 2FSK or both GFSK |
| Datarate mismatch | Match `datarate_bps` |
| Deviation mismatch | Match `deviation` register |
| Preamble too short | RX preamble ≥ TX preamble |
| CRC enabled one side | Both CRC on or both off |
| Address filtering | `ADR_CHK_NONE` on both |

**Debug**: Use `cc1101_verify_config()` on both sides.

---

### RX Works but TX Fails
**Symptoms**: ESP32 RX receives, but TX not seen on HackRF

| Cause | Fix |
|-------|-----|
| PA not configured | Check `PA_VALUE` / `PATABLE` |
| GDO0 ISR not enabled | `CONFIG_CC1101_ISR_ENABLE=y` for `wait_tx_done` |
| GDO0 mode wrong | `IOCFG0 = 0x06` (SYNC_WORD) |
| Not calling `set_tx_mode` | `cc1101_set_tx_mode()` before TX |
| Crystal off | Check `MARCSTATE` progression |

---

## USB / Serial Issues

### Can't Open COM Port
| Error | Cause | Fix |
|-------|-------|-----|
| "Access denied" | Port in use | Close PulseView, monitor, other apps |
| "File not found" | Wrong port | Check Device Manager |
| "Permission denied" (Linux) | User not in dialout | `sudo usermod -a -G dialout $USER` |

---

### USB Disconnects During Capture
| Cause | Fix |
|-------|-----|
| TX buffer overflow | Increase `USB_INTERFACE_USJ_TX_BUFFER_SIZE` to 8192 |
| Firmware crash | Check monitor for panic |
| Power issue | Use powered hub, check USB cable |

---

### UART Transport Unreliable
**Symptoms**: PulseView connects then fails, garbage ID

| Cause | Fix |
|-------|-----|
| DTR reset | **Use USB transport instead** |
| 10µF capacitor | Add on EN-GND |
| DTR guard too short | Increase `CONFIG_SUMP_DTR_GUARD_MS` to 5000 |
| Baud mismatch | Match exactly: firmware `CONFIG_SUMP_UART_BAUD` = PulseView baud |

---

## HackRF / Dragon OS Issues

### "No HackRF Found"
| Cause | Fix |
|-------|-----|
| Not connected | Check `lsusb`, USB cable |
| Permission denied | `sudo usermod -a -G plugdev $USER` |
| Firmware outdated | `hackrf_info`, update if needed |
| USB power | Use powered hub |

---

### HackRF Capture Empty
| Cause | Fix |
|-------|-----|
| Wrong frequency | Match ESP32 TX freq |
| Wrong sample rate | 2 MS/s minimum for sub-GHz |
| Gain too low | Increase LNA/VGA: `-l 32 -g 40` |
| Antenna | Attach correct antenna |

---

### SSH Connection Issues
| Error | Fix |
|-------|-----|
| Connection timeout | Check IP, ping 192.168.1.101 |
| Permission denied | Use SSH key: `ssh-copy-id dragon@192.168.1.101` |
| Host key changed | `ssh-keygen -R 192.168.1.101` |
| Slow transfer | Use `rsync -z` or `scp -C` |

---

## Performance Issues

### Low Sample Rate Achieved
| Cause | Fix |
|-------|-----|
| ISR too slow | Move work out of ISR, use IRAM_ATTR |
| Stream buffer full | Increase `CAPTURE_STREAM_BUF_SIZE` |
| USB write blocking | Increase chunk size, check `transport_write_timeout` |

---

### High CPU Usage
| Cause | Fix |
|-------|-----|
| Status monitor too frequent | Increase period: `cc1101_start_status_monitor(dev, 1000)` |
| Logging too verbose | Reduce log level in menuconfig |
| Busy wait loops | Replace with `vTaskDelay()` or notifications |

---

## AI-Readable Troubleshooting Index

```yaml
troubleshooting:
  categories:
    hardware:
      cc1101_not_detected:
        symptoms: ["MISO didn't go low", "PARTNUM=0xFF"]
        causes: ["wrong MISO pin", "wrong CS pin", "SPI pins", "no power", "bad crystal"]
        debug: "scope SPI during init"
      
      no_rf_signal:
        symptoms: ["no peak on HackRF", "RSSI -100 dBm"]
        causes: ["no antenna", "wrong antenna", "PA not configured", "TX not running", "freq mismatch"]
      
      gdo_pins_not_working:
        symptoms: ["capture all zeros", "GDO always 0"]
        causes: ["wrong GPIO", "not input", "wrong GDO mode", "not in RX"]
    
    firmware:
      build_fails:
        causes: ["sdkconfig missing", "missing CMakeLists.txt", "duplicate symbols", "missing REQUIRES"]
      
      runtime_crash:
        exceptions:
          LoadProhibited: "null pointer"
          StoreProhibited: "buffer overflow"
          IntegerDivideByZero: "div by zero"
          IllegalInstruction: "corrupted flash"
          DoubleException: "exception in handler"
        debug: "idf.py monitor, addr2line"
      
      watchdog_reset:
        causes: ["task starved", "ISR too long", "infinite loop", "printf in ISR"]
    
    sump_capture:
      no_device_found:
        causes: ["wrong firmware", "transport mismatch", "wrong port", "DTR reset", "boot log not drained"]
        debug: "check boot marker 0xAA, manual ID command"
      
      capture_timeout:
        causes: ["rate too low for count", "firmware not sampling", "transport stalled"]
        fix: "increase timeout, check GDO pins, increase USB buffer"
      
      garbage_data:
        causes: ["wrong sample rate", "async/sync mismatch", "GDO2 mode wrong", "baud mismatch"]
    
    tx_rx_comms:
      tx_works_rx_fails:
        causes: ["freq mismatch", "sync mismatch", "mod mismatch", "rate mismatch", "dev mismatch", "preamble", "CRC mismatch", "addr filtering"]
        debug: "cc1101_verify_config on both sides"
      
      rx_works_tx_fails:
        causes: ["PA not configured", "ISR not enabled", "GDO0 mode wrong", "not set_tx_mode", "crystal off"]
    
    usb_serial:
      cant_open_port:
        causes: ["port in use", "wrong port", "permissions"]
      
      usb_disconnects:
        causes: ["TX buffer overflow", "firmware crash", "power issue"]
        fixes: ["increase USB buffer", "check monitor", "powered hub"]
      
      uart_unreliable:
        causes: ["DTR reset", "baud mismatch"]
        fixes: ["USE USB TRANSPORT", "10uF EN-GND", "increase DTR guard", "match baud exactly"]
    
    hackrf:
      not_found:
        causes: ["not connected", "permissions", "firmware old", "USB power"]
      
      empty_capture:
        causes: ["wrong freq", "wrong rate", "gain low", "no antenna"]
    
    ssh:
      connection_issues:
        causes: ["wrong IP", "key not authorized", "host key changed", "slow transfer"]
        fixes: ["ping", "ssh-copy-id", "ssh-keygen -R", "rsync -z"]
```

---

## Related

- [[07-workflow/debugging|Debugging Guide]]
- [[07-workflow/development|Development Workflow]]
- [[05-dragon-os/ssh-setup|Dragon OS SSH Setup]]
- [[04-host-scripts/index|Host Scripts]]
- [[02-firmware/cc1101-driver|CC1101 Driver API]]