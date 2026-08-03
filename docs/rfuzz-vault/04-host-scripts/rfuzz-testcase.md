# RFuzz 2FSK End-to-End Test Case

> Reproducible bench validation of the CC1101 USB capture path: HackRF One → CC1101 → ESP32-S3 → host decode/regen.

## What it proves

| Check | Result |
|-------|--------|
| CC1101 packet engine decodes the 2FSK link bit-perfect | `[04 00] 01 02 03 04` frames on COM7, 0 bad |
| Host GDO2 decoder agrees with the CC1101 packet engine | same packet count, `payload_ok 100%` |
| Capture waveform is analyzable in PulseView | `.sr` has GDO0, GDO2, GDO2_CLEAN, FSK |
| `GDO2_CLEAN` UART-decodes the packet at 2400 baud, MSB-first | `AA×preamble DE AF 01 02 03 04`, no glitches |
| Signal can be regenerated for URH comparison | `.c8` (full I/Q) + `.c8` (I-only) |

## One-shot reproduction

```bash
cd scripts
python rfuzz_testcase.py
```

Runs end-to-end: **generate → deploy → PASS 1 (packet RX) → PASS 2 (capture) → decode → regen**.
Defaults: 60 packets / 50 ms gap, TX `-x 26 -a 0` (single pass), capture 250 kHz / 262144 samples
(`CONFIG_SUMP_MAX_SAMPLES`, 1.05 s window), COM7. Name the run with `--name`
(e.g. `python rfuzz_testcase.py --name test1` → `test1.sr`, `test1.vcd`, …).

Output (`--out-dir`, default `./`; `--name`, default `rfuzz_testcase`):

```
rfuzz_2fsk_gap.c8           generated TX signal (12 MB @ 60 pkts)
<name>.raw                  capture bytes (bit0=GDO0, bit1=GDO2)
<name>.sr                   PulseView: GDO0 + GDO2 + GDO2_CLEAN (logic), FSK (analog)
<name>.vcd                  same, VCD format (FSK as real channel)
<name>_regen.c8             regenerated 2FSK I/Q (URH)
<name>_regen_i.c8           regenerated I-only (Q=0)
```

Expected tail of the run:

```
PASS 1:  ~30 packets, payload 01 02 03 04 (0 bad frames)
DECODE:  ~30 packets, payload_ok 100%
RESULT:  PASS
```

## Manual step-by-step

Each phase is one script call, so it can also be run by hand:

```bash
# 1. Generate the gapped signal (3rd arg = idle ms between packets)
python gen_2fsk.py rfuzz_2fsk_gap.c8 60 50

# 2. Deploy to the Dragon OS VM
scp rfuzz_2fsk_gap.c8 dragon@192.168.1.101:~/rfuzz_2fsk_gap.c8

# 3. PASS 1 — verify CC1101 packet RX during one TX pass
#    (open COM7 with any serial tool and watch [len:2LE] frames:
#     04 00 01 02 03 04 repeated)
ssh dragon "hackrf_transfer -t ~/rfuzz_2fsk_gap.c8 -f 433920000 -s 2400000 -x 26 -a 0"

# 4. PASS 2 — capture GDO0/GDO2 while TX runs (capture-first timing)
python capture_custom.py --port COM7 --rate 250000 --samples 262144 --out cap

# 5. Decode the capture back into packets
python rfuzz_tools.py decode --raw cap.raw --rate 250000

# 6. Regenerate the decoded packets to 2FSK I/Q for URH
python rfuzz_tools.py regen --raw cap.raw --rate 250000 --out cap_regen.c8
```

## Channel 3: FSK analog in PulseView

`capture_custom.py` writes a Sigrok session v2 `.sr` with **four channels**
in one device section (matching libsigrok's own `srzip` writer, with
continuous channel indices `probe1`, `probe2`, `probe3`, `analog4`):

