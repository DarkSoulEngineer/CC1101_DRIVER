# RX Packet Firmware — RFuzz_RX.c

> **Source**: `main/RFuzz_RX.c`

---

## Overview

`RFuzz_RX.c` implements a **packet receiver** that matches the TX beacon configuration (2FSK, 2.4 kbps, sync 0xDEAF). It receives packets with CRC checking and logs parsed payload.

**Use Case**: Validate TX beacon, receive sensor/remote data, protocol analysis with packet framing.

---

## Configuration (Matches rfuzz_tx_2fsk.c)

```c
static const cc1101_config_t rx_cfg = {
    .freq_hz   = 433920000,
    .channel   = 0,
    .pa_value  = CC1101_PA_0dBm,
    .isr_enabled = false,

    .modem = {
        .modulation      = CC1101_MOD_2FSK_E,
        .sync_mode       = CC1101_SYNC_16_16_E,   // Must match TX
        .dc_filter_off   = false,
        .manchester      = false,
        .fec_enable      = false,
        .preamble_bytes  = 4,                      // Must match TX
        .datarate_bps    = 2400,                   // Must match TX
        .deviation       = 0x47,                   // ~12 kHz (was 0x47 in RX, 0x47 in TX example)
        .chanbw          = 0x03,
        .channel_spacing = 248,
    },

    .packet = {
        .mode          = CC1101_PKT_VARIABLE_E,    // Variable length
        .crc_enable    = false,                    // TX has CRC disabled
        .whitening     = false,
        .append_status = true,                     // RSSI + CRC in FIFO
        .max_length    = 64,
        .addr_check    = CC1101_ADR_CHK_NONE,
        .sync1         = 0x2D,                     // Must match TX (0xDE)
        .sync0         = 0xD4,                     // Must match TX (0xAF)
    },

    .radio = {
        .autocal     = CC1101_AUTOCAL_ALWAYS,
        .pin_mode    = 0x00,
        .pin_output  = true,
        .gdo0_mode   = CC1101_GDO_HIGH_Z,          // No interrupt
        .gdo2_mode   = CC1101_GDO_HIGH_Z,
    },
};
```

**Critical Matching Requirements**:
| Parameter | TX Value | RX Value | Must Match |
|-----------|----------|----------|------------|
| Frequency | 433.92 MHz | 433.92 MHz | ✓ |
| Modulation | 2FSK | 2FSK | ✓ |
| Datarate | 2400 bps | 2400 bps | ✓ |
| Sync Mode | 16/16 | 16/16 | ✓ |
| Sync Word | 0xDEAF | 0xDEAF | ✓ |
| Preamble | 4 bytes | 4 bytes | ✓ |
| Deviation | 0x47 | 0x47 | ✓ |
| Channel BW | 0x03 | 0x03 | ✓ |
| CRC | Disabled | Disabled | ✓ |

---

## Receive Loop

```c
cc1101_set_rx_mode(&radio);
ESP_LOGI(TAG, "Entering RX mode, waiting for packets...");

while (1) {
    // 1. Check MARCSTATE - re-enter RX if needed
    uint8_t marcstate = cc1101_read_status_reg(&radio, CC1101_MARCSTATE) & 0x1F;
    if (marcstate != 0x0D) {  // 0x0D = RX
        ESP_LOGW(TAG, "Not in RX (MARCSTATE=0x%02X), re-entering RX", marcstate);
        cc1101_set_rx_mode(&radio);
    }

    // 2. Try to receive packet
    size_t len = CC1101_MAX_PACKET_LEN;
    if (cc1101_receive_packet(&radio, data, &len)) {
        ESP_LOGI(TAG, "RX OK (%zu bytes):", len);

        // 3. Parse payload (expected format: [SRC][DST][TYPE][SEQ_H][SEQ_L][DATA...])
        if (len >= 9) {
            ESP_LOGI(TAG, "  SRC=0x%02X DST=0x%02X TYPE=0x%02X SEQ=0x%02X%02X DATA=%02X%02X%02X%02X",
                     data[0], data[1], data[2],
                     data[3], data[4],
                     data[5], data[6], data[7], data[8]);
        } else {
            // Raw hex dump for short packets
            char hex[CC1101_MAX_PACKET_LEN * 3 + 1] = {0};
            for (size_t i = 0; i < len; i++)
                sprintf(hex + i * 3, "%02X ", data[i]);
            ESP_LOGI(TAG, "  %s", hex);
        }

        // 4. Re-enter RX (receive_packet already does this, but explicit)
        cc1101_set_rx_mode(&radio);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
}
```

---

## Packet Format (Expected)

```
Byte 0: SRC (source address)
Byte 1: DST (destination address)
Byte 2: TYPE (message type)
Byte 3: SEQ_H (sequence high)
Byte 4: SEQ_L (sequence low)
Byte 5+: DATA (variable payload)
```

