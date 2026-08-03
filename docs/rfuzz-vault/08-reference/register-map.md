# CC1101 Register Map Quick Reference

> **Source**: `components/cc1101/include/cc1101.h`, CC1101 Datasheet

---

## Configuration Registers (0x00-0x2E)

| Addr | Name | Description | Key Bits |
|------|------|-------------|----------|
| 0x00 | IOCFG2 | GDO2 output config | [6:0] = mode |
| 0x01 | IOCFG1 | GDO1 output config | [6:0] = mode |
| 0x02 | IOCFG0 | GDO0 output config | [6:0] = mode |
| 0x03 | FIFOTHR | FIFO threshold | [6:4] = bytes, [3:0] = threshold |
| 0x04 | SYNC1 | Sync word MSB | 8 bits |
| 0x05 | SYNC0 | Sync word LSB | 8 bits |
| 0x06 | PKTLEN | Packet length | 8 bits (0=255 var) |
| 0x07 | PKTCTRL1 | Packet control 1 | [7]=CRC, [6]=whitening, [2:0]=addr check |
| 0x08 | PKTCTRL0 | Packet control 0 | [5:4]=format, [3:2]=len config, [0]=CRC |
| 0x09 | ADDR | Device address | 8 bits |
| 0x0A | CHANNR | Channel number | 8 bits |
| 0x0B | FSCTRL1 | Freq synth control 1 | [7:4]=freq offset |
| 0x0C | FSCTRL0 | Freq synth control 0 | [7:0]=freq offset |
| 0x0D | FREQ2 | Frequency MSB | 8 bits |
| 0x0E | FREQ1 | Frequency MID | 8 bits |
| 0x0F | FREQ0 | Frequency LSB | 8 bits |
| 0x10 | MDMCFG4 | Modem config 4 | [7:6]=chanbw_e, [5:4]=chanbw_m, [3:0]=drate_e |
| 0x11 | MDMCFG3 | Modem config 3 | [7:0]=drate_m |
| 0x12 | MDMCFG2 | Modem config 2 | [7]=dc_filter, [6:4]=mod, [3]=manch, [2:0]=sync |
| 0x13 | MDMCFG1 | Modem config 1 | [7]=fec, [6:4]=preamble, [3:0]=chanspc_e |
| 0x14 | MDMCFG0 | Modem config 0 | [7:0]=chanspc_m |
| 0x15 | DEVIATN | Modem deviation | [7:4]=deviation_e, [3:0]=deviation_m |
| 0x16 | MCSM2 | Main radio ctrl 2 | [7:4]=rx_time, [3:0]=rx_time_rssi |
| 0x17 | MCSM1 | Main radio ctrl 1 | [7:6]=cca_mode, [5:4]=rxoff, [3:0]=txoff |
| 0x18 | MCSM0 | Main radio ctrl 0 | [7:6]=autocal, [5:4]=po_timeout, [3:0]=fs_autocal |
| 0x19 | FOCCFG | Freq offset comp | [7:4]=foc_limit, [3:0]=foc_pre_k |
| 0x1A | BSCFG | Bit sync config | [7:4]=bs_limit, [3:0]=bs_pre_k |
| 0x1B | AGCCTRL2 | AGC control 2 | [7:6]=magn_target, [5:4]=max_lna, [3:0]=max_dvga |
| 0x1C | AGCCTRL1 | AGC control 1 | [7:6]=carrier_sense, [5:4]=abs_thr, [3:0]=rel_thr |
| 0x1D | AGCCTRL0 | AGC control 0 | [7:6]=hyst_level, [5:4]=wait_time, [3:0]=agc_freeze |
| 0x1E | WOREVT1 | Wake on radio event 1 | [7:0]=event1 |
| 0x1F | WOREVT0 | Wake on radio event 0 | [7:0]=event0 |
| 0x20 | WORCTRL | Wake on radio ctrl | [7:5]=worevt1, [4:0]=rc_cal |
| 0x21 | FREND1 | Front end Rx config | [7:4]=lodiv_buf, [3:0]=lna |
| 0x22 | FREND0 | Front end Tx config | [7:6]=pa_pow, [5:4]=reserved, [3:0]=lnapower |
| 0x23 | FSCAL3 | Freq synth cal 3 | [7:0]=cal3 |
| 0x24 | FSCAL2 | Freq synth cal 2 | [7:0]=cal2 |
| 0x25 | FSCAL1 | Freq synth cal 1 | [7:0]=cal1 |
| 0x26 | FSCAL0 | Freq synth cal 0 | [7:0]=cal0 |
| 0x27 | RCCTRL1 | RC oscillator ctrl 1 | [7:0]=rc_cal |
| 0x28 | RCCTRL0 | RC oscillator ctrl 0 | [7:0]=rc_cal |
| 0x29 | FSTEST | Freq synth test | Test only |
| 0x2A | PTEST | Production test | Test only |
| 0x2B | AGCTEST | AGC test | Test only |
| 0x2C | TEST2 | Various test 2 | Test only |
| 0x2D | TEST1 | Various test 1 | Test only |
| 0x2E | TEST0 | Various test 0 | Test only |

