#!/usr/bin/env python3
"""Generate the 2FSK test signal used by the RFuzz end-to-end test case.

Packet = 0xAA x4 (preamble) + 0xDE 0xAF (sync) + 0x01 0x02 0x03 0x04 (payload),
bits MSB-first, 2FSK at +50 kHz / -50 kHz, phase-continuous, int8 I/Q interleaved.

Output matches what the CC1101 is configured to receive (433.92 MHz, 2400 bps,
sync 0xDEAF 16/16, fixed 4-byte payload, no CRC, no whitening).

Usage:
    python gen_2fsk.py out.c8 [repeat] [gap_ms]
    # repeat = number of packets (default 100)
    # gap_ms = idle silence between packets (default 0 = continuous)

Optional:
    python gen_2fsk.py out.c8 1 0        # single shot (short preamble)
    python gen_2fsk.py out.c8 1 0 64    # single shot with 64-byte preamble
    python gen_2fsk.py out.c8 1 50 16   # single shot, 16-byte preamble, 50 ms gap

The gap between packets is silent (carrier off, 0 amplitude).  Use a longer
preamble (>= 8 bytes) for single-shot bursts: the CC1101 bit-synchronizer
needs a few bit cells to lock, so the demod glitches the first ~8 bits of an
incoming burst.  A longer TX preamble absorbs that settling glitch and leaves
the pre-sync window clean.
"""
import sys

import numpy as np

FS = 2400000.0
BPS = 2400.0
DEV = 50000.0
AMP = 90
SPS = int(FS / BPS)
PACKET = bytes([170]) * 4 + bytes([222, 175]) + bytes([1, 2, 3, 4])


def bits_msb(byte):
    return [(byte >> i) & 1 for i in range(7, -1, -1)]


def packet_bits(preamble_bytes=4):
    bits = []
    pre = bytes([170]) * preamble_bytes
    for b in pre + bytes([222, 175]) + bytes([1, 2, 3, 4]):
        bits += bits_msb(b)
    return bits


def generate(out_path, repeat=100, gap_ms=0.0, fs=FS, dev=DEV, amp=AMP,
             preamble_bytes=4):
    bits = packet_bits(preamble_bytes)
    sps = int(fs / BPS)
    gap_samples = int(gap_ms * fs / 1000.0)
    total = len(bits) * repeat * sps + gap_samples * repeat
    iq = np.zeros(total, dtype=np.complex64)
    phase = 0.0
    k = 0
    for _ in range(repeat):
        for b in bits:
            f = dev if b else -dev
            dphi = 2.0 * np.pi * f / fs
            for _ in range(sps):
                iq[k] = amp * np.exp(1j * phase)
                phase += dphi
                k += 1
        if gap_samples:
            k += gap_samples
    iq8 = np.stack((np.real(iq), np.imag(iq)), axis=-1).astype(np.int8)
    iq8.tofile(out_path)
    dur = total / fs
    print(f'wrote {out_path}: {iq8.shape[0]} samples, {dur:.2f} s, '
          f'{iq8.nbytes / 1000000.0:.1f} MB ({repeat} packets, '
          f'gap {gap_ms} ms)', file=sys.stderr)
    return iq8.shape[0]


def main():
    if len(sys.argv) < 2:
        print('usage: gen_2fsk.py out.c8 [repeat] [gap_ms] [preamble_bytes]',
              file=sys.stderr)
        sys.exit(1)
    out = sys.argv[1]
    repeat = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    gap_ms = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    preamble_bytes = int(sys.argv[4]) if len(sys.argv) > 4 else 4
    print(f'samples/symbol={SPS}, bits/packet={len(packet_bits(preamble_bytes))}',
          file=sys.stderr)
    generate(out, repeat, gap_ms, preamble_bytes=preamble_bytes)


if __name__ == '__main__':
    main()
