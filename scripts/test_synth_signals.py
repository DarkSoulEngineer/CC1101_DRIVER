#!/usr/bin/env python3
"""Offline test harness for the RFuzz host decode/capture pipeline.

The end-to-end test case (`rfuzz_testcase.py`) needs a HackRF + CC1101 to make
real `.raw` captures.  This script builds *synthetic* `.raw` captures instead:
it renders the digital GDO0 sync strobe and the GDO2 async-data line that the
CC1101 *would* produce for a known packet, including the realistic artifacts
the host scripts must cope with:

  - bit-sync settling: the first few bits of the burst are corrupt (the modem
    needs a couple of bit cells to lock onto the preamble);
  - per-cell timing jitter: bit boundaries wobble;
  - sub-bit glitches: short spurious pulses the glitch filter must swallow.

It then runs the real pipeline (`rfuzz_tools` decode path + `capture_custom`
channel rendering) on each capture and checks that:

  - the payload decodes with 0 bit errors;
  - the regenerated GDO2_CLEAN_RAW (probe3) and GDO2_CLEAN (probe4) both
    UART-decode (2400 baud, MSB-first) to the original packet bytes.

Each case is written to its own folder under `signals/test/<case>/`:

    cap.raw        synthetic capture (bit0=GDO0, bit1=GDO2)
    cap_clean.raw  <base>_clean.raw from the `clean` subcommand
    cap.sr         Sigrok session (5 channels)
    cap.vcd        VCD

Usage:
    python test_synth_signals.py            # generate + verify all cases
    python test_synth_signals.py --list     # show cases only
"""
import argparse
import os
import sys

import numpy as np

import rfuzz_tools as R
import capture_custom as C

SYNC_BITS = R.SYNC_BITS
SYNC_BYTES = bytes([0xDE, 0xAF])
DEFAULT_BAUD = 2400
DEFAULT_RATE = 250000
GLITCH = 10
SETTLE_BITS = 3  # CC1101 bit-sync settling: first ~3 preamble bits are corrupt


def preamble_bits(pre_bytes):
    return R.preamble_bits(pre_bytes)


def bits_msb(data):
    return R.bits_msb(data)


def render_gdo2(packet_bits, spb, jitter=0.0, glitch_samples=0, rng=None):
    """Render a bit sequence as NRZ GDO2 with jitter + injected glitches.

    Bit `k` occupies samples [round(k*spb + phase_k), round((k+1)*spb + phase_k))
    where phase_k accumulates a per-bit jitter. Idle level = 0 (matches raw GDO2).
    """
    if rng is None:
        rng = np.random.default_rng(0)
    nbits = len(packet_bits)
    # cell starts
    starts = np.zeros(nbits + 1, dtype=np.int64)
    for k in range(1, nbits + 1):
        starts[k] = starts[k - 1] + int(round(spb + rng.uniform(-jitter, jitter)))
    total = int(starts[nbits] + spb + 4 * spb)
    gdo2 = np.zeros(total, dtype=np.uint8)
    for k in range(nbits):
        a = starts[k]
        b = starts[k + 1]
        if a < total:
            gdo2[a:min(b, total)] = int(packet_bits[k])
    # inject short sub-bit glitches (run shorter than the glitch threshold)
    if glitch_samples:
        n = glitch_samples
        for _ in range(n):
            s = int(rng.integers(0, total - 1))
            gdo2[s] = 1 - gdo2[s]
            if s + 1 < total:
                gdo2[s + 1] = 1 - gdo2[s + 1]
    return gdo2, starts


