#!/usr/bin/env python3
"""Test harness for the RFuzz 2FSK capture/decode pipeline.

Runs the *real* end-to-end flow (generate -> HackRF TX -> CC1101 capture ->
decode -> regenerate FSK) for a matrix of varied single-shot signals, each in
its own folder, and verifies the host scripts decode them and UART-frame them
correctly:

    generate  gen_2fsk.py -> <case>.c8            (varied preamble / payload)
    deploy    scp .c8 to the Dragon OS VM         (rshell-less)
    PASS 1    single TX pass + CC1101 packet RX   (COM7 frames received)
    PASS 2    single TX pass while capturing      -> <case>.raw / .sr / .vcd
    decode    rfuzz_tools.decode on the capture   (payload matches input)
    regen     rfuzz_tools.regen -> <case>_regen.c8 (FSK I/Q from the packet)
    verify    UART-decode probe3 (GDO2_CLEAN_RAW)
              and probe4 (GDO2_CLEAN) -> AA*p SYNC PAYLOAD

Folders:
    signals/testhw/<case>/        real hardware flow output
        <case>.c8                generated TX signal
        cap.raw                  GDO0/GDO2 capture
        cap.sr                   Sigrok session (5 channels)
        cap.vcd                  VCD
        cap_regen.c8             regenerated 2FSK I/Q
        result.txt               PASS/FAIL + decode details

The hardware flow needs the bench (COM7 CC1101 board + Dragon OS HackRF).  For a
fast, hardware-free sanity check use `--offline`, which feeds the same per-case
signals through a synthetic GDO0/GDO2 model and the *same* decode/channel/UART
path — no HackRF or COM7 required.

Usage (hardware):
    python test_synth_signals.py --host dragon@192.168.1.101 --port COM7
    python test_synth_signals.py --list

Usage (offline sanity):
    python test_synth_signals.py --offline
"""
import argparse
import os
import sys
import types

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import capture_custom  # noqa: E402
import gen_2fsk  # noqa: E402
import rfuzz_tools  # noqa: E402
import rfuzz_testcase  # noqa: E402

SIGNS = "2FSK"
SYNC_BYTES = bytes([0xDE, 0xAF])
DEFAULT_BAUD = 2400
DEFAULT_HOST = rfuzz_testcase.DEFAULT_HOST
DEFAULT_REMOTE_PATH = rfuzz_testcase.DEFAULT_REMOTE_PATH
DEFAULT_PORT = rfuzz_testcase.DEFAULT_PORT
DEFAULT_RATE = rfuzz_testcase.DEFAULT_RATE
DEFAULT_SAMPLES = rfuzz_testcase.DEFAULT_SAMPLES
GLITCH = 10

# (label, preamble_bytes, payload_bytes, gap_ms, desc)
# The CC1101 RX firmware uses a fixed 4-byte frame, so payloads are 4 bytes
# (any 4 bytes are relayed on COM7; 01020304 is the historical default).
CASES = [
    ("std_p8", 8, bytes([1, 2, 3, 4]), 0.0, "standard 8-byte preamble"),
    ("std_p4", 4, bytes([1, 2, 3, 4]), 0.0, "4-byte preamble"),
    ("long_p16", 16, bytes([1, 2, 3, 4]), 0.0, "16-byte preamble"),
    ("long_p128", 128, bytes([1, 2, 3, 4]), 0.0, "128-byte preamble"),
    ("rand_p8", 8, bytes([0x8b, 0x4a, 0xe5, 0xf1]), 0.0, "random 4-byte payload"),
    ("rand_p4", 4, bytes([0x12, 0x34, 0x56, 0x78]), 0.0, "random payload, p=4"),
    ("deadbeef", 4, bytes([0xDE, 0xAD, 0xBE, 0xEF]), 0.0, "deadbeef payload"),
    ("big_p8", 8, bytes([1, 2, 3, 4]), 0.0, "big-burst single-shot"),
    ("gapped_p8", 8, bytes([1, 2, 3, 4]), 50.0, "single-shot with 50ms gap"),
]

# Extra realism knobs for the offline synthetic model (jitter, glitches, seed,
# preamble settling bits).  Hardware captures carry these artifacts naturally;
# the synthetic model injects them so the same decode path is exercised.
OFFLINE_KW = {
    "std_p8": dict(jitter=0.0, glitch_samples=0, seed=1, settle=3),
    "std_p4": dict(jitter=0.0, glitch_samples=0, seed=2, settle=2),
    "long_p16": dict(jitter=0.0, glitch_samples=0, seed=3, settle=3),
    "long_p128": dict(jitter=0.0, glitch_samples=0, seed=4, settle=3),
    "rand_p8": dict(jitter=0.0, glitch_samples=0, seed=5, settle=3),
    "rand_p4": dict(jitter=0.0, glitch_samples=0, seed=6, settle=2),
    "deadbeef": dict(jitter=0.0, glitch_samples=0, seed=7, settle=3),
    "big_p8": dict(jitter=0.2, glitch_samples=20, seed=8, settle=4),
    "gapped_p8": dict(jitter=0.0, glitch_samples=0, seed=9, settle=3),
}


