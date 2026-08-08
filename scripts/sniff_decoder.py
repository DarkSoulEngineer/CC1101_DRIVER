#!/usr/bin/env python3
"""Protocol-agnostic RF sniffing analyzer for CC1101 GDO0/GDO2 captures.

The CC1101 demodulator output (GDO2 = async serial data, GDO0 = sync strobe)
is just a logic bitstream at the capture sample rate.  This module turns a
raw capture (or a .sr) into an *auto-detected* protocol summary without
assuming the fixed 2FSK/2400/0xAA/0xDEAF framing that rfuzz_tools uses:

    * baud detection      - the fundamental edge interval (mode of the
                            edge-to-edge histogram) gives the cell period;
    * encoding detection  - NRZ vs Manchester, using a user baud hint when
                            given (a pure bitstream cannot separate
                            NRZ@B from Manchester@B/2 - an inherent
                            ambiguity, resolved by the transition density);
    * framing             - preamble (alternating bits) + sync-word scan,
                            variable-length payload read after the sync.

NRZ (the RFuzz RX path) is decoded at the cell rate and frames are located
with find_frames().  Manchester streams are grouped into data bits before
frame search.  This mirrors the Bit Pirate `receive` / `sniff` workflow: get
the baud and encoding first, then decode frames.

Usage (see sniff.py for the CLI):
    report = analyze_logic(ch2, ch0, rate, baud_hint=2400)
    frames = report["frames"]
"""
import os
import sys
import zipfile

import numpy as np

import rfuzz_tools

DEFAULT_BAUD = 2400.0
DEFAULT_RATE = 250000.0
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
