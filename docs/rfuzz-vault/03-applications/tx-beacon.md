# TX Beacon Firmware — RFuzz_TX.c

> **Source**: `main/RFuzz_TX.c`, `examples/rfuzz_tx_2fsk.c`

---

## Overview

`RFuzz_TX.c` implements a **beacon transmitter** that periodically sends a fixed packet. Used for:
- TX verification with logic analyzer / SDR
- Range testing
- Loopback testing with RX firmware
- Signal source for reverse engineering

Two variants exist:
1. **`main/RFuzz_TX.c`** — GFSK, 38.4 kbps, variable packet, CRC enabled
2. **`examples/rfuzz_tx_2fsk.c`** — 2FSK, 2.4 kbps, fixed packet, no CRC (easier URH decode)

---

## Variant 1: RFuzz_TX.c (GFSK 38.4 kbps)

```c
static const cc1101_config_t beacon_cfg = {
    .freq_hz   = 433920000,
    .channel   = 0,
    .pa_value  = CC1101_PA_0dBm,       // 0 dBm
    .isr_enabled = true,               // GDO0 interrupt for TX done

    .modem = {
        .modulation      = CC1101_MOD_GFSK_E,
        .sync_mode       = CC1101_SYNC_30_32_E,  // 30/32 sync
        .dc_filter_off   = false,
        .manchester      = false,
        .fec_enable      = false,
        .preamble_bytes  = 2,
        .datarate_bps    = 38400,
        .deviation       = 0x03,
        .chanbw          = 0x03,
        .channel_spacing = 248,
    },

    .packet = {
        .mode          = CC1101_PKT_VARIABLE_E,
        .crc_enable    = true,
        .whitening     = false,
        .append_status = true,
        .max_length    = 255,
        .addr_check    = CC1101_ADR_CHK_NONE,
        .sync1         = 0x2D,
        .sync0         = 0xD4,
    },

    .radio = {
        .autocal     = CC1101_AUTOCAL_IDLE_TO_RXTX,
        .pin_mode    = 0x00,
        .pin_output  = true,
        .gdo0_mode   = CC1101_GDO_SYNC_WORD,  // 0x06
        .gdo2_mode   = CC1101_GDO_HIGH_Z,
    },
};
```

**Payload**: `{0xDE, 0xAD, 0xBE, 0xEF}` (4 bytes)

**Key Settings**:
| Parameter | Value |
|-----------|-------|
| Modulation | GFSK |
| Datarate | 38,400 bps |
| Sync | 30/32 bits (0x2DD4...) |
| Preamble | 2 bytes |
| CRC | Enabled |
| TX Power | 0 dBm |
| GDO0 Mode | Sync Word (0x06) — interrupt on TX done |

**Loop**:
```c
while (1) {
    cc1101_transmit(&radio, data, sizeof(data));
    if (cc1101_wait_tx_done(&radio, 500)) {
        ESP_LOGI(TAG, "TX Complete (Hardware Confirmed)");
    } else {
        ESP_LOGW(TAG, "TX Timeout! Interrupt failed.");
    }
    vTaskDelay(pdMS_TO_TICKS(3000));  // 3 second interval
}
```

---

## Variant 2: rfuzz_tx_2fsk.c (2FSK 2.4 kbps)

```c
static const cc1101_config_t tx_cfg = {
    .freq_hz   = 433920000,
    .channel   = 0,
    .pa_value  = CC1101_PA_POS10dBm,   // +10 dBm
    .isr_enabled = false,

    .modem = {
        .modulation      = CC1101_MOD_2FSK_E,
        .sync_mode       = CC1101_SYNC_16_16_E,
        .dc_filter_off   = false,
        .manchester      = false,
        .fec_enable      = false,
        .preamble_bytes  = 4,
        .datarate_bps    = 2400,
        .deviation       = 0x47,    // ~24 kHz deviation
        .chanbw          = 0x03,
        .channel_spacing = 248,
    },

    .packet = {
        .mode          = CC1101_PKT_FIXED_E,
        .crc_enable    = false,
        .whitening     = false,
        .append_status = false,
        .max_length    = 1,
        .addr_check    = CC1101_ADR_CHK_NONE,
        .sync1         = 0xDE,
        .sync0         = 0xAF,
    },

    .radio = {
        .autocal     = CC1101_AUTOCAL_ALWAYS,
        .pin_mode    = 0x00,
        .pin_output  = true,
        .gdo0_mode   = CC1101_GDO_SYNC_WORD,
        .gdo2_mode   = CC1101_GDO_HIGH_Z,
    },
};
```

**Payload**: `{0x01}` (1 byte fixed)

**Key Settings**:
| Parameter | Value |
|-----------|-------|
| Modulation | 2FSK |
| Datarate | 2,400 bps |
| Sync | 16/16 bits (0xDEAF) |
| Preamble | 4 bytes (0xAA AA AA AA) |
| CRC | Disabled |
| TX Power | +10 dBm |
| Packet Mode | Fixed length (1 byte) |

**On-Air Layout**:
```
[AA AA AA AA]  — Preamble (4 bytes of 0xAA = 10101010...)
[DE AF]        — Sync word (16 bits)
[01]           — Payload (1 byte)
```

