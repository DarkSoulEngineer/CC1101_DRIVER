#!/usr/bin/env python3
"""Protocol sniffer for the CC1101 GDO0/GDO2 capture path (Bit-Pirate style).

Analyzes a raw .raw or .sr capture - or grabs a fresh one from the CC1101
firmware on COM7 - and auto-detects baud / encoding (NRZ vs Manchester) and
extracts frames (alternating preamble + sync + payload) without assuming the
fixed 2FSK/2400/0xDEAF framing.

Commands:
    analyze  FILE [--rate R] [--baud B] [--sync HEX] [--max-bytes N]
             Auto-detect a saved capture (.raw or .sr).  --baud is a hint that
             disambiguates NRZ@B from Manchester@B/2.
    receive  --port COM7 [--rate R] [--samples N] [--baud B] [--sync HEX]
             [--out BASE] [--no-strict]
             Grab a firmware capture (0x01), save .raw/.sr/.vcd and analyze.
    stream   --port COM7 [--rate R] [--seconds S] [--baud B] [--sync HEX]
             [--out BASE]
             Continuous stream (0x03) for S seconds, save + analyze.
    selftest
             Offline regression tests on synthetic NRZ/Manchester/noise
             streams (no hardware); exits 0 on pass, 1 on failure.

Examples:
    python sniff.py analyze cap.raw --rate 250000 --baud 2400
    python sniff.py analyze cap.sr
    python sniff.py receive --port COM7 --baud 2400 --out signals/sniff/x
    python sniff.py stream --port COM7 --seconds 5 --baud 2400

The --sync word list is comma-separated hex, e.g. 'deadbeef,01020304'.
"""
import argparse
import os
import sys
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import capture_custom  # noqa: E402
import rfuzz_tools  # noqa: E402

DEFAULT_PORT = "COM7"
DEFAULT_BAUD_SER = 115200
DEFAULT_RATE = 250000
DEFAULT_SAMPLES = 262144


# ======================================================================
# Sniffer analysis library (merged from sniff_decoder.py)
#
# Protocol-agnostic RF sniffing analyzer for CC1101 GDO0/GDO2 captures.
#
# The CC1101 demodulator output (GDO2 = async serial data, GDO0 = sync strobe)
# is just a logic bitstream at the capture sample rate.  This module turns a
# raw capture (or a .sr) into an *auto-detected* protocol summary without
# assuming the fixed 2FSK/2400/0xAA/0xDEAF framing that rfuzz_tools uses:
#
#     * baud detection      - the fundamental edge interval (mode of the
#                             edge-to-edge histogram) gives the cell period;
#     * encoding detection  - NRZ vs Manchester, using a user baud hint when
#                             given (a pure bitstream cannot separate
#                             NRZ@B from Manchester@B/2 - an inherent
#                             ambiguity, resolved by the transition density);
#     * framing             - preamble (alternating bits) + sync-word scan,
#                             variable-length payload read after the sync.
#
# NRZ (the RFuzz RX path) is decoded at the cell rate and frames are located
# with find_frames().  Manchester streams are grouped into data bits before
# frame search.  This mirrors the Bit Pirate `receive` / `sniff` workflow: get
# the baud and encoding first, then decode frames.
# ======================================================================

DEFAULT_BAUD = 2400.0
DEFAULT_SYNC = bytes([0xDE, 0xAF])
MIN_PREAMBLE_BITS = 8


def edge_intervals(bits):
    """Sample distances between consecutive level transitions (edge-to-edge)."""
    bits = np.asarray(bits, dtype=np.int8)
    if bits.size < 2:
        return np.array([], dtype=np.int64)
    idx = np.flatnonzero(np.diff(bits) != 0) + 1
    return np.diff(idx)


def _mode_interval(intervals):
    """Mode of an integer interval array via a histogram (robust to noise)."""
    if intervals.size == 0:
        return None
    lo = int(intervals.min())
    hi = int(intervals.max())
    if hi <= lo:
        return float(lo)
    w = max(1, int(round((hi - lo) / 40.0)))
    bins = np.arange(lo, hi + w, w, dtype=np.int64)
    counts, _ = np.histogram(intervals, bins=bins)
    b0 = int(bins[int(np.argmax(counts))])
    sel = intervals[(intervals >= b0) & (intervals < b0 + w)]
    return float(np.mean(sel)) if sel.size else float(b0)


