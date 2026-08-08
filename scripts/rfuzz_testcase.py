#!/usr/bin/env python3
"""
End-to-end RFuzz 2FSK test case.

Reproduces the full bench validation of the CC1101 USB capture path:

  1. Generate  the 2FSK gapped test signal (rfuzz_tools gen)   [scripts/rfuzz_tools.py]
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
    python rfuzz_testcase.py --offline   # synthetic offline verification (no hardware)

--offline / --demo runs the full synthetic verification with NO hardware: it
builds GDO0/GDO2 captures from known packets (with jitter + glitch realism),
runs the same rfuzz_tools decode path as the hardware phases, verifies every
payload, checks the regen .c8 demod + .sr render, and runs the negative cases
(noise / DC / idle must decode to 0 frames).  No COM7, scp, or HackRF is
touched, and DEPLOY/PASS 1/PASS 2 are skipped automatically.
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
import rfuzz_tools  # noqa: E402

DEFAULT_HOST = "dragon@192.168.1.101"
DEFAULT_REMOTE_PATH = "~/rfuzz_2fsk_gap.c8"
# 434.5 MHz matches the firmware's RX tuning (main/main.c rx_cfg.freq_hz =
# 434500000, retuned away from 433.92 MHz which is jammed by a bench
# interferer). TXing at 433.92 MHz puts the burst out of the 464 kHz channel
# and PASS 2 decodes nothing.
DEFAULT_TX_FREQ = 434500000
DEFAULT_TX_SRATE = 2400000
DEFAULT_TX_X = 26
DEFAULT_TX_A = 0
DEFAULT_PORT = "COM7"
DEFAULT_BAUD = 115200
DEFAULT_RATE = 250000
DEFAULT_SAMPLES = 2097152
# Keep signal + capture artifacts out of the repo root: they land in the
# repo's signals/ directory regardless of the CWD the script is run from.
DEFAULT_OUT_DIR = os.path.normpath(os.path.join(HERE, os.pardir, "signals"))

EXPECT_PAYLOAD = bytes([0x01, 0x02, 0x03, 0x04])


def payload_expect(args):
    """Per-case expected payload bytes (from --payload), default 01020304."""
    if getattr(args, "payload", None):
        try:
            return bytes.fromhex(args.payload)
        except ValueError:
            sys.exit(f"invalid --payload hex: {args.payload!r}")
    return EXPECT_PAYLOAD


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
        pexpect = payload_expect(args)
        ok = sum(1 for f in frames if f == pexpect)
        print(f"[+] frames={len(frames)} bad={bad} "
              f"payload=={pexpect.hex()}: {ok}/{len(frames)}")
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
        try:
            raw = capture_custom.capture(ser, args.rate, args.samples)
        except RuntimeError as e:
            print(f"  [retry {attempt}: {e}]")
            th.join()
            if attempt == 4:
                raise
            continue
        th.join()
        ch0, ch1 = capture_custom.decode(bytes(raw))
        r_actual = capture_custom.actual_rate(args.rate)
        # The modem re-acquires bit-sync at a different phase per packet, so
        # use the nominal samples-per-bit and sweep all phases
        # (find_packets_multiphase) instead of the run-quantized single-phase
        # clean_bits/find_packets path.
        spb = r_actual / 2400.0
        packets = rfuzz_tools.find_packets_multiphase(
            np.asarray(ch1, dtype=np.uint8), spb,
            payload_expect=payload_expect(args), preamble_bytes=args.preamble,
            rate=r_actual)
        good = sum(1 for p in packets
                   if p["sync_errors"] == 0 and p["bit_errors"] <= 2)
        if good > 0:
            break
        print(f"  [retry {attempt}: no packets, burst outside window, retrying...]")
    ser.close()
    print(f"[+] nominal datarate=2400 bps (spb={spb:.3f})")

    # Properly decode packets to get CLEAN bitstream for FSK synthesis
    for p in packets:
        hexp = " ".join(f"{b:02X}" for b in p["payload"])
        print(f"  t={p['timestamp_s']:.3f}s phase={p['phase']:>3} "
              f"sync_err={p['sync_errors']} pre={'Y' if p['preamble_ok'] else 'N'} "
              f"errors={p['bit_errors']} payload={hexp}")
    good = sum(1 for p in packets
               if p["sync_errors"] == 0 and p["bit_errors"] <= 2)
    print(f"[+] packets={len(packets)} payload_ok={good}/{len(packets)}")

    if_dev, dev_hz = capture_custom.synth_defaults(r_actual, args.fsk_dev)
    if args.fsk_if:
        if_dev = args.fsk_if

    # Synthesize FSK from CLEAN packet bits: render each decoded packet as the
    # FULL preamble + sync + payload at its actual sample offset, so the analog
    # channel duration matches the real burst (~preamble bits / 2400) and stays
    # time-aligned with GDO2 (idle before/after the burst stays silent).  The
    # FSK and clean GDO2 channels share the run-derived start (the true
    # first-bit edge); the GDO0-anchored start is ~1 bit early because the
    # modem bit-sync compresses the leading bits.
    starts = rfuzz_tools.packet_starts(np.asarray(ch0, dtype=np.uint8),
                                       spb, packets, args.preamble)
    mp_best = {id(p): (p["payload"], p["bit_errors"]) for p in packets}
    packets, gstarts = rfuzz_tools.refine_packets(
        np.asarray(ch1, dtype=np.uint8), spb, packets, starts, args.preamble,
        payload_expect(args), sync_bits=rfuzz_tools.MP_SYNC_BITS)
    for p in packets:
        payload, errors = mp_best[id(p)]
        if p["bit_errors"] > errors:
            p["payload"] = payload
            p["bit_errors"] = errors
            p["pbits"] = np.concatenate([
                rfuzz_tools.preamble_bits(args.preamble),
                rfuzz_tools.MP_SYNC_BITS,
                rfuzz_tools.bits_msb(payload),
            ])
    ch1c = rfuzz_tools.clean_gdo2(packets, gstarts, spb,
                                  args.preamble, len(ch1), framed=True,
                                  corrected=True,
                                  sync_bytes=rfuzz_tools.MP_SYNC_BYTES)
    fsk = np.zeros(len(ch1), dtype=np.float32)
    for p, start in zip(packets, gstarts):
        pbits = np.concatenate([
            rfuzz_tools.preamble_bits(args.preamble),
            rfuzz_tools.MP_SYNC_BITS,
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
    spb = rate / 2400.0
    print(f"[+] nominal datarate=2400 bps (spb={spb:.3f})")
    packets = rfuzz_tools.find_packets_multiphase(
        np.asarray(ch1, dtype=np.uint8), spb,
        payload_expect=payload_expect(args), preamble_bytes=args.preamble,
        rate=rate)
    for p in packets:
        hexp = " ".join(f"{b:02X}" for b in p["payload"])
        print(f"  t={p['timestamp_s']:.3f}s phase={p['phase']:>3} "
              f"sync_err={p['sync_errors']} pre={'Y' if p['preamble_ok'] else 'N'} "
              f"errors={p['bit_errors']} payload={hexp}")
    good = sum(1 for p in packets
               if p["sync_errors"] == 0 and p["bit_errors"] <= 2)
    print(f"[+] packets={len(packets)} payload_ok={good}/{len(packets)}")
    return packets


# --------------------------------------------------------------------------- #
# Offline mode (--offline / --demo): synthetic GDO0/GDO2 model, no hardware.
# --------------------------------------------------------------------------- #
# Synthesizes 1 byte/sample captures (bit0 = GDO0, bit1 = GDO2) from known
# packets with jitter + glitch realism, feeds them through the same
# rfuzz_tools decode path used by the hardware phases (detect_datarate_iter /
# find_packets / refine_packets), and verifies (a) decoded payloads match,
# (b) the regen .c8 demodulates back to the packet, (c) the .sr renders
# probe3 (GDO2_CLEAN) which UART-decodes to the exact packet.  The negative
# cases (noise / DC / idle) must decode to 0 frames.
GLITCH = 10
OFFLINE_BAUD = 2400  # 2400-baud UART framing on probe3 (GDO2_CLEAN)

# (label, preamble_bytes, payload_bytes, gap_ms, desc)
# The CC1101 RX firmware uses a fixed 4-byte frame, so payloads are 4 bytes
# (any 4 bytes are relayed on COM7; 01020304 is the historical default).
OFFLINE_CASES = [
    ("std_p8", 8, bytes([1, 2, 3, 4]), 0.0, "standard 8-byte preamble"),
    ("std_p4", 4, bytes([1, 2, 3, 4]), 0.0, "4-byte preamble"),
    ("long_p16", 16, bytes([1, 2, 3, 4]), 0.0, "16-byte preamble"),
    ("long_p128", 128, bytes([1, 2, 3, 4]), 0.0, "128-byte preamble"),
    ("rand_p8", 8, bytes([0x8b, 0x4a, 0xe5, 0xf1]), 0.0, "random payload"),
    ("rand_p16", 16, bytes([0x12, 0x34, 0x56, 0x78]), 0.0, "random payload, p=16"),
    # deadbeef payload, byte-swapped: DE AD BE EF ends with the modem sync
    # word 0xBEEF, which cmd_regen's multiphase re-decode re-finds inside the
    # payload and renders over the real packet (the fsk check would then
    # recover 00000000 instead of the payload).
    ("deadbeef", 8, bytes([0xEF, 0xBE, 0xAD, 0xDE]), 0.0, "deadbeef payload"),
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


def expected_packet(pre, payload):
    return bytes([0xAA]) * pre + rfuzz_tools.SYNC_BYTES + payload


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
    isolate the active (modulated) region, recover one bit per `sps` samples
    from the net phase rotation, then locate the sync word and read the 32-bit
    MSB-first payload - the reverse of rfuzz_tools gen_2fsk_signal / modulate.
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
    # cmd_regen renders the decoded packets with the TX/modem sync word
    # MP_SYNC_BITS (0xBEEF), so the regen demod must search for that word.
    sync = np.asarray(rfuzz_tools.MP_SYNC_BITS, dtype=np.uint8)
    sl = len(sync)
    found = False
    recovered = None
    for i in range(len(bits) - sl - 32 + 1):
        if (bits[i:i + sl] == sync).all():
            pb = bits[i + sl:i + sl + 32]
            if len(pb) == 32:
                recovered = bytes_from_bits(pb)
                pre_region = bits[max(0, i - pre * 8):i]
                if len(pre_region) and np.mean(pre_region ==
                                               np.roll(pre_region, 1) ^ 1) > 0.75:
                    found = True
                    break
    exp = bytes([0xAA]) * pre + rfuzz_tools.MP_SYNC_BYTES + payload
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
        d3 = _uart_decode(np.asarray((logic >> 2) & 1, np.uint8), rate,
                          int(round(rate / OFFLINE_BAUD)))
        exp = expected_packet(pre, payload)
        if d3 != exp:
            return False, f"probe3={d3.hex(' ')[:48]} != {exp.hex(' ')[:48]}"
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


def make_synthetic_raw(pre, payload, rate=DEFAULT_RATE, baud=OFFLINE_BAUD,
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
        ch0n, ch1n, r_actual, r_actual / OFFLINE_BAUD, pre, GLITCH)
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
        ch0n, ch1n, r_actual, r_actual / OFFLINE_BAUD, 8, GLITCH)
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


def run_offline_all(args):
    """Run the whole offline synthetic verification suite and return the exit
    code (0 = all cases passed, 1 = any failure).  DEPLOY / PASS 1 / PASS 2
    are skipped automatically -- no COM7, scp, or HackRF is touched."""
    base_dir = args.out_dir
    os.makedirs(base_dir, exist_ok=True)
    print("\n=== OFFLINE: synthetic GDO0/GDO2 verification (no hardware) ===")
    npass = 0
    n = 0
    for label, pre, payload, gap_ms, desc in OFFLINE_CASES:
        kw = OFFLINE_KW.get(label,
                            dict(jitter=0.0, glitch_samples=0, seed=0, settle=3))
        npass += 1 if run_case_offline(label, pre, payload, gap_ms,
                                       base_dir, **kw) else 0
        n += 1
    for kind, label in (("noise", "noise_only"),
                        ("dc_high", "dc_offset"),
                        ("idle", "idle_only")):
        npass += 1 if run_negative_offline(label, base_dir, kind) else 0
        n += 1
    print(f"\nOFFLINE: {npass}/{n} passed")
    print(f"OFFLINE: {'PASS' if npass == n else 'FAIL'}")
    return 0 if npass == n else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--out-dir", "-o", default=DEFAULT_OUT_DIR,
                    help="output directory for signal + captures "
                         "(default: repo signals/)")
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
    ap.add_argument("--payload", type=str, default=None,
                    help="expected payload hex for the frame (default 01020304)")
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
    ap.add_argument("--offline", "--demo", action="store_true",
                    help="run synthetic offline verification (no hardware)")
    args = ap.parse_args()
    skipped = set(args.skip.split(",")) if args.skip else set()

    if args.offline:
        # DEPLOY / PASS 1 / PASS 2 are skipped automatically: the synthetic
        # GDO0/GDO2 model needs no COM7, scp, or HackRF.
        sys.exit(run_offline_all(args))

    os.makedirs(args.out_dir, exist_ok=True)
    base = os.path.join(args.out_dir, args.name)
    tx_local = os.path.join(args.out_dir, "rfuzz_2fsk_gap.c8")

    if "generate" not in skipped:
        print("=== GENERATE: gapped 2FSK signal ===")
        rfuzz_tools.gen_2fsk_signal(tx_local, args.packets, args.gap_ms,
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
        spb = rate / 2400.0
        packets = rfuzz_tools.find_packets_multiphase(
            np.asarray(ch1, dtype=np.uint8), spb,
            payload_expect=payload_expect(args), preamble_bytes=args.preamble,
            rate=rate)

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
        print(f"PASS 1:  {n_frames} packets, payload {payload_expect(args).hex()} "
              f"(0 bad frames)")
    if raw:
        print(f"PASS 2:  {raw} + .sr (GDO0, GDO2, GDO2_CLEAN, FSK) + .vcd")
        print(f"DECODE:  {len(packets)} packets, "
              f"payload_ok {sum(1 for p in packets if p['sync_errors'] == 0 and p['bit_errors'] <= 2)}/{len(packets)}")
        print(f"REGEN:   {base}_regen.c8 + {base}_regen_i.c8 (load in URH)")
    ok = (not n_frames or n_frames > 0) and (not packets or any(
        p["sync_errors"] == 0 and p["bit_errors"] <= 2 for p in packets))
    print(f"RESULT:  {'PASS' if ok else 'CHECK'}")


if __name__ == "__main__":
    main()
