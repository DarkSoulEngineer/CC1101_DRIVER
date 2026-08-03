#!/usr/bin/env python3
"""Custom-protocol capture for RFuzz firmware (main.c).

Firmware protocol (USB Serial/JTAG = COM7):
  0x01 [rate:4LE][count:4LE]  -> capture `count` samples at `rate` Hz
  0x02                        -> TX trigger
  0x03 [rate:4LE]             -> start continuous stream
  0x04                        -> stop stream
After a capture command the firmware streams `count` raw bytes:
  each byte bit0 = GDO0, bit1 = GDO2 (1 byte per sample).

Outputs:
  .raw  raw capture bytes (bit0=GDO0, bit1=GDO2)
  .sr   Sigrok session with 4 logic probes + 1 analog for PulseView:
          probe1 GDO0          (logic, sync / TX active)
          probe2 GDO2          (logic, raw async demodulated data)
          probe3 GDO2_CLEAN_RAW (logic, error-corrected UART frame of the
                                 received sync+payload bits with ideal 0xAA
                                 preamble -> decodes at 2400 baud)
          probe4 GDO2_CLEAN    (logic, ideal UART frame of the decoded packet)
          analog5 FSK          (analog, synthesized modulation sinusoid)
  .vcd  VCD with GDO0, GDO2, GDO2_CLEAN_RAW, GDO2_CLEAN and a real-valued
        FSK channel

Channel 3 (FSK): the CC1101 only exposes digital GDO0/GDO2, so the analog
trace is a sinusoid synthesized from the glitch-cleaned GDO2 bits; the
modulation is selected with --mod:

  2fsk  phase-continuous 2FSK at two positive real tones IF +/- dev
        (mark = IF+dev for bit 1, space = IF-dev for bit 0). Rendering only
        the I component at +/-deviation around DC would degenerate to PSK
        (cos is even, so a negative frequency is indistinguishable from a
        positive one); two distinct positive tones make the carrier visibly
        jump between two frequencies, as true 2FSK.
  2psk  constant carrier at IF with 0/pi phase states (BPSK).
  ask   carrier at IF, full amplitude for bit 1, silent for bit 0 (OOK).

By default the synth reproduces the source signal's deviation (--fsk-dev,
default 50000 Hz, matching gen_2fsk.py / rfuzz_tools DEFAULT_DEV): the two
tones sit at IF +- dev, with IF auto-chosen so both fit the capture band
(IF = 22.5% of the real rate -> 56250 Hz at the default 250000 Hz capture,
tones 6250 Hz / 106250 Hz, the true +-50 kHz deviation). If the requested
deviation cannot fit the band (dev >= 42.5% of Nyquist) it falls back to a
visualization scale (IF = rate/10, dev = IF/2).

Note on sample rate: the firmware timer alarm is `1000000/rate` (integer
division), so requesting 250000 Hz actually runs at 250000 Hz (alarm = 4 us).
Do not request above 250 kHz on the 160 MHz ESP32-S3 build: alarm < 4 us
makes the capture ISR overrun and trip the interrupt watchdog
("Interrupt wdt timeout on CPU0" -> reboot). The .sr samplerate and the VCD
timescale always use the real rate.

Usage:
    python capture_custom.py                        # COM7, 250kHz, 262k samples
    python capture_custom.py --rate 24000 --samples 100000 --out cap
    python capture_custom.py --mod 2psk             # BPSK analog channel
    python capture_custom.py --fsk-if 100000 --fsk-dev 50000
    python capture_custom.py --dump                 # hex dump raw bytes
"""
import argparse
import os
import struct
import sys
import time
import zipfile
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import serial
    import numpy as np
except ImportError:
    print('pyserial + numpy not installed. Run: pip install pyserial numpy')
    sys.exit(1)

import rfuzz_tools

CMD_CAPTURE = 1
CMD_TX = 2
CMD_STREAM = 3
CMD_STOP = 4
DEFAULT_AMP = 90
DEFAULT_GLITCH = 10
DEFAULT_DEV = 50000.0
MODS = ('2fsk', '2psk', 'ask')
SYNC_BITS = np.array([int(b) for b in '1101111010101111'], dtype=np.uint8)


def actual_rate(requested):
    alarm = 1000000 // requested
    return 1000000.0 / alarm


