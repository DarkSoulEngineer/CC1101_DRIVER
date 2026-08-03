# Board Support & Variants

> **Source**: `components/hw_init/Kconfig`, `components/hw_init/include/hw_init.h`

---

## Supported Configurations

### ESP32-S3 DevKitC-1 (Default)
All pins broken out, native USB Serial/JTAG available.

| Pin | Function | Notes |
|-----|----------|-------|
| GPIO 0 | Boot strapping | Don't use for CC1101 |
| GPIO 10 | GDO0 (default) | USB Serial/JTAG RX (shared) |
| GPIO 11 | GDO2 (default) | USB Serial/JTAG TX (shared) |
| GPIO 5 | CS (default) | SPI CS |
| GPIO 6 | MISO (default) | SPI MISO |
| GPIO 7 | MOSI (default) | SPI MOSI |
| GPIO 15 | SCK (default) | SPI SCK |
| GPIO 19 | USB D- | Fixed |
| GPIO 20 | USB D+ | Fixed |

⚠️ **USB Serial/JTAG conflict**: GDO0/GDO2 default pins (3/4) share with USB Serial/JTAG. If using USB transport for SUMP, consider moving GDO pins.

---

### Alternative Pin Assignments (via `menuconfig`)

```bash
idf.py menuconfig
# → CC1101 Hardware (Board Support) → CC1101 Pinout
```

| Kconfig | Default | Range | Description |
|---------|---------|-------|-------------|
| `CONFIG_CC1101_PIN_GDO0` | 10 | 0-48 | GDO0 interrupt/data |
| `CONFIG_CC1101_PIN_GDO2` | 11 | 0-48 | GDO2 data/clock |
| `CONFIG_CC1101_PIN_CS` | 5 | 0-48 | SPI chip select |
| `CONFIG_CC1101_PIN_SCK` | 15 | 0-48 | SPI clock |
| `CONFIG_CC1101_PIN_MOSI` | 7 | 0-48 | SPI MOSI |
| `CONFIG_CC1101_PIN_MISO` | 6 | 0-48 | SPI MISO |
| `CONFIG_CC1101_SPI_HZ` | 1000000 | 100k-8M | SPI clock speed |

---

### ESP32-S3 DevKitM-1 (Module Variant)
Same pinout as DevKitC-1. Module has integrated flash/PSRAM.

---

### Custom Board Template

Create `sdkconfig.defaults` for your board:

```ini
# Custom board: MyCC1101Board
CONFIG_CC1101_PIN_GDO0=10
CONFIG_CC1101_PIN_GDO2=11
CONFIG_CC1101_PIN_CS=12
CONFIG_CC1101_PIN_SCK=13
CONFIG_CC1101_PIN_MOSI=14
CONFIG_CC1101_PIN_MISO=9
CONFIG_CC1101_SPI_HZ=2000000
```

Apply:
```bash
cp sdkconfig.defaults sdkconfig
idf.py build
```

---

## USB Interface Configuration

> ⚠️ **Two USB interfaces must be connected.** The board exposes a CH343 UART bridge (console/flashing, **COM6**) and a native USB Serial/JTAG (SUMP data dump, **COM7**) as **two separate COM devices**. Both cables must be plugged in for full workflows (flash/program on COM6, capture on COM7). COM numbers are machine-dependent; on the reference workstation: **COM6 = UART0 (CH343)**, **COM7 = USB Serial/JTAG**. List them with:
> ```powershell
> Get-PnpDevice -Class Ports
> ```

### USB Serial/JTAG (Recommended for SUMP)
```ini
# In sdkconfig / menuconfig
CONFIG_ESP_CONSOLE_UART_DEFAULT=y          # Console on UART0 (COM6)
CONFIG_SUMP_TRANSPORT_USB=y                # SUMP on USB JTAG (COM7)
CONFIG_USB_INTERFACE_USJ_TX_BUFFER_SIZE=4096
CONFIG_USB_INTERFACE_USJ_RX_BUFFER_SIZE=256
```
**Advantages**: No DTR reset, stable PulseView connection, higher throughput.

### UART0 Transport (Legacy)
```ini
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y       # Console on USB JTAG
CONFIG_SUMP_TRANSPORT_UART=y               # SUMP on UART0
CONFIG_SUMP_UART_BAUD=921600               # Must match PulseView
CONFIG_SUMP_DTR_RESET_GUARD=y              # Drain boot log
CONFIG_SUMP_DTR_GUARD_MS=3000
```
**Issues**: PulseView toggles DTR → resets ESP32 → boot log corrupts SUMP handshake.

---

## CC1101 Module Variants

| Module | Frequency | PA | Notes |
|--------|-----------|----|-------|
| **CC1101-433** | 433 MHz | +10 dBm | Most common |
| **CC1101-868** | 868 MHz | +10 dBm | EU SRD |
| **CC1101-915** | 915 MHz | +10 dBm | US ISM |
| **CC1101-Multi** | 300-928 MHz | +12 dBm | Wideband, external matching |

All use same SPI/register interface. Frequency set via `FREQ2/1/0` registers.

---

## Antenna Considerations

| Type | Connector | Frequency Match |
|------|-----------|-----------------|
| **SMA** | Module edge | Requires pigtail |
| **u.FL** | Module edge | Small, needs adapter |
| **PCB Trace** | On-module | Fixed frequency |
| **Wire** | Solder pad | Quick prototype |

**Matching**: CC1101 has integrated PA/LNA. External matching network recommended for max range.

---

## Power Supply

| Rail | Voltage | Current | Notes |
|------|---------|---------|-------|
| **VCC** | 3.3V | ~30 mA RX, ~50 mA TX | From ESP32 3V3 or dedicated LDO |
| **GND** | — | — | Common ground essential |

⚠️ **Do not power CC1101 from 5V** — it's a 3.3V device. ESP32-S3 3V3 pin can supply ~500 mA.

---

## AI-Readable Board Config

```yaml
boards:
  - name: "ESP32-S3-DevKitC-1"
    default: true
    pins:
      gdo0: 3
      gdo2: 4
      cs: 5
      sck: 15
      mosi: 7
      miso: 6
    usb_conflict: true
    transport_recommended: "USB Serial/JTAG"
  
  - name: "ESP32-S3-DevKitM-1"
    pins: same_as_devkitc1
  
  - name: "Custom"
    pins: "configured via menuconfig"
    kconfig_prefix: "CONFIG_CC1101_PIN_"

transport_options:
  usb_serial_jtag:
    console_port: "UART0 (COM6)"
    sump_port: "USB JTAG (COM7)"
    dtr_reset: false
    recommended: true
  uart0:
    console_port: "USB JTAG"
    sump_port: "UART0 (COM6)"
    dtr_reset: true
    guard_ms: 3000
    recommended: false
```

---

## Related

- [[01-hardware/pinout|Pinout Details]]
- [[06-build-config/kconfig|Kconfig Reference]]
- [[06-build-config/menuconfig-guide|Menuconfig Guide]]
- [[02-firmware/sump-capture|SUMP Transport Selection]]