def make_args(host, port, rate, samples, gap_ms, mod="2fsk"):
    """Build the namespace the rfuzz_testcase phase functions expect."""
    return types.SimpleNamespace(
        host=host, port=port, rate=rate, samples=samples, gap_ms=gap_ms,
        gap=0.0, mod=mod, analog_amp=90, fsk_if=0.0, fsk_dev=rfuzz_tools.DEFAULT_DEV,
        preamble=4, tx_freq=rfuzz_testcase.DEFAULT_TX_FREQ,
        tx_srate=rfuzz_testcase.DEFAULT_TX_SRATE,
        tx_x=rfuzz_testcase.DEFAULT_TX_X, tx_a=rfuzz_testcase.DEFAULT_TX_A,
    )


def expected_packet(pre, payload):
    return bytes([0xAA]) * pre + SYNC_BYTES + payload


# --------------------------------------------------------------------------- #
# Hardware flow (real HackRF + CC1101)
# --------------------------------------------------------------------------- #
def run_case_hardware(label, pre, payload, gap_ms, base_dir, host, port,
                       rate, samples, tx_freq, tx_srate, tx_x, tx_a, mod,
                       fsk_if, fsk_dev):
    folder = os.path.join(base_dir, label)
    os.makedirs(folder, exist_ok=True)
    base = os.path.join(folder, "cap")
    a = make_args(host, port, rate, samples, gap_ms, mod)
    a.preamble = pre
    a.fsk_if = fsk_if
    a.fsk_dev = fsk_dev
    a.out_dir = folder
    a.name = "cap"

    tx_file = os.path.join(folder, "cap.c8")
    print(f"\n=== {label}: pre={pre} payload={payload.hex()} gap={gap_ms}ms ===")

    # 1. Generate the TX signal with the per-case preamble/payload.
    gen_2fsk.generate(tx_file, repeat=1, gap_ms=gap_ms,
                      preamble_bytes=pre, payload=payload)

    # 2. Deploy to the Dragon OS VM and PASS 1 (CC1101 packet RX).
    n_frames = rfuzz_testcase.phase_packets(a, DEFAULT_REMOTE_PATH)
    recv_ok = n_frames > 0
    print(f"  PASS1: CC1101 RX frames={n_frames} received={'OK' if recv_ok else 'FAIL'}")

    # 3/4/5/6. PASS 2 capture -> raw/sr/vcd, decode, regen.
    raw = rfuzz_testcase.phase_capture(a, DEFAULT_REMOTE_PATH, base)
    rfuzz_testcase.phase_decode(raw, a)
    regen_out = base + "_regen.c8"
    rfuzz_tools.cmd_regen(types.SimpleNamespace(
        raw=raw, rate=rate, spb=0.0, glitch=10, out=regen_out,
        amp=90, dev=50000.0, preamble=pre))

    # UART-decode probe3/probe4 from the generated .sr.
    import zipfile
    zf = zipfile.ZipFile(base + ".sr")
    data = np.frombuffer(zf.read("logic-1-1"), dtype=np.uint8)
    cr = (data >> 2) & 1
    cc = (data >> 3) & 1
    exp = expected_packet(pre, payload)
    m = int(round(rate / DEFAULT_BAUD))
    d3 = _uart_decode(np.asarray(cr, np.uint8), rate, m)
    d4 = _uart_decode(np.asarray(cc, np.uint8), rate, m)
    ok = recv_ok and (d3 == exp) and (d4 == exp)
    with open(os.path.join(folder, "result.txt"), "w") as f:
        f.write(f"label={label} pre={pre} payload={payload.hex()}\n")
        f.write(f"frames={n_frames} probe3={d3.hex(' ')}\n")
        f.write(f"probe4={d4.hex(' ')}\n")
        f.write(f"expected={exp.hex(' ')}\n")
        f.write(f"RESULT={'PASS' if ok else 'FAIL'}\n")
    print(f"  probe3={d3.hex(' ')[:48]} probe4={d4.hex(' ')[:48]} "
          f"-> {'PASS' if ok else 'FAIL'}")
    return ok