---

## Command Strobes (Write Only)

| Strobe | Name | Description |
|--------|------|-------------|
| 0x30 | SRES | Reset chip |
| 0x31 | SFSTXON | Enable/calibrate freq synth |
| 0x32 | SXOFF | Turn off crystal oscillator |
| 0x33 | SCAL | Calibrate freq synth |
| 0x34 | SRX | Enable RX |
| 0x35 | STX | Enable TX |
| 0x36 | SIDLE | Exit RX/TX, turn off freq synth |
| 0x37 | SAFC | AFC adjustment |
| 0x38 | SWOR | Start automatic RX polling |
| 0x39 | SPWD | Power down |
| 0x3A | SFRX | Flush RX FIFO |
| 0x3B | SFTX | Flush TX FIFO |
| 0x3C | SWORRST | Reset WOR time |
| 0x3D | SNOP | No operation |

---

## Status Registers (Read Only, Burst Access)

| Addr | Name | Description | Key Bits |
|------|------|-------------|----------|
| 0x30 | PARTNUM | Part number | 0x00 = CC1101 |
| 0x31 | VERSION | Version | 0x14 typical |
| 0x32 | FREQEST | Freq offset estimate | Signed |
| 0x33 | LQI | Link quality indicator | [7]=CRC_OK, [6:0]=LQI |
| 0x34 | RSSI | Received signal strength | Signed, dBm = RSSI/2 - 74 |
| 0x35 | MARCSTATE | Main radio state | [4:0]=state |
| 0x36 | WORTIME1 | WOR timer high | 8 bits |
| 0x37 | WORTIME0 | WOR timer low | 8 bits |
| 0x38 | PKTSTATUS | Packet status | [7]=CRC_OK, [6]=CS, [5:4]=PQT, [3:0]=length |
| 0x39 | VCO_VC_DAC | VCO voltage DAC | [7:0]=value |
| 0x3A | TXBYTES | TX FIFO status | [7]=underflow, [6:0]=bytes |
| 0x3B | RXBYTES | RX FIFO status | [7]=overflow, [6:0]=bytes |
| 0x3C | RCCTRL1_STATUS | RC oscillator status | - |
| 0x3D | RCCTRL0_STATUS | RC oscillator status | - |

---

## MARCSTATE Values

| Value | State | Description |
|-------|-------|-------------|
| 0x00 | SLEEP | Sleep |
| 0x01 | IDLE | Idle |
| 0x02 | XOFF | Crystal off |
| 0x03 | VCOON_MC | VCO on, calibration |
| 0x04 | REGON_MC | Regulator on, calibration |
| 0x05 | MANCAL | Manual calibration |
| 0x06 | VCOON | VCO on |
| 0x07 | REGON | Regulator on |
| 0x08 | STARTCAL | Start calibration |
| 0x09 | BWBOOST | Bandwidth boost |
| 0x0A | FS_LOCK | Freq synth locked |
| 0x0B | TX | Transmit |
| 0x0C | RX | Receive |
| 0x0D | RX | Receive (alt) |
| 0x0E | RXFIFO_OVERFLOW | RX overflow |
| 0x0F | FSTXON | Fast TX on |
| 0x10 | TXFIFO_UNDERFLOW | TX underflow |
| 0x11 | RXFIFO_UNDERFLOW | RX underflow |

---

## GDO Mode Values (IOCFG0/IOCFG2)

