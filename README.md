# CC1101 Driver for ESP-IDF

A standalone C component for the Texas Instruments CC1101 sub-GHz transceiver.
It provides a handle-based SPI interface, checked radio configuration, and
packet RX/TX. It does not include an application, board pin assignments, or a
background task. Nothing transmits unless the application explicitly starts TX.

## Add to an ESP-IDF project

Use ESP-IDF v6.1. The reference MCU is the ESP32-S3; other targets and ESP-IDF
versions are not claimed to be validated.

1. Clone the [`v1.0.0` tag](https://github.com/DarkSoulEngineer/CC1101_DRIVER/tree/v1.0.0)
   and copy `components/cc1101` into your application's `components/` directory.
2. Add `cc1101` to the consuming component's `PRIV_REQUIRES` (or `REQUIRES` if its
   public headers include `cc1101.h`). Include `cc1101.h` in your source.
3. Run `idf.py build` from **your application's** root. This repository is a
   component, not a firmware project, and has no standalone image to flash.

The component requires ESP-IDF's GPIO and SPI drivers; it does not need any
board-support component or registry package.

## Wiring and ownership

| CC1101 signal | Host connection |
| --- | --- |
| VCC, GND | Follow your module's supply specification (typically 3.3 V); share ground with the MCU. |
| SCLK, SI, SO | SPI clock, MOSI and MISO. The driver also reads SO/MISO as a GPIO input to wait for radio readiness. |
| CSn | GPIO output, idle high. The driver controls this pin in software. |
| GDO0 | GPIO input. Packet RX uses it to track reception in progress. |
| GDO2 | Not required by the driver. |

The application must initialize SPI and GPIO before calling `cc1101_init()`.
Preload the CSn output high before enabling it. Use SPI mode 0 (1 MHz is a
starting point), `.spics_io_num = -1` for software CS, and an SPI bus that
supports at least 65-byte transactions (one command byte plus up to 64 data
bytes; use `SPI_DMA_CH_AUTO` on ESP32-S3). Keep MISO readable after SPI setup.

Use one `cc1101_handle_t` per radio. Call the driver from task context, not an
ISR. The driver does not take a mutex: the caller must serialize access to the
handle and radio, including resets, FIFO service and profile changes. If a bus
is shared, ensure other SPI traffic cannot interleave with a CC1101
software-CS transaction. The `_locked` suffix denotes caller-owned
serialization, not an internal lock.

## Receive a packet

With one radio owner and the SPI device and pins already configured:

```c
#include "cc1101.h"

esp_err_t start_packet_rx(cc1101_handle_t *radio, spi_device_handle_t spi,
                          gpio_num_t cs, gpio_num_t miso, gpio_num_t gdo0)
{
    esp_err_t err = cc1101_init(radio, spi, cs, miso, gdo0);
    if (err != ESP_OK) {
        return err;
    }
    err = cc1101_config_checked(radio);
    if (err != ESP_OK) {
        return err;
    }
    return cc1101_start_rx_locked(radio);
}

void poll_packet(cc1101_handle_t *radio)
{
    uint8_t payload[CC1101_MAX_PAYLOAD_LEN];
    size_t len = sizeof(payload);  // Pass the buffer capacity on every call.
    bool crc_ok;
    if (cc1101_poll_packet(radio, payload, &len, &crc_ok) && crc_ok) {
        // Process len bytes in payload.
    }
}
```

Call `poll_packet()` regularly from the radio owner task after successful
startup. `cc1101_poll_packet()` returns `true` when it consumes a complete
frame, even if the hardware CRC failed; inspect `crc_ok` separately.
`cc1101_poll_packet_with_status()` can also return the raw RSSI and LQI/CRC
bytes. Pass a 61-byte buffer to accept any supported payload: the variable-length
byte, payload and two appended status bytes must fit the 64-byte RX FIFO.
Longer-packet streaming is not implemented. Incomplete frames remain pending
across polls, and a successful read does not flush queued frames.

## API guide

| Operation | API and notes |
| --- | --- |
| Initialize | `cc1101_init()` binds the SPI device and pins, resets the chip and checks its identity. `cc1101_config_checked()` applies the default profile and returns the radio to IDLE. |
| Tune | `cc1101_set_frequency()`, `cc1101_set_channel()`, `cc1101_set_datarate()` and `cc1101_set_tx_power()`. The settings use a 26 MHz crystal assumption; arbitrary combinations are not validated. |
| Change profile | `cc1101_apply_profile_locked()` applies a single-radio OOK, ASK, 2-FSK or GFSK candidate for packet mode or raw digital RX. It leaves the radio IDLE; suspend application-owned GDO interrupts or capture before transition. A failed apply is not rolled back. Raw mode exposes digital demodulator output, not I/Q, capture, or packet decoding. |
| Receive | `cc1101_start_rx_locked()` enters RX. Use `cc1101_poll_packet()` only with compatible variable-length framing, CRC enabled, CRC autoflush off, appended status enabled and RX-after-packet behavior. |
| Send explicitly | `cc1101_tx_prepare_locked()` checks and loads the FIFO without sending. `cc1101_tx_start_locked()` starts one transmission; `cc1101_tx_status_locked()` reports FIFO and radio state for completion checks. There are no automatic retries or delivery acknowledgements. |
| Diagnose | `cc1101_profile_readback_locked()`, `cc1101_read_status_reg()` and `cc1101_dump_registers()`. |

See [`cc1101.h`](components/cc1101/include/cc1101.h) for declarations and
caller requirements. Prefer the checked APIs when errors must reach the
application.

## Default profile and RF limits

The default is 433.92 MHz OOK packet mode, approximately 38.383 kbaud
(38,400 baud requested), with 101.5625 kHz RX bandwidth, sync word `0xD391`
(30/32 detection), variable-length framing, and hardware CRC. Normal OOK uses
PATABLE `{0x00, 0xC8}` with PA index 1. TI lists `0xC8` at 433 MHz as nominal
**+7 dBm conducted**; neither conducted power nor ERP is guaranteed.
The optional `CONFIG_CC1101_OOK_CLOSE_RANGE_BENCH` defaults off; enabling it
selects `{0x00, 0x12}` (nominal −30 dBm conducted) for OOK only. It is an
explicit bench setting, not automatic power adaptation.

Follow local frequency, power and airtime rules before transmitting. For
example, [ANCOM RO-IR SRD-01-17b, Edition 5/2025, p. 30](https://www.ancom.ro/wp-content/uploads/2026/01/RO_IR_SRD_01_Dispozitive_radio_de_proximitate_cu_raza_mica_de_actiune_nespecifice_ro_2025.pdf)
lists 433.05–434.79 MHz, up to 10 mW ERP and a duty cycle of at most 10% for
the relevant Romanian SRD entry. Verify the rules applicable to your device and
measure the assembled system; PA register settings alone do not establish ERP
or compliance. Count all on-air framing and retries when assessing duty cycle.

References: [TI CC1101 datasheet](https://www.ti.com/lit/ds/symlink/cc1101.pdf),
[silicon errata](https://www.ti.com/lit/swrz020),
[DN010: Close-in reception](https://www.ti.com/lit/an/swra147b/swra147b.pdf),
[DN013: PA tables](https://www.ti.com/lit/an/swra151a/swra151a.pdf).

## License

[MIT](LICENSE)
