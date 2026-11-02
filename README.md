# CC1101_DRIVER
**A handle-based ESP-IDF C driver for the TI CC1101 sub-1 GHz RF transceiver, shipped with the RFuzz dual-radio console on ESP32-S3.**

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
- [Staged Roadmap](#staged-roadmap)
- [License](#license)

## Description

CC1101_DRIVER is an ESP-IDF component for the Texas Instruments CC1101 sub-1 GHz RF transceiver, written in C. It wraps the CC1101 register interface and command strobes in a handle-based API so one application can address one or more radios over SPI, and it exposes runtime control of frequency, channel, data rate, TX power, and packet length. RFuzz initializes two radios on an ESP32-S3: Radio A is configured but stays idle (no automatic transmission), and Radio B listens and reports received packets. There is no golden-payload validation in the current lean firmware.

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
- **RFuzz application**: `components/rfuzz` is the top-level component; it depends on `cc1101`, `hw_init`, and three nested sub-components discovered via `EXTRA_COMPONENT_DIRS`: `components/rfuzz/components/terminal` (UART console), `components/rfuzz/components/profile` (coordinated modulation), and `components/rfuzz/components/radio` (Radio A/B TX and RX). The top-level component compiles only `src/rfuzz.c` (the main loop) and includes the sub-components' headers; the application entry point is `rfuzz_run()`.

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

#### Application organization

The application is split into ESP-IDF components under `components/`: `cc1101` (the chip driver), `hw_init` (shared SPI bus, GPIO, RMT capture), and `rfuzz` (everything else). `main/` contains only the thin `app_main` entrypoint. Inside `rfuzz`, the top-level component compiles only `src/rfuzz.c` (the main loop, entry `rfuzz_run()`); the radio transport, profile coordination, and console live in the nested sub-components `components/rfuzz/components/{radio,profile,terminal}`. Headers are included, **not `.c` files**. There is no automatic transmission or golden-payload validation; Radio A is configured but stays idle, and Radio B reports whatever it receives. There is one main-loop owner, with no new framework or tasks. Verification evidence is version-specific; see below.

| Component | Responsibility |
|-----------|----------------|
| `main` | Thin `app_main` entrypoint that calls `rfuzz_run()` (provided by `rfuzz`). |
| `components/cc1101` | CC1101 chip driver; reusable on its own. |
| `components/hw_init` | Shared SPI3 bus, software CS, dual GDO0 interrupts, bus mutex, RMT capture. |
| `components/rfuzz` | Radio A TX send/idle, Radio B packet/raw reception, coordinated A/B profile transitions, bounded UART console, and the main loop. Depends on `cc1101` and `hw_init`. |

The current firmware does **not** transmit automatically: Radio A is configured as a packet transmitter but stays idle, and there is no five-second automatic TX cadence, TX-completion wait, or golden-payload/CRC validation loop. Radio B listens and reports whatever it receives (packet or raw), including externally received packets in packet mode, with no comparison to `DE AD BE EF` and no miss/crash accounting. The historical automatic-link-test behavior (DE AD BE EF, 100 ms TX wait, 250 ms RX window, five-second spacing) is described under the evidence sections below and is not active in this lean build.

A bounded 40-second hardware run with the C8 profile booted successfully, identified both radios (`PARTNUM=00 VERSION=14`), and observed eight matching CRC-valid packets with no misses. This validates that packet link only, not RF power or regulatory compliance.

#### Receive-mode console selector

Serial controls select Radio B's receive path and apply one of four coordinated modulation profiles. There is no automatic transmitter in the current lean build, so Radio A stays idle and there are no `tx` commands; broader independent serial knobs remain **planned, not implemented**. Commands (newline-terminated) use the existing **UART0 console at 115200 baud**, through the CH343 adapter. `COM14` is typical on this development machine only; confirm your own port. Commands and arguments are ASCII case-insensitive and allow repeated spaces/tabs; extra arguments, binary input, and oversized lines are rejected. One console reader passes typed requests to the main owner; do not run competing serial consumers.

| Command | Purpose |
|---------|---------|
| `help` | List supported commands. |
| `status` | Report requested/last-applied modulation and B mode, validity/pending state, actual A/B register readbacks, and cumulative capture/output counters. |
| `mode pck` | Select B packet RX (`mode packet` and legacy `rx packet` also work). B reports received packets and their CRC without any golden comparison. |
| `mode raw` | Select digital demodulator output on B GDO0 GPIO5 (`rx raw` also works), using bounded RMT level-duration capture. Radio A remains configured as a packet transmitter but is idle. |
| `mod ook` / `mod ask` / `mod 2fsk` / `mod gfsk` | Queue the latest coordinated A/B profile request and apply it; `mod fsk` aliases `mod 2fsk`. Repeated modulation commands reapply safely. |

Raw output is **digital demodulator output for the selected modulation**, not IQ, analog samples, or automatically decoded bits. GDO0 cannot provide true I/Q. Mode requests change B only, preserve the selected modulation, and defer to any pending coordinated profile transition. A modulation command always leaves the profile transition pending until it is applied; repeating the same modulation safely reapplies it. Any transition failure stops capture, invalidates the coordinated profile, and makes a best-effort A/B IDLE request; there is no promised rollback. A bus-lock timeout retains the cleanup latch for retry without applying profiles. Recover explicitly with `mod ...`; mode-only changes cannot repair invalid state.

Example sequence (no RF transmission is enabled by these commands; the firmware does not transmit automatically):

```text
mod ook
mode raw
status
```

Use `mod ASK`, `mod 2FSK` (or `mod FSK`), or `mod GFSK` instead of `mod ook` for the other candidate profiles. Wait for successful apply/status before changing mode; modulation commands are rejected while a profile transition is pending or invalid.

The current firmware does **not** transmit automatically and does not gate or enable TX. `status` reports the requested/last-applied modulation and B mode, validity/pending state, actual A/B register readbacks, and cumulative RMT capture/output counters. There is no TX completion deadline or completion history in this lean build. Radio A stays idle; if automatic or manual transmission is needed later, it must be added as a separate change.

In packet mode, B polls once per approximately 10 ms main-loop tick and prints externally received payloads with hardware CRC outcome, without comparing them to `DE AD BE EF` or adding absent-local-TX/crash counters. External packets still need the configured frequency, modulation, rate, sync, framing, and CRC profile; this is not an arbitrary-protocol decoder. The earlier module organization changes passed ESP-IDF v6.1 build/host regression checks but were not flashed. **The coordinated profiles and RMT capture are also not flashed or hardware-validated by this change**; the earlier selector test below does not cover them.

The selector at `f06cca6` passed an ESP-IDF v6.1 build, host tests of the C command parser, and mocked driver transition/failure-path tests. After authorized flashing to the ESP32-S3 through CH343 on COM14, a bounded hardware run exercised three raw-to-packet restoration cycles. Status readbacks matched raw `PKTCTRL0/MDMCFG2/IOCFG0=30/30/0D` and packet `05/33/06`, with `MARCSTATE=0D`; eight matching CRC-good golden packets were observed, including at least two after each restoration. Commands remained responsive during raw capture, invalid/oversized input was rejected safely, and observed TX intervals remained at least five seconds. No application error, panic, watchdog reset, or visible heap-failure symptom was observed during this run.

That historical selector run does not validate the later TX pause/listen-only path, module reorganization, coordinated modulation profiles, or replacement RMT backend. These require separately approved hardware validation.

For the new integration, host tests compile the production coordinator, RX module, and main command dispatch with mocked UART/radio/capture interfaces. They passed four-profile transitions, B-only mode restoration, partial-failure gating/recovery, deferred/coalesced in-flight requests, rejected premature starts, preserved TX spacing, listen-only packets, bounded RMT summary/counter checks, and deferred A/B IDLE cleanup after in-flight capture failure (including lock-timeout retries and queued recovery). The final ESP-IDF v6.1 whole firmware build, including the corrected RMT teardown backend and deferred cleanup fix, passed using `ninja -j1` in the existing temporary build directory after the initial parallel build encountered an unrelated LCD compiler crash. `RFuzz.bin` is 254688 bytes (76% of the app partition remains free), SHA-256 `f4993da09679150ec49c8466db1c1c2dca30df4735aa3f885e43b69d14625fb2`. No flashing was performed. None of these host/build checks validates hardware timing, RF performance, or capture loss.

The module tree was then reorganized into per-module components (`components/serial_console`, `radio`, `profile_control`, `app_loop`) and `main/` reduced to a thin entrypoint; TX and RX shared the `radio` component. That layout compiled and linked cleanly in a clean ESP-IDF v6.1 ESP32-S3 build (`RFuzz.bin` 0x3E2E0 bytes, 76% of the app partition free); it was not flashed, so no hardware claim is attached to it. The later lean change removed the automatic-transmission and golden-payload features and merged the RFuzz application into a single `components/rfuzz` component; the current lean build is also not flashed.

In that older GPIO-edge test, raw batches and sample-drop counter/reset behavior were observed, but accurate timing, losslessness, overflow/backpressure stress, and RF power/compliance were **not validated**. Its zero observed overflow/drop counters are not evidence for the new backend.

The RMT backend uses **1 us resolution**, a **1 us glitch filter**, **5 ms idle termination**, 1024-symbol partial blocks, and two static delivery slots (~12 KiB total capture buffers including the receive buffer). Each symbol contains up to two level/duration pairs; zero-duration terminators are skipped. Service/rearm precedes formatting; at most two blocks drain per main background call, with at most one serial batch per second and up to 16 pulse previews. Output includes epoch/sequence/is-last metadata; partial or final capture framing is **not a radio packet boundary**, and callback time is notification time, **not the first edge timestamp**. Capture gaps and unknown hardware loss prevent any promise of a continuous or lossless waveform. Cumulative callback/event drops, known dropped symbols, rearms, and gaps remain separate from explicitly named output-suppression counters; output omitted for serial bandwidth is not itself capture loss. Further flashing or additional RF activity requires explicit approval.

## Hardware Interfacing

### Default radio profile

At startup both radios use **OOK packet mode** at 433.92 MHz and approximately 38.4 kbps
(approximately 38.383 kbps after register quantization),
with the existing 101.5625 kHz RX bandwidth (26 MHz crystal). This is not raw
GPIO/asynchronous OOK: FIFO framing, preamble, sync, variable-length packets,
hardware CRC, and appended RX status remain unchanged in the default packet path.
The receive selector changes only Radio B to raw asynchronous demodulator output when requested;
it does not change Radio A's packet transmitter.

The coordinated modulation commands provide fixed **candidate, hardware-unvalidated** profiles at the same 433.92 MHz, approximately 38.383 kbps, and 101.5625 kHz RX bandwidth. OOK uses PA levels `00/C8`; true two-level ASK uses two nonzero levels `34/C8` (nominal -10/+7 dBm, about 17 dB contrast, not measured). 2-FSK and GFSK use `DEVIATN=35`, approximately +/-20.63 kHz deviation. `C8` remains the PA ceiling; the lean firmware does not transmit automatically, so there is no automatic TX rate to increase. These are selected compatible profiles, not arbitrary independent parameter combinations or guaranteed RF performance.

The OOK configuration uses `MDMCFG2=0x33`, clears `FOCCFG.FOC_LIMIT`, and starts
RX AGC at `AGCCTRL2/1/0=0x03/0x00/0x91` (TI DN022 starting profile).
These AGC settings require hardware tuning; they are not guaranteed optimal.
`FREND1` remains at reset `0xB6` for the existing bandwidth above 101 kHz.

`cc1101_set_tx_power()` writes two PATABLE entries for OOK (`{0x00, on_power}`)
or ASK (`{0x34, on_power}`), selecting `FREND0.PA_POWER=1` and preserving
other front-end bits. For 2-FSK/GFSK it writes a single power entry and selects index 0.
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

Duty cycle must include complete packets and any retries. The historical
automatic-test payload gave 4 preamble + 4 sync + 1 length + 4 payload + 2 CRC
= 15 transmitted bytes: approximately 3.13 ms at 38.4 kbps, or 0.063% for
one packet per 5 seconds. This is a calculation for that historical cadence,
not a measured duty cycle; the current lean firmware does not transmit.

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

Pin definitions are in `components/hw_init/include/hw_init.h`. The bus is initialized on `SPI3_HOST` with `SPI_DMA_CH_AUTO`, 1 MHz mode 0, and a maximum transfer size of 256 bytes. Chip select is not handled by the SPI peripheral; `cc1101.c` raises and lowers each CS pin directly around transactions, polling MISO for readiness. Both GDO0 interrupts use negative edges in packet mode; B raw uses RMT exclusively instead of a GPIO edge ISR, restoring packet IRQ handling on return. GDO2 is not wired; `CC1101_IOCFG2` is configured as a sync-word output.

## Staged Roadmap

Packet/raw selection, four coordinated modulation profiles, and bounded RMT capture are implemented in source, with the validation limits above. There is no automatic transmission, golden-payload validation, or TX stop/start command in the current lean build. The following are **future work, not implemented serial commands**:

1. Hardware tuning/validation of the candidate profiles, then coordinated data-rate, RX-bandwidth, and deviation controls with unsupported-combination rejection.
2. Sync word/detection and preamble-length controls; CRC, whitening, and address-filtering controls with compatible packet framing.
3. Signal-type and TX-waveform choices, plus a manual transmit command if needed (the lean build has no automatic TX). Receive-only listening is the default behavior of the current firmware.
4. Measured RMT timing/overflow/backpressure limits and approved multi-profile packet/raw transition tests; further capture reliability improvements without a lossless guarantee.

See [AGENTS.md](AGENTS.md) for ownership, ISR safety, verification, and approval boundaries. These plans do not imply hardware validation or RF compliance.

## License

MIT. See [LICENSE](LICENSE).
