# Host ↔ ESP32 Protocol Reference

> **Protocols**: Custom Raw Streaming, SUMP/OLS, Console UART

---

## Protocol 1: Custom Raw Streaming (capture.py)

Used by `scripts/capture.py` and `scripts/sump_stream_capture.py`.

### Host → ESP32 Commands

| Command | Byte | Arguments | Total Bytes | Description |
|---------|------|-----------|-------------|-------------|
| START | `0x01` | `rate` (u32 LE), `count` (u32 LE) | 9 | Triggered capture |
| TX_TRIGGER | `0x02` | — | 1 | Trigger TX callback |
| STREAM | `0x03` | `rate` (u32 LE) | 5 | Continuous stream start |
| STOP | `0x04` | — | 1 | Stop stream |
| SWEEP | `0x05` | `start` (u32), `end` (u32), `step` (u32) | 13 | Frequency sweep |

**Little-Endian Encoding**:
```python
rate = 24000  # 0x00005E40
bytes = rate.to_bytes(4, 'little')  # b'\x40\x5e\x00\x00'
```

### ESP32 → Host Responses

| Phase | Data | Format |
|-------|------|--------|
| Boot | `0xAA` | Single byte marker |
| Capture | `count` bytes | 1 byte/sample: bit0=GDO0, bit1=GDO2 |
| Stream | Continuous bytes | Same format, until STOP |
| Sweep | 1 byte/step | RSSI value per frequency step |

### Sample Format (1 byte per sample)
```
Bit 0: GDO0 level (0/1)
Bit 1: GDO2 level (0/1) — if 2-channel mode
Bits 2-7: Reserved (0)
```

### Capture Flow (START)
```
Host                    ESP32
  │                        │
  ├─ 0x01 + rate + count──►│
  │                        │  [Timer ISR samples GDO0/GDO2]
  │                        │  [Fills buffer]
  │                        │  [ISR completes, notifies task]
  │◄─ raw bytes ───────────│  [transport_write(buffer, count)]
  │                        │
```

### Stream Flow (STREAM)
```
Host                    ESP32
  │                        │
  ├─ 0x03 + rate──────────►│
  │                        │  [stream_start(rate)]
  │                        │  [Timer ISR → StreamBuffer]
  │◄─ raw bytes ───────────│  [stream_task: StreamBuffer → transport]
  │        ...             │  [Loop until STOP or timeout]
  ├─ 0x04─────────────────►│
  │                        │  [stream_stop()]
  │                        │
```

---

## Protocol 2: SUMP/OLS (capture_sump.py)

Full OLS protocol subset for PulseView compatibility. Used when `CONFIG_SUMP_TRANSPORT_USB=y` or `UART`.

### Host → Device Commands

| Byte | Command | Arguments | Description |
|------|---------|-----------|-------------|
| `0x00` | RESET | — | Reset device |
| `0x01` | RUN | — | Start capture |
| `0x02` | ID | — | Request identification |
| `0x03` | SELFTEST | `pattern` (1 byte) | Self-test with pattern |
| `0x80` | SET_DIV | `divider` (3 bytes LE) | Set sample rate divider |
| `0x81` | SET_COUNT | `read_count` (2 bytes), `delay` (2 bytes) | Set sample count |
| `0x82` | SET_FLAGS | `flags` (1 byte) | Set flags |
| `0xC0` | TRIG_MASK0 | `mask` (1 byte) | Trigger mask ch0 |
| `0xC1` | TRIG_VALUE0 | `value` (1 byte) | Trigger value ch0 |
| `0xC2` | TRIG_CONFIG0 | `config` (1 byte) | Trigger config ch0 |
| `0x04` | GET_METADATA | — | Request metadata |

### Device → Host Responses

#### ID Response (4 bytes)
```
"1ALS"  # OLS signature
"SUMP"  # SUMP signature (alternative)
```

#### Metadata (TLV Format)
```
Byte 0: Tag
Byte 1: Length
Bytes 2..2+Len-1: Value

Tags:
  0x01: Device name (ASCII)
  0x02: Version (ASCII)
  0x21: Memory size (u32 BE)
  0x22: Max sample rate (u32 BE)
  0x23: Protocol version (1 byte)
  0x40: Number of channels (1 byte)
```

