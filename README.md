# Native CC1101 ESP-IDF Driver

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