def make_capture(preamble_bytes, payload, rate=DEFAULT_RATE, baud=DEFAULT_BAUD,
                 jitter=0.0, glitch_samples=0, seed=0, settle=SETTLE_BITS,
                 idle_pre=64, idle_post=64):
    """Build a synthetic GDO0/GDO2 .raw capture for one packet.

    Returns (raw_bytes, spb, preamble_bytes).  The GDO2 idle is 0 (low), matching
    a real CC1101 async-data line.  GDO0 is a rising pulse at sync arrival
    (burst_start + (preamble+sync)*spb) sustained through the payload, used by
    packet_anchors/packet_windows to anchor and gate the packet.
    """
    rng = np.random.default_rng(seed)
    spb = rate / baud
    m = int(round(spb))

    pbits = np.concatenate([
        preamble_bits(preamble_bytes),
        SYNC_BITS,
        bits_msb(payload),
    ]).astype(np.uint8)
    nbits = len(pbits)

    # Bit-sync settling: the first `settle` preamble bits are corrupt (random).
    settled = pbits.copy()
    if settle:
        n_settle = min(settle, preamble_bytes * 8)
        settled[:n_settle] = rng.integers(0, 2, size=n_settle).astype(np.uint8)

    gdo2, cell_starts = render_gdo2(settled, spb, jitter=jitter,
                                    glitch_samples=glitch_samples, rng=rng)

    # Size the idle buffers so the window can hold a UART frame (10 bits/byte),
    # which is 25% longer than the raw NRZ packet (8 bits/byte).  The host
    # scripts render the frame over the detected bit grid; the capture buffer
    # must be larger than the frame, like a real 262 k-sample window.
    pay_bytes = len(payload)
    uart_bits = (preamble_bytes + 2 + pay_bytes) * 10
    margin = uart_bits * m
    pre_pad_total = max(idle_pre, margin)
    post_pad_total = max(idle_post, margin)

    # GDO2 idles LOW (matches a real CC1101 async-data line); pad the burst
    # with idle samples before and after so packet_windows has room to backfill
    # the preamble ahead of the GDO0 sync strobe.
    pre_pad = np.zeros(pre_pad_total, dtype=np.uint8)
    post_pad = np.zeros(post_pad_total, dtype=np.uint8)
    gdo2_full = np.concatenate([pre_pad, gdo2, post_pad]).astype(np.uint8)
    burst_start = pre_pad_total

    # GDO0 sync strobe: rises at sync arrival (burst_start + (pre+sync)*spb)
    # and stays high through the payload, so packet_anchors/packet_windows can
    # anchor and gate the packet.
    sync_arrival = burst_start + int(round((preamble_bytes * 8 + len(SYNC_BITS)) * spb))
    gdo0 = np.zeros(len(gdo2_full), dtype=np.uint8)
    pay_bits = 32
    strobe_end = min(len(gdo0), int(round(sync_arrival + (pay_bits + 4) * spb)))
    gdo0[sync_arrival:strobe_end] = 1

    packed = bytes((int(gdo0[i]) & 1) | ((int(gdo2_full[i]) & 1) << 1)
                   for i in range(len(gdo0)))
    return packed, spb, preamble_bytes


def uart_decode(ch, rate, baud=DEFAULT_BAUD):
    """Sample async UART (start bit, 8 data MSB-first, stop bit).

    Lock on the first idle->start falling edge, then walk frame-by-frame on the
    integer sample-per-bit grid `m = round(rate/baud)` (the renderer uses m
    samples/bit).  A stock 2400-baud MSB-first UART decoder reads exactly this.
    """
    m = int(round(rate / baud))
    n = len(ch)
    out = []
    i = 0
    while i + 1 < n:
        if ch[i] == 1 and ch[i + 1] == 0 and i + 10 * m < n:
            start = i + 1
            vals = [int(ch[start + int(round((k + 1.5) * m))]) for k in range(8)]
            stop = int(ch[start + int(round(9.5 * m))])
            if stop == 1:
                val = 0
                for k, b in enumerate(vals):
                    val = (val << 1) | b
                out.append(val)
                # next frame's start bit begins at start + 10*m; the falling edge
                # (stop 1 -> start 0) is at start + 10*m - 1
                i = start + 10 * m - 1
            else:
                i += 1
        else:
            i += 1
    return bytes(out)


CASES = [
    ("std_p8", 8, bytes([1, 2, 3, 4]), 0.0, 0, 0, "standard 8-byte preamble"),
    ("std_p4", 4, bytes([1, 2, 3, 4]), 0.0, 0, 0, "4-byte preamble"),
    ("long_p16", 16, bytes([1, 2, 3, 4]), 0.0, 0, 0, "16-byte preamble"),
    ("long_p128", 128, bytes([1, 2, 3, 4]), 0.0, 0, 0, "128-byte preamble"),
    ("rand_p8", 8, bytes([0x8b, 0x4a, 0xe5, 0xf1]), 0.0, 0, 123, "4-byte random payload"),
    ("rand_p4", 4, bytes([0x12, 0x34, 0x56, 0x78]), 0.0, 0, 4242, "4-byte random payload, p=4"),
    ("deadbeef", 4, bytes([0xDE, 0xAD, 0xBE, 0xEF]), 0.0, 0, 7, "deadbeef payload"),
    ("settling_p8", 8, bytes([1, 2, 3, 4]), 0.0, 0, 99, "with preamble settling"),
    ("jitter_p8", 8, bytes([1, 2, 3, 4]), 0.25, 0, 0, "with bit-jitter"),
    ("glitch_p8", 8, bytes([1, 2, 3, 4]), 0.0, 30, 0, "with sub-bit glitches"),
    ("full_p8", 8, bytes([1, 2, 3, 4]), 0.2, 25, 55, "jitter+glitches+settling"),
]


