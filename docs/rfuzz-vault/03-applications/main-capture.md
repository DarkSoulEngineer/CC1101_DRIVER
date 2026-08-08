# Default Capture Firmware — main.c

> **Source**: `main/main.c`

---

## Overview

The default firmware (`main.c`) implements a **2FSK packet RX + capture** mode:
- CC1101 configured for 2FSK packet reception (sync word 0xDEAF, fixed 4-byte
  payload) plus async demodulated bit output
- GDO0 = sync strobe (rises on sync match), GDO2 = raw async demodulated bits
- SUMP capture engine samples GDO0/GDO2 at configurable rate
- Host commands via USB Serial/JTAG (or UART0) control capture

**Use Case**: RF signal sniffing, protocol reverse engineering, signal analysis with capture + packet RX in one firmware.

---

## Configuration (Hardcoded in main.c)

```c
cc1101_config_t rx_cfg = CC1101_DEFAULT_CONFIG();
rx_cfg.freq_hz = 433920000;           // 433.92 MHz
rx_cfg.modem.modulation    = CC1101_MOD_2FSK_E;
rx_cfg.modem.sync_mode     = CC1101_SYNC_16_16_E; // 16-bit sync word
rx_cfg.modem.preamble_bytes = 4;
rx_cfg.modem.datarate_bps  = 2400;                    // 2400 baud
rx_cfg.modem.deviation     = 0x47;  // ~47.6 kHz, matches the TX signal
rx_cfg.modem.chanbw        = 0x0C;  // ~203 kHz channel BW
rx_cfg.packet.mode         = CC1101_PKT_FIXED_E;  // fixed-length frames
rx_cfg.packet.crc_enable   = false;
rx_cfg.packet.whitening    = false;
rx_cfg.packet.append_status = false;
rx_cfg.packet.max_length   = 4;
rx_cfg.packet.sync1        = 0xDE;
rx_cfg.packet.sync0        = 0xAF;
rx_cfg.radio.gdo0_mode     = CC1101_GDO_SYNC_WORD;  // 0x06, sync strobe
rx_cfg.radio.gdo2_mode     = CC1101_GDO_ASYNC_DATA; // 0x0D, raw demod bits
rx_cfg.radio.autocal       = CC1101_AUTOCAL_ALWAYS;
rx_cfg.radio.pin_mode      = 0x3F;
rx_cfg.radio.pin_output    = true;
```

> After `cc1101_configure()` the firmware restores two known-good packet
> registers: `PKTCTRL1 = 0x04` and `MCSM1 = 0x3F`.

**Key Settings**:
| Parameter | Value | Reason |
|-----------|-------|--------|
| Modulation | 2FSK | Common for simple remotes/sensors |
| Sync Mode | 16/16 (0xDEAF) | Packet framing for the RX loop |
| Datarate | 2400 bps | Typical for 433 MHz OOK/2FSK remotes |
| Deviation | 0x47 (~47.6 kHz) | Matches the TX signal (gen_2fsk.py, 50 kHz) |
| Channel BW | 0x0C (~203 kHz) | Wide enough for deviation |
| Packet Mode | Fixed, 4 bytes | Test frames |
| GDO0 Mode | Sync Word (0x06) | Sync strobe; anchors packet windows |
| GDO2 Mode | Async Data (0x0D) | Raw demodulated bitstream (captured) |

---

## Startup Sequence

```c
void app_main(void)
{
    // 1. Initialize hardware (SPI + USB)
    if (init_hardware() != ESP_OK) { fail(); }

    // 2. Initialize CC1101
    if (cc1101_init(&s_radio, hw_cc1101_spi,
                    PIN_NUM_CS, PIN_NUM_MISO, PIN_NUM_GDO0) != ESP_OK) { fail(); }

    // 3. Log PARTNUM/VERSION
    ESP_LOGI(TAG, "PARTNUM=0x%02X  VERSION=0x%02X",
             cc1101_read_status_reg(&s_radio, CC1101_PARTNUM),
             cc1101_read_status_reg(&s_radio, CC1101_VERSION));

    // 4. Configure radio for 2FSK packet RX + async GDO2
    if (cc1101_configure(&s_radio, &rx_cfg) != ESP_OK) { fail(); }
    cc1101_set_rx_mode(&s_radio);

    // 5. Diagnostics
    cc1101_log_registers(&s_radio);
    cc1101_start_status_monitor(&s_radio, 500);  // Log status every 500ms

    // 6. Initialize SUMP capture
    gpio_num_t gdo2_pin = (CONFIG_SUMP_GDO2_MODE != 0x2E) ? PIN_NUM_GDO2 : -1;
    if (capture_init(PIN_NUM_GDO0, gdo2_pin) != ESP_OK) { fail(); }

    // 7. Ready
    ESP_LOGI(TAG, "Ready. Commands: [0x01][rate:4LE][count:4LE] capture, "
                 "[0x03][rate:4LE] stream, [0x04] stop stream");
}
```

---

## Host Commands

| Command | Bytes | Description |
|---------|-------|-------------|
| Capture | `0x01` + `rate` (u32 LE) + `count` (u32 LE) | Triggered capture |
| Stream | `0x03` + `rate` (u32 LE) | Continuous stream |
| Stop | `0x04` | Stop stream |
| TX Trigger | `0x02` | Calls `capture_set_tx_cb()` callback |
| Sweep | `0x05` + `start` + `end` + `step` | Frequency sweep |