def fundamental_cell(ch, rate):
    """Return (cell_samples, cell_hz, transitions_per_cell) for a logic channel.

    The fundamental cell is the shortest recurring pulse period - one bit for
    NRZ, one half-bit for Manchester.  `transitions_per_cell` (edge count
    scaled by the cell) is ~1 for a clean NRZ preamble and ~2 for Manchester,
    which is used to classify the encoding when no baud hint is available.
    """
    ch = np.asarray(ch, dtype=np.uint8)
    ints = edge_intervals(ch)
    if ints.size == 0:
        return None, None, 0.0
    mode = _mode_interval(ints)
    if mode is None:
        return None, None, 0.0
    # Iterate: drop glitch-short and idle-long intervals and re-measure.
    for _ in range(3):
        sel = ints[(ints >= 0.5 * mode) & (ints <= 8.0 * mode)]
        if sel.size == 0:
            break
        new = _mode_interval(sel)
        if abs(new - mode) / mode < 0.05:
            mode = new
            break
        mode = new
    cell_hz = rate / mode
    density = ints.size * mode / max(len(ch), 1)
    return mode, cell_hz, density


def classify_encoding(cell, cell_hz, density, baud_hint=None):
    """Classify NRZ vs Manchester.  A baud hint resolves the ambiguity that is
    inherent to a raw bitstream (NRZ@B is indistinguishable from Manchester@B/2
    by edges alone); without it, use the transition density per cell."""
    if cell is None:
        return "unknown", {}
    if baud_hint:
        ratio = cell_hz / float(baud_hint)
        if 0.9 <= ratio <= 1.1:
            return "nrz", {"ratio": round(ratio, 3)}
        if 1.8 <= ratio <= 2.2:
            return "manchester", {"ratio": round(ratio, 3)}
        return "unknown", {"ratio": round(ratio, 3)}
    if density > 1.25:
        return "manchester", {"density": round(density, 2),
                              "note": "confirm with a baud hint (NRZ@cell/2 "
                                      "has the same edges)"}
    return "nrz", {"density": round(density, 2)}


def sample_bits(ch, spb, glitch=None):
    """Quantize a logic channel to bits at `spb` samples/bit (glitch filter +
    run-length decode, reusing rfuzz_tools).

    `glitch` defaults to a fraction of the bit cell (spb/10, min 2 samples) so
    the debounce scales with the baud rate instead of destroying short cells.

    Returns (bits, starts): `starts` holds the raw-sample position of each
    decoded bit so frame offsets map back to the capture timeline even when
    long idle runs collapse to a single bit in the re-encoded stream."""
    if glitch is None:
        glitch = max(2, int(round(spb / 10.0)))
    g = rfuzz_tools.remove_glitches(np.asarray(ch, dtype=np.uint8), glitch)
    return rfuzz_tools.runs_to_bits_pos(rfuzz_tools.rl_encode(g), spb)


