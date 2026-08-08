#!/usr/bin/env python3
"""Generate the 2FSK test signal used by the RFuzz end-to-end test case.

Packet = 0xAA x`preamble_bytes` (preamble) + 0xDE 0xAF (sync) + `payload`
(a 4-byte default of 01 02 03 04), bits MSB-first, 2FSK at +50 kHz / -50 kHz,
phase-continuous, int8 I/Q interleaved.

Output matches what the CC1101 is configured to receive (433.92 MHz, 2400 bps,
sync 0xDEAF 16/16, fixed payload, no CRC, no whitening).  The payload and
preamble length can be overridden (including a random payload via --random)
so the host pipeline can be exercised against varied / random signals.

Usage:
    python gen_2fsk.py out.c8 [repeat] [gap_ms] [preamble_bytes] [payload_hex]
    # repeat         = number of packets (default 100)
    # gap_ms         = idle silence between packets (default 0 = continuous)
    # preamble_bytes = preamble length in bytes (default 4)
    # payload_hex    = payload bytes, e.g. 01020304 (default 01020304)

Same thing as flags (preferred for new tests):
    python gen_2fsk.py out.c8 --repeat 1 --gap-ms 0 --preamble 8
    python gen_2fsk.py out.c8 --random 16 12345     # 16 random payload bytes
    python gen_2fsk.py out.c8 --payload deadbeef     # fixed arbitrary payload

The gap between packets is silent (carrier off, 0 amplitude).  Use a longer
preamble (>= 8 bytes) for single-shot bursts: the CC1101 bit-synchronizer
needs a few bit cells to lock, so the demod glitches the first ~8 bits of an
incoming burst.  A longer TX preamble absorbs that settling glitch and leaves
the pre-sync window clean.
"""
import argparse
import sys

import numpy as np

FS = 2400000.0
BPS = 2400.0
DEV = 50000.0
AMP = 90
SPS = int(FS / BPS)
SYNC = bytes([0xBE, 0xEF])
DEFAULT_PAYLOAD = bytes([1, 2, 3, 4])
DEFAULT_PREAMBLE = 4


def bits_msb(byte):
    return [(byte >> i) & 1 for i in range(7, -1, -1)]


def packet_bits(preamble_bytes=DEFAULT_PREAMBLE, payload=DEFAULT_PAYLOAD):
    bits = []
    pre = bytes([170]) * preamble_bytes
    for b in pre + SYNC + payload:
        bits += bits_msb(b)
    return bits


def _random_payload(length, seed):
    rng = np.random.default_rng(seed)
    return bytes(int(x) for x in rng.integers(0, 256, size=length, dtype=np.uint8))


def generate(out_path, repeat=100, gap_ms=0.0, fs=FS, dev=DEV, amp=AMP,
              preamble_bytes=DEFAULT_PREAMBLE, payload=DEFAULT_PAYLOAD,
              baud=BPS):
    bits = packet_bits(preamble_bytes, payload)
    sps = int(fs / baud)
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
          f'baud={baud:.0f} dev={dev:.0f} preamble={preamble_bytes} '
          f'payload={payload.hex()} gap {gap_ms} ms)',
          file=sys.stderr)
    return iq8.shape[0], bits


def _parse_args(argv):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', help='output .c8 file (int8 IQ interleaved)')
    ap.add_argument('--repeat', '-n', type=int, default=100,
                    help='number of packets (default 100)')
    ap.add_argument('--gap-ms', type=float, default=0.0,
                    help='idle silence between packets in ms (default 0)')
    ap.add_argument('--preamble', '-p', type=int, default=DEFAULT_PREAMBLE,
                    help='preamble bytes (default 4; >=8 for single-shot bursts)')
    ap.add_argument('--baud', type=float, default=BPS,
                    help='symbol rate in bps (default 2400)')
    ap.add_argument('--dev', type=float, default=DEV,
                    help='FSK deviation in Hz (default 50000)')
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--payload', type=str, default=None,
                   help='payload bytes as hex (default 01020304)')
    g.add_argument('--random', type=int, nargs=2, default=None,
                   metavar=('LEN', 'SEED'),
                   help='generate LEN random payload bytes seeded by SEED')
    return ap.parse_args(argv)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    # Backwards-compatible positional parsing: out [repeat] [gap_ms] [preamble]
    # [payload_hex], so legacy `gen_2fsk.py out.c8 1 50 8` still works.
    if len(argv) >= 2 and not argv[1].startswith('-'):
        rest = [a for a in argv[1:] if not a.startswith('--')]
        flags = [a for a in argv[1:] if a.startswith('--')]
        if rest:
            if len(rest) >= 2:
                flags += ['--repeat', rest[0], '--gap-ms', rest[1]]
            if len(rest) >= 3:
                flags += ['--preamble', rest[2]]
            if len(rest) >= 4:
                flags += ['--payload', rest[3]]
        argv = [argv[0]] + flags
    args = _parse_args(argv)
    if args.random is not None:
        length, seed = args.random
        payload = _random_payload(length, seed)
    elif args.payload is not None:
        try:
            payload = bytes.fromhex(args.payload)
        except ValueError:
            sys.exit(f"invalid --payload hex: {args.payload!r}")
    else:
        payload = DEFAULT_PAYLOAD
    if not payload:
        sys.exit('payload must not be empty')
    print(f'samples/symbol={SPS}, bits/packet={len(packet_bits(args.preamble, payload))}',
          file=sys.stderr)
    generate(args.out, args.repeat, args.gap_ms,
             preamble_bytes=args.preamble, payload=payload, baud=args.baud,
             dev=args.dev)


if __name__ == '__main__':
    main()