| Ch | Probe | Type | Meaning |
|----|-------|------|---------|
| 1 | `GDO0` | logic | sync-word / TX-active pulse |
| 2 | `GDO2` | logic | async demodulated data bits (raw, noisy) |
| 3 | `GDO2_CLEAN` | logic | glitch-free, UART-framed reconstruction |
| 4 | `FSK`  | analog | synthesized modulation **sinusoid** (`--mod`) |

**`GDO2_CLEAN`** is the recommended channel for UART decoding. The raw `GDO2`
is the CC1101 async-serial demodulator output: sub-bit glitches, bit-sync
settling at the start of each burst and a free-running noise tail between
bursts (no carrier → the demod toggles freely) make a 2400-baud async-UART
decode error throughout the packet. Worse, the `0xAA` preamble is
**phase-ambiguous** to an async UART decoder — it locks an even number of bits
off the true byte boundary and then misreads the sync/payload. `GDO2_CLEAN`
rebuilds the intended byte stream (preamble + sync + decoded payload) as
proper async UART frames (start bit, 8 MSB-first data bits, stop bit) at
exactly 2400 baud with the line idle high between packets, so a stock
2400-baud MSB-first UART decoder reads `AA…AA DE AF 01 02 03 04` with zero
errors. See `rfuzz_tools.clean_gdo2` / `uart_encode_bytes`.