def manchester_to_bits(cells, starts=None):
    """Group half-bit cells into data bits.  CC1101 manchester: data 0 -> '01',
    data 1 -> '10'.  Invalid (equal) pairs are dropped.  When `starts` is given
    returns (bits, starts) with the sample position of each data bit."""
    n = (len(cells) // 2) * 2
    out = []
    outs = []
    for k in range(0, n, 2):
        a, b = cells[k], cells[k + 1]
        if a == b:
            continue
        out.append(0 if (a == 0 and b == 1) else 1)
        if starts is not None:
            outs.append(starts[k])
    bits = np.asarray(out, dtype=np.uint8)
    if starts is None:
        return bits
    return bits, np.asarray(outs, dtype=np.int64)


def parse_sync(sync_hex):
    """--sync 'deadbeef,0102' -> [b'\\xde\\xad\\xbe\\xef', b'\\x01\\x02']."""
    if not sync_hex:
        return [DEFAULT_SYNC]
    out = []
    for tok in sync_hex.replace(" ", "").split(","):
        tok = tok.strip()
        if not tok:
            continue
        try:
            out.append(bytes.fromhex(tok))
        except ValueError:
            sys.exit(f"invalid --sync hex {tok!r}")
    return out or [DEFAULT_SYNC]


def bytes_from_bits(bits):
    """MSB-first bits -> bytes (drops a trailing partial byte)."""
    return bytes(int("".join(str(b) for b in bits[j:j + 8]), 2)
                 for j in range(0, len(bits) - 7, 8))


def find_frames(bits, spb, sync_words, max_payload_bytes=64,
                min_preamble_bits=MIN_PREAMBLE_BITS, preamble_alt_frac=0.5,
                starts=None, trim_trailing_idle=True):
    """Scan a bit stream for sync words preceded by an alternating preamble;
    read up to max_payload_bytes after each sync.  Returns frame dicts with
    bit/sample offsets (at spb from `starts` when given, otherwise p*spb) and
    payload bytes."""
    n = len(bits)
    frames = []
    i = 0
    while i < n - 16:
        matched = False
        for sw in sync_words:
            sl = len(sw) * 8
            if i + sl > n:
                continue
            sw_bits = np.array([(b >> k) & 1 for b in sw for k in range(7, -1, -1)],
                               dtype=np.uint8)
            if not (bits[i:i + sl] == sw_bits).all():
                continue
            pre = bits[max(0, i - 2 * min_preamble_bits):i]
            alt = None
            if pre.size >= min_preamble_bits:
                alt = float(np.mean(pre[1:] != pre[:-1]))
            if alt is None or alt < preamble_alt_frac:
                continue
            p = i
            while p > 0 and bits[p] != bits[p - 1]:
                p -= 1
            end = min(n, i + sl + max_payload_bytes * 8)
            payload = rfuzz_tools.bytes_from_bits(bits[i + sl:end])
            # Trim trailing idle bytes (the demod tail after the last real bit
            # reads as all-0x00 or all-0xFF at the line's idle level) whenever
            # the capture's final run is a whole idle byte or longer.  A
            # payload that legitimately ends in that byte is indistinguishable
            # at the bit level - the raw bits are always preserved in the
            # .raw/.sr/.vcd.  `trim_trailing_idle=False` keeps such bytes.
            if trim_trailing_idle:
                diffs = np.flatnonzero(np.diff(bits) != 0)
                trailing = int(diffs[-1]) + 1 if diffs.size else 0
                if len(bits) - trailing >= 8:
                    idle_byte = 0xFF if bits[-1] else 0x00
                    while payload.endswith(bytes([idle_byte])):
                        payload = payload[:-1]
            sample_off = (int(starts[p]) if starts is not None
                          else int(round(p * spb)))
            frames.append({
                "bit_off": int(i),
                "preamble_start_bit": int(p),
                "sample_off": sample_off,
                "sync_sample_off": (int(starts[i]) if starts is not None
                                    else int(round(i * spb))),
                "preamble_alt": alt,
                "sync": sw.hex(),
                "payload": payload,
            })
            i = end
            matched = True
            break
        if not matched:
            i += 1
    return frames


def analyze_logic(ch2, ch0=None, rate=DEFAULT_RATE, baud_hint=None,
                  sync_hex=None, max_payload_bytes=64,
                  trim_trailing_idle=True):
    """Auto-detect baud/encoding and extract frames from the GDO2 channel.

    The fundamental edge interval gives the cell period; NRZ and Manchester
    are the two common interpretations.  Because NRZ@B has the same edges as
    Manchester@B/2 with an alternating preamble, the analyzer *decodes both*
    and prefers the interpretation that yields frames at the sync word (the
    user's --baud hint narrows the candidates when given).  Returns a dict
    report (rate, cell, cell_hz, density, encoding, spb, baud, frames)."""
    ch2 = np.asarray(ch2, dtype=np.uint8)
    cell, cell_hz, density = fundamental_cell(ch2, rate)
    report = {
        "rate": rate,
        "cell_samples": cell,
        "cell_hz": cell_hz,
        "transitions_per_cell": density,
        "baud_hint": baud_hint,
        "frames": [],
    }
    if cell is None:
        report["encoding"] = "unknown"
        report["baud"] = None
        report["spb"] = None
        report["note"] = "no level transitions (constant channel / no carrier)"
        return report

    sync_words = parse_sync(sync_hex)
    if baud_hint:
        if baud_hint <= 0 or baud_hint > rate / 2.0:
            sys.exit(f"error: --baud {baud_hint} out of range for rate {rate}")
        candidates = [
            {"encoding": "nrz", "spb": rate / baud_hint, "baud": baud_hint},
            {"encoding": "manchester", "spb": rate / (2.0 * baud_hint),
             "baud": baud_hint},
        ]
    else:
        candidates = [{"encoding": "nrz", "spb": cell, "baud": cell_hz}]
        if cell > 3.0:
            candidates.append({"encoding": "manchester", "spb": cell / 2.0,
                               "baud": cell_hz / 2.0})

    best = None
    for cand in candidates:
        spb = cand["spb"]
        if spb < 2.0:
            continue
        bits, starts = sample_bits(ch2, spb)
        if cand["encoding"] == "manchester":
            bits, starts = manchester_to_bits(bits, starts)
            frame_spb = 2.0 * spb
        else:
            frame_spb = spb
        frames = find_frames(bits, frame_spb, sync_words, max_payload_bytes,
                             starts=starts,
                             trim_trailing_idle=trim_trailing_idle)
        cand.update(bits=bits, frame_spb=frame_spb, frames=frames)
        if best is None or len(frames) > len(best["frames"]):
            best = cand
    if best is None:
        best = candidates[0]

    info = {}
    if baud_hint:
        info["ratio"] = round(cell_hz / baud_hint, 3)
    else:
        info["density"] = round(density, 2)
    report["encoding"] = best["encoding"]
    report["encoding_info"] = info
    report["baud"] = best["baud"]
    report["spb"] = best["frame_spb"]
    report["frames"] = best["frames"]
    if not report["frames"]:
        report["note"] = ("no frames found; cell-rate %.0f Hz - if this is "
                          "Manchester, try --baud %.0f"
                          % (cell_hz, cell_hz / 2.0))
    return report


def load_capture(path, rate=DEFAULT_RATE, probe="gdo2"):
    """Load a .raw (bit0=GDO0, bit1=GDO2) or .sr (logic-1-1) into
    (ch0, ch1, actual_rate).  `rate` is only used for .raw (the requested
    rate; the firmware's integer alarm is corrected via actual_rate)."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".raw":
        import capture_custom
        data = np.fromfile(path, dtype=np.uint8)
        if data.size == 0:
            sys.exit(f"error: empty capture {path}")
        ch0 = data & 1
        ch1 = (data >> 1) & 1
        r = capture_custom.actual_rate(int(rate))
        return ch0, ch1, r
    if ext == ".sr":
        with zipfile.ZipFile(path) as zf:
            meta = zf.read("metadata").decode()
            r = None
            for line in meta.splitlines():
                if line.startswith("samplerate = "):
                    r = float(line.split("=", 1)[1].strip())
            if r is None:
                sys.exit(f"error: no samplerate in {path}")
            data = np.frombuffer(zf.read("logic-1-1"), dtype=np.uint8)
        ch0 = data & 1
        ch1 = (data >> 1) & 1
        return ch0, ch1, r
    sys.exit(f"error: unsupported capture type {path!r} (use .raw or .sr)")


def format_report(rep, probe_name="GDO2"):
    lines = []
    if rep["cell_samples"] is None:
        lines.append(f"  {probe_name}: no level transitions (constant channel)")
        return lines
    lines.append(f"  {probe_name}: cell={rep['cell_samples']:.2f} samples, "
                 f"cell-rate={rep['cell_hz']:.0f} Hz, "
                 f"transitions/cell={rep['transitions_per_cell']:.2f}")
    info = rep.get("encoding_info") or {}
    src = (f" (cell-rate/baud-hint ratio {info['ratio']})"
           if "ratio" in info else
           f" (density {info['density']})")
    lines.append(f"  encoding: {rep['encoding']}{src}"
                 + (f" -> baud {rep['baud']:.0f}" if rep["baud"] else ""))
    return lines


# ======================================================================
# CLI commands (analyze / receive / stream)
# ======================================================================

def save_logic(base, ch0, ch1, rate):
    """Write a 2-probe sigrok .sr and a VCD (GDO0, GDO2), no synthesized
    analog channel (the sniffer keeps the captured channels honest)."""
    parent = os.path.dirname(os.path.abspath(base))
    os.makedirs(parent, exist_ok=True)
    packed = bytearray(len(ch0))
    for i in range(len(ch0)):
        packed[i] = (int(ch0[i]) & 1) | ((int(ch1[i]) & 1) << 1)
    meta = ('[global]\nsigrok version = 2\n\n[device 1]\n'
            'capturefile = logic-1\nunitsize = 1\ntotal probes = 2\n'
            'samplerate = %d\ntotal analog = 0\n'
            'probe1 = GDO0\nprobe2 = GDO2\n' % int(round(rate)))
    sr = base + ".sr"
    import zipfile
    with zipfile.ZipFile(sr, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("version", "2")
        zf.writestr("metadata", meta)
        zf.writestr("logic-1-1", bytes(packed))
    tps = 1000000000.0 / rate
    vcd = base + ".vcd"
    with open(vcd, "w", newline="\n") as f:
        f.write("$timescale 1ns $end\n")
        f.write("$scope module CC1101 $end\n")
        f.write("$var wire 1 ! GDO0 $end\n")
        f.write("$var wire 1 @ GDO2 $end\n")
        f.write("$upscope $end\n$enddefinitions $end\n")
        f.write("$dumpvars\nx!\nx@\n$end\n")
        p0 = p1 = None
        for i in range(len(ch0)):
            if ch0[i] != p0 or ch1[i] != p1:
                f.write("#%d\n" % int(i * tps))
                if ch0[i] != p0:
                    f.write("%d!\n" % ch0[i])
                if ch1[i] != p1:
                    f.write("%d@\n" % ch1[i])
                p0, p1 = ch0[i], ch1[i]
    with open(base + ".raw", "wb") as f:
        f.write(bytes(packed))
    return sr, vcd


def _print_report(rep, base=None):
    for line in format_report(rep, "GDO2"):
        print(line)
    n = len(rep["frames"])
    print(f"  frames: {n}")
    for fr in rep["frames"]:
        print(f"    sync={fr['sync']} bit@{fr['bit_off']} "
              f"sample@{fr['sample_off']} "
              f"preamble_alt={fr['preamble_alt']:.2f} "
              f"payload={fr['payload'].hex(' ')}")
    if base:
        print(f"  saved: {base}.raw / {base}.sr / {base}.vcd")


def cmd_analyze(args):
    ch0, ch1, rate = load_capture(args.file, args.rate)
    print(f"capture: {args.file} rate={rate:.0f} Hz "
          f"samples={len(ch1)} ({len(ch1) / rate:.2f} s)")
    probe = ch1 if args.probe == "gdo2" else ch0
    rep = analyze_logic(
        probe, ch0, rate=rate, baud_hint=args.baud, sync_hex=args.sync,
        max_payload_bytes=args.max_bytes,
        trim_trailing_idle=not args.no_trim_trailing_idle)
    _print_report(rep)
    return 0


def cmd_receive(args):
    import serial
    ser = capture_custom.open_port(args.port, DEFAULT_BAUD_SER)
    capture_custom.drain(ser)
    try:
        raw = capture_custom.capture(ser, args.rate, args.samples,
                                     strict=not args.no_strict)
    finally:
        ser.close()
    ch0, ch1 = capture_custom.decode(bytes(raw))
    r_actual = capture_custom.actual_rate(args.rate)
    print(f"captured {len(raw)} samples at actual {r_actual:.0f} Hz")
    rep = analyze_logic(
        ch1, ch0, rate=r_actual, baud_hint=args.baud, sync_hex=args.sync,
        max_payload_bytes=args.max_bytes,
        trim_trailing_idle=not args.no_trim_trailing_idle)
    base = args.out or (args.name or "sniff_capture")
    save_logic(base, ch0, ch1, r_actual)
    _print_report(rep, base)
    return 0


def cmd_stream(args):
    import serial
    ser = capture_custom.open_port(args.port, DEFAULT_BAUD_SER)
    capture_custom.drain(ser)
    ser.reset_input_buffer()
    ser.write(bytes([capture_custom.CMD_STREAM]))
    ser.write(np.array([args.rate], dtype="<u4").tobytes())
    print(f"[*] streaming at {args.rate} Hz for {args.seconds}s "
          f"(~{args.rate * args.seconds} bytes)...")
    want = int(args.rate * args.seconds)
    buf = bytearray()
    t0 = time.time()
    ser.timeout = 0.2
    while time.time() - t0 < args.seconds + 2.0 and len(buf) < want:
        b = ser.read(65536)
        if b:
            buf.extend(b)
    try:
        ser.write(bytes([capture_custom.CMD_STOP]))
    finally:
        ser.close()
    if len(buf) == 0:
        sys.exit("error: stream returned no bytes (is the firmware flashed "
                 "and the port right?)")
    ch0, ch1 = capture_custom.decode(bytes(buf))
    r_actual = capture_custom.actual_rate(args.rate)
    print(f"[+] streamed {len(buf)} bytes at actual {r_actual:.0f} Hz "
          f"({len(buf) / r_actual:.2f} s)")
    rep = analyze_logic(
        ch1, ch0, rate=r_actual, baud_hint=args.baud, sync_hex=args.sync,
        max_payload_bytes=args.max_bytes,
        trim_trailing_idle=not args.no_trim_trailing_idle)
    base = args.out or (args.name or "sniff_stream")
    save_logic(base, ch0, ch1, r_actual)
    _print_report(rep, base)
    return 0


# ======================================================================
# Offline regression self-test (merged from sniff_test.py)
#
# Builds synthetic GDO2 logic streams at various bauds and encodings and
# asserts the auto-detector recovers the baud, classifies the encoding, and
# extracts the exact frames - without any HackRF / COM7 / RF hardware:
#
#   NRZ   at 1200 / 2400 / 9600 bps with a 0xAA preamble + 0xDEAF sync
#   Manchester at 2400 bps (each data bit -> two half-bit cells, CC1101 mapping)
#   robustness: pure noise -> 0 frames, constant channel -> graceful report
# ======================================================================

RATE = 250000.0
SYNC = bytes([0xDE, 0xAF])
IDLE = 512


def _render(bits, spb, jitter=0.0, seed=0):
    rng = np.random.default_rng(seed)
    starts = [IDLE]
    for _ in bits:
        starts.append(starts[-1] + max(1, int(round(spb + rng.uniform(-jitter, jitter)))))
    total = starts[-1] + int(round(spb)) + IDLE
    out = np.full(total, 1, dtype=np.uint8)
    for k, b in enumerate(bits):
        out[starts[k]:starts[k + 1]] = int(b)
    return out


def nrz_stream(pre, payload, baud, jitter=0.0, seed=0):
    bits = np.concatenate([
        rfuzz_tools.preamble_bits(pre), rfuzz_tools.SYNC_BITS,
        rfuzz_tools.bits_msb(payload)]).astype(np.uint8)
    return _render(bits, RATE / baud, jitter=jitter, seed=seed)


def manchester_bits(bits):
    out = []
    for b in bits:
        out += [int(b), 1 - int(b)]  # data 1 -> '10', data 0 -> '01'
    return np.asarray(out, dtype=np.uint8)


def manchester_stream(pre, payload, baud, jitter=0.0, seed=0):
    bits = np.concatenate([
        rfuzz_tools.preamble_bits(pre), rfuzz_tools.SYNC_BITS,
        rfuzz_tools.bits_msb(payload)]).astype(np.uint8)
    cells = manchester_bits(bits)
    return _render(cells, RATE / (2.0 * baud), jitter=jitter, seed=seed)


def check(label, ok, detail=""):
    print(f"[{'PASS' if ok else 'FAIL'}] {label:34s} {detail}")
    return ok


def case_nrz(label, baud, pre, payload, jitter=0.0):
    ch = nrz_stream(pre, payload, baud, jitter=jitter)
    rep = analyze_logic(ch, None, rate=RATE,
                        baud_hint=baud,
                        sync_hex="deaf")
    ok_cell = rep["cell_hz"] is not None and abs(rep["cell_hz"] - baud) / baud < 0.05
    ok_enc = rep["encoding"] == "nrz"
    ok_frame = (len(rep["frames"]) == 1 and rep["frames"][0]["payload"] == payload)
    off = rep["frames"][0]["sample_off"] if rep["frames"] else None
    ok_off = off is not None and abs(off - IDLE) <= RATE / baud * 1.5
    ok = ok_cell and ok_enc and ok_frame and ok_off
    detail = (f"cell={rep['cell_hz']:.0f} enc={rep['encoding']} "
              f"off={off} payload={rep['frames'][0]['payload'].hex(' ') if rep['frames'] else '-'}")
    return check(f"NRZ {baud:.0f} {label}", ok, detail)


def case_manchester(baud, pre, payload):
    ch = manchester_stream(pre, payload, baud)
    rep = analyze_logic(ch, None, rate=RATE,
                        baud_hint=baud,
                        sync_hex="deaf")
    ok_enc = rep["encoding"] == "manchester"
    ok_baud = rep["baud"] is not None and abs(rep["baud"] - baud) / baud < 0.05
    ok_frame = (len(rep["frames"]) == 1 and rep["frames"][0]["payload"] == payload)
    off = rep["frames"][0]["sample_off"] if rep["frames"] else None
    ok_off = off is not None and abs(off - IDLE) <= RATE / baud * 2.0
    ok = ok_enc and ok_baud and ok_frame and ok_off
    detail = (f"enc={rep['encoding']} baud={rep['baud']:.0f} off={off} "
              f"payload={rep['frames'][0]['payload'].hex(' ') if rep['frames'] else '-'}")
    return check(f"Manchester {baud:.0f}", ok, detail)


def case_nrz_nohint(baud, pre, payload):
    ch = nrz_stream(pre, payload, baud)
    rep = analyze_logic(ch, None, rate=RATE, baud_hint=None)
    ok_cell = rep["cell_hz"] is not None and abs(rep["cell_hz"] - baud) / baud < 0.05
    ok_density = rep["transitions_per_cell"] is not None and \
        rep["transitions_per_cell"] < 1.25
    ok_frame = (len(rep["frames"]) == 1 and rep["frames"][0]["payload"] == payload)
    detail = (f"cell={rep['cell_hz']:.0f} den={rep['transitions_per_cell']:.2f} "
              f"enc={rep['encoding']} frames={len(rep['frames'])}")
    return check(f"NRZ {baud:.0f} no-hint", ok_cell and ok_density and ok_frame, detail)


def case_noise():
    rng = np.random.default_rng(0)
    ch = rng.integers(0, 2, size=20000).astype(np.uint8)
    rep = analyze_logic(ch, None, rate=RATE, baud_hint=2400)
    ok = len(rep["frames"]) == 0
    return check("pure noise -> 0 frames", ok, f"frames={len(rep['frames'])}")


def case_constant():
    ch = np.zeros(2000, dtype=np.uint8)
    rep = analyze_logic(ch, None, rate=RATE, baud_hint=2400)
    ok = rep["cell_samples"] is None and "note" in rep
    return check("constant channel -> graceful", ok, str(rep.get("note")))


def case_idle_with_noise_tail():
    # A real capture has demod noise between bursts; the preamble window must
    # still be found.  GDO2 free-runs on noise, then a clean packet arrives.
    ch = nrz_stream(8, bytes([1, 2, 3, 4]), 2400, jitter=0.1)
    rng = np.random.default_rng(1)
    noise = rng.integers(0, 2, size=4000).astype(np.uint8)
    ch = np.concatenate([noise, ch])
    rep = analyze_logic(ch, None, rate=RATE, baud_hint=2400)
    ok = len(rep["frames"]) == 1 and rep["frames"][0]["payload"] == bytes([1, 2, 3, 4])
    return check("noise tail + packet", ok,
                 f"frames={len(rep['frames'])} "
                 f"payload={rep['frames'][0]['payload'].hex(' ') if rep['frames'] else '-'}")


def cmd_selftest(args):
    """Offline regression tests; exit 0 on all-pass, 1 on any failure."""
    results = [
        case_nrz("std", 2400, 8, bytes([1, 2, 3, 4])),
        case_nrz("jittery", 2400, 8, bytes([0xDE, 0xAD, 0xBE, 0xF0]), jitter=0.2),
        case_nrz("slow", 1200, 8, bytes([1, 2, 3, 4])),
        case_nrz("fast", 9600, 8, bytes([1, 2, 3, 4])),
        case_nrz("p16", 2400, 16, bytes([0x12, 0x34, 0x56, 0x78])),
        case_manchester(2400, 8, bytes([1, 2, 3, 4])),
        case_nrz_nohint(2400, 8, bytes([1, 2, 3, 4])),
        case_noise(),
        case_constant(),
        case_idle_with_noise_tail(),
    ]
    n = len(results)
    passed = sum(results)
    print(f"\n{passed}/{n} sniff self-tests passed")
    return 0 if passed == n else 1


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("analyze", help="auto-detect a saved .raw/.sr capture")
    a.add_argument("file")
    a.add_argument("--rate", type=int, default=DEFAULT_RATE,
                   help="requested capture rate for .raw (default 250000)")
    a.add_argument("--probe", default="gdo2", choices=("gdo0", "gdo2"),
                   help="channel to analyze (default gdo2)")
    _common(a)
    a.set_defaults(func=cmd_analyze)

    r = sub.add_parser("receive", help="firmware capture + analyze")
    _port(r)
    r.add_argument("--rate", type=int, default=DEFAULT_RATE)
    r.add_argument("--samples", type=int, default=DEFAULT_SAMPLES)
    r.add_argument("--no-strict", action="store_true",
                   help="accept a truncated capture")
    _common(r)
    _out(r, "sniff_capture")
    r.set_defaults(func=cmd_receive)

    s = sub.add_parser("stream", help="live stream + analyze")
    _port(s)
    s.add_argument("--rate", type=int, default=DEFAULT_RATE)
    s.add_argument("--seconds", type=float, default=2.0)
    _common(s)
    _out(s, "sniff_stream")
    s.set_defaults(func=cmd_stream)

    t = sub.add_parser("selftest", help="offline regression self-test (no hardware)")
    t.set_defaults(func=cmd_selftest)

    args = ap.parse_args()
    return args.func(args)


def _common(ap):
    ap.add_argument("--baud", type=float, default=None,
                    help="baud hint to disambiguate NRZ@B vs Manchester@B/2")
    ap.add_argument("--sync", type=str, default=None,
                    help="comma-separated sync words in hex (default deadbeef "
                         "style 0xDEAF -> 'deaf')")
    ap.add_argument("--max-bytes", type=int, default=64,
                    help="max payload bytes read after a sync (default 64)")
    ap.add_argument("--no-trim-trailing-idle", action="store_true",
                    help="keep trailing idle bytes (0x00/0xFF) in payloads")


def _port(ap):
    ap.add_argument("--port", "-p", default=DEFAULT_PORT)


def _out(ap, default):
    ap.add_argument("--out", "-o", default=None, help="output base name/path")
    ap.add_argument("--name", default=default, help=argparse.SUPPRESS)


if __name__ == "__main__":
    sys.exit(main())