def run_case(case, out_dir):
    label, pre, payload, jitter, glitches, seed, desc = case
    raw, spb, _ = make_capture(pre, payload, jitter=jitter,
                               glitch_samples=glitches, seed=seed)
    folder = os.path.join(out_dir, label)
    os.makedirs(folder, exist_ok=True)
    raw_path = os.path.join(folder, "cap.raw")
    with open(raw_path, "wb") as f:
        f.write(raw)

    ch0, ch1 = C.decode(raw)
    r_actual = C.actual_rate(DEFAULT_RATE)
    ch0n = np.asarray(ch0, np.uint8)
    ch1n = np.asarray(ch1, np.uint8)
    det_spb, det_bps, windows = R.detect_datarate_iter(
        ch0n, ch1n, r_actual, r_actual / DEFAULT_BAUD, pre)
    dec = R.clean_bits(ch1, det_spb, GLITCH)
    packets = R.find_packets(dec, det_spb, pre)
    starts = R.packet_starts(ch0n, det_spb, packets, pre)
    packets, gstarts = R.refine_packets(ch1n, det_spb, packets, starts, pre)

    expected = bytes([0xAA]) * pre + SYNC_BYTES + payload
    # `decode_ok` uses the pipeline's bit_errors, which compare against the
    # hardcoded PAYLOAD_EXPECT (01 02 03 04); it is informational only.  The
    # real correctness checks are: the decoded payload equals the one we
    # generated, the per-cell-refine bits match it, and both UART channels
    # decode to the original packet.
    decode_ok = bool(packets) and all(not p["bit_errors"] for p in packets)
    if packets:
        decoded_payload = packets[0]["payload"]
        payload_ok = decoded_payload == payload
        pbits = packets[0]["pbits"]
        pbits_ok = (R.bytes_from_bits(pbits[pre * 8:pre * 8 + 16]) == SYNC_BYTES
                    and R.bytes_from_bits(pbits[pre * 8 + 16:]) == payload)
    else:
        decoded_payload = None
        payload_ok = False
        pbits_ok = False

    # Build probe3/probe4 and UART-decode them.
    ch1cr = R.clean_gdo2(packets, gstarts, det_spb, pre, len(ch1),
                        framed=True, corrected=True)
    ch1c = R.clean_gdo2(packets, gstarts, det_spb, pre, len(ch1), framed=True)
    d3 = uart_decode(np.asarray(ch1cr, np.uint8), r_actual)
    d4 = uart_decode(np.asarray(ch1c, np.uint8), r_actual)
    probe3_ok = d3 == expected
    probe4_ok = d4 == expected

    C.save_sr(ch0, ch1, ch1cr, ch1c, np.zeros(len(ch1), np.float32),
              int(round(r_actual)), os.path.join(folder, "cap.sr"))
    C.save_vcd(ch0, ch1, ch1cr, ch1c, np.zeros(len(ch1), np.float32),
               r_actual, os.path.join(folder, "cap.vcd"))

    status = "PASS" if (payload_ok and pbits_ok and probe3_ok and probe4_ok) else "FAIL"
    print(f"[{status}] {label:16s} {desc:28s} pre={pre} pay={payload.hex()} "
          f"det_spb={det_spb:.2f} pay={payload_ok} pbits={pbits_ok} "
          f"probe3={probe3_ok} probe4={probe4_ok} (decode_exp={'Y' if decode_ok else 'N'})")
    if status == "FAIL":
        print(f"        expected={expected.hex(' ')}")
        print(f"        probe3 ={d3.hex(' ')[:80]}")
        print(f"        probe4 ={d4.hex(' ')[:80]}")
    return status == "PASS"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default="signals/test")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    cases = CASES
    print(f"{len(cases)} test cases")
    if args.list:
        for c in cases:
            print(f"  {c[0]:16s} {c[-1]}")
        return 0
    npass = 0
    for c in cases:
        if run_case(c, args.out_dir):
            npass += 1
    n = len(cases)
    print(f"\n{npass}/{n} passed")
    return 0 if npass == n else 1


if __name__ == "__main__":
    sys.exit(main())
