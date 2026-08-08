# SUMP Capture Internals

> **Source**: `components/sump_capture/src/sump_capture.c`, `components/sump_capture/include/sump_capture.h`, `components/sump_capture/Kconfig`

---

## Overview

The `sump_capture` component implements a **SUMP/OLS compatible logic analyzer** on the ESP32-S3, sampling CC1101 GDO0/GDO2 pins at up to **500 kS/s** using a hardware timer ISR.

**Key Features**:
- 1 or 2 channels (GDO0 + GDO2)
- Timer-driven sampling (GPTIMER) → deterministic, low jitter
- Two modes: **Triggered Capture** (buffered) and **Continuous Stream**
- SUMP/OLS protocol subset for PulseView/Sigrok compatibility
- Dual transport: **USB Serial/JTAG** (recommended) or **UART0**

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              sump_capture                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────────┐     ┌─────────────────┐     ┌────────────────────────┐   │
│  │  GPTIMER     │────►│  Timer ISR      │────►│  Capture Buffer        │   │
│  │  (1μs res)   │     │  (IRAM)         │     │  [s_capture_buf]       │   │
│  │  Alarm =     │     │  sample =       │     │  1 byte/sample         │   │
│  │  1e6/rate    │     │  (GDO0\|GDO2<<1)│     │  Max: CONFIG_SUMP_     │   │
│  └──────────────┘     └─────────────────┘     │  MAX_SAMPLES (100k)    │   │
│                          │                    └───────────┬────────────┘   │
│                          │                            │                  │
│                    ┌─────┴─────┐                       │                  │
│                    ▼           ▼                       ▼                  │
│            ┌───────────┐ ┌───────────┐         ┌──────────────┐          │
│            │  Stream   │ │  Triggered│         │  stream_task │          │
│            │  Buffer   │ │  Capture  │         │  (CPU0,      │          │
│            │ (2KB ring)│ │  (blocking)         │  prio 2)     │          │
│            └─────┬─────┘ └─────┬─────┘         └──────┬───────┘          │
│                  │             │                    │                    │
│                  ▼             ▼                    ▼                    │
│            ┌──────────────────────────────────────────────────┐           │
│            │              Transport Layer                      │           │
│            │  USB Serial/JTAG  OR  UART0 (Kconfig selectable) │           │
│            └──────────────────────────────────────────────────┘           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Sampling — Timer ISR

```c
// components/sump_capture/src/sump_capture.c:timer_isr_cb()
static IRAM_ATTR bool timer_isr_cb(gptimer_handle_t timer,
                                    const gptimer_alarm_event_data_t *edata,
                                    void *user_data)
{
    uint8_t sample = 0;
    if (gpio_get_level(g_state.gdo0_pin)) sample |= 0x01;  // bit 0
    if (g_state.gdo2_pin >= 0 && gpio_get_level(g_state.gdo2_pin)) sample |= 0x02;  // bit 1

    if (s_streaming) {
        // Continuous stream: push to FreeRTOS StreamBuffer
        xStreamBufferSendFromISR(s_stream_buf, &sample, 1, &wake);
        return wake;
    }

    // Triggered capture: write to linear buffer
    if (!s_capture_active) return false;
    if (s_write_idx >= s_total_samples) {
        s_capture_active = false;
        gptimer_stop(timer);
        s_done = true;
        vTaskNotifyGiveFromISR(s_stream_task, &wake);
        return wake;
    }
    s_capture_buf[s_write_idx++] = sample;
    return false;
}
```

**Key Points**:
- Runs in **IRAM** (`IRAM_ATTR`) for deterministic timing
- 1 sample = 1 byte: `bit0=GDO0`, `bit1=GDO2`
- Timer resolution: 1 MHz → alarm = `1,000,000 / sample_rate_hz` ticks
- Max sustained sample rate: **250 kHz** (alarm < 4 µs trips the interrupt
  watchdog on the 160 MHz build; the firmware clamps stream requests to
  500 kHz, which only survives for short bursts)

> **⚠ Actual rate vs requested**: `alarm_count` uses integer division. The real sample rate is
> `1,000,000 / (1,000,000 / rate)` (integer). E.g. requesting `48000` yields alarm=20 µs →
> **actual 50 000 Hz** (not 48 000). Host scripts that compute bit timing must use the *actual*
> rate. For a CC1101 at 2400 bps captured with `rate=48000`, the bit cell is `50000/2400 = 20.833`
> samples/bit — decoding at 20 samples/bit drifts ~0.83 samples/bit and corrupts the payload.
> Requested rates that divide 1 000 000 exactly (e.g. 50 000, 25 000, 100 000) avoid this.