| Value | Mode | Description |
|-------|------|-------------|
| 0x00 | RX FIFO Threshold | Asserted when RX FIFO ≥ threshold |
| 0x01 | RX FIFO Overflow | Asserted on RX overflow |
| 0x02 | TX FIFO Threshold | Asserted when TX FIFO ≤ threshold |
| 0x03 | TX FIFO Underflow | Asserted on TX underflow |
| 0x04 | RX FIFO Not Empty | Asserted when RX FIFO has data |
| 0x05 | TX FIFO Not Full | Asserted when TX FIFO has space |
| 0x06 | Sync Word Detect | Asserted on sync word match |
| 0x07 | CRC OK | Asserted on valid CRC |
| 0x08 | Preamble Detect | Asserted on preamble quality |
| 0x09 | Clear Channel | Asserted when channel clear |
| 0x0A | Lock Detect | Asserted when PLL locked |
| 0x0B | RSSI Valid | Asserted when RSSI updated |
| 0x0C | Carrier Sense | Asserted when carrier detected |
| 0x0D | Async Serial Data | Raw demodulated data output |
| 0x0E | Serial Clock | Clock for async data |
| 0x0F | Serial Data | Data for async serial |
| 0x2E | High-Z | Disabled (input) |
| 0x2F | HW to 0 | Always low |

---

## Modulation Formats (MDMCFG2[6:4])

| Value | Format |
|-------|--------|
| 0 | 2-FSK |
| 1 | GFSK |
| 2 | Reserved |
| 3 | ASK/OOK |
| 4 | 4-FSK |
| 5 | Reserved |
| 6 | Reserved |
| 7 | MSK |

---

## Sync Modes (MDMCFG2[2:0])

| Value | Mode |
|-------|------|
| 0 | No sync |
| 1 | 15/16 bits |
| 2 | 16/16 bits |
| 3 | 30/32 bits |
| 4 | No sync (carrier) |
| 5 | 15/16 + carrier |
| 6 | 16/16 + carrier |
| 7 | 30/32 + carrier |

---

## Packet Formats (PKTCTRL0[5:4])

| Value | Format |
|-------|--------|
| 0 | Normal |
| 1 | Reserved |
| 2 | Random |
| 3 | Async serial |

---

## Length Configs (PKTCTRL0[3:2])

| Value | Config |
|-------|--------|
| 0 | Fixed |
| 1 | Variable |
| 2 | Infinite |
| 3 | Reserved |

---

## Autocal (MCSM0[7:6])

| Value | Mode |
|-------|------|
| 0 | Never |
| 1 | Idle → RX/TX |
| 2 | RX/TX → Idle |
| 3 | Always |

---

## PA Table Values (Common)

| Value | Power | Use Case |
|-------|-------|----------|
| 0x00 | -30 dBm | Minimal |
| 0x01 | -20 dBm | Low |
| 0x02 | -15 dBm | Low |
| 0x03 | -10 dBm | Low |
| 0x50 | 0 dBm | Medium |
| 0x84 | +5 dBm | High |
| 0x85 | +7 dBm | High |
| 0xC5 | +10 dBm | Max (default) |
| 0xC7 | +12 dBm | Max |

**OOK/ASK**: PATABLE[0]=0x00, PATABLE[7]=PA_VALUE
**FSK/GFSK/MSK**: PATABLE[0]=PA_VALUE

---

## Frequency Presets

| Band | FREQ2 | FREQ1 | FREQ0 | Frequency |
|------|-------|-------|-------|-----------|
| 433 MHz | 0x10 | 0xA7 | 0x62 | 433.92 MHz |
| 868 MHz | 0x21 | 0x65 | 0x6A | 868.30 MHz |
| 915 MHz | 0x23 | 0x31 | 0x3B | 915.00 MHz |

**Calculation**: `freq_reg = (freq_hz << 16) / 26000000`

---

## SPI Access

| Operation | Byte 0 | Byte 1 | Notes |
|-----------|--------|--------|-------|
| Write Single | `reg_addr` | `value` | - |
| Read Single | `reg_addr \| 0x80` | dummy | Returns value in byte 1 |
| Write Burst | `reg_addr \| 0x40` | `data...` | Auto-increments addr |
| Read Burst | `reg_addr \| 0xC0` | dummy... | Returns data in bytes 1+ |

---

## AI-Readable Register Index

