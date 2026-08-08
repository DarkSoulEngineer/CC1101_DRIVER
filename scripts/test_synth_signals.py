#!/usr/bin/env python3
"""Test harness for the RFuzz 2FSK capture/decode pipeline.

Runs the *real* end-to-end flow (generate -> HackRF TX -> CC1101 capture ->
decode -> regenerate FSK) for a matrix of varied single-shot signals, each in
its own folder, and verifies the host scripts decode them and UART-frame the
clean channel correctly:

    generate  gen_2fsk.py -> <case>.c8            (varied preamble / payload)
    deploy    scp .c8 to the Dragon OS VM
    PASS 1    single TX pass + CC1101 packet RX     (COM7 frames received)
    PASS 2    single TX pass while capturing       -> <case>.raw / .sr / .vcd
    decode    rfuzz_tools.decode on the capture    (payload matches input)
    regen     rfuzz_tools.regen -> <case>_regen.c8 (FSK I/Q from the packet)
    verify    UART-decode probe3 (GDO2_CLEAN) -> AA*p DEAF PAYLOAD

Folder layout (each case):
    signals/testhw/<case>/
        <case>.c8        generated TX signal
        cap.raw          GDO0/GDO2 capture
        cap.sr           Sigrok session (4 channels)
        cap.vcd          VCD
        cap_regen.c8     regenerated 2FSK I/Q
        result.txt       PASS/FAIL + decode details

Captures are 4 channels:
    probe1 = GDO0         (logic, sync / TX active, strobe on sync match)
    probe2 = GDO2         (logic, async demodulated data, NRZ)
    probe3 = GDO2_CLEAN   (logic, UART 2400-baud frame of the corrected packet)
    analog4 = FSK         (analog, synthesized 2FSK sinusoid)

The hardware flow needs the bench (COM7 CC1101 board + Dragon OS HackRF).  For a
fast, hardware-free sanity check use `--offline`, which feeds the same per-case
signals through a synthetic GDO0/GDO2 model and the *same* decode/channel/UART
path -- no HackRF or COM7 required.

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
    ("rand_p8", 8, bytes([0x8b, 0x4a, 0xe5, 0xf1]), 0.0, "random payload"),
    ("rand_p16", 16, bytes([0x12, 0x34, 0x56, 0x78]), 0.0, "random payload, p=16"),
    ("deadbeef", 8, bytes([0xDE, 0xAD, 0xBE, 0xEF]), 0.0, "deadbeef payload"),
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
    "rand_p16": dict(jitter=0.0, glitch_samples=0, seed=42, settle=3),
    "deadbeef": dict(jitter=0.0, glitch_samples=0, seed=7, settle=3),
    "big_p8": dict(jitter=0.2, glitch_samples=20, seed=8, settle=4),
    "gapped_p8": dict(jitter=0.0, glitch_samples=0, seed=9, settle=3),
}


def make_args(host, port, rate, samples, gap_ms, mod="2fsk",
              payload=None):
    """Build the namespace the rfuzz_testcase phase functions expect."""
    return types.SimpleNamespace(
        host=host, port=port, rate=rate, samples=samples, gap_ms=gap_ms,
        gap=0.0, mod=mod, analog_amp=90, fsk_if=0.0, fsk_dev=rfuzz_tools.DEFAULT_DEV,
        payload=payload.hex() if payload else None,
        preamble=4, tx_freq=rfuzz_testcase.DEFAULT_TX_FREQ,
        tx_srate=rfuzz_testcase.DEFAULT_TX_SRATE,
        tx_x=rfuzz_testcase.DEFAULT_TX_X, tx_a=rfuzz_testcase.DEFAULT_TX_A,
    )


def expected_packet(pre, payload):
    return bytes([0xAA]) * pre + SYNC_BYTES + payload


def _uart_decode(ch, rate, m):
    """Decode a UART-framed bitstream (8N1, MSB-first) from a clean NRZ channel.

    A UART byte is framed [start=0][8 data MSB-first][stop=1], each bit lasting
    `m` samples.  This walks the bit stream for a start edge (1->0) and samples
    the 8 data bits near each cell centre, validating the stop bit.
    """
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


def load_regen_iq(path):
    """Read a regen .c8 (int8 I/Q interleaved) as a complex64 array."""
    b = np.fromfile(path, dtype=np.int8)
    if b.size == 0:
        return np.zeros(0, dtype=np.complex64)
    iq = b[0::2].astype(np.float32) + 1j * b[1::2].astype(np.float32)
    return iq.astype(np.complex64)


def bytes_from_bits(bits):
    """Group an MSB-first bit sequence into bytes (8 bits per byte)."""
    return bytes(int("".join(str(b) for b in bits[j:j + 8]), 2)
                 for j in range(0, len(bits) - 7, 8))


def verify_fsk_regen(regen_c8, pre, payload, rate, bps):
    """Demod the regenerated 2FSK I/Q and confirm it carries the packet.

    The regen .c8 is complex 2FSK (freq = +50 kHz for bit 1, -50 kHz for bit 0,
    phase-continuous) sampled at FS=2.4 MHz, embedded in a silent window.  We
    isolate the active (modulated) region, recover one bit per `sps`
    samples from the net phase rotation, then locate the sync word and read the
    32-bit MSB-first payload - the reverse of gen_2fsk/modulate.
    """
    if not os.path.exists(regen_c8):
        return False, "no regen file"
    iq = load_regen_iq(regen_c8)
    if iq.size == 0:
        return False, "empty regen"
    fs = rfuzz_tools.FS
    sps = int(round(fs / bps))
    amp = np.abs(iq)
    active = amp > 5.0
    if not np.any(active):
        return False, "no modulated region"
    idx = np.flatnonzero(active)
    lo, hi = idx[0], idx[-1] + 1
    region = iq[lo:hi]
    # Recover bits from net phase rotation over each sps-sample window.
    phase = np.unwrap(np.angle(region))
    nbits = max(0, len(phase) // sps)
    bits = []
    for k in range(nbits):
        s = k * sps
        e = s + sps
        if e >= len(phase):
            break
        rot = phase[e] - phase[s]
        bits.append(1 if rot > 0 else 0)
    bits = np.asarray(bits, dtype=np.uint8)
    # Walk the bit stream: find SYNC_BITS, then read 32 payload bits, and
    # require a valid (alternating) preamble before it for a strong match.
    sync = np.asarray(rfuzz_tools.SYNC_BITS, dtype=np.uint8)
    sl = len(sync)
    found = False
    recovered = None
    for i in range(len(bits) - sl - 32 + 1):
        if (bits[i:i + sl] == sync).all():
            pb = bits[i + sl:i + sl + 32]
            if len(pb) == 32:
                recovered = bytes_from_bits(pb)
                pre_region = bits[max(0, i - pre * 8):i]
                # preamble is 0xAA (alternating); accept if mostly alternating
                if len(pre_region) and np.mean(pre_region ==
                                               np.roll(pre_region, 1) ^ 1) > 0.75:
                    found = True
                    break
    exp = bytes([0xAA]) * pre + SYNC_BYTES + payload
    if found and recovered == payload:
        return True, f"fsk {recovered.hex()} == {payload.hex()}"
    return False, f"fsk recovered={recovered.hex() if recovered else 'none'} " \
                  f"expected payload={payload.hex()}"


def verify_sr(sr_path, raw_path, rate, pre, payload):
    """Validate a produced .sr against the sigrok/v2 session format and the
    source raw capture (structure + self-consistency + content).

    Checks, mirroring libsigrok's File_format:Sigrok/v2 reader and srzip
    writer conventions:
      - zip members: version, metadata, logic-1-1, analog-1-4
        (analog channel number = total_logic + 1, i.e. 4 for 3 probes)
      - metadata keys: capturefile/unitsize/total probes/samplerate/
        total analog/probe1..3/analog4, version == "2"
      - logic-1-1 length == raw length; probe1/probe2 bits == raw bit0/bit1
      - analog-1-4 is float32, length == raw length, bounded by amp, with a
        spectral peak near IF +- dev (the synthesized 2FSK tones)
      - probe3 (GDO2_CLEAN) UART-decodes to the exact expected packet bytes
    Returns (ok, message).
    """
    import zipfile
    raw = open(raw_path, "rb").read()
    with zipfile.ZipFile(sr_path) as zf:
        names = set(zf.namelist())
        required = {"version", "metadata", "logic-1-1", "analog-1-4"}
        if not required <= names:
            return False, f"missing members {sorted(required - names)}"
        if zf.read("version") != b"2":
            return False, "version != '2'"
        meta = zf.read("metadata").decode()
        want = [
            "capturefile = logic-1", "unitsize = 1", "total probes = 3",
            "samplerate = %d" % int(round(rate)), "total analog = 1",
            "probe1 = GDO0", "probe2 = GDO2", "probe3 = GDO2_CLEAN",
            "analog4 = FSK",
        ]
        for key in want:
            if key not in meta:
                return False, f"metadata missing {key!r} (rate={int(round(rate))})"
        logic = np.frombuffer(zf.read("logic-1-1"), dtype=np.uint8)
        raw8 = np.frombuffer(raw, dtype=np.uint8)
        if len(logic) != len(raw8):
            return False, f"logic len {len(logic)} != raw {len(raw8)}"
        if ((logic & 1) != (raw8 & 1)).any():
            return False, "probe1 != raw bit0"
        if (((logic >> 1) & 1) != ((raw8 >> 1) & 1)).any():
            return False, "probe2 != raw bit1"
        an = np.frombuffer(zf.read("analog-1-4"), dtype="<f4")
        if len(an) != len(raw8):
            return False, f"analog len {len(an)} != raw {len(raw8)}"
        if not np.isfinite(an).all():
            return False, "analog has NaN/Inf"
        if an.size and an[np.abs(an).argmax()] > 90 * 1.1:
            return False, f"analog amplitude {an.max():.0f} > amp 90"
        # probe3 (GDO2_CLEAN) must UART-decode to the expected packet.
        d3 = _uart_decode(np.asarray((logic >> 2) & 1, np.uint8), rate,
                          int(round(rate / DEFAULT_BAUD)))
        exp = expected_packet(pre, payload)
        if d3 != exp:
            return False, f"probe3={d3.hex(' ')[:48]} != {exp.hex(' ')[:48]}"
        # spectral sanity: max peak near one of the two FSK tones.
        if an.size:
            active = np.abs(an) > 1
            if active.any():
                idx = np.flatnonzero(active)
                seg = an[idx[0]:idx[-1] + 1]
                if len(seg) >= 8:
                    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
                    fr = np.fft.rfftfreq(len(seg), 1.0 / rate)
                    top = float(fr[np.argmax(spec)])
                    if_dev, dev_hz = capture_custom.synth_defaults(
                        rate, rfuzz_tools.DEFAULT_DEV)
                    lo, hi = if_dev - dev_hz, if_dev + dev_hz
                    if not (lo - dev_hz <= top <= hi + dev_hz):
                        return False, f"analog peak {top:.0f} Hz outside " \
                                      f"[{lo:.0f},{hi:.0f}]"
    return True, "structure + probes + analog + probe3 all consistent"


def run_negative_offline(label, base_dir, kind="noise"):
    """Robustness cases: the pipeline must degrade cleanly (0 packets, no
    crash) when the capture carries no valid signal.  `kind` is one of
    'noise' (random GDO0/GDO2), 'dc_high' (GDO2 stuck high), 'idle'
    (all samples 0)."""
    rate = DEFAULT_RATE
    n = 20000
    rng = np.random.default_rng(0)
    if kind == "noise":
        g0 = rng.integers(0, 2, size=n).astype(np.uint8)
        g1 = rng.integers(0, 2, size=n).astype(np.uint8)
    elif kind == "dc_high":
        g0 = np.zeros(n, dtype=np.uint8)
        g1 = np.ones(n, dtype=np.uint8)
    else:
        g0 = np.zeros(n, dtype=np.uint8)
        g1 = np.zeros(n, dtype=np.uint8)
    raw = bytes((int(g0[i]) & 1) | ((int(g1[i]) & 1) << 1)
                for i in range(n))
    folder = os.path.join(base_dir, label)
    os.makedirs(folder, exist_ok=True)
    raw_path = os.path.join(folder, "cap.raw")
    with open(raw_path, "wb") as f:
        f.write(raw)

    ch0, ch1 = capture_custom.decode(raw)
    r_actual = capture_custom.actual_rate(rate)
    ch0n = np.asarray(ch0, dtype=np.uint8)
    ch1n = np.asarray(ch1, dtype=np.uint8)
    spb, _, windows = rfuzz_tools.detect_datarate_iter(
        ch0n, ch1n, r_actual, r_actual / DEFAULT_BAUD, 8, GLITCH)
    bits = rfuzz_tools.clean_bits(ch1, spb, GLITCH)
    packets = rfuzz_tools.find_packets(bits, spb, 8, bytes([1, 2, 3, 4]))
    ok = (spb is not None and len(packets) == 0)
    with open(os.path.join(folder, "result.txt"), "w") as f:
        f.write(f"label={label} kind={kind}\n")
        f.write(f"spb={spb} packets={len(packets)}\n")
        f.write(f"RESULT={'PASS' if ok else 'FAIL'}\n")
    status = "PASS" if ok else "FAIL"
    print(f"[{status}] {label:14s} kind={kind:8s} spb={spb} "
          f"packets={len(packets)} (expect 0)")
    return ok


# --------------------------------------------------------------------------- #
# Hardware flow (real HackRF + CC1101)
# --------------------------------------------------------------------------- #
def run_case_hardware(label, pre, payload, gap_ms, base_dir, host, port,
                      rate, samples, tx_freq, tx_srate, tx_x, tx_a, mod,
                      fsk_if, fsk_dev):
    folder = os.path.join(base_dir, label)
    os.makedirs(folder, exist_ok=True)
    base = os.path.join(folder, "cap")
    a = make_args(host, port, rate, samples, gap_ms, mod, payload)
    a.preamble = pre
    a.fsk_if = fsk_if
    a.fsk_dev = fsk_dev
    a.out_dir = folder
    a.name = "cap"

    tx_file = os.path.join(folder, "cap.c8")
    print(f"\n=== {label}: pre={pre} payload={payload.hex()} gap={gap_ms}ms ===")

    # 1. Generate the TX signal with the per-case preamble/payload and deploy it
    #    to the Dragon OS VM (rfuzz_testcase TXes from DEFAULT_REMOTE_PATH).
    gen_2fsk.generate(tx_file, repeat=1, gap_ms=gap_ms,
                      preamble_bytes=pre, payload=payload)
    r = rfuzz_testcase.sh(f'scp "{tx_file}" {host}:{DEFAULT_REMOTE_PATH}',
                          timeout=60)
    if r.returncode != 0:
        sys.exit(f"scp of {tx_file} failed (rc={r.returncode}): {r.stderr[-300:]}")

    # 2. Deploy to the Dragon OS VM and PASS 1 (CC1101 packet RX).
    n_frames = rfuzz_testcase.phase_packets(a, DEFAULT_REMOTE_PATH)
    recv_ok = n_frames > 0
    print(f"  PASS1: CC1101 RX frames={n_frames} received={'OK' if recv_ok else 'FAIL'}")

    # 3/4/5/6. PASS 2 capture -> raw/sr/vcd, decode, regen.
    raw_path = rfuzz_testcase.phase_capture(a, DEFAULT_REMOTE_PATH, base)
    rfuzz_testcase.phase_decode(raw_path, a)
    regen_out = base + "_regen.c8"
    rfuzz_tools.cmd_regen(types.SimpleNamespace(
        raw=raw_path, rate=rate, spb=0.0, glitch=10, out=regen_out,
        amp=90, dev=50000.0, preamble=pre))

    # FSK view: demod the regenerated .c8 back to bits and confirm the packet.
    ch0, ch1 = capture_custom.decode(open(raw_path, "rb").read())
    det_spb, det_bps, _ = rfuzz_tools.detect_datarate_iter(
        np.asarray(ch0, np.uint8), np.asarray(ch1, np.uint8),
        capture_custom.actual_rate(rate), capture_custom.actual_rate(rate) / DEFAULT_BAUD,
        pre, GLITCH)
    fsk_ok, fsk_msg = verify_fsk_regen(regen_out, pre, payload, rate, det_bps)
    print(f"  fsk  : regen .c8 demod -> {'PASS' if fsk_ok else 'FAIL'}: {fsk_msg}")

    # UART-decode probe3 (GDO2_CLEAN) from the generated 4-channel .sr.
    import zipfile
    zf = zipfile.ZipFile(base + ".sr")
    data = np.frombuffer(zf.read("logic-1-1"), dtype=np.uint8)
    d3 = (data >> 2) & 1  # probe3 = GDO2_CLEAN (bit 2)
    exp = expected_packet(pre, payload)
    m = int(round(det_spb))
    d3 = _uart_decode(np.asarray(d3, np.uint8), rate, m)
    ok = recv_ok and (d3 == exp) and fsk_ok
    with open(os.path.join(folder, "result.txt"), "w") as f:
        f.write(f"label={label} pre={pre} payload={payload.hex()}\n")
        f.write(f"frames={n_frames} probe3={d3.hex(' ')}\n")
        f.write(f"fsk={fsk_msg}\n")
        f.write(f"expected={exp.hex(' ')}\n")
        f.write(f"RESULT={'PASS' if ok else 'FAIL'}\n")
    print(f"  probe3={d3.hex(' ')[:48]} "
          f"fsk={'PASS' if fsk_ok else 'FAIL'} "
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
    ch0n = np.asarray(ch0, dtype=np.uint8)
    ch1n = np.asarray(ch1, dtype=np.uint8)
    det_spb, det_bps, windows = rfuzz_tools.detect_datarate_iter(
        ch0n, ch1n, r_actual, r_actual / DEFAULT_BAUD, pre, GLITCH)
    bits = rfuzz_tools.clean_bits(ch1, det_spb, GLITCH)
    packets = rfuzz_tools.find_packets(bits, det_spb, pre, payload)
    starts = rfuzz_tools.packet_starts(ch0n, det_spb, packets, pre)
    packets, gstarts = rfuzz_tools.refine_packets(ch1n, det_spb, packets, starts, pre,
                                                  payload)
    decoded = packets[0]["payload"] if packets else None
    bit_errors = sum(p["bit_errors"] for p in packets)

    ch1c = rfuzz_tools.clean_gdo2(packets, gstarts, det_spb, pre, len(ch1),
                                  framed=True, corrected=True)
    fsk = np.zeros(len(ch1), dtype=np.float32)
    if_dev, dev_hz = capture_custom.synth_defaults(r_actual, rfuzz_tools.DEFAULT_DEV)
    for p, start in zip(packets, gstarts):
        pbits = np.concatenate([
            rfuzz_tools.preamble_bits(pre), rfuzz_tools.SYNC_BITS,
            rfuzz_tools.bits_msb(p["payload"])])
        fsk += capture_custom.mod_synth_from_bits(
            pbits, 90, "2fsk", if_dev, dev_hz, r_actual, len(ch1), det_spb, start)
    base = os.path.join(folder, "cap")
    capture_custom.save_sr(ch0, ch1, ch1c, fsk,
                           int(round(r_actual)), base + ".sr")
    capture_custom.save_vcd(ch0, ch1, ch1c, fsk, r_actual, base + ".vcd")
    # regen FSK I/Q from the decoded packet
    regen_out = base + "_regen.c8"
    rfuzz_tools.cmd_regen(types.SimpleNamespace(
        raw=raw_path, rate=rate, spb=0.0, glitch=10, out=regen_out,
        amp=90, dev=50000.0, preamble=pre, payload_expect=payload.hex()))
    fsk_ok, fsk_msg = verify_fsk_regen(regen_out, pre, payload, rate,
                                       det_bps)
    print(f"  fsk  : regen .c8 demod -> {'PASS' if fsk_ok else 'FAIL'}: {fsk_msg}")

    sr_ok, sr_msg = verify_sr(base + ".sr", raw_path, r_actual, pre, payload)
    print(f"  sr   : .sr conformance -> {'PASS' if sr_ok else 'FAIL'}: {sr_msg}")

    m = int(round(det_spb))
    exp = expected_packet(pre, payload)
    d3 = _uart_decode(np.asarray(ch1c, np.uint8), rate, m)
    ok = (packets and decoded == payload and not bit_errors and d3 == exp
          and fsk_ok and sr_ok)
    with open(os.path.join(folder, "result.txt"), "w") as f:
        f.write(f"label={label} pre={pre} payload={payload.hex()}\n")
        f.write(f"decoded={decoded.hex() if decoded else '(none)'}\n")
        f.write(f"bit_errors={bit_errors} probe3={d3.hex(' ')}\n")
        f.write(f"sr={sr_msg}\n")
        f.write(f"expected={exp.hex(' ')}\nRESULT={'PASS' if ok else 'FAIL'}\n")
    status = "PASS" if ok else "FAIL"
    print(f"[{status}] {label:14s} pre={pre} pay={payload.hex()} "
          f"decoded={'Y' if decoded == payload else 'N'} "
          f"probe3={len(d3)}B")
    if not ok:
        print(f"   expected={exp.hex(' ')[:60]}")
        print(f"   probe3 ={d3.hex(' ')[:60]}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--rate", type=int, default=DEFAULT_RATE)
    ap.add_argument("--samples", type=int, default=DEFAULT_SAMPLES)
    ap.add_argument("--tx-freq", type=int, default=rfuzz_testcase.DEFAULT_TX_FREQ)
    ap.add_argument("--tx-srate", type=int, default=rfuzz_testcase.DEFAULT_TX_SRATE)
    ap.add_argument("--tx-x", type=int, default=rfuzz_testcase.DEFAULT_TX_X)
    ap.add_argument("--tx-a", type=int, default=rfuzz_testcase.DEFAULT_TX_A)
    ap.add_argument("--fsk-if", type=float, default=0.0)
    ap.add_argument("--fsk-dev", type=float, default=rfuzz_tools.DEFAULT_DEV)
    ap.add_argument("--mod", default="2fsk", choices=capture_custom.MODS,
                    help="basic modulation rendered on channel 3 "
                         "(default 2fsk; choices: " + ", ".join(capture_custom.MODS) + ")")
    ap.add_argument("--gap-ms", type=float, default=0.0,
                    help="gap ms override applied to all cases")
    ap.add_argument("--out-dir", default="signals/testhw")
    ap.add_argument("--offline", action="store_true",
                    help="synthetic GDO0/GDO2 (no HackRF/COM7 needed); "
                         "defaults --out-dir to signals/offline so hardware "
                         "captures under signals/testhw are not overwritten")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--only", default=None,
                    help="comma-separated labels to run")
    args = ap.parse_args()
    if args.offline and args.out_dir == "signals/testhw":
        args.out_dir = "signals/offline"

    if args.list:
        for c in CASES:
            print(f"  {c[0]:14s} pre={c[1]} pay={c[2].hex():10s} "
                  f"{c[3]:<5} {c[4]}")
        return 0

    npass = 0
    run = [c for c in CASES
           if not args.only or c[0] in args.only.split(",")]
    for c in run:
        label, pre, payload, gap_ms, desc = c
        if args.offline:
            kw = OFFLINE_KW.get(label, dict(jitter=0.0, glitch_samples=0, seed=0, settle=3))
            ok = run_case_offline(label, pre, payload, gap_ms, args.out_dir, **kw)
        else:
            try:
                ok = run_case_hardware(label, pre, payload, gap_ms, args.out_dir,
                                       args.host, args.port, args.rate, args.samples,
                                       args.tx_freq, args.tx_srate, args.tx_x,
                                       args.tx_a, args.mod, args.fsk_if,
                                       args.fsk_dev)
            except SystemExit as e:
                # rfuzz_testcase hard-fails a whole run on a bad capture; treat
                # that single case as a failure and keep going (short preambles
                # are inherently flaky single-shots - see gen_2fsk docstring).
                print(f"  [case {label} aborted capture: {e}]")
                ok = False
            except Exception as e:
                print(f"  [case {label} error: {e!r}]")
                ok = False
        npass += 1 if ok else 0
    n = len(run)
    if args.offline:
        for kind, label in (("noise", "noise_only"),
                            ("dc_high", "dc_offset"),
                            ("idle", "idle_only")):
            npass += 1 if run_negative_offline(label, args.out_dir, kind) else 0
            n += 1
    print(f"\n{npass}/{n} passed ({'offline' if args.offline else 'hardware'})")
    return 0 if npass == n else 1


if __name__ == "__main__":
    sys.exit(main())