---

## Capture Modes

### 1. Triggered Capture (Command `0x01`)

**Host sends**: `[0x01][rate:4 LE][count:4 LE]` (9 bytes)

**Flow**:
1. `stream_task` receives command, reconfigures timer
2. Clears buffer, sets `s_write_idx=0`, `s_total_samples=count`
3. Starts timer, waits for ISR notification (`ulTaskNotifyTake`)
4. ISR fills `s_capture_buf[]` until `count` reached
5. ISR stops timer, notifies task
6. Task streams raw buffer via `transport_write()`
7. Returns to command loop

**Timing**: Firmware buffers entire capture, then streams. Host timeout must account for capture duration.

### 2. Continuous Stream (Command `0x03`)

**Host sends**: `[0x03][rate:4 LE]` (5 bytes)

**Flow**:
1. `stream_task` receives command, calls `stream_start(rate)`
2. Reconfigures timer, resets StreamBuffer, sets `s_streaming=true`
3. Timer ISR pushes samples to `s_stream_buf` (ring buffer)
4. Task loop: `xStreamBufferReceive()` → `transport_write_timeout()`
5. Watches for `0x04` (STOP) via `transport_read_available()`
6. On stop: `stream_stop()`, returns to command loop

**Advantage**: Real-time streaming, no capture duration limit.

### 3. Frequency Sweep (Command `0x05`)

**Host sends**: `[0x05][start_hz:4][end_hz:4][step_hz:4]` (13 bytes)

**Flow**:
1. Task calls registered sweep callback: `s_sweep_cb(start, end, step)`
2. Default: `cc1101_freq_sweep()` (from cc1101 component)
3. Sweeps frequency, reads RSSI, streams bytes via `capture_send()`
4. Returns to command loop

---

## Transport Layer

### USB Serial/JTAG (Default, Recommended)

```c
// transport_init()
if (!usb_serial_jtag_is_driver_installed()) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
}

// transport_write() — chunks to fit TX ring buffer
const size_t chunk = 1024;
while (written < len) {
    int w = usb_serial_jtag_write_bytes(data + written, min(chunk, len - written), pdMS_TO_TICKS(1000));
    ...
}
```

**Advantages**:
- No DTR/RTS reset when host opens port
- Native USB, higher throughput
- PulseView connects to COM7 (USB JTAG)
- IDF console on UART0 (COM6) — separate!

### UART0 (Legacy)

```c
// transport_init()
uart_config_t cfg = { .baud_rate = CONFIG_SUMP_UART_BAUD, ... };
uart_driver_install(UART_NUM_0, 8192, 0, 0, NULL, 0);
uart_param_config(UART_NUM_0, &cfg);
```

**Issues**:
- PulseView toggles DTR → resets ESP32 via CH340/CP2102
- Boot log (115200 baud) corrupts SUMP handshake (921600 baud)
- Requires `CONFIG_SUMP_DTR_RESET_GUARD` (drains buffer 3s)

---

## Command Protocol

### Host → ESP32

| Command | Bytes | Description |
|---------|-------|-------------|
| `CAPTURE_CMD_START` (0x01) | `0x01` + rate(4 LE) + count(4 LE) | Triggered capture |
| `CAPTURE_CMD_TX` (0x02) | `0x02` | Trigger TX callback |
| `CAPTURE_CMD_STREAM` (0x03) | `0x03` + rate(4 LE) | Start continuous stream |
| `CAPTURE_CMD_STOP` (0x04) | `0x04` | Stop stream |
| `CAPTURE_CMD_SWEEP` (0x05) | `0x05` + start(4) + end(4) + step(4) | Frequency sweep |

### ESP32 → Host

| Phase | Data |
|-------|------|
| Boot | `0xAA` (marker) |
| Capture | `count` raw bytes (1 byte/sample) |
| Stream | Continuous raw bytes until STOP |
| Sweep | 1 RSSI byte per frequency step |

**Sample Format** (1 byte per sample):
```
bit 0: GDO0 level
bit 1: GDO2 level (if 2-channel)
bits 2-7: Reserved (0)
```

---