```yaml
registers:
  config:
    - {addr: 0x00, name: "IOCFG2", bits: "GDO2 mode[6:0]"}
    - {addr: 0x01, name: "IOCFG1", bits: "GDO1 mode[6:0]"}
    - {addr: 0x02, name: "IOCFG0", bits: "GDO0 mode[6:0]"}
    - {addr: 0x03, name: "FIFOTHR", bits: "threshold[3:0], bytes[6:4]"}
    - {addr: 0x04, name: "SYNC1", bits: "sync word MSB"}
    - {addr: 0x05, name: "SYNC0", bits: "sync word LSB"}
    - {addr: 0x06, name: "PKTLEN", bits: "length (0=255 var)"}
    - {addr: 0x07, name: "PKTCTRL1", bits: "CRC[7], whitening[6], addr_chk[2:0]"}
    - {addr: 0x08, name: "PKTCTRL0", bits: "format[5:4], len_cfg[3:2], CRC[0]"}
    - {addr: 0x09, name: "ADDR", bits: "device address"}
    - {addr: 0x0A, name: "CHANNR", bits: "channel number"}
    - {addr: 0x0D, name: "FREQ2", bits: "freq MSB"}
    - {addr: 0x0E, name: "FREQ1", bits: "freq MID"}
    - {addr: 0x0F, name: "FREQ0", bits: "freq LSB"}
    - {addr: 0x10, name: "MDMCFG4", bits: "chanbw_e[7:6], chanbw_m[5:4], drate_e[3:0]"}
    - {addr: 0x11, name: "MDMCFG3", bits: "drate_m[7:0]"}
    - {addr: 0x12, name: "MDMCFG2", bits: "dc_filter[7], mod[6:4], manch[3], sync[2:0]"}
    - {addr: 0x13, name: "MDMCFG1", bits: "fec[7], preamble[6:4], chanspc_e[3:0]"}
    - {addr: 0x14, name: "MDMCFG0", bits: "chanspc_m[7:0]"}
    - {addr: 0x15, name: "DEVIATN", bits: "dev_e[7:4], dev_m[3:0]"}
    - {addr: 0x17, name: "MCSM1", bits: "cca[7:6], rxoff[5:4], txoff[3:0]"}
    - {addr: 0x18, name: "MCSM0", bits: "autocal[7:6]"}
    - {addr: 0x22, name: "FREND0", bits: "pa_pow[7:6]"}
  
  status:
    - {addr: 0x30, name: "PARTNUM", value: "0x00"}
    - {addr: 0x31, name: "VERSION", value: "0x14"}
    - {addr: 0x33, name: "LQI", bits: "CRC_OK[7], LQI[6:0]"}
    - {addr: 0x34, name: "RSSI", formula: "dBm = RSSI/2 - 74"}
    - {addr: 0x35, name: "MARCSTATE", bits: "state[4:0]"}
    - {addr: 0x38, name: "PKTSTATUS", bits: "CRC_OK[7], CS[6], PQT[5:4]"}
    - {addr: 0x3A, name: "TXBYTES", bits: "underflow[7], bytes[6:0]"}
    - {addr: 0x3B, name: "RXBYTES", bits: "overflow[7], bytes[6:0]"}
  
  strobes:
    - {cmd: 0x30, name: "SRES", desc: "Reset"}
    - {cmd: 0x33, name: "SCAL", desc: "Calibrate"}
    - {cmd: 0x34, name: "SRX", desc: "Enable RX"}
    - {cmd: 0x35, name: "STX", desc: "Enable TX"}
    - {cmd: 0x36, name: "SIDLE", desc: "Idle"}
    - {cmd: 0x3A, name: "SFRX", desc: "Flush RX FIFO"}
    - {cmd: 0x3B, name: "SFTX", desc: "Flush TX FIFO"}
  
  gdo_modes:
    - {val: 0x06, name: "SYNC_WORD", desc: "Sync detect"}
    - {val: 0x07, name: "CRC_OK", desc: "CRC valid"}
    - {val: 0x0B, name: "RSSI_VALID", desc: "RSSI updated"}
    - {val: 0x0D, name: "ASYNC_DATA", desc: "Raw demod bits"}
    - {val: 0x2E, name: "HIGH_Z", desc: "Disabled"}
  
  modulations:
    - {val: 0, name: "2FSK"}
    - {val: 1, name: "GFSK"}
    - {val: 3, name: "ASK/OOK"}
    - {val: 4, name: "4FSK"}
    - {val: 7, name: "MSK"}
  
  sync_modes:
    - {val: 0, name: "NONE"}
    - {val: 1, name: "15/16"}
    - {val: 2, name: "16/16"}
    - {val: 3, name: "30/32"}
    - {val: 5, name: "CARRIER_15/16"}
    - {val: 6, name: "CARRIER_16/16"}
    - {val: 7, name: "CARRIER_30/32"}
  
  freq_presets:
    - {band: "433", regs: [0x10, 0xA7, 0x62], freq: 433920000}
    - {band: "868", regs: [0x21, 0x65, 0x6A], freq: 868300000}
    - {band: "915", regs: [0x23, 0x31, 0x3B], freq: 915000000}
```

---

## Related

- [[08-reference/protocol|Host Protocol]]
- [[08-reference/troubleshooting|Troubleshooting]]
- [[02-firmware/cc1101-driver|CC1101 Driver API]]
- [[06-build-config/kconfig|Kconfig Reference]]