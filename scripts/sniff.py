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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import capture_custom  # noqa: E402
import rfuzz_tools  # noqa: E402
import sniff_decoder  # noqa: E402

DEFAULT_PORT = "COM7"
DEFAULT_BAUD_SER = 115200
DEFAULT_RATE = 250000
DEFAULT_SAMPLES = 262144


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
    for line in sniff_decoder.format_report(rep, "GDO2"):
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
    ch0, ch1, rate = sniff_decoder.load_capture(args.file, args.rate)
    print(f"capture: {args.file} rate={rate:.0f} Hz "
          f"samples={len(ch1)} ({len(ch1) / rate:.2f} s)")
    probe = ch1 if args.probe == "gdo2" else ch0
    rep = sniff_decoder.analyze_logic(
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
    rep = sniff_decoder.analyze_logic(
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
    rep = sniff_decoder.analyze_logic(
        ch1, ch0, rate=r_actual, baud_hint=args.baud, sync_hex=args.sync,
        max_payload_bytes=args.max_bytes,
        trim_trailing_idle=not args.no_trim_trailing_idle)
    base = args.out or (args.name or "sniff_stream")
    save_logic(base, ch0, ch1, r_actual)
    _print_report(rep, base)
    return 0


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
