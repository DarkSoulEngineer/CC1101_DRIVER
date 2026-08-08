#!/usr/bin/env python3
"""Offline self-test for the protocol sniffer (sniff_decoder.py).

Builds synthetic GDO2 logic streams at various bauds and encodings and asserts
the auto-detector recovers the baud, classifies the encoding, and extracts the
exact frames - without any HackRF / COM7 / RF hardware:

  NRZ   at 1200 / 2400 / 9600 bps with a 0xAA preamble + 0xDEAF sync
  Manchester at 2400 bps (each data bit -> two half-bit cells, CC1101 mapping)
  robustness: pure noise -> 0 frames, constant channel -> graceful report

Usage:
    python sniff_test.py
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import rfuzz_tools  # noqa: E402
import sniff_decoder  # noqa: E402

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
    rep = sniff_decoder.analyze_logic(ch, None, rate=RATE,
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
    rep = sniff_decoder.analyze_logic(ch, None, rate=RATE,
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
    rep = sniff_decoder.analyze_logic(ch, None, rate=RATE, baud_hint=None)
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
    rep = sniff_decoder.analyze_logic(ch, None, rate=RATE, baud_hint=2400)
    ok = len(rep["frames"]) == 0
    return check("pure noise -> 0 frames", ok, f"frames={len(rep['frames'])}")


def case_constant():
    ch = np.zeros(2000, dtype=np.uint8)
    rep = sniff_decoder.analyze_logic(ch, None, rate=RATE, baud_hint=2400)
    ok = rep["cell_samples"] is None and "note" in rep
    return check("constant channel -> graceful", ok, str(rep.get("note")))


def case_idle_with_noise_tail():
    # A real capture has demod noise between bursts; the preamble window must
    # still be found.  GDO2 free-runs on noise, then a clean packet arrives.
    ch = nrz_stream(8, bytes([1, 2, 3, 4]), 2400, jitter=0.1)
    rng = np.random.default_rng(1)
    noise = rng.integers(0, 2, size=4000).astype(np.uint8)
    ch = np.concatenate([noise, ch])
    rep = sniff_decoder.analyze_logic(ch, None, rate=RATE, baud_hint=2400)
    ok = len(rep["frames"]) == 1 and rep["frames"][0]["payload"] == bytes([1, 2, 3, 4])
    return check("noise tail + packet", ok,
                 f"frames={len(rep['frames'])} "
                 f"payload={rep['frames'][0]['payload'].hex(' ') if rep['frames'] else '-'}")


def main():
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


if __name__ == "__main__":
    sys.exit(main())