**Default TX Callback**: None (set via `capture_set_tx_cb()`)

---

## Usage with Host Scripts

### Simple Raw Capture (`capture.py`)
```bash
python scripts/capture.py --port COM7 --rate 24000 --samples 100000
# Output: capture_YYYYMMDD_HHMMSS.sr (Sigrok session)
```

### SUMP/OLS Capture (`capture_sump.py`) — Recommended
```bash
python scripts/capture_sump.py --port COM7 --rate 24000 --samples 100000 --format sr
# Output: capture_YYYYMMDD_HHMMSS.sr (PulseView compatible)
```

### Continuous Stream
```bash
python scripts/sump_stream_capture.py
# Streams for 10 seconds at 24 kHz
```

---

## Sample Rate Selection

| Target Signal | Oversample | Capture Rate | Notes |
|---------------|------------|--------------|-------|
| 2400 baud 2FSK | 10x | 24,000 Hz | Default, good for analysis |
| 2400 baud 2FSK | 20x | 48,000 Hz | Better edge resolution |
| 9600 baud GFSK | 10x | 96,000 Hz | Higher rate signals |
| Unknown | 50x | 120,000 Hz | Oversample for unknown baud |

**Max**: 250 kHz sustained (the 160 MHz capture ISR trips the interrupt watchdog
for alarm < 4 µs); the firmware clamps stream rates to 500 kHz but that only
works for short bursts.

---

## Output Analysis

### Sigrok PulseView
1. Open `.sr` file in PulseView
2. Add **UART** or **Manchester** decoder
3. Set baud rate (e.g., 2400)
4. Decode packets

### Python Analysis
```python
import zipfile

with zipfile.ZipFile("capture_20240101_120000.sr") as zf:
    metadata = zf.read("metadata").decode()
    logic = zf.read("logic-1-1")

# logic[i] = sample i: bit0=GDO0, bit1=GDO2
samples = [(b & 1, (b >> 1) & 1) for b in logic]
```

---

## Status Monitor Output

Every 500ms (configurable):
```
MARCSTATE=0x0D PKTSTATUS=0x00 RSSI=-74 dBm RXBYTES=0
```

| Field | Meaning |
|-------|---------|
| MARCSTATE=0x0D | RX state (0x0D = RX) |
| PKTSTATUS | Packet status flags |
| RSSI | Received signal strength |
| RXBYTES | Bytes in RX FIFO (0 in async mode) |

---

## Customization

### Change Frequency
```c
rx_cfg.freq_hz = 868300000;  // 868 MHz
rx_cfg.freq_hz = 915000000;  // 915 MHz
```

### Change Datarate
```c
rx_cfg.modem.datarate_bps = 9600;  // Adjust deviation/chanbw accordingly
```

### Change Modulation
```c
rx_cfg.modem.modulation = CC1101_MOD_GFSK_E;  // Gaussian FSK
rx_cfg.modem.modulation = CC1101_MOD_ASK_E;   // ASK/OOK
```

### Disable GDO2 (1-channel)
```c
rx_cfg.radio.gdo2_mode = CC1101_GDO_HIGH_Z;  // 0x2E
// Or in Kconfig: CONFIG_SUMP_GDO2_MODE=46 (0x2E)
```

---

## AI-Readable Config

```yaml
main_capture:
  name: "2FSK Packet RX + Capture"
  source: "main/main.c"
  config:
    freq_hz: 433920000
    modulation: "2FSK"
    sync_mode: "16/16"
    sync_bytes: "DEAF"
    preamble_bytes: 4
    datarate_bps: 2400
    deviation_reg: 0x47  # ~47.6 kHz, matches TX
    chanbw_reg: 0x0C
    packet_mode: "FIXED"
    max_length: 4
    crc: false
    whitening: false
    append_status: false
    gdo0_mode: 0x06  # SYNC_WORD strobe
    gdo2_mode: "CONFIG_SUMP_GDO2_MODE (default 0x0D ASYNC_DATA)"
    autocal: "ALWAYS"
    pin_mode: 0x3F
    patched_after_configure: ["PKTCTRL1=0x04", "MCSM1=0x3F"]
  status_monitor_period_ms: 500
  capture_gpio:
    gdo0: "PIN_NUM_GDO0 (default 3)"
    gdo2: "PIN_NUM_GDO2 (default 4) if gdo2_mode != 0x2E"
  commands:
    - {cmd: 0x01, name: "capture", args: "rate(u32), count(u32)"}
    - {cmd: 0x03, name: "stream", args: "rate(u32)"}
    - {cmd: 0x04, name: "stop"}
    - {cmd: 0x02, name: "tx_trigger", callback: "capture_set_tx_cb"}
    - {cmd: 0x05, name: "sweep", args: "start,end,step"}
```

---

## Related

- [[03-applications/tx-beacon|TX Beacon Firmware]]
- [[03-applications/rx-packet|RX Packet Firmware]]
- [[04-host-scripts/capture.py|Raw Capture Script]]
- [[04-host-scripts/capture_sump.py|SUMP Capture Script]]
- [[02-firmware/sump-capture|SUMP Capture Internals]]
- [[02-firmware/cc1101-driver|CC1101 Driver API]]