# --------------------------------------------------------------------------- #
# Offline flow (synthetic GDO0/GDO2 model, same decode/channel/UART path)
# --------------------------------------------------------------------------- #
def _render_gdo2(settled_bits, spb, jitter=0.0, glitch_samples=0, rng=None):
    if rng is None:
        rng = np.random.default_rng(0)
    m = int(round(spb))
    nbits = len(settled_bits)
    starts = np.zeros(nbits + 1, dtype=np.int64)
    for k in range(1, nbits + 1):
        starts[k] = starts[k - 1] + int(round(spb + rng.uniform(-jitter, jitter)))
    total = int(starts[nbits] + spb + 4 * spb)
    g = np.zeros(total, dtype=np.uint8)
    for k in range(nbits):
        a, b = starts[k], starts[k + 1]
        if a < total:
            g[a:min(b, total)] = int(settled_bits[k])
    if glitch_samples:
        for _ in range(glitch_samples):
            s = int(rng.integers(0, total - 1))
            g[s] = 1 - g[s]
            if s + 1 < total:
                g[s + 1] = 1 - g[s + 1]
    return g


def make_synthetic_raw(pre, payload, rate=DEFAULT_RATE, baud=DEFAULT_BAUD,
                       jitter=0.0, glitch_samples=0, seed=0, settle=3):
    rng = np.random.default_rng(seed)
    spb = rate / baud
    bits = np.concatenate([
        rfuzz_tools.preamble_bits(pre),
        rfuzz_tools.SYNC_BITS,
        rfuzz_tools.bits_msb(payload),
    ]).astype(np.uint8)
    settled = bits.copy()
    if settle:
        n = min(settle, pre * 8)
        settled[:n] = rng.integers(0, 2, size=n).astype(np.uint8)
    gdo2 = _render_gdo2(settled, spb, jitter=jitter,
                       glitch_samples=glitch_samples, rng=rng)
    uart_bits = (pre + 2 + len(payload)) * 10
    margin = uart_bits * int(round(spb))
    idle = max(margin, 512)
    pre_pad = np.zeros(idle, dtype=np.uint8)
    post_pad = np.zeros(idle, dtype=np.uint8)
    gdo2 = np.concatenate([pre_pad, gdo2, post_pad]).astype(np.uint8)
    burst_start = idle
    sync_arrival = burst_start + int(round((pre * 8 + 16) * spb))
    gdo0 = np.zeros(len(gdo2), dtype=np.uint8)
    strobe_end = min(len(gdo0), int(round(sync_arrival + 40 * spb)))
    gdo0[sync_arrival:strobe_end] = 1
    packed = bytes((int(gdo0[i]) & 1) | ((int(gdo2[i]) & 1) << 1)
                   for i in range(len(gdo0)))
    return packed, spb


def _uart_decode(ch, rate, m):
    n = len(ch)
    out = []
    i = 0
    while i + 1 < n:
        if ch[i] == 1 and ch[i + 1] == 0 and i + 10 * m < n:
            start = i + 1
            vals = [int(ch[start + int(round((k + 1.5) * m))]) for k in range(8)]
            stop = int(ch[start + int(round(9.5 * m))])
            if stop != 1:
                i += 1
                continue
            v = 0
            for b in vals:
                v = (v << 1) | b
            out.append(v)
            i = start + 10 * m - 1
        else:
            i += 1
    return bytes(out)