#### Capture Data (4-byte blocks)
```
Block format (repeated):
  Byte 0: 8 samples of channel 0 (bit 0 = earliest)
  Byte 1: 8 samples of channel 1
  Byte 2: 8 samples of channel 2 (if 4ch)
  Byte 3: 8 samples of channel 3 (if 4ch)

Our firmware (2 channels):
  Byte 0: 8 samples of GDO0 (bit 0 = earliest)
  Byte 1: 8 samples of GDO2
  Bytes 2-3: 0x00 (unused)
```

### Sample Rate Calculation
```
divider = (clock_freq / sample_rate) - 1
clock_freq = CONFIG_SUMP_CLOCK_FREQ (default 240 MHz)
SET_DIV sends 3 bytes LE (24-bit divider)

read_count_wire = min((num_samples - 1) // 4, 0xFFFF)
actual_samples = (read_count_wire + 1) * 4
# Max 262,144 samples per capture (OLS protocol limit)
```

### Session Flow
```
Host                    Device
  │                        │
  ├─ 0x00 (RESET) ───────►│  [Drain boot log]
  │                        │
  ├─ 0x02 (ID) ──────────►│
  │◄─ "1ALS" ─────────────│  [4 bytes]
  │                        │
  ├─ 0x04 (METADATA) ────►│
  │◄─ TLV metadata ───────│
  │                        │
  ├─ 0x80 + divider ──────►│  [SET_DIV]
  ├─ 0x81 + count + delay►│  [SET_COUNT]
  ├─ 0x82 + flags ────────►│  [SET_FLAGS]
  ├─ 0xC0 + mask ────────►│  [TRIG_MASK0]
  ├─ 0xC1 + value ────────►│  [TRIG_VALUE0]
  ├─ 0xC2 + config ───────►│  [TRIG_CONFIG0]
  │                        │
  ├─ 0x01 (RUN) ──────────►│
  │                        │  [Timer ISR captures samples]
  │                        │  [Fills buffer]
  │◄─ 4-byte blocks ──────│  [Stream until count reached]
  │                        │
```

---

## Protocol 3: ESP-IDF Console (UART0)

Standard ESP-IDF logging output on UART0 (typically COM6 at 115200 baud).

### Log Format
```
E (123) TAG: Error message
W (456) TAG: Warning message
I (789) TAG: Info message
D (999) TAG: Debug message
V (111) TAG: Verbose message
```

### Common Tags
| Tag | Component |
|-----|-----------|
| `MAIN` | main.c app_main |
| `RFUZZ` | RFuzz_TX.c |
| `RFUZZ_RX` | RFuzz_RX.c |
| `CC1101` | CC1101 driver |
| `CAP` | sump_capture |
| `HW_INIT` | hw_init |
| `SPI_INIT` | spi_init |

### Status Monitor Output (cc1101_start_status_monitor)
```
I (500) CC1101: MARCSTATE=0x0D PKTSTATUS=0x00 RSSI=-74 dBm RXBYTES=0
I (1000) CC1101: MARCSTATE=0x0D PKTSTATUS=0x00 RSSI=-73 dBm RXBYTES=0
```

### TX Verification Output (RFuzz_TX.c)
```
I (3000) RFUZZ: Transmitting Beacon...
I (3001) RFUZZ: TX pre-strobe: GDO0=0 isr=1 tx_pending=1
I (3001) RFUZZ: TX started, payload=4 bytes, status=0x01
I (3002) RFUZZ: TX Complete (Hardware Confirmed)
```

---

## Transport Layer Details

### USB Serial/JTAG (Recommended)
- **Port**: Typically COM7 (Windows), /dev/ttyACM0 (Linux)
- **Baud**: Ignored (USB CDC)
- **Flow Control**: None needed
- **Reset on Connect**: **No** — native USB, no DTR/RTS
- **Buffer Sizes**: TX=4096, RX=256 (configurable)

