# RFuzz TX Debug Progress

## Current state
- 2FSK modulation, 2400 bps, dev ~24 kHz
- Fixed length mode, 1 byte payload (0x01)
- Sync word DE AF, 4-byte preamble AA AA AA AA
- PATABLE[0] = 0xC5 (+10 dBm)
- FREND0 = 0x10 (default, uses PATABLE[0])
- SPI at 1 MHz, manual CS with t_CSS/t_CSH delays
- Burst writes fixed (single contiguous SPI transaction)

## What was fixed
1. **SPI burst writes**: Two-transaction -> single transaction (was corrupting CC1101 SPI state machine)
2. **SPI burst reads**: Same single-transaction fix
3. **FREND0 for OOK**: Added0x11 to use PATABLE[7] (now moot, switched to 2FSK)
4. **PATABLE for OOK**: PATABLE[0]=0x00, PATABLE[7]=0xC5 (now moot, switched to 2FSK)
5. **Packet mode**: VARIABLE_LENGTH -> FIXED_LENGTH (was causing TX FIFO underflow with empty payload)
6. **cc1101_transmit**: Removed auto length-prefix (raw FIFO writes only)
7. **CS timing**: Added1us delays for t_CSS/t_CSH
8. **SPI speed**: 8 MHz -> 1 MHz
9. **cc1101_verify_config()**: New function that reads back all written registers and compares against expected values

## Verification output format
```
==== REGISTER VERIFICATION ====
  IOCFG2  = 0x2E  == 0x2E  [OK]
  IOCFG0  = 0x06  == 0x06  [OK]
  SYNC1   = 0xDE  == 0xDE  [OK]
  ...
  RESULT: 16 pass, 0 fail
```

## Hardware
- ESP32-D0WD-V3 rev3.1
- CC1101 on SPI3_HOST (MOSI=23, MISO=19, CLK=18, CS=5, GDO0=25)
- HackRF One for RX (URH)