def run_case_offline(label, pre, payload, gap_ms, base_dir, jitter=0.0,
                     glitch_samples=0, seed=0, settle=3):
    folder = os.path.join(base_dir, label)
    os.makedirs(folder, exist_ok=True)
    rate = DEFAULT_RATE
    raw, spb = make_synthetic_raw(pre, payload, rate=rate, jitter=jitter,
                                 glitch_samples=glitch_samples, seed=seed,
                                 settle=settle)
    raw_path = os.path.join(folder, "cap.raw")
    with open(raw_path, "wb") as f:
        f.write(raw)

    ch0, ch1 = capture_custom.decode(bytes(raw))
    r_actual = capture_custom.actual_rate(rate)
    ch0n = np.asarray(ch0, np.uint8)
    ch1n = np.asarray(ch1, np.uint8)
    det_spb, _, windows = rfuzz_tools.detect_datarate_iter(
        ch0n, ch1n, r_actual, r_actual / DEFAULT_BAUD, pre, GLITCH)
    bits = rfuzz_tools.clean_bits(ch1, det_spb, GLITCH)
    packets = rfuzz_tools.find_packets(bits, det_spb, pre)
    starts = rfuzz_tools.packet_starts(ch0n, det_spb, packets, pre)
    packets, gstarts = rfuzz_tools.refine_packets(ch1n, det_spb, packets, starts, pre)
    decoded = packets[0]["payload"] if packets else None

    ch1cr = rfuzz_tools.clean_gdo2(packets, gstarts, det_spb, pre, len(ch1),
                                   framed=True, corrected=True)
    ch1c = rfuzz_tools.clean_gdo2(packets, gstarts, det_spb, pre, len(ch1),
                                  framed=True)
    fsk = np.zeros(len(ch1), dtype=np.float32)
    if_dev, dev_hz = capture_custom.synth_defaults(r_actual, rfuzz_tools.DEFAULT_DEV)
    for p, start in zip(packets, gstarts):
        pbits = np.concatenate([
            rfuzz_tools.preamble_bits(pre), rfuzz_tools.SYNC_BITS,
            rfuzz_tools.bits_msb(p["payload"])])
        fsk += capture_custom.mod_synth_from_bits(
            pbits, 90, "2fsk", if_dev, dev_hz, r_actual, len(ch1), det_spb, start)
    base = os.path.join(folder, "cap")
    capture_custom.save_sr(ch0, ch1, ch1cr, ch1c, fsk,
                           int(round(r_actual)), base + ".sr")
    capture_custom.save_vcd(ch0, ch1, ch1cr, ch1c, fsk, r_actual, base + ".vcd")
    # regen FSK I/Q from the decoded packet
    regen_out = base + "_regen.c8"
    rfuzz_tools.cmd_regen(types.SimpleNamespace(
        raw=raw_path, rate=rate, spb=0.0, glitch=10, out=regen_out,
        amp=90, dev=50000.0, preamble=pre))

    m = int(round(rate / DEFAULT_BAUD))
    exp = expected_packet(pre, payload)
    d3 = _uart_decode(np.asarray(ch1cr, np.uint8), rate, m)
    d4 = _uart_decode(np.asarray(ch1c, np.uint8), rate, m)
    ok = (packets and decoded == payload and d3 == exp and d4 == exp)
    with open(os.path.join(folder, "result.txt"), "w") as f:
        f.write(f"label={label} pre={pre} payload={payload.hex()}\n")
        f.write(f"decoded={decoded.hex() if decoded else '(none)'}\n")
        f.write(f"probe3={d3.hex(' ')}\nprobe4={d4.hex(' ')}\n")
        f.write(f"expected={exp.hex(' ')}\nRESULT={'PASS' if ok else 'FAIL'}\n")
    status = "PASS" if ok else "FAIL"
    print(f"[{status}] {label:14s} pre={pre} pay={payload.hex()} "
          f"decoded={'Y' if decoded == payload else 'N'} "
          f"probe3={len(d3)}B probe4={len(d4)}B")
    if not ok:
        print(f"   expected={exp.hex(' ')[:60]}")
        print(f"   probe3 ={d3.hex(' ')[:60]}")
        print(f"   probe4 ={d4.hex(' ')[:60]}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--rate", type=int, default=DEFAULT_RATE)
    ap.add_argument("--samples", type=int, default=DEFAULT_SAMPLES)
    ap.add_argument("--tx-freq", type=int, default=rfuzz_testcase.DEFAULT_TX_FREQ)
    ap.add_argument("--tx-srate", type=int,
                    default=rfuzz_testcase.DEFAULT_TX_SRATE)
    ap.add_argument("--tx-x", type=int, default=rfuzz_testcase.DEFAULT_TX_X)
    ap.add_argument("--tx-a", type=int, default=rfuzz_testcase.DEFAULT_TX_A)
    ap.add_argument("--fsk-if", type=float, default=0.0)
    ap.add_argument("--fsk-dev", type=float, default=rfuzz_tools.DEFAULT_DEV)
    ap.add_argument("--mod", default="2fsk", choices=capture_custom.MODS)
    ap.add_argument("--gap-ms", type=float, default=0.0,
                    help="gap ms override applied to all cases")
    ap.add_argument("--out-dir", default="signals/testhw")
    ap.add_argument("--offline", action="store_true",
                    help="synthetic GDO0/GDO2 (no HackRF/COM7 needed)")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    if args.list:
        for c in CASES:
            print(f"  {c[0]:14s} pre={c[1]} pay={c[2].hex():10s} "
                  f"{c[3]:<5} {c[4]}")
        return 0

    npass = 0
    for c in CASES:
        label, pre, payload, gap_ms, desc = c
        if args.offline:
            kw = OFFLINE_KW.get(label, dict(jitter=0.0, glitch_samples=0, seed=0, settle=3))
            ok = run_case_offline(label, pre, payload, gap_ms, args.out_dir, **kw)
        else:
            ok = run_case_hardware(label, pre, payload, gap_ms, args.out_dir,
                                   args.host, args.port, args.rate, args.samples,
                                   args.tx_freq, args.tx_srate, args.tx_x,
                                   args.tx_a, args.mod, args.fsk_if,
                                   args.fsk_dev)
        npass += 1 if ok else 0
    n = len(CASES)
    print(f"\n{npass}/{n} passed ({'offline' if args.offline else 'hardware'})")
    return 0 if npass == n else 1


if __name__ == "__main__":
    sys.exit(main())