The CC1101 has no analog output, so the analog channel is synthesized
host-side from the **clean decoded packet bits** (preamble + sync + payload),
not from the raw noisy GDO2. This produces a proper phase-continuous waveform
with constant amplitude per bit cell. With the default `--mod 2fsk` the sinusoid
jumps between **two positive, distinct tones** `IF ± dev`. When the requested
deviation fits inside the capture band (`dev < 42.5% of Nyquist`) the **true
source frequencies** are rendered: `IF = 22.5% of the real rate`, `dev = 50000`
(the HackRF's real ±50 kHz). At the default 250 kHz rate that is **IF 56250 Hz →
tones 106250 Hz / 6250 Hz** — the actual deviation, not a scaled fit. If the deviation would alias, it
falls back to the old visualization scale (`IF = rate/10, dev = IF/2`).
`--fsk-if` / `--fsk-dev` override; `--mod` selects the renderer:
`2fsk` (tone pair), `2psk` (constant carrier, phase 0/π), `ask` (carrier
on/off). Amplitude via `--analog-amp` (default 90).

> Why not `+IF/−IF`? Rendering only the I component (`I = A·cos(φ)`) at
> ±deviation around DC degenerates to PSK: cos is even, so a negative
> frequency is indistinguishable from a positive one — the carrier rate never
> changes, only the phase reverses at bit boundaries. Two distinct positive
> tones make the frequency jump visible.

Open `rfuzz_testcase.sr` in PulseView: all three channels are enabled —
`GDO0` (sync pulses, sparse), `GDO2` (data bits), `FSK` (analog). The `.vcd`
carries the same FSK channel as a real-valued signal for GTKWave.

## Single-shot mode

For a small single-shot signal (1 packet), use capture-first timing so the 33 ms
burst lands inside the 1.05 s window:

```bash
python rfuzz_testcase.py --name shot1 --packets 1 --gap-ms 0
```

The capture window opens first, then TX starts 150 ms later — HackRF startup
(~80–300 ms) places the burst safely inside the window. At 250 kHz this gives
~104 samples/bit for hand verification.

## Key parameters & gotchas

- **Firmware rate ceiling**: the timer alarm is `1000000/rate` (integer
  microseconds); alarms below ~4 µs starve CPU0 and trigger an interrupt-watchdog
  panic/reboot, so **250 kHz is the safe max capture rate** (333 kHz reboots).
  262144 samples at 250 kHz = 1.05 s window (bounded by the internal-RAM buffer,
  `CONFIG_SUMP_MAX_SAMPLES`; PSRAM is not enabled).
- **Sample rate quantization**: `--rate 48000` actually runs at **50000 Hz**
  (alarm 20 µs), so the 2400 bps bit cell is **20.833 samples**. `rfuzz_tools.py`
  derives this automatically (`--spb` overrides).
- **Capture timing**: PASS 2 opens the capture window first, then starts TX
  (150 ms delay). This catches the burst reliably regardless of HackRF startup
  jitter. TX-first (`*--tx-settle`) was tried but is fragile for short bursts.
- **CPU split (firmware)**: the capture gptimer ISR and SUMP stream task run on
  **CPU0** (allocated there via `capture_init` in `app_main`), while the CC1101
  packet RX loop runs on **CPU1** (`rx_loop_task`), so FIFO draining continues
  during a high-rate capture.
- **TX rules**: `-a 0` always (no RF amp), `-x 18–26` (`-x 26` = reliable lock),
  single pass — never `-R`/loops. See [[05-dragon-os/hackrf-ops]].
- **Clean captures**: while a capture is active the firmware suppresses packet
  logging on the USB stream (`capture_is_active()` guard in `main.c`), so the
  capture bytes are never polluted by `[len:2LE]` packet frames.
- **`pre=N` in decode output**: the CC1101 async-data output (GDO2) includes a
  few settling bits before the preamble and the preamble may not align to the
  32-bit window; sync and payload always decode exactly regardless.
- **FSK channel 3**: synthesized from the **clean decoded packet bits**
  (preamble + sync + payload), not raw GDO2, so it shows a proper phase-continuous
  2FSK with constant amplitude per bit cell; see [[04-host-scripts/rfuzz-tools|rfuzz_tools.py]]
  for the `manual` subcommand.
- **Ports**: COM7 = USB Serial/JTAG (capture + packet stream), COM6 = UART0
  console (ESP-IDF logs).

## Hardcoded test parameters (preamble / sync / payload)

This test setup **hardcodes the link parameters in both the firmware and the
scripts** for the single bench configuration (2FSK, 2400 bps, sync `0xDEAF`,
4-byte payload). This is intentional for reproducible validation, but it is a
**test-only shortcut** — the code will silently mis-decode if any of these are
changed without updating all of them:

| Parameter | Firmware | Scripts |
|-----------|----------|---------|
| Sync word `0xDEAF` | `main/main.c` → `rx_cfg.packet.sync1/sync0` | `rfuzz_tools.SYNC_BITS`/`SYNC_BYTES` (duplicated in `capture_custom.SYNC_BITS`) |
| Preamble length (default 4) | `main/main.c` → `rx_cfg.modem.preamble_bytes` | `rfuzz_tools.DEFAULT_PREAMBLE_BYTES` + `--preamble` on the test scripts |
| Payload `01 02 03 04` | `main/main.c` → `max_length`/fixed 4-byte | `rfuzz_tools.PAYLOAD_EXPECT`, `rfuzz_testcase.EXPECT_PAYLOAD` |
| Data rate 2400 bps | `rx_cfg.modem.datarate_bps` | `2400.0` literals (`--spb`/`--baud` in `rfuzz_tools`) |

For a general/parameterized use case these should become CLI arguments /
Kconfig options and the constants derived from them (e.g. `SYNC_BITS` built
from a `sync1/sync0` pair, `preamble_bits()` from a length argument) rather
than module-level literals. Note that `rfuzz_tools.clean_gdo2` builds the
clean channel from the **decoded** payload, so it needs the same sync-word
assumption to reconstruct the frame; the sync-majority check in
`refine_packets` likewise validates against `SYNC_BITS`.

## Manual bit-by-bit decode

`rfuzz_tools.py manual` decodes one packet directly from the raw samples by
majority-voting each bit cell, and prints a per-bit table with sample ranges so
the captured GDO2 waveform can be verified by hand in PulseView/GTKWave:

```bash
python rfuzz_tools.py manual --raw shot1.raw --rate 250000
```

Output shows each bit index, its sample range, the majority bit, the expected
bit, and a cell 0/1 ratio, then a byte view grouped into preamble / sync /
payload. Sync `DE AF` and payload `01 02 03 04` should match exactly.

## Related

- [[02-firmware/sump-capture|SUMP Capture Internals]]
- [[05-dragon-os/hackrf-ops|HackRF One operations]]
- [[04-host-scripts/index|Host scripts index]]
