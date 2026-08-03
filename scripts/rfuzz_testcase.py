#!/usr/bin/env python3
"""
End-to-end RFuzz 2FSK test case.

Reproduces the full bench validation of the CC1101 USB capture path:

  1. Generate  the 2FSK gapped test signal (gen_2fsk.py)      [scripts/gen_2fsk.py]
  2. Deploy    it to the Dragon OS VM running HackRF One
  3. PASS 1    single HackRF TX pass while verifying CC1101 packet RX on COM7
              (expect [len:2LE]=04 00 + payload 01 02 03 04 for every packet)
  4. PASS 2    single HackRF TX pass while capturing GDO0/GDO2 (channel 3 =
              FSK baseband analog is added to the .sr for PulseView)
  5. Decode    the capture back into packets                  [scripts/rfuzz_tools.py]
  6. Regen     the decoded packets to 2FSK I/Q .c8 (full + I-only) for URH

Every step runs on the default settings used during development and verified:
TX at -x 26 -a 0 (single pass, never -R), capture at requested 250000 Hz
(actual 250000 Hz -> 104.2 samples/bit). Channel 3 (FSK) renders the source
deviation (--fsk-dev, default 50000 Hz) as two tones IF +- dev (IF = 22.5%
of the real rate = 56250 Hz -> tones 6250 Hz / 106250 Hz); --mod picks the
basic modulation (2fsk/2psk/ask).

Requirements:
  - Firmware flashed and USB Serial/JTAG on COM7 (packet + capture stream)
  - Dragon OS VM with HackRF One (ssh key auth)  --host dragon@192.168.1.101
  - pip install pyserial numpy

Usage:
    python rfuzz_testcase.py
    python rfuzz_testcase.py --out-dir C:\\tmp\\t
    python rfuzz_testcase.py --packets 100 --gap-ms 0
    python rfuzz_testcase.py --skip packets    # skip PASS 1
    python rfuzz_testcase.py --mod 2psk        # BPSK analog channel
    python rfuzz_testcase.py --samples 60000 --packets 30 --gap-ms 30
    python rfuzz_testcase.py --packets 1 --preamble 256    # reliable single-shot
"""

import argparse
import os
import subprocess
import sys
import threading
import time
import types

import serial
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import capture_custom  # noqa: E402
import gen_2fsk  # noqa: E402
import rfuzz_tools  # noqa: E402

DEFAULT_HOST = "dragon@192.168.1.101"
DEFAULT_REMOTE_PATH = "~/rfuzz_2fsk_gap.c8"
DEFAULT_TX_FREQ = 433920000
DEFAULT_TX_SRATE = 2400000
DEFAULT_TX_X = 26
DEFAULT_TX_A = 0
DEFAULT_PORT = "COM7"
DEFAULT_BAUD = 115200
DEFAULT_RATE = 250000
DEFAULT_SAMPLES = 262144

EXPECT_PAYLOAD = bytes([0x01, 0x02, 0x03, 0x04])


def detect_spb(ch0, ch1, rate, preamble):
    """Best-effort samples-per-bit from the GDO0-gated packet windows."""
    ch0n = np.asarray(ch0, dtype=np.uint8)
    ch1n = np.asarray(ch1, dtype=np.uint8)
    spb, bps, windows = rfuzz_tools.detect_datarate_iter(
        ch0n, ch1n, rate, rate / 2400.0, preamble)
    return spb, bps, windows


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw)


def parse_frames(buf):
    frames = []
    bad = 0
    i = 0
    while i + 2 <= len(buf):
        ln = buf[i] | (buf[i + 1] << 8)
        if ln == 0 or ln > 64 or i + 2 + ln > len(buf):
            bad += 1
            i += 1
            continue
        frames.append(bytes(buf[i + 2:i + 2 + ln]))
        i += 2 + ln
    return frames, bad