**With `append_status=true`**, `cc1101_receive_packet()` reads 2 extra bytes from FIFO:
- Status byte 0: RSSI
- Status byte 1: CRC_OK (bit 7) + other flags
- Function returns `true` only if CRC_OK bit is set

Since TX has CRC disabled, RX also has CRC disabled — the status byte CRC_OK will be 0, but function still returns true (checks `(status[1] & 0x80) != 0`).

---

## Status Output

```
=== RFuzz RX (2FSK 2400 bps, dev 12 kHz) ===
freq=433.92MHz  modulation=2FSK  rate=2400bps  dev=12kHz
preamble=4B  sync=2DD4  CRC=OFF  append_status=ON
Entering RX mode, waiting for packets...
RX OK (9 bytes):
  SRC=0xDE DST=0xAD TYPE=0xBE SEQ=0xEF01 DATA=00000000
```

---

## Building

```bash
# Copy to main
cp main/RFuzz_RX.c main/main.c
idf.py build flash monitor
```

Or create a new example:
```bash
mkdir -p examples/rfuzz_rx
cp main/RFuzz_RX.c examples/rfuzz_rx/main.c
# Create CMakeLists.txt, build
```

---

## Verification with TX Beacon

### 1. Flash TX on one ESP32, RX on another
```bash
# ESP32 #1: TX beacon
cp main/RFuzz_TX.c main/main.c  # or rfuzz_tx_2fsk.c
idf.py flash monitor

# ESP32 #2: RX packet
cp main/RFuzz_RX.c main/main.c
idf.py flash monitor
```

### 2. Monitor RX Logs
```
RX OK (9 bytes):
  SRC=0xDE DST=0xAD TYPE=0xBE SEQ=0xEF01 DATA=00000000
```

### 3. Cross-check with HackRF (Dragon OS)
```bash
ssh dragon@192.168.1.101
hackrf_transfer -r rx_verify.c8 -f 433920000 -s 2000000 -n 4000000
# Open in inspectrum/URH - should see packets every 10s (RFuzz_TX) or 3s (RFuzz_TX.c)
```

---

## Customization

### Change Frequency Band
```c
.freq_hz = 868300000,  // 868 MHz EU
.freq_hz = 915000000,  // 915 MHz US
```

### Enable CRC (if TX also enables)
```c
.packet.crc_enable = true,
```
Then RX will only return packets with valid CRC.

### Increase Buffer for Longer Packets
```c
.packet.max_length = 255,
uint8_t data[CC1101_MAX_PACKET_LEN] = {0};  // 255 bytes
```

### Add Address Filtering
```c
.packet.addr_check = CC1101_ADR_CHK_0_BCAST,  // Accept addr 0 + broadcast
// Requires TX to set address in PKTCTRL1/ADDR register
```

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| No packets received | Frequency mismatch | Verify both TX/RX on same freq |
| "Not in RX" warnings | Radio left RX state | Check MARCSTATE, increase re-entry delay |
| CRC errors | CRC enabled on one side only | Match CRC enable/disable on both |
| Garbage data | Sync word mismatch | Verify sync1/sync0 match exactly |
| Short packets | Preamble too short | Increase preamble on both sides |
| RSSI very low | Antenna/cable issue | Check antenna connection |

---

## AI-Readable Config

```yaml
rx_packet:
  name: "RFuzz RX Packet"
  source: "main/RFuzz_RX.c"
  config:
    freq_hz: 433920000
    modulation: "2FSK"
    datarate_bps: 2400
    sync_mode: "16/16"
    sync_word: "0xDEAF"
    preamble_bytes: 4
    deviation_reg: 0x47
    chanbw_reg: 0x03
    packet_mode: "VARIABLE"
    crc: false
    whitening: false
    append_status: true
    max_length: 64
    addr_check: "NONE"
    gdo0_mode: "HIGH_Z"
    gdo2_mode: "HIGH_Z"
    autocal: "ALWAYS"
  receive_loop:
    marcstate_check: true
    re_enter_rx_on_fail: true
    poll_interval_ms: 10
    payload_parser:
      min_len: 9
      format: "[SRC][DST][TYPE][SEQ_H][SEQ_L][DATA...]"
  matching_tx: "rfuzz_tx_2fsk.c"
```

---

## Related

- [[03-applications/tx-beacon|TX Beacon Firmware]]
- [[03-applications/main-capture|Async RX Capture]]
- [[04-host-scripts/rssi_mon.py|RSSI Monitor]]
- [[05-dragon-os/hackrf-ops|HackRF Operations]]
- [[02-firmware/cc1101-driver|CC1101 Driver API]]