### UART0 (Legacy)
- **Port**: Typically COM6 (Windows), /dev/ttyUSB0 (Linux)
- **Baud**: `CONFIG_SUMP_UART_BAUD` (default 15200, use 921600 for SUMP)
- **Flow Control**: None
- **Reset on Connect**: **Yes** — DTR/RTS toggles EN/BOOT
- **Mitigation**: `CONFIG_SUMP_DTR_RESET_GUARD` drains buffer for 3s

---

## AI-Readable Protocol Spec

```yaml
protocols:
  custom_raw:
    name: "Custom Raw Streaming"
    used_by: ["capture.py", "sump_stream_capture.py"]
    host_to_device:
      - {cmd: 0x01, name: "START", args: ["rate:u32le", "count:u32le"], bytes: 9}
      - {cmd: 0x02, name: "TX_TRIGGER", args: [], bytes: 1}
      - {cmd: 0x03, name: "STREAM", args: ["rate:u32le"], bytes: 5}
      - {cmd: 0x04, name: "STOP", args: [], bytes: 1}
      - {cmd: 0x05, name: "SWEEP", args: ["start:u32le", "end:u32le", "step:u32le"], bytes: 13}
    device_to_host:
      boot: {marker: 0xAA, bytes: 1}
      capture: {format: "raw_bytes", bytes_per_sample: 1, bits: "bit0=GDO0, bit1=GDO2"}
      stream: {format: "continuous_raw_bytes"}
      sweep: {format: "rssi_per_step", bytes_per_step: 1}
    sample_format: "1 byte = 1 sample, bit0=GDO0, bit1=GDO2"
  
  sump_ols:
    name: "SUMP/OLS Protocol"
    used_by: ["capture_sump.py"]
    host_to_device:
      - {cmd: 0x00, name: "RESET", args: []}
      - {cmd: 0x01, name: "RUN", args: []}
      - {cmd: 0x02, name: "ID", args: []}
      - {cmd: 0x03, name: "SELFTEST", args: ["pattern:u8"]}
      - {cmd: 0x80, name: "SET_DIV", args: ["divider:u24le"]}
      - {cmd: 0x81, name: "SET_COUNT", args: ["read_count:u16", "delay:u16"]}
      - {cmd: 0x82, name: "SET_FLAGS", args: ["flags:u8"]}
      - {cmd: 0xC0, name: "TRIG_MASK0", args: ["mask:u8"]}
      - {cmd: 0xC1, name: "TRIG_VALUE0", args: ["value:u8"]}
      - {cmd: 0xC2, name: "TRIG_CONFIG0", args: ["config:u8"]}
      - {cmd: 0x04, name: "GET_METADATA", args: []}
    device_to_host:
      id: {signatures: ["1ALS", "SUMP"], bytes: 4}
      metadata: {format: "TLV", tags: {0x01: "name", 0x02: "version", 0x21: "mem_u32be", 0x22: "max_rate_u32be", 0x23: "proto_ver", 0x40: "num_channels"}}
      capture: {format: "4_byte_blocks", ch0: "byte0[7:0]", ch1: "byte1[7:0]", ch2: "byte2[7:0]", ch3: "byte3[7:0]"}
    limits:
      max_samples_per_capture: 262144
      max_rate_hz: 500000
  
  console_uart:
    name: "ESP-IDF Console"
    port: "UART0 (COM6 default)"
    baud: 115200
    format: "TAG (timestamp) MESSAGE"
    tags: ["MAIN", "RFUZZ", "RFUZZ_RX", "CC1101", "CAP", "HW_INIT", "SPI_INIT"]
  
  transports:
    usb_serial_jtag:
      port: "COM7 / /dev/ttyACM0"
      baud: "ignored"
      dtr_reset: false
      buffers: {tx: 4096, rx: 256}
      recommended: true
    uart0:
      port: "COM6 / /dev/ttyUSB0"
      baud: "CONFIG_SUMP_UART_BAUD (921600 for SUMP)"
      dtr_reset: true
      guard_ms: 3000
      recommended: false
```

---

## Related

- [[04-host-scripts/capture_sump.py|SUMP Capture Script]]
- [[02-firmware/sump-capture|SUMP Capture Internals]]
- [[08-reference/register-map|Register Map]]
- [[08-reference/troubleshooting|Troubleshooting]]