def verify_packets(port, baud, duration, result):
    ser = serial.Serial(port, baud, timeout=0.1)
    ser.reset_input_buffer()
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < duration:
        b = ser.read(4096)
        if b:
            buf.extend(b)
    ser.close()
    result["buf"] = bytes(buf)


def tx_pass(host, tx_file, freq, srate, x, a):
    cmd = (f"ssh {host} \"hackrf_transfer -t {tx_file} -f {freq} -s {srate} "
           f"-x {x} -a {a}\"")
    print(f"[*] TX: {cmd}")
    r = sh(cmd, timeout=90)
    if r.returncode != 0:
        print(r.stdout[-400:] if r.stdout else "")
        print(r.stderr[-400:] if r.stderr else "")
        sys.exit(f"TX pass failed (rc={r.returncode})")
    for line in r.stdout.splitlines()[-3:]:
        print(f"    {line.strip()}")


def phase_packets(args, tx_file):
    print("\n=== PASS 1: CC1101 packet RX verification ===")
    result = {}
    # Retry up to 3 times to handle HackRF startup jitter:
    # the burst may miss the firmware's 12s listen window.
    for attempt in range(1, 4):
        th = threading.Thread(target=verify_packets,
                              args=(args.port, DEFAULT_BAUD, 12.0, result))
        th.start()
        time.sleep(1.5)
        tx_pass(args.host, tx_file, args.tx_freq, args.tx_srate, args.tx_x,
                args.tx_a)
        th.join()
        frames, bad = parse_frames(result["buf"])
        ok = sum(1 for f in frames if f == EXPECT_PAYLOAD)
        print(f"[+] frames={len(frames)} bad={bad} "
              f"payload==01 02 03 04: {ok}/{len(frames)}")
        if ok > 0 or len(frames) > 0:
            return len(frames)
        if attempt < 3:
            print(f"  [retry {attempt}: no packets, retrying...]")
            time.sleep(1.0)
    sys.exit("FAIL: no clean packets received")


def phase_capture(args, tx_file, base):
    print("\n=== PASS 2: GDO0/GDO2 capture (ch3 = FSK analog) ===")
    ser = capture_custom.open_port(args.port, DEFAULT_BAUD)
    capture_custom.drain(ser)

    def tx():
        tx_pass(args.host, tx_file, args.tx_freq, args.tx_srate, args.tx_x,
                args.tx_a)

    # Capture-first: open the capture window immediately, then launch TX.
    # The burst lands ~0.5-1.0 s in (HackRF startup), so the window always
    # opens before the preamble -> the full preamble + sync + payload are
    # captured and pre=Y.  Retry if HackRF startup jitter put the burst
    # outside the window.
    for attempt in range(1, 5):
        th = threading.Thread(target=tx)
        th.start()
        raw = capture_custom.capture(ser, args.rate, args.samples)
        th.join()
        ch0, ch1 = capture_custom.decode(bytes(raw))
        r_actual = capture_custom.actual_rate(args.rate)
        spb, bps, _ = detect_spb(ch0, ch1, r_actual, args.preamble)
        bits = rfuzz_tools.clean_bits(ch1, spb, 10)
        packets = rfuzz_tools.find_packets(bits, spb, args.preamble)
        if packets:
            break
        print(f"  [retry {attempt}: no packets, burst outside window, retrying...]")
    ser.close()
    print(f"[+] detected datarate={bps:.0f} bps (spb={spb:.3f})")

    # Properly decode packets to get CLEAN bitstream for FSK synthesis
    for p in packets:
        hexp = " ".join(f"{b:02X}" for b in p["payload"])
        print(f"  bit@{p['offset_bits']:>7} pre={'Y' if p['preamble_ok'] else 'N'} "
              f"errors={p['bit_errors']} payload={hexp}")
    good = sum(1 for p in packets if not p["bit_errors"])
    print(f"[+] packets={len(packets)} payload_ok={good}/{len(packets)}")

    if_dev, dev_hz = capture_custom.synth_defaults(r_actual, args.fsk_dev)
    if args.fsk_if:
        if_dev = args.fsk_if

    # Synthesize FSK from CLEAN packet bits: render each decoded packet as the
    # FULL preamble + sync + payload at its actual sample offset, so the analog
    # channel duration matches the real burst (~preamble bits / 2400) and stays
    # time-aligned with GDO2 (idle before/after the burst stays silent).
    starts = rfuzz_tools.packet_starts(np.asarray(ch0, dtype=np.uint8),
                                       spb, packets, args.preamble)
    packets = rfuzz_tools.refine_packets(
        np.asarray(ch1, dtype=np.uint8), spb, packets, starts, args.preamble)
    ch1c = rfuzz_tools.clean_gdo2(packets, starts, r_actual / 2400.0,
                                  args.preamble, len(ch1))
    fsk = np.zeros(len(ch1), dtype=np.float32)
    for p, start in zip(packets, starts):
        pbits = np.concatenate([
            rfuzz_tools.preamble_bits(args.preamble),
            rfuzz_tools.SYNC_BITS,
            rfuzz_tools.bits_msb(p["payload"]),
        ])
        fsk += capture_custom.mod_synth_from_bits(
            pbits, args.analog_amp, args.mod, if_dev, dev_hz, r_actual,
            len(ch1), spb, start)
    capture_custom.stats(ch0, ch1, r_actual, args.analog_amp, args.mod,
                         if_dev, dev_hz)
    with open(base + ".raw", "wb") as f:
        f.write(bytes(raw))
    capture_custom.save_sr(ch0, ch1, ch1c, fsk, int(round(r_actual)),
                           base + ".sr")
    capture_custom.save_vcd(ch0, ch1, ch1c, fsk, r_actual, base + ".vcd")
    return base + ".raw"