def synth_defaults(r_actual, dev_hz):
    nyq = r_actual / 2.0
    if dev_hz > 0 and dev_hz < nyq * 0.425:
        return round(r_actual * 0.225), dev_hz
    if_hz = round(r_actual / 10.0)
    return if_hz, round(if_hz / 2.0)


def open_port(port, baud):
    ser = serial.Serial(port, baud, timeout=1)
    time.sleep(0.5)
    ser.reset_input_buffer()
    return ser


def drain(ser, seconds=1.0):
    t0 = time.time()
    n = 0
    while time.time() - t0 < seconds:
        n += len(ser.read(ser.in_waiting if ser.in_waiting else 1))
        time.sleep(0.02)
    print(f'[*] drained {n} bytes')
    return n


def capture(ser, rate, count, timeout=30.0):
    ser.reset_input_buffer()
    ser.write(bytes([CMD_CAPTURE]))
    ser.write(struct.pack('<II', rate, count))
    print(f'[*] sent capture cmd rate={rate} count={count}')
    raw = bytearray()
    t0 = time.time()
    while len(raw) < count:
        if ser.in_waiting:
            raw.extend(ser.read(ser.in_waiting))
        elif time.time() - t0 > timeout:
            print(f'[!] timeout got {len(raw)}/{count}')
            break
        else:
            time.sleep(0.002)
    raw = bytes(raw[:count])
    print(f'[+] got {len(raw)} bytes')
    return raw


def decode(raw):
    ch0 = bytearray(len(raw))
    ch1 = bytearray(len(raw))
    for i, b in enumerate(raw):
        ch0[i] = b & 1
        ch1[i] = (b >> 1) & 1
    return ch0, ch1


def stats(ch0, ch1, rate, amp, mod, if_dev, dev_hz):
    total = len(ch0)
    ones = sum(ch0)
    trans = sum(ch0[i] != ch0[i - 1] for i in range(1, total))
    dur = total / rate
    print(f'[*] samples={total} dur={dur * 1000:.1f}ms rate={rate}')
    if trans:
        print(f'[*] GDO0: high={ones} ({100 * ones / total:.1f}%) '
              f'trans={trans} period={dur / trans * 1000000.0:.1f}us')
    else:
        print('GDO0 no transitions')
    ones1 = sum(ch1)
    trans1 = sum(ch1[i] != ch1[i - 1] for i in range(1, total))
    if trans1:
        print(f'[*] GDO2: high={ones1} ({100 * ones1 / total:.1f}%) '
              f'trans={trans1} period={dur / trans1 * 1000000.0:.1f}us')
    else:
        print('GDO2 no transitions')
    if mod == '2fsk':
        print(f'[*] FSK (ch3 analog): 2FSK tones {if_dev + dev_hz:.0f}/'
              f'{if_dev - dev_hz:.0f} Hz (IF {if_dev:.0f} +- {dev_hz:.0f}), '
              f'amp {amp}')
    elif mod == '2psk':
        print(f'[*] FSK (ch3 analog): 2PSK carrier {if_dev:.0f} Hz, '
              f'phase 0/pi, amp {amp}')
    else:
        print(f'[*] FSK (ch3 analog): ASK/OOK carrier {if_dev:.0f} Hz, '
              f'amp {amp}')
    return dur


def mod_synth(ch1, amp, mod, if_hz, dev_hz, rate_actual, glitch=DEFAULT_GLITCH):
    spb = rate_actual / 2400.0
    cleaned = rfuzz_tools.remove_glitches(np.asarray(ch1, dtype=np.uint8), glitch)
    runs = rfuzz_tools.rl_encode(cleaned)
    bits = rfuzz_tools.runs_to_bits(runs, spb)

    n_cells = len(bits)
    base = len(cleaned) // n_cells
    extra = len(cleaned) % n_cells
    cell_lens = [base + (1 if i < extra else 0) for i in range(n_cells)]

    fsk = np.empty(len(cleaned), dtype=np.float32)
    phase = 0.0
    k = 0
    for b, n in zip(bits, cell_lens):
        if mod == '2fsk':
            f = if_hz + dev_hz if b else if_hz - dev_hz
            dphi = 2.0 * np.pi * f / rate_actual
            for _ in range(n):
                fsk[k] = amp * np.cos(phase)
                phase += dphi
                k += 1
        elif mod == '2psk':
            for _ in range(n):
                fsk[k] = amp * np.cos(phase + (0.0 if b else np.pi))
                k += 1
            phase += 2.0 * np.pi * if_hz / rate_actual * n
        else:
            val = amp if b else 0.0
            for _ in range(n):
                fsk[k] = val
                k += 1
    return fsk


