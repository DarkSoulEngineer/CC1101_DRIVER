# CC1101_DRIVER
**A handle-based ESP-IDF C driver for the TI CC1101 sub-1 GHz RF transceiver, shipped with the RFuzz receive and transmit demos.**

[![License](https://img.shields.io/github/license/DarkSoulEngineer/CC1101_DRIVER)](LICENSE)
![Language](https://img.shields.io/badge/language-C-blue)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-red)

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

CC1101_DRIVER is an ESP-IDF component for the Texas Instruments CC1101 sub-1 GHz RF transceiver, written in C. It wraps the CC1101 register interface and command strobes in a handle-based API so one application can address one or more radios over SPI, and it exposes runtime control of frequency, channel, data rate, TX power, and packet length. The repository also contains RFuzz, a small ESP32 demo application that initializes the radio and then either receives packets or transmits a periodic beacon.

The driver targets the CC1101 bands at 300–348 MHz, 387–464 MHz, and 779–928 MHz. `cc1101_set_frequency` programs the FREQ2/FREQ1/FREQ0 registers from a target frequency in Hz against the 26 MHz crystal, and `cc1101_config` applies a working baseline (433.92 MHz, GFSK modulation, 30/32 sync word, CRC enabled).

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
- **Hardware init component**: `hw_init` sets up SPI3_HOST over `SPI_DMA_CH_AUTO` and installs a GDO0 GPIO interrupt used as a TX-done signal.
- **RFuzz demos**: a receive loop (`main.c`, identical to `RFuzz_RX.c`) and a transmit beacon (`RFuzz_TX.c`).

## Requirements

- **ESP-IDF v5.x**. The components require the `esp_driver_spi` and `esp_driver_gpio` components.
- **ESP32 target**. `sdkconfig` sets `CONFIG_IDF_TARGET="esp32"` and a 4 MB flash size.
- **A CC1101 module** wired to the ESP32 over SPI (see [Hardware Interfacing](#hardware-interfacing)).
- The `main/idf_component.yml` file declares the registry dependency `nopnop2002/esp-idf-cc1101` version `1.0.0`.

## Installation

1. Clone the repository:

   ```bash
   git clone https://github.com/DarkSoulEngineer/CC1101_DRIVER.git
   cd CC1101_DRIVER
   ```

2. Ensure the ESP-IDF environment is exported (`IDF_PATH` set), then set the target and build:

   ```bash
   idf.py set-target esp32
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
| `cc1101_set_tx_power` | Writes the PATABLE power value. |
| `cc1101_set_tx_len` | Writes PKTLEN for fixed-length mode. |
| `cc1101_set_tx_mode` | SIDLE then SFTX. |
| `cc1101_set_rx_mode` | SIDLE, SFRX, then SRX. |
| `cc1101_transmit` | Frames and sends a payload up to `CC1101_MAX_PAYLOAD_LEN` bytes. |
| `cc1101_receive_packet` | Reads a packet from the RX FIFO; returns the payload and CRC status. |
| `cc1101_read_status_reg` | Reads a status register (for example `CC1101_PARTNUM`). |
| `cc1101_strobe` | Sends a command strobe and returns the status byte. |
| `cc1101_dump_registers` | Logs the configuration registers and key status values. |

### Receive and Transmit Examples

Initialization and receive loop, adapted from `main/main.c`:

```c
#include "hw_init.h"
#include "cc1101.h"

void app_main(void)
{
    cc1101_handle_t radio;

    init_hardware();  // SPI3_HOST + GDO0 interrupt (hw_init.c)

    cc1101_init(&radio,
                cc1101_handle,   // spi_device_handle_t from hw_init
                PIN_NUM_CS,      // GPIO5
                PIN_NUM_MISO,    // GPIO19
                PIN_NUM_GDO0);   // GPIO25

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

Transmit, adapted from `main/RFuzz_TX.c`:

```c
uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};

cc1101_set_tx_power(&radio, 0x1F);  // PATABLE value
cc1101_transmit(&radio, payload, sizeof(payload));
```

### Running the RFuzz Demos

`main/CMakeLists.txt` compiles a single source file:

```cmake
idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS ""
                    REQUIRES hw_init cc1101 freertos log)
```

The default `main.c` runs the receive loop. To run the transmit beacon instead, change the `SRCS` entry to `"RFuzz_TX.c"` and rebuild. `RFuzz_TX.c` sets TX power, transmits the 4-byte payload `DE AD BE EF` every 3000 ms, and waits up to 100 ms on the `tx_done_sem` semaphore that the GDO0 interrupt handler releases. `main/main.c` and `main/RFuzz_RX.c` contain the same receive application, so either can be selected as the entry point.

## Hardware Interfacing

The driver leaves the SPI bus and pin setup to the host application through the `hw_init` component. Pin assignments come from `components/hw_init/include/hw_init.h`.

| CC1101 pin | ESP32 GPIO | Macro | Notes |
|------------|------------|-------|-------|
| CSN | GPIO5 | `PIN_NUM_CS` | Driven in software (`spics_io_num = -1` in `hw_init.c`) |
| SCK | GPIO18 | `PIN_NUM_CLK` | SPI clock, 1 MHz, mode 0 |
| MOSI | GPIO23 | `PIN_NUM_MOSI` | SPI data out |
| MISO | GPIO19 | `PIN_NUM_MISO` | Polled for the CC1101 ready signal before each transaction |
| GDO0 | GPIO25 | `PIN_NUM_GDO0` | Negative-edge GPIO interrupt for TX-done (`tx_done_sem`) |
| GDO2 | not wired | `CC1101_IOCFG2` | Configured as a sync-word output in `cc1101_config` |

The bus is initialized on `SPI3_HOST` with `SPI_DMA_CH_AUTO` and a maximum transfer size of 256 bytes. Chip select is not handled by the SPI peripheral; `cc1101.c` raises and lowers the CS pin directly around each transaction.

## License

MIT. See [LICENSE](LICENSE).
