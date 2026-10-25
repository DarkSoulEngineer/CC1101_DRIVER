# Project instructions

## Context and invariants
- ESP32-S3 firmware using ESP-IDF v6.1. Native driver: `components/cc1101`; shared-bus bring-up: `components/hw_init`; application: `main/main.c`, which imports `rfuzz_tx.c` and `rfuzz_rx.c` into one translation unit. Do not compile those imports separately.
- Two CC1101 radios share SPI3 with manual chip selects: A CS GPIO9 / GDO0 GPIO4, B CS GPIO10 / GDO0 GPIO5; shared MISO13, MOSI11, SCK12. GDO2 is not wired.
- Serialize shared-bus access with the existing bus mutex and preserve its ownership contract. Do not introduce nested locking, competing register writers, or mode changes during another owner's transaction. The main application owns mode transitions; a single console reader submits commands, rather than multiple consumers racing for UART input.
- ISR work must be bounded: no SPI, logging, or allocation. Defer processing to task context; use bounded capture storage and report drops instead of promising lossless data.
- Preserve the baseline: 433.92 MHz OOK, requested 38400 baud (approximately 38.383 kbps with register quantization), four-byte `DE AD BE EF` payload with hardware CRC, and about five seconds between test transmissions. Preserve `CC1101_DEFAULT_PA_433_MHZ=0xC8`, OOK PATABLE `{0x00, 0xC8}`, PA index 1: nominal +7 dBm conducted, not a measured power or RF compliance guarantee. Do not silently increase TX power, repetition rate, or airtime.

## Current implementation goal (not a completion claim)
Implement only Radio B receive selection: packet (default) versus asynchronous digital raw OOK on existing GDO0 GPIO5. Commands are `help`, `status`, `rx packet`, and `rx raw`, newline-terminated over UART0 at 115200 baud. Keep Radio A's original periodic packet transmitter running in either mode. Packet mode must retain golden-payload/CRC checks; raw mode reports bounded level-duration batches and drop counts, not IQ, analog samples, lossless capture, or automatically decoded bits. Restore the packet profile and interrupt behavior on return. Check actual source and verification evidence before marking any item complete; documentation alone is not evidence.

## Staged roadmap — future items are NOT implemented by this goal
1. **Current scope:** finish the packet/raw selector, document its limits, and verify profile restoration and existing packet behavior.
2. **Future validated serial profiles:** add modulation selection with supported-combination checks, then coordinated rate / RX bandwidth / deviation controls. Reject arbitrary invalid combinations; distinguish requested settings from quantized register values and readbacks.
3. **Future packet controls:** sync word and detection, preamble length, CRC, whitening, and address filtering, with validated profile transitions and compatible framing on both radios.
4. **Future signal / TX choices:** explicit signal types and TX waveform choices, plus a receive-only option that pauses Radio A. None is provided by `rx raw`; RF-changing work needs separate approval.
5. **Future capture and verification:** reliable RMT-based raw capture with documented timing/overflow limits; parser and profile tests, register readbacks, repeated packet/raw transitions, and approved hardware tests. Do not label capture lossless without evidence.

## Verification and permission boundaries
- Use read-only source inspection and `git diff --check` for documentation changes. For implementation work, run only the checks assigned by the task owner; ESP-IDF v6.1 `idf.py build` is the normal build command when requested and its environment is available. Do not claim builds or hardware results that were not observed.
- Require explicit consent before flashing, starting RF transmissions beyond the existing runtime, committing/pushing, or making system/toolchain changes. Do not launch a serial monitor casually: reset/boot may resume the existing transmitter. Machine-specific ports (often CH343 on COM14 here) must be confirmed rather than hard-coded.
- Keep README RF/legal source caveats intact. A historical packet-link test does not validate a new selector, raw capture, radiated power, or regulatory compliance.
