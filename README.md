# CONFIDENTIAL: Native CC1101 ESP-IDF Driver

A zero-dependency, professional-grade driver for the Texas Instruments CC1101 sub-1 GHz RF transceiver. Engineered exclusively for the ESP-IDF SDK, this framework provides deterministic execution, thread-safe SPI management, and uncompromising physical layer (PHY) control for high-performance ESP32 applications.

---

## Core Architecture

Built from the ground up to bridge high-level usability with professional register-level granularity.

- **Handle-Based Routing:** Supports multiple CC1101 modules on independent buses using isolated device handles (`cc1101_handle_t`).
- **SDK Native Integration:** Leverages native ESP-IDF SPI Master drivers for DMA-accelerated burst transfers, avoiding the overhead of generic platform wrappers.
- **Dual-Layer API:** Developers can seamlessly switch between abstracted helper functions and raw, unprotected register access.

---

## PHY-Layer Capabilities

The library exposes the full analog state machine of the CC1101, enabling precise modulation tweaks and timing-critical packet manipulation.

- **Frequency Agility:** Full support for 300-348 MHz, 387-464 MHz, and 779-928 MHz ISM bands (programmable in 1 Hz steps).
- **Baud Rates:** Configurable symbol rates from 0.6 kbps up to 600 kbps.
- **Modulation Matrix:** Native support for OOK, ASK, 2-FSK, 4-FSK, GFSK, and MSK formats.
- **Asynchronous Interrupts:** Granular GPIO interrupt binding for GDO0/GDO2, achieving 240μs Wake-to-TX transitions.

---

## Hardware Interfacing

The driver relies on the host application to initialize the physical SPI bus, ensuring the component remains decoupled from specific ESP32 pinouts or SPI peripherals (VSPI/HSPI).

| CC1101 Pin | ESP-IDF Context | Function                             |
| :--------- | :-------------- | :----------------------------------- |
| **CSN**    | `PIN_NUM_CS`    | SPI Chip Select (Managed per handle) |
| **SCK**    | SPI Host        | SPI Clock                            |
| **MOSI**   | SPI Host        | Master Out Slave In                  |
| **MISO**   | SPI Host        | Master In Slave Out                  |
| **GDO0**   | `PIN_NUM_GDO0`  | Interrupt / Sync Word / TX Status    |
| **GDO2**   | Optional        | Interrupt / CCA / FIFO Threshold     |

---

## The Dual-Layer API Strategy

### 1. High-Level Abstraction Layer

Automates complex state transitions (IDLE -> Flush -> Load FIFO -> STX) and register calculations.

```c
// Bind handle to SPI bus
cc1101_init(&radio, spi_handle, PIN_CS, PIN_MISO, PIN_GDO0);

// High-level parameter configuration
cc1101_config(&radio);
cc1101_set_frequency(&radio, 433920000);
cc1101_set_datarate(&radio, 38400);

// Automated transmission lifecycle
cc1101_transmit(&radio, payload, length);
```