## Configuration (Kconfig)

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_SUMP_TRANSPORT` | USB | USB Serial/JTAG or UART0 |
| `CONFIG_SUMP_UART_BAUD` | 15200 | UART baud (also console baud in USB mode) |
| `CONFIG_SUMP_CLOCK_FREQ` | 240000000 | Reference clock for divider calc |
| `CONFIG_SUMP_MAX_SAMPLES` | 100000 | Capture buffer size (bytes) |
| `CONFIG_SUMP_NUM_CHANNELS` | 2 | 1 or 2 channels |
| `CONFIG_SUMP_GDO2_MODE` | 13 (0x0D) | IOCFG2 value for GDO2 |
| `CONFIG_SUMP_DTR_RESET_GUARD` | y | Drain UART buffer on boot |
| `CONFIG_SUMP_DTR_GUARD_MS` | 3000 | Drain duration |

> The GDO2 **pin** is not a SUMP option — `capture_init()` receives
> `PIN_NUM_GDO2`, which is `CONFIG_CC1101_PIN_GDO2` from `hw_init.h`
> (the cc1101 Kconfig). A former `CONFIG_SUMP_GDO2_PIN` was removed:
> it duplicated the cc1101 pin and nothing ever read it.

---

## Integration with cc1101

```c
// main.c: capture_init() called after cc1101_configure()
gpio_num_t gdo2_pin = (CONFIG_SUMP_GDO2_MODE != 0x2E) ? PIN_NUM_GDO2 : -1;
esp_err_t ret = capture_init(PIN_NUM_GDO0, gdo2_pin);

// Optional: register callbacks
capture_set_tx_cb(my_tx_callback);          // Called on 0x02 command
capture_set_freq_sweep_cb(cc1101_freq_sweep); // Called on 0x05 command
```

**GDO2 Mode Selection**:
- `0x0D` (Async Data): Same demodulated bits as GDO0 — **default for 2-ch capture**
- `0x06` (Sync Word): Packet timing reference
- `0x07` (CRC OK): Data quality
- `0x2E` (High-Z): **Disables GDO2 → 1-channel mode**

---

## Host Tools

| Script | Protocol | Output |
|--------|----------|--------|
| `scripts/capture.py` | Custom raw streaming | `.sr` (Sigrok) |
| `scripts/capture_sump.py` | Full SUMP/OLS | `.sr`, `.vcd`, `.bin`, `.hex` |
| `scripts/sump_stream_capture.py` | Continuous stream | Debug/analysis |

See [[04-host-scripts/capture_sump.py|SUMP Capture Script]] for details.

---

## Performance Limits

| Parameter | Limit | Notes |
|-----------|-------|-------|
| Max Sample Rate (sustained) | 250 kHz | ISR overhead, GPIO read latency; alarm < 4 µs trips the watchdog |
| Max Capture Buffer | 4M samples | `CONFIG_SUMP_MAX_SAMPLES`, PSRAM if >~300k |
| Stream Buffer | 2 KB | `CAPTURE_STREAM_BUF_SIZE`, ring buffer |
| USB Write Chunk | 1 KB | `transport_write()` chunk size |
| Timer Resolution | 1 μs | GPTIMER at 1 MHz |

---

## AI-Readable Spec

```yaml
sump_capture:
  max_rate_hz: 500000
  channels: [1, 2]
  sample_format: "bit0=GDO0, bit1=GDO2"
  timer:
    type: "GPTIMER"
    resolution_hz: 1000000
    iram_isr: true
  modes:
    triggered:
      cmd: 0x01
      args: "rate(u32 LE), count(u32 LE)"
      buffering: "linear buffer (CONFIG_SUMP_MAX_SAMPLES)"
      flow: "blocking ISR → transport_write"
    stream:
      cmd: 0x03
      args: "rate(u32 LE)"
      buffering: "FreeRTOS StreamBuffer (2KB)"
      flow: "ISR → StreamBuffer → stream_task → transport"
    sweep:
      cmd: 0x05
      args: "start(u32), end(u32), step(u32) LE"
      callback: "capture_set_freq_sweep_cb()"
  transports:
    - name: "USB Serial/JTAG"
      recommended: true
      dtr_reset: false
      console_separate: true
    - name: "UART0"
      recommended: false
      dtr_reset: true
      guard_ms: 3000
  kconfig_prefix: "CONFIG_SUMP_"
  boot_marker: 0xAA
```

---

## Related

- [[02-firmware/architecture|Firmware Architecture]]
- [[02-firmware/cc1101-driver|CC1101 Driver]]
- [[03-applications/main-capture|Default Capture Firmware]]
- [[04-host-scripts/capture_sump.py|SUMP Capture Script]]
- [[06-build-config/kconfig|Kconfig Reference]]