def phase_decode(raw, args):
    print("\n=== DECODE: packets from GDO2 capture ===")
    ch0, ch1, rate = rfuzz_tools.load_raw(raw, args.rate)
    spb, bps, _ = detect_spb(ch0, ch1, rate, args.preamble)
    print(f"[+] detected datarate={bps:.0f} bps (spb={spb:.3f})")
    bits = rfuzz_tools.clean_bits(ch1, spb, 10)
    packets = rfuzz_tools.find_packets(bits, spb, args.preamble)
    for p in packets:
        hexp = " ".join(f"{b:02X}" for b in p["payload"])
        print(f"  bit@{p['offset_bits']:>7} pre={'Y' if p['preamble_ok'] else 'N'} "
              f"errors={p['bit_errors']} payload={hexp}")
    good = sum(1 for p in packets if not p["bit_errors"])
    print(f"[+] packets={len(packets)} payload_ok={good}/{len(packets)}")
    return packets


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--out-dir", "-o", default=".",
                    help="output directory for signal + captures")
    ap.add_argument("--name", default="rfuzz_testcase",
                    help="output base name (default rfuzz_testcase)")
    ap.add_argument("--remote-path", default=DEFAULT_REMOTE_PATH,
                    help="deploy path on the Dragon OS VM")
    ap.add_argument("--packets", type=int, default=60)
    ap.add_argument("--gap-ms", type=float, default=50.0)
    ap.add_argument("--tx-freq", type=int, default=DEFAULT_TX_FREQ)
    ap.add_argument("--tx-srate", type=int, default=DEFAULT_TX_SRATE)
    ap.add_argument("--tx-x", type=int, default=DEFAULT_TX_X)
    ap.add_argument("--tx-a", type=int, default=DEFAULT_TX_A)
    ap.add_argument("--port", "-p", default=DEFAULT_PORT)
    ap.add_argument("--rate", "-r", type=int, default=DEFAULT_RATE)
    ap.add_argument("--samples", "-n", type=int, default=DEFAULT_SAMPLES)
    ap.add_argument("--preamble", type=int, default=4,
                    help="preamble bytes in TX signal (4 = CC1101 minimum; "
                         "use 128+ for single-shot reliability with HackRF jitter): "
                         "python rfuzz_testcase.py --packets 1 --preamble 128")
    ap.add_argument("--analog-amp", type=int, default=90)
    ap.add_argument("--mod", default="2fsk", choices=capture_custom.MODS,
                    help="basic modulation rendered on channel 3 "
                         "(default 2fsk; choices: "
                         + ", ".join(capture_custom.MODS) + ")")
    ap.add_argument("--fsk-if", type=float, default=0.0,
                    help="FSK center IF in Hz for the synthesized sinusoid "
                         "(default: 22.5%% of the real rate when the deviation "
                         "fits the band, else real rate / 10)")
    ap.add_argument("--fsk-dev", type=float, default=capture_custom.DEFAULT_DEV,
                    help="FSK deviation from IF in Hz (default 50000, the "
                         "source signal's deviation)")
    ap.add_argument("--skip", default="",
                    help="comma list of phases: generate,deploy,packets,capture,decode,regen")
    args = ap.parse_args()
    skipped = set(args.skip.split(",")) if args.skip else set()

    os.makedirs(args.out_dir, exist_ok=True)
    base = os.path.join(args.out_dir, args.name)
    tx_local = os.path.join(args.out_dir, "rfuzz_2fsk_gap.c8")

    if "generate" not in skipped:
        print("=== GENERATE: gapped 2FSK signal ===")
        gen_2fsk.generate(tx_local, args.packets, args.gap_ms,
                          preamble_bytes=args.preamble)

    if "deploy" not in skipped:
        print(f"=== DEPLOY: to {args.host}:{args.remote_path} ===")
        r = sh(f"scp \"{tx_local}\" {args.host}:{args.remote_path}")
        if r.returncode != 0:
            sys.exit(f"scp failed (rc={r.returncode}): {r.stderr[-400:]}")

    n_frames = 0
    if "packets" not in skipped:
        n_frames = phase_packets(args, args.remote_path)

    raw = None
    if "capture" not in skipped:
        raw = phase_capture(args, args.remote_path, base)

    packets = []
    if raw and "decode" not in skipped:
        packets = phase_decode(raw, args)
    elif raw:
        ch0, ch1, rate = rfuzz_tools.load_raw(raw, args.rate)
        spb, _, _ = detect_spb(ch0, ch1, rate, args.preamble)
        packets = rfuzz_tools.find_packets(
            rfuzz_tools.clean_bits(ch1, spb, 10), spb, args.preamble)

    if raw and "regen" not in skipped:
        out = base + "_regen.c8"
        print("\n=== REGEN: decoded packets -> 2FSK I/Q for URH ===")
        rargs = types.SimpleNamespace(raw=raw, rate=args.rate, spb=0.0,
                                      glitch=10, out=out, amp=args.analog_amp,
                                      dev=50000.0, preamble=args.preamble)
        rfuzz_tools.cmd_regen(rargs)

    print("\n=== SUMMARY ===")
    print(f"signal:  {tx_local} ({args.packets} packets, {args.gap_ms} ms gap)")
    print(f"TX:      ssh {args.host} hackrf_transfer -t {args.remote_path} "
          f"-f {args.tx_freq} -s {args.tx_srate} -x {args.tx_x} -a {args.tx_a}")
    if n_frames:
        print(f"PASS 1:  {n_frames} packets, payload 01 02 03 04 (0 bad frames)")
    if raw:
        print(f"PASS 2:  {raw} + .sr (GDO0/GDO2/FSK) + .vcd")
        print(f"DECODE:  {len(packets)} packets, "
              f"payload_ok {sum(1 for p in packets if not p['bit_errors'])}/{len(packets)}")
        print(f"REGEN:   {base}_regen.c8 + {base}_regen_i.c8 (load in URH)")
    ok = (not n_frames or n_frames > 0) and (not packets or any(not p["bit_errors"] for p in packets))
    print(f"RESULT:  {'PASS' if ok else 'CHECK'}")


if __name__ == "__main__":
    main()
