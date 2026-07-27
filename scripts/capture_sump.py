#!/usr/bin/env python3
"""
CC1101 SUMP Capture — talks SUMP/OLS protocol to ESP32-S3 logic analyzer.

Supports 1 or 2 channels (GDO0 + GDO2).

Transport modes (set via firmware Kconfig: SUMP_TRANSPORT_USB / SUMP_TRANSPORT_UART):
  USB mode  (default): OLS on USB Serial JTAG (COM6), use --port COM6
  UART mode:           OLS on UART0 (COM7), use --port COM7

Usage:
    python capture_sump.py                         # default COM6, 24kHz, 2ch
    python capture_sump.py --port COM7             # custom port (e.g. UART mode)
    python capture_sump.py --samples 500000        # 500k samples
    python capture_sump.py --rate 100000           # 100 kHz sample rate
    python capture_sump.py --channels 1            # single channel
    python capture_sump.py --format vcd            # save as VCD (PulseView import)
    python capture_sump.py --format sr             # save as Sigrok .sr
    python capture_sump.py --output capture.bin    # save raw bits
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
    print("pyserial not installed. Run: pip install pyserial")
    sys.exit(1)

SUMP_CMD_RESET     = 0x00
SUMP_CMD_RUN       = 0x01
SUMP_CMD_ID        = 0x02
SUMP_CMD_SET_DIV   = 0x80
SUMP_CMD_SET_COUNT = 0x81
SUMP_CMD_SET_FLAGS = 0x82

SUMP_TRIG_MASK0    = 0xC0
SUMP_TRIG_VALUE0   = 0xC1
SUMP_TRIG_CONFIG0  = 0xC2


def sump_find(ser, timeout=3.0):
    """Send ID command and look for SUMP/OLS response."""
    print("[*] Draining boot log output...")
    for _ in range(5):
        ser.write(bytes([SUMP_CMD_RESET]))
        time.sleep(0.3)
        ser.reset_input_buffer()
        time.sleep(0.1)

    print("[*] Sending ID query...")
    for _ in range(10):
        ser.write(bytes([SUMP_CMD_ID]))
        time.sleep(0.3)
        resp = ser.read(4)
        if len(resp) == 4:
            ident = resp.decode("ascii", errors="replace")
            if "S" in ident and "L" in ident and "A" in ident:
                print(f"[+] Device identified: {repr(ident)}")
                return ident
        ser.reset_input_buffer()
    return None


def sump_get_metadata(ser):
    """Request and parse device metadata to determine channel count."""
    ser.write(bytes([0x04]))
    time.sleep(0.5)

    meta = {}
    data = ser.read(256)
    if not data:
        return meta

    i = 0
    while i < len(data):
        if data[i] == 0x00:
            break
        key = data[i]
        if i + 1 >= len(data):
            break
        length = data[i + 1]
        if i + 2 + length > len(data):
            break
        value = data[i + 2:i + 2 + length]
        meta[key] = value
        i += 2 + length

    if 0x01 in meta:
        print(f"[+] Device name: {meta[0x01].decode('ascii', errors='replace')}")
    if 0x02 in meta:
        print(f"[+] Version: {meta[0x02].decode('ascii', errors='replace')}")
    if 0x40 in meta:
        num_ch = meta[0x40][0]
        print(f"[+] Channels: {num_ch}")
        meta["num_channels"] = num_ch
    if 0x21 in meta:
        mem = struct.unpack(">I", meta[0x21])[0]
        print(f"[+] Memory: {mem} bytes")
    if 0x22 in meta:
        max_rate = struct.unpack(">I", meta[0x22])[0]
        print(f"[+] Max sample rate: {max_rate} Hz")
    if 0x23 in meta:
        proto = meta[0x23][0]
        print(f"[+] Protocol version: {proto}")

    return meta


def sump_configure(ser, sample_rate, num_samples, clock_freq=240000000):
    """Send configuration commands (OLS standard protocol). Returns actual sample count."""
    divider = max(0, (clock_freq // sample_rate) - 1)
    print(f"[*] Configuring: {sample_rate} Hz (divider={divider}), {num_samples} samples")

    ser.write(bytes([SUMP_CMD_SET_DIV]))
    ser.write(struct.pack("<I", divider)[:3])

    OLS_MAX_READ_COUNT = 0xFFFF  # 16-bit wire field, firmware multiplies by 4
    read_count_wire = min((num_samples - 1) // 4, OLS_MAX_READ_COUNT)
    actual_samples = (read_count_wire + 1) * 4
    if actual_samples != num_samples:
        print(f"[!] Capped to {actual_samples} samples (OLS protocol limit: 262144)")
    ser.write(bytes([SUMP_CMD_SET_COUNT]))
    ser.write(struct.pack("<H", read_count_wire))
    ser.write(struct.pack("<H", 0))

    ser.write(bytes([SUMP_CMD_SET_FLAGS, 0x00]))

    ser.write(bytes([SUMP_TRIG_MASK0, 0x00]))
    ser.write(bytes([SUMP_TRIG_VALUE0, 0x00]))
    ser.write(bytes([SUMP_TRIG_CONFIG0, 0x00]))

    return actual_samples


def sump_capture(ser, num_samples, num_channels=2, timeout=30.0):
    """Arm capture and read back sample data."""
    print(f"[*] Starting capture ({num_channels} channels)...")
    ser.reset_input_buffer()
    ser.write(bytes([SUMP_CMD_RUN]))

    bytes_per_channel = (num_samples + 7) // 8
    expected_bytes = bytes_per_channel * 4
    data = bytearray()
    t0 = time.time()

    while len(data) < expected_bytes:
        waiting = ser.in_waiting
        if waiting > 0:
            chunk = ser.read(waiting)
            data.extend(chunk)
        elif time.time() - t0 > timeout:
            print(f"[!] Timeout after {timeout}s, got {len(data)}/{expected_bytes} bytes")
            break
        else:
            time.sleep(0.001)

    print(f"[+] Received {len(data)} bytes ({len(data) * 8} bits)")
    return bytes(data)


def decode_ols_samples(raw, num_samples, num_channels=2):
    """Decode OLS 4-byte-block format into per-sample bits for each channel.

    Each 4-byte block: byte0 = 8 samples of ch0, byte1 = 8 samples of ch1.
    Bit 0 of each byte = earliest sample.
    """
    ch0 = bytearray()
    ch1 = bytearray()
    blocks = len(raw) // 4

    for blk in range(blocks):
        for bit in range(8):
            if len(ch0) >= num_samples:
                break
            ch0.append((raw[blk * 4] >> bit) & 1)
            if num_channels >= 2:
                ch1.append((raw[blk * 4 + 1] >> bit) & 1)

    if num_channels == 1:
        return ch0
    return ch0, ch1


def save_vcd(ch0, ch1, sample_rate, filename):
    """Save samples as VCD file with 2 signals: GDO0 and GDO2."""
    t_per_sample_ns = 1e9 / sample_rate
    num_samples = len(ch0)

    with open(filename, "w", newline="\n") as f:
        f.write("$timescale 1ns $end\n")
        f.write("$version CC1101 RFuzz SUMP Capture $end\n")
        f.write("$comment Sample rate: %d Hz, Samples: %d, Channels: %d $end\n"
                % (sample_rate, num_samples, 2 if ch1 else 1))
        f.write("$scope module CC1101 $end\n")
        f.write("$var wire 1 ! GDO0 $end\n")
        if ch1:
            f.write("$var wire 1 @ GDO2 $end\n")
        f.write("$upscope $end\n")
        f.write("$enddefinitions $end\n")

        f.write("$dumpvars\n")
        f.write("x!\n")
        if ch1:
            f.write("x@\n")
        f.write("$end\n")

        prev0 = None
        prev1 = None
        for i in range(num_samples):
            val0 = ch0[i]
            val1 = ch1[i] if ch1 else None

            changed = (val0 != prev0) or (val1 is not None and val1 != prev1)
            if changed:
                t_ns = int(i * t_per_sample_ns)
                f.write("#%d\n" % t_ns)
                if val0 != prev0:
                    f.write("%d!\n" % (1 if val0 else 0))
                if val1 is not None and val1 != prev1:
                    f.write("%d@\n" % (1 if val1 else 0))
                prev0 = val0
                prev1 = val1

    ch_count = 2 if ch1 else 1
    print(f"[+] Saved VCD: {filename} ({num_samples} samples, {ch_count}ch, {sample_rate} Hz)")


def save_bin(ch0, ch1, filename):
    """Save raw bit samples as packed binary."""
    packed = bytearray()
    samples = ch0
    for i in range(0, len(samples), 8):
        byte = 0
        for bit in range(8):
            if i + bit < len(samples) and samples[i + bit]:
                byte |= (1 << bit)
        packed.append(byte)

    if ch1:
        for i in range(0, len(ch1), 8):
            byte = 0
            for bit in range(8):
                if i + bit < len(ch1) and ch1[i + bit]:
                    byte |= (1 << bit)
            packed.append(byte)

    with open(filename, "wb") as f:
        f.write(packed)

    print(f"[+] Saved binary: {filename} ({len(ch0)} samples)")


def save_hex(ch0, ch1, sample_rate, filename):
    """Save samples as hex text."""
    with open(filename, "w", newline="\n") as f:
        f.write("# Sample rate: %d Hz\n" % sample_rate)
        f.write("# Total samples: %d\n" % len(ch0))
        f.write("# Channel: GDO0\n")
        nibbles = []
        for i in range(0, len(ch0), 4):
            nib = 0
            for bit in range(4):
                if i + bit < len(ch0) and ch0[i + bit]:
                    nib |= (1 << bit)
            nibbles.append("%X" % nib)
        for i in range(0, len(nibbles), 32):
            f.write(" ".join(nibbles[i:i+32]) + "\n")

        if ch1:
            f.write("# Channel: GDO2\n")
            nibbles = []
            for i in range(0, len(ch1), 4):
                nib = 0
                for bit in range(4):
                    if i + bit < len(ch1) and ch1[i + bit]:
                        nib |= (1 << bit)
                nibbles.append("%X" % nib)
            for i in range(0, len(nibbles), 32):
                f.write(" ".join(nibbles[i:i+32]) + "\n")

    print(f"[+] Saved hex: {filename} ({len(ch0)} samples)")


def save_sr(ch0, ch1, sample_rate, filename):
    """Save as Sigrok .sr session file."""
    def pack_bits(samples):
        packed = bytearray()
        for i in range(0, len(samples), 8):
            byte = 0
            for bit in range(8):
                if i + bit < len(samples) and samples[i + bit]:
                    byte |= (1 << bit)
            packed.append(byte)
        return packed

    ch_count = 2 if ch1 else 1
    packed = pack_bits(ch0)

    metadata = (
        "[driver sigrok-session]\n"
        "sigrok-version=0.7.2\n"
        "\n"
        "[device 1]\n"
        "driver=virtual\n"
        "connection-type=parallel\n"
        "channels=%d\n"
        "channel_1=GDO0\n"
    ) % ch_count

    if ch1:
        metadata += "channel_2=GDO2\n"

    metadata += (
        "total_samples=%d\n"
        "samplerate=%d\n"
        "unitsize=1\n"
    ) % (len(ch0), sample_rate)

    with zipfile.ZipFile(filename, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("metadata", metadata)
        zf.writestr("logic-1-%d.sr" % ch_count, bytes(packed))
        if ch1:
            zf.writestr("logic-1-2.sr", bytes(pack_bits(ch1)))

    print(f"[+] Saved Sigrok: {filename} ({len(ch0)} samples, {ch_count}ch, {sample_rate} Hz)")


def print_stats(ch0, ch1, sample_rate):
    """Print capture statistics."""
    if not ch0:
        return
    total = len(ch0)
    ones = sum(ch0)
    zeros = total - ones
    transitions = sum(1 for i in range(1, total) if ch0[i] != ch0[i-1])
    duration = total / sample_rate

    print(f"\n[*] Capture statistics:")
    print(f"    Samples:     {total}")
    print(f"    Duration:    {duration*1000:.2f} ms")
    print(f"    Sample rate: {sample_rate} Hz")
    print(f"    Ch0 (GDO0):  high={ones} ({100*ones/total:.1f}%) low={zeros} ({100*zeros/total:.1f}%)")
    print(f"    Transitions: {transitions}")
    if transitions > 0:
        avg_bit_period = duration / transitions * 1e6
        print(f"    Avg period:  {avg_bit_period:.1f} us ({1e6/avg_bit_period:.0f} Hz)")

    if ch1:
        ones2 = sum(ch1)
        zeros2 = len(ch1) - ones2
        trans2 = sum(1 for i in range(1, len(ch1)) if ch1[i] != ch1[i-1])
        print(f"    Ch1 (GDO2):  high={ones2} ({100*ones2/len(ch1):.1f}%) low={zeros2} ({100*zeros2/len(ch1):.1f}%)")
        print(f"    Ch1 trans:   {trans2}")


def main():
    parser = argparse.ArgumentParser(description="CC1101 SUMP 2-Channel Logic Analyzer Capture")
    parser.add_argument("--port", "-p", default="COM6",
                        help="Serial port (default: COM6 for USB mode, COM7 for UART mode)")
    parser.add_argument("--baud", "-b", type=int, default=921600,
                        help="Baud rate (default: 921600, must match firmware)")
    parser.add_argument("--rate", "-r", type=int, default=24000,
                        help="Sample rate in Hz (default: 24000)")
    parser.add_argument("--samples", "-n", type=int, default=100000,
                        help="Number of samples (default: 100000)")
    parser.add_argument("--channels", "-c", type=int, default=2, choices=[1, 2],
                        help="Number of channels (default: 2)")
    parser.add_argument("--clock", type=int, default=240000000,
                        help="Reference clock in Hz (default: 240000000)")
    parser.add_argument("--output", "-o", default=None,
                        help="Output file (.vcd, .bin, .hex, .sr)")
    parser.add_argument("--format", "-f", choices=["vcd", "bin", "hex", "sr"],
                        default="sr", help="Output format (default: sr)")
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"[!] Cannot open {args.port}: {e}")
        sys.exit(1)

    print(f"[*] CC1101 SUMP Capture — {args.port} @ {args.baud} baud, {args.channels}ch")

    ident = sump_find(ser)
    if not ident:
        print("[!] No SUMP device found. Is the firmware in SUMP mode?")
        ser.close()
        sys.exit(1)

    meta = sump_get_metadata(ser)
    if "num_channels" in meta:
        args.channels = meta["num_channels"]

    actual_samples = sump_configure(ser, args.rate, args.samples, args.clock)
    raw = sump_capture(ser, actual_samples, args.channels)

    if args.channels >= 2:
        ch0, ch1 = decode_ols_samples(raw, actual_samples, 2) if raw else (bytearray(), bytearray())
    else:
        ch0 = decode_ols_samples(raw, actual_samples, 1) if raw else bytearray()
        ch1 = None

    if ch0:
        print_stats(ch0, ch1, args.rate)

        if args.output:
            out = args.output
        else:
            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
            out = "capture_%s.%s" % (ts, args.format)

        if args.format == "vcd" or out.endswith(".vcd"):
            save_vcd(ch0, ch1, args.rate, out)
        elif args.format == "bin" or out.endswith(".bin"):
            save_bin(ch0, ch1, out)
        elif args.format == "hex" or out.endswith(".hex"):
            save_hex(ch0, ch1, args.rate, out)
        elif args.format == "sr" or out.endswith(".sr"):
            save_sr(ch0, ch1, args.rate, out)
    else:
        print("[!] No samples captured")

    ser.close()


if __name__ == "__main__":
    main()