**Loop** (with diagnostics):
```c
while (1) {
    // Pre-TX diagnostics
    ESP_LOGI(TAG, "Pre-TX: MARCSTATE=0x%02X TXBYTES=0x%02X GDO0=%d",
             marcstate, txbytes, gpio_get_level(PIN_NUM_GDO0));

    uint8_t pkt = 0x01;
    cc1101_transmit(&radio, &pkt, 1);

    // Post-TX strobe diagnostics
    ESP_LOGI(TAG, "Post-TX strobe: MARCSTATE=0x%02X TXBYTES=0x%02X", ...);

    cc1101_wait_tx_done(&radio, 500);

    // Post-TX done diagnostics
    ESP_LOGI(TAG, "Post-TX done: MARCSTATE=0x%02X", ...);

    vTaskDelay(pdMS_TO_TICKS(10000));  // 10 second interval
}
```

---

## Comparison

| Feature | RFuzz_TX.c | rfuzz_tx_2fsk.c |
|---------|------------|-----------------|
| **Modulation** | GFSK | 2FSK |
| **Datarate** | 38.4 kbps | 2.4 kbps |
| **Sync Mode** | 30/32 | 16/16 |
| **Sync Word** | 0x2DD4... | 0xDEAF |
| **Preamble** | 2 bytes | 4 bytes |
| **Packet Mode** | Variable | Fixed |
| **CRC** | Enabled | Disabled |
| **TX Power** | 0 dBm | +10 dBm |
| **Payload** | 4 bytes (DEADBEEF) | 1 byte (0x01) |
| **Interval** | 3 seconds | 10 seconds |
| **ISR** | Enabled | Disabled |
| **Diagnostics** | Minimal | Verbose (MARCSTATE/TXBYTES) |
| **Best For** | General beacon | URH/PulseView analysis |

---

## Building Each Variant

### Default (main.c = RFuzz_TX.c)
```bash
# Already set in main/CMakeLists.txt
idf.py build flash monitor
```

### 2FSK Variant
```bash
# Option 1: Copy example to main
cp examples/rfuzz_tx_2fsk.c main/main.c
idf.py build flash monitor

# Option 2: Build example separately
cd examples/rfuzz_tx_2fsk
# (needs its own CMakeLists.txt)
```

---

## Verification with Host Tools

### Logic Analyzer (SUMP)
```bash
# Capture TX on GDO0 (sync word pulse)
python scripts/capture_sump.py --port COM7 --rate 100000 --samples 50000 --format sr
# In PulseView: look for GDO0 high during sync word transmission
```

### HackRF / SDR (Dragon OS)
```bash
ssh dragon@192.168.1.101
hackrf_transfer -r tx_capture.c8 -f 433920000 -s 2000000 -n 4000000
# Analyze with inspectrum, URH, or GNU Radio
```

### RSSI Monitor
```bash
python scripts/rssi_mon.py 30 rssi_log.txt
# On Dragon OS: watch RSSI during TX
```

---

## Customization

### Change Frequency
```c
.freq_hz = 868300000,  // 868 MHz
.freq_hz = 915000000,  // 915 MHz
```

### Change Payload
```c
uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};  // Up to 255 bytes (variable mode)
```

### Change Interval
```c
vTaskDelay(pdMS_TO_TICKS(1000));  // 1 second
vTaskDelay(pdMS_TO_TICKS(60000)); // 1 minute
```

### Enable Manchester Encoding
```c
.modem.manchester = true,  // Requires 2x oversampling
```

---

## AI-Readable Config

```yaml
tx_beacons:
  - name: "RFuzz_TX (GFSK 38.4k)"
    source: "main/RFuzz_TX.c"
    config:
      freq_hz: 433920000
      modulation: "GFSK"
      datarate_bps: 38400
      sync_mode: "30/32"
      sync_word: "0x2DD4..."
      preamble_bytes: 2
      packet_mode: "VARIABLE"
      crc: true
      payload: "DE AD BE EF"
      tx_power: "0 dBm"
      gdo0_mode: "SYNC_WORD (0x06)"
      isr_enabled: true
      interval_ms: 3000
      diagnostics: minimal
  
  - name: "rfuzz_tx_2fsk (2FSK 2.4k)"
    source: "examples/rfuzz_tx_2fsk.c"
    config:
      freq_hz: 433920000
      modulation: "2FSK"
      datarate_bps: 2400
      sync_mode: "16/16"
      sync_word: "0xDEAF"
      preamble_bytes: 4
      packet_mode: "FIXED"
      crc: false
      payload: "01"
      tx_power: "+10 dBm"
      gdo0_mode: "SYNC_WORD (0x06)"
      isr_enabled: false
      interval_ms: 10000
      diagnostics: verbose (MARCSTATE, TXBYTES, GDO0)
```

---

## Related

- [[03-applications/main-capture|Async RX Capture]]
- [[03-applications/rx-packet|RX Packet Firmware]]
- [[04-host-scripts/tx_verify.py|TX Verify Script]]
- [[05-dragon-os/hackrf-ops|HackRF Operations]]
- [[02-firmware/cc1101-driver|CC1101 Driver API]]