def mod_synth_from_bits(bits, amp, mod, if_hz, dev_hz, rate_actual, target_len,
                        spb, offset=0):
    fsk = np.zeros(target_len, dtype=np.float32)
    if len(bits) == 0:
        return fsk
    phase = 0.0
    k = offset
    n = int(spb)
    for b in bits:
        if mod == '2fsk':
            f = if_hz + dev_hz if b else if_hz - dev_hz
            dphi = 2.0 * np.pi * f / rate_actual
            for _ in range(n):
                if k < target_len:
                    fsk[k] = amp * np.cos(phase)
                phase += dphi
                k += 1
        elif mod == '2psk':
            for _ in range(n):
                if k < target_len:
                    fsk[k] = amp * np.cos(phase + (0.0 if b else np.pi))
                k += 1
            phase += 2.0 * np.pi * if_hz / rate_actual * n
        else:
            val = amp if b else 0.0
            for _ in range(n):
                if k < target_len:
                    fsk[k] = val
                k += 1
    return fsk


def save_vcd(ch0, ch1, ch1cr, ch1c, fsk, rate, fn):
    tps = 1000000000.0 / rate
    with open(fn, 'w', newline='\n') as f:
        f.write('$timescale 1ns $end\n')
        f.write('$version RFuzz capture $end\n')
        f.write('$scope module CC1101 $end\n')
        f.write('$var wire 1 ! GDO0 $end\n')
        f.write('$var wire 1 @ GDO2 $end\n')
        f.write('$var wire 1 # GDO2_CLEAN_RAW $end\n')
        f.write('$var wire 1 $ GDO2_CLEAN $end\n')
        f.write('$var real 1 % FSK $end\n')
        f.write('$upscope $end\n$enddefinitions $end\n')
        f.write('$dumpvars\nx!\nx@\nx#\nx$\n0.0%\n$end\n')
        p0 = None
        p1 = None
        pr = None
        pc = None
        pf = None
        for i in range(len(ch0)):
            if (ch0[i] != p0 or ch1[i] != p1 or ch1cr[i] != pr
                    or ch1c[i] != pc or fsk[i] != pf):
                f.write('#%d\n' % int(i * tps))
                if ch0[i] != p0:
                    f.write(f'{ch0[i]}!\n')
                if ch1[i] != p1:
                    f.write(f'{ch1[i]}@\n')
                if ch1cr[i] != pr:
                    f.write(f'{ch1cr[i]}#\n')
                if ch1c[i] != pc:
                    f.write(f'{ch1c[i]}$\n')
                if fsk[i] != pf:
                    f.write(f'{fsk[i]:.2f}%\n')
                pf, pc, pr, p1, p0 = fsk[i], ch1c[i], ch1cr[i], ch1[i], ch0[i]
    print(f'[+] {fn}')


