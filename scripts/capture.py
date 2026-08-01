#!/usr/bin/env python3
"""
RFuzz Capture — raw streaming capture from ESP32 to .sr file.

Protocol:
  Host → ESP32:  [0x01][rate_hz:4 LE][count:4 LE]   (9 bytes total)
  ESP32 → Host:  count raw bytes (1 byte/sample, bit0=GDO0, bit1=GDO2)

Usage:
    python capture.py                         # default COM7, 24kHz, 100k samples
    python capture.py --port COM7
    python capture.py --rate 100000 -n 500000
    python capture.py --channels 1
"""

import argparse
import struct
import sys
import time
import zipfile
from datetime import datetime

try:
    import serial
except ImportError:
    print("pip install pyserial")
    sys.exit(1)

CMD_START = 0x01


def capture(ser, rate, count, timeout=30.0):
    hdr = struct.pack("<BI", CMD_START, rate) + struct.pack("<I", count)
    ser.reset_input_buffer()
    ser.write(hdr)

    data = bytearray()
    t0 = time.time()
    while len(data) < count:
        if ser.in_waiting > 0:
            data.extend(ser.read(ser.in_waiting))
        elif time.time() - t0 > timeout:
            print(f"Timeout: got {len(data)}/{count} bytes")
            break
        else:
            time.sleep(0.001)

    return bytes(data)


def save_sr(ch0, ch1, sample_rate, filename):
    ch_count = 2 if ch1 else 1
    num_samples = len(ch0)

    packed = bytearray(num_samples)
    for i in range(num_samples):
        b = 0
        if ch0[i]:
            b |= 0x01
        if ch1 and i < len(ch1) and ch1[i]:
            b |= 0x02
        packed[i] = b

    metadata = (
        "[global]\n"
        "sigrok version = 2\n"
        "\n"
        "[device 1]\n"
        "capturefile = logic-1-1\n"
        "unitsize = 1\n"
        "total probes = %d\n"
        "total analog = 0\n"
        "samplerate = %d\n"
        "probe1 = GDO0\n"
    ) % (ch_count, sample_rate)
    if ch_count >= 2:
        metadata += "probe2 = GDO2\n"

    with zipfile.ZipFile(filename, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("version", "2")
        zf.writestr("metadata", metadata)
        zf.writestr("logic-1-1", bytes(packed))

    print(f"Saved: {filename} ({num_samples} samples, {ch_count}ch, {sample_rate} Hz)")


def main():
    p = argparse.ArgumentParser(description="RFuzz Capture")
    p.add_argument("--port", "-p", default="COM7")
    p.add_argument("--rate", "-r", type=int, default=24000)
    p.add_argument("--samples", "-n", type=int, default=100000)
    p.add_argument("--channels", "-c", type=int, default=2, choices=[1, 2])
    p.add_argument("--output", "-o", default=None)
    args = p.parse_args()

    try:
        ser = serial.Serial(args.port, 115200, timeout=1)
    except serial.SerialException as e:
        print(f"Cannot open {args.port}: {e}")
        sys.exit(1)

    time.sleep(1.0)
    ser.reset_input_buffer()

    print(f"Capturing: {args.rate} Hz, {args.samples} samples, {args.channels}ch")
    raw = capture(ser, args.rate, args.samples)

    if not raw:
        print("No data received")
        ser.close()
        sys.exit(1)

    print(f"Received {len(raw)} bytes")

    if args.channels >= 2:
        ch0 = bytearray(raw[i] & 0x01 for i in range(len(raw)))
        ch1 = bytearray((raw[i] >> 1) & 0x01 for i in range(len(raw)))
    else:
        ch0 = bytearray(raw[i] & 0x01 for i in range(len(raw)))
        ch1 = None

    # Stats
    ones = sum(ch0)
    total = len(ch0)
    trans = sum(1 for i in range(1, total) if ch0[i] != ch0[i - 1])
    print(f"Ch0: {ones}/{total} high ({100 * ones / total:.1f}%), {trans} transitions")

    if args.output:
        out = args.output
    else:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        out = f"capture_{ts}.sr"

    save_sr(ch0, ch1, args.rate, out)
    ser.close()


if __name__ == "__main__":
    main()
