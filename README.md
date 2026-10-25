# CC1101_DRIVER
**A handle-based ESP-IDF C driver for the TI CC1101 sub-1 GHz RF transceiver, shipped with RFuzz dual-radio link validation on ESP32-S3.**

[![License](https://img.shields.io/github/license/DarkSoulEngineer/CC1101_DRIVER)](LICENSE)
![Language](https://img.shields.io/badge/language-C-blue)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1-red)

## Table of Contents

- [Description](#description)
  - [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Usage](#usage)
  - [API Overview](#api-overview)
  - [Receive and Transmit Examples](#receive-and-transmit-examples)
  - [Running the RFuzz Demos](#running-the-rfuzz-demos)
- [Hardware Interfacing](#hardware-interfacing)
- [License](#license)

## Description

CC1101_DRIVER is an ESP-IDF component for the Texas Instruments CC1101 sub-1 GHz RF transceiver, written in C. It wraps the CC1101 register interface and command strobes in a handle-based API so one application can address one or more radios over SPI, and it exposes runtime control of frequency, channel, data rate, TX power, and packet length. RFuzz initializes two radios on an ESP32-S3: Radio A transmits a periodic test packet and Radio B validates its payload and CRC.

The driver targets the CC1101 bands at 300–348 MHz, 387–464 MHz, and 779–928 MHz. `cc1101_set_frequency` programs the FREQ2/FREQ1/FREQ0 registers from a target frequency in Hz against the 26 MHz crystal. `cc1101_config` applies a 433.92 MHz OOK packet-mode baseline at approximately 38.4 kbps, with 30/32 sync detection and CRC enabled. See the [default radio profile](#default-radio-profile) for power and regulatory limitations.

### Features

- **Handle-based API**: `cc1101_handle_t` binds a CC1101 to an ESP-IDF SPI device plus its chip-select, MISO, and GDO0 pins, allowing multiple modules to coexist.
- **Full register access**: `cc1101.h` defines the complete CC1101 configuration register map, command strobes (`CC1101_SIDLE`, `CC1101_STX`, `CC1101_SRX`, and others), status registers, and MDMCFG builder macros.
- **Runtime control**: `cc1101_set_frequency`, `cc1101_set_channel`, `cc1101_set_datarate`, `cc1101_set_tx_power`, and `cc1101_set_tx_len`.
- **Packet engine**: variable-length packets with CRC (`CC1101_CRC_ENABLE | CC1101_PKT_VARIABLE`), optional address checking, and two status bytes appended to the RX FIFO.
- **RX and TX paths**: `cc1101_receive_packet` reads a length-prefixed packet from the FIFO and reports CRC validity; `cc1101_transmit` frames the payload with a length byte, flushes the TX FIFO, and issues the STX strobe.
- **Data rates** from 0.6 to 600 kbps; `cc1101_set_datarate` selects the MDMCFG4 exponent and MDMCFG3 mantissa for a requested baud.
- **Modulation formats** defined in `cc1101.h`: 2-FSK, GFSK, ASK/OOK, 4-FSK, and MSK.
- **Frequency presets** for 433.92 MHz, 868.3 MHz, and 915.0 MHz (`CC1101_FREQ_433_92_x`, `CC1101_FREQ_868_3_x`, `CC1101_FREQ_915_0_x`).
- **Register dump helper**: `cc1101_dump_registers` logs the configuration register space plus PARTNUM, VERSION, and MARCSTATE.
- **Hardware init component**: `hw_init` sets up shared SPI3_HOST over `SPI_DMA_CH_AUTO`, separate software CS lines, dual GDO0 interrupts, and a bus mutex.
- **RFuzz application**: `main/main.c` imports `rfuzz_tx.c` and `rfuzz_rx.c` in one translation unit, with a shared link profile and packet/error counters.

## Requirements

- **ESP-IDF v6.1**, used for the validated build. The components require `esp_driver_spi` and `esp_driver_gpio`.
- **ESP32-S3 target**. `sdkconfig` sets `CONFIG_IDF_TARGET="esp32s3"` and a 2 MB flash image size. The tested board reports 16 MB physical flash; firmware currently uses the configured 2 MB layout.
- **Two CC1101 modules** wired to the ESP32-S3 over SPI (see [Hardware Interfacing](#hardware-interfacing)).
- The root `idf_component.yml` declares the registry dependency `nopnop2002/esp-idf-cc1101` version `1.0.0`; the native driver lives in `components/cc1101`.

## Installation

1. Clone the repository:

   ```bash
   git clone https://github.com/DarkSoulEngineer/CC1101_DRIVER.git
   cd CC1101_DRIVER
   ```

2. Ensure the ESP-IDF environment is exported (`IDF_PATH` set), then set the target and build:

   ```bash
   idf.py set-target esp32s3
   idf.py build
   ```

3. Flash and monitor (adjust the port for your system):

   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

The driver itself lives in `components/cc1101` and the board bring-up in `components/hw_init`. To reuse the driver in another project, copy the `components/cc1101` directory into that project's `components/` folder.

## Usage

### API Overview

The public API is declared in `components/cc1101/include/cc1101.h`.

| Function | Description |
|----------|-------------|
| `cc1101_init` | Binds a handle to an SPI device and the CS, MISO, and GDO0 pins, then resets the radio. |
| `cc1101_reset` | Issues the SRES strobe and waits for the radio to settle. |
| `cc1101_config` | Applies a baseline register configuration. |
| `cc1101_set_frequency` | Sets the carrier frequency in Hz (writes FREQ2/1/0). |
| `cc1101_set_channel` | Writes CHANNR. |
| `cc1101_set_datarate` | Sets the symbol rate in baud (writes MDMCFG4/MDMCFG3). |
| `cc1101_set_tx_power` | Configures PATABLE and PA index for the active modulation; OOK uses off/on entries. |
| `cc1101_set_tx_len` | Writes PKTLEN for fixed-length mode. |
| `cc1101_set_tx_mode` | SIDLE then SFTX. |
| `cc1101_set_rx_mode` | SIDLE, SFRX, then SRX. |
| `cc1101_transmit` | Frames and sends a payload up to `CC1101_MAX_PAYLOAD_LEN` bytes. |
| `cc1101_receive_packet` | Reads a packet from the RX FIFO; returns the payload and CRC status. |
| `cc1101_poll_packet` | Reports a consumed frame separately from its hardware CRC outcome, including failed-CRC frames. |
| `cc1101_read_status_reg` | Reads a status register (for example `CC1101_PARTNUM`). |
| `cc1101_strobe` | Sends a command strobe and returns the status byte. |
| `cc1101_dump_registers` | Logs the configuration registers and key status values. |

### Receive and Transmit Examples

Minimal Radio B initialization and single-packet receive check (error handling omitted):

```c
#include "hw_init.h"
#include "cc1101.h"

void app_main(void)
{
    cc1101_handle_t radio;

    init_hardware();  // Shared SPI3_HOST + dual GDO0 interrupts

    cc1101_init(&radio,
                cc1101_handle,   // spi_device_handle_t from hw_init
                PIN_NUM_CS_B,    // GPIO10
                PIN_NUM_MISO,    // GPIO13
                PIN_NUM_GDO0_B); // GPIO5

    uint8_t partnum = cc1101_read_status_reg(&radio, CC1101_PARTNUM);
    uint8_t version = cc1101_read_status_reg(&radio, CC1101_VERSION);

    cc1101_config(&radio);
    cc1101_set_frequency(&radio, 433920000);  // Hz
    cc1101_set_channel(&radio, 0);
    cc1101_set_datarate(&radio, 38400);       // baud
    cc1101_set_rx_mode(&radio);

    uint8_t buffer[CC1101_MAX_PACKET_LEN];
    size_t len = sizeof(buffer);

    if (cc1101_receive_packet(&radio, buffer, &len))
    {
        // len bytes were read into buffer and passed CRC
    }
}
```

Transmit using an initialized Radio A handle (CS GPIO9, GDO0 GPIO4):

```c
uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};

cc1101_set_tx_power(&radio, CC1101_DEFAULT_PA_433_MHZ); // Nominal +7 dBm, not guaranteed ERP
cc1101_transmit(&radio, payload, sizeof(payload));
```

### Running the RFuzz Demos

`main/CMakeLists.txt` compiles a single source file:

```cmake
idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES hw_init cc1101 freertos log esp_timer)
```

Keep `main.c` as the entry point: it imports both lowercase `rfuzz_tx.c` and `rfuzz_rx.c`; they are not standalone applications. Radio A sends `DE AD BE EF`, waits up to 100 ms for `tx_done_sem_a`, and Radio B checks the received packet within a 250 ms window. Each completed iteration is followed by a 5000 ms delay. Logs report TX confirmation, matching payloads, CRC failures, corruption, and misses.

A bounded 40-second hardware run with the C8 profile booted successfully, identified both radios (`PARTNUM=00 VERSION=14`), and observed eight matching CRC-valid packets with no misses. This validates that packet link only, not RF power or regulatory compliance.

## Hardware Interfacing

### Default radio profile

Both radios use **OOK packet mode** at 433.92 MHz and approximately 38.4 kbps,
with the existing 101.5625 kHz RX bandwidth (26 MHz crystal). This is not raw
GPIO/asynchronous OOK: FIFO framing, preamble, sync, variable-length packets,
hardware CRC, and appended RX status remain unchanged.

The OOK configuration uses `MDMCFG2=0x33`, clears `FOCCFG.FOC_LIMIT`, and starts
RX AGC at `AGCCTRL2/1/0=0x03/0x00/0x91` (TI DN022 starting profile).
These AGC settings require hardware tuning; they are not guaranteed optimal.
`FREND1` remains at reset `0xB6` for the existing bandwidth above 101 kHz.

`cc1101_set_tx_power()` writes OOK PATABLE entries `{0x00, on_power}` in one
burst and selects `FREND0.PA_POWER=1`, preserving other front-end bits.
For non-OOK modulation it writes a single entry and selects index 0.
Both radios use shared `CC1101_DEFAULT_PA_433_MHZ=0xC8`, including Radio A's
explicit override: OOK PATABLE is `{0x00, 0xC8}`, with index 1 selected.
[TI SWRS061I, Table 39](https://www.ti.com/lit/ds/symlink/cc1101.pdf) lists
`0xC8` at 433 MHz as nominal +7 dBm (~5 mW conducted), rather than `0xC0`
at nominal +10 dBm. These are not guaranteed conducted or radiated powers.

For Romania, [ANCOM RO-IR SRD-01-17b, Edition 5/2025, p. 30](https://www.ancom.ro/wp-content/uploads/2026/01/RO_IR_SRD_01_Dispozitive_radio_de_proximitate_cu_raza_mica_de_actiune_nespecifice_ro_2025.pdf)
specifies 433.05–434.79 MHz, maximum **10 mW ERP**, and duty cycle **<=10%**.
A 90% linear-power target is exactly **9 mW ERP (~9.542 dBm ERP)**; the
selected nominal 5 mW conducted setting is a conservative candidate, not an
exact implementation of that ERP target. Unknown antenna gain, losses, and
hardware tolerances mean this configuration cannot guarantee ERP compliance;
the assembled system requires measurement and applicable-rule verification.

Duty cycle must include complete packets and any retries. The current
4-byte test payload gives 4 preamble + 4 sync + 1 length + 4 payload + 2 CRC
= 15 transmitted bytes: approximately 3.13 ms at 38.4 kbps, or 0.063% for
one packet per 5 seconds. This is a calculation, not a measured duty cycle.

The driver relies on the host application to initialize the physical SPI bus, ensuring the component remains decoupled from specific ESP32 pinouts or SPI peripherals. Dual-radio setup: both CC1101s share one SPI3 bus; each has its own software-managed CS line and GDO0 interrupt.

| CC1101 Pin | ESP-IDF Context | Function                             |
| :--------- | :-------------- | :----------------------------------- |
| **MISO**   | `PIN_NUM_MISO` (13)   | SPI Data In (shared bus)        |
| **MOSI**   | `PIN_NUM_MOSI` (11)   | SPI Data Out (shared bus)       |
| **SCK**    | `PIN_NUM_CLK`  (12)   | SPI Clock (shared bus)          |
| **CSN A**  | `PIN_NUM_CS_A` (9)    | Chip Select - Radio A (TX/fuzzer)   |
| **GDO0 A** | `PIN_NUM_GDO0_A` (4)  | Interrupt - Radio A (TX done)       |
| **CSN B**  | `PIN_NUM_CS_B` (10)   | Chip Select - Radio B (RX/target)   |
| **GDO0 B** | `PIN_NUM_GDO0_B` (5)  | Interrupt - Radio B (RX/sync)       |

Pin definitions are in `components/hw_init/include/hw_init.h`. The bus is initialized on `SPI3_HOST` with `SPI_DMA_CH_AUTO`, 1 MHz mode 0, and a maximum transfer size of 256 bytes. Chip select is not handled by the SPI peripheral; `cc1101.c` raises and lowers each CS pin directly around transactions, polling MISO for readiness. Both GDO0 interrupts use negative edges. GDO2 is not wired; `CC1101_IOCFG2` is configured as a sync-word output.

## License

MIT. See [LICENSE](LICENSE).