def save_sr(ch0, ch1, ch1cr, ch1c, fsk, rate, fn):
    packed = bytearray(len(ch0))
    for i in range(len(ch0)):
        b = 0
        if ch0[i]:
            b |= 1
        if ch1[i]:
            b |= 2
        if ch1cr[i]:
            b |= 4
        if ch1c[i]:
            b |= 8
        packed[i] = b
    meta = ('[global]\nsigrok version = 2\n\n'
            '[device 1]\ncapturefile = logic-1\nunitsize = 1\n'
            'total probes = 4\nsamplerate = %d\ntotal analog = 1\n'
            'probe1 = GDO0\nprobe2 = GDO2\nprobe3 = GDO2_CLEAN_RAW\n'
            'probe4 = GDO2_CLEAN\nanalog5 = FSK\n' % rate)
    with zipfile.ZipFile(fn, 'w', zipfile.ZIP_DEFLATED) as zf:
        zf.writestr('version', '2')
        zf.writestr('metadata', meta)
        zf.writestr('logic-1-1', bytes(packed))
        zf.writestr('analog-1-5', fsk.astype('<f4').tobytes())
    print(f'[+] {fn} (GDO0, GDO2, GDO2_CLEAN_RAW[UART], GDO2_CLEAN[UART] logic | FSK analog)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', '-p', default='COM7')
    ap.add_argument('--baud', '-b', type=int, default=115200)
    ap.add_argument('--rate', '-r', type=int, default=250000)
    ap.add_argument('--samples', '-n', type=int, default=262144)
    ap.add_argument('--out', '-o', default=None)
    ap.add_argument('--analog-amp', type=int, default=DEFAULT_AMP,
                    help='FSK analog channel amplitude (default 90)')
    ap.add_argument('--mod', default='2fsk', choices=MODS,
                    help='basic modulation rendered on channel 3 '
                         '(default 2fsk; choices: ' + ', '.join(MODS) + ')')
    ap.add_argument('--fsk-if', type=float, default=0.0,
                    help='FSK center IF in Hz for the synthesized sinusoid '
                         '(default: 22.5%% of the real rate when the deviation '
                         'fits the band, else real rate / 10)')
    ap.add_argument('--fsk-dev', type=float, default=DEFAULT_DEV,
                    help="FSK deviation from IF in Hz (default 50000, the "
                         "source signal's deviation)")
    ap.add_argument('--preamble', type=int,
                    default=rfuzz_tools.DEFAULT_PREAMBLE_BYTES,
                    help='preamble bytes in the TX signal (default 4)')
    ap.add_argument('--dump', action='store_true', help='hex dump raw bytes')
    args = ap.parse_args()

    ser = open_port(args.port, args.baud)
    drain(ser)
    raw = capture(ser, args.rate, args.samples)
    ser.close()

    if args.dump:
        for i in range(0, len(raw), 32):
            print(' '.join(f'{b:02X}' for b in raw[i:i + 32]))

    ch0, ch1 = decode(raw)
    r_actual = actual_rate(args.rate)
    if_dev, dev_hz = synth_defaults(r_actual, args.fsk_dev)
    if args.fsk_if:
        if_dev = args.fsk_if

    ch0n = np.asarray(ch0, dtype=np.uint8)
    ch1n = np.asarray(ch1, dtype=np.uint8)
    seed = r_actual / 2400.0
    spb, bps, windows = rfuzz_tools.detect_datarate_iter(
        ch0n, ch1n, r_actual, seed, args.preamble)
    print(f"[*] gated windows={len(windows)} "
          f"datarate={bps:.0f} bps (spb={spb:.3f})")
    bits = rfuzz_tools.clean_bits(ch1, spb, DEFAULT_GLITCH)
    packets = rfuzz_tools.find_packets(bits, spb, args.preamble)

    # Render each decoded packet as the full preamble + sync + payload at its
    # actual sample offset so the analog channel matches the real burst and
    # stays time-aligned with GDO2 (idle before/after stays silent).  Both the
    # FSK and the clean GDO2 channel are anchored on the same run-derived
    # start (the true first-bit edge): the GDO0 sync-strobe start is ~1 bit
    # early because the modem bit-sync compresses the leading bits, which
    # would misalign the analog render against the real data.
    starts = rfuzz_tools.packet_starts(ch0n, spb, packets, args.preamble)
    packets, gstarts = rfuzz_tools.refine_packets(ch1n, spb, packets, starts,
                                                  args.preamble)
    fsk = np.zeros(len(ch1), dtype=np.float32)
    for p, start in zip(packets, gstarts):
        pbits = np.concatenate([
            rfuzz_tools.preamble_bits(args.preamble),
            SYNC_BITS,
            rfuzz_tools.bits_msb(p['payload']),
        ])
        fsk += mod_synth_from_bits(pbits, args.analog_amp, args.mod, if_dev,
                                   dev_hz, r_actual, len(ch1), spb, start)

    ch1cr = rfuzz_tools.clean_gdo2(packets, gstarts, spb,
                                   args.preamble, len(ch1), framed=True,
                                   corrected=True)
    ch1c = rfuzz_tools.clean_gdo2(packets, gstarts, spb,
                                  args.preamble, len(ch1), framed=True)

    stats(ch0, ch1, r_actual, args.analog_amp, args.mod, if_dev, dev_hz)

    ts = datetime.now().strftime('%Y%m%d_%H%M%S')
    base = args.out if args.out else f'capture_{ts}'
    with open(base + '.raw', 'wb') as f:
        f.write(raw)
    save_sr(ch0, ch1, ch1cr, ch1c, fsk, int(round(r_actual)), base + '.sr')
    save_vcd(ch0, ch1, ch1cr, ch1c, fsk, r_actual, base + '.vcd')


if __name__ == '__main__':
    main()
