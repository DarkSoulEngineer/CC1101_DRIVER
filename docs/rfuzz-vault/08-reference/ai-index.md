# AI-Readable Project Index

> Machine-readable summary of the entire RFuzz project for AI agents and automated tools.

---

## Project Metadata

```yaml
project:
  name: "RFuzz"
  description: "CC1101 Sub-GHz RF Analysis Framework for ESP32-S3"
  version: "1.0.0"
  target_hardware: "ESP32-S3 DevKitC-1 + CC1101 Module"
  radio_bands: ["433 MHz", "868 MHz", "915 MHz"]
  modulations: ["2FSK", "GFSK", "ASK/OOK", "4FSK", "MSK"]
  max_baud: 600000
  capture_channels: 2
  capture_max_rate: 500000
  repository: "CC1101_DRIVER"
  license: "MIT"
```

---

## Firmware Components

```yaml
firmware:
  components:
    - name: "hw_init"
      type: "BSP"
      path: "components/hw_init"
      provides: ["spi_handle", "usb_driver", "gpio_config"]
      files: ["src/hw_init.c", "src/spi_init.c", "src/usb_interface.c"]
      kconfig_prefix: "CONFIG_CC1101_PIN_"
    
    - name: "cc1101"
      type: "driver"
      path: "components/cc1101"
      api_layers: ["low", "mid", "high", "app"]
      handle_based: true
      thread_safe: true
      features: ["burst_spi", "gdo0_isr", "async_rx", "freq_sweep", "status_monitor"]
      files: ["src/cc1101.c", "include/cc1101.h"]
      kconfig_prefix: "CONFIG_CC1101_"
    
    - name: "sump_capture"
      type: "logic_analyzer"
      path: "components/sump_capture"
      max_rate_hz: 500000
      channels: [1, 2]
      sample_format: "bit0=GDO0, bit1=GDO2"
      transports: ["USB_Serial_JTAG", "UART0"]
      protocol: "SUMP/OLS subset"
      files: ["src/sump_capture.c", "include/sump_capture.h"]
      kconfig_prefix: "CONFIG_SUMP_"
  
  applications:
    - name: "main (async RX capture)"
      source: "main/main.c"
      config:
        freq_hz: 433920000
        modulation: "2FSK"
        sync_mode: "NONE"
        datarate_bps: 2400
        deviation_reg: 0x27
        chanbw_reg: 0x0C
        packet_mode: "INFINITE"
        crc: false
        gdo0_mode: 0x0D
        gdo2_mode: 0x0D
      commands: [0x01, 0x02, 0x03, 0x04, 0x05]
      output: "Raw bit stream via SUMP"
    
    - name: "RFuzz_TX (GFSK beacon)"
      source: "main/RFuzz_TX.c"
      config:
        freq_hz: 433920000
        modulation: "GFSK"
        datarate_bps: 38400
        sync_mode: "30/32"
        sync_word: "0x2DD4..."
        preamble_bytes: 2
        packet_mode: "VARIABLE"
        crc: true
        payload: "DE AD BE EF"
        tx_power: "0 dBm"
        interval_ms: 3000
    
    - name: "RFuzz_RX (packet RX)"
      source: "main/RFuzz_RX.c"
      config:
        freq_hz: 433920000
        modulation: "2FSK"
        datarate_bps: 2400
        sync_mode: "16/16"
        sync_word: "0xDEAF"
        preamble_bytes: 4
        packet_mode: "VARIABLE"
        crc: false
        append_status: true
      matches_tx: "rfuzz_tx_2fsk.c"
    
    - name: "rfuzz_tx_2fsk (2FSK beacon)"
      source: "examples/rfuzz_tx_2fsk.c"
      config:
        freq_hz: 433920000
        modulation: "2FSK"
        datarate_bps: 2400
        sync_mode: "16/16"
        sync_word: "0xDEAF"
        preamble_bytes: 4
        packet_mode: "FIXED"
        crc: false
        payload: "01"
        tx_power: "+10 dBm"
        interval_ms: 10000
```

---

## Host Scripts

```yaml
host_scripts:
  - name: "capture.py"
    protocol: "custom_raw"
    commands: [0x01, 0x03, 0x04]
    outputs: [".sr"]
    features: ["triggered_capture", "continuous_stream", "auto_timeout"]
    default_port: "COM7"
    default_rate: 24000
    default_samples: 100000
  
  - name: "capture_sump.py"
    protocol: "SUMP/OLS"
    commands: [0x00, 0x01, 0x02, 0x03, 0x80, 0x81, 0x82, 0xC0, 0xC1, 0xC2]
    outputs: [".sr", ".vcd", ".bin", ".hex"]
    features: ["metadata", "trigger_config", "selftest", "multi_format", "stats"]
    default_port: "COM7"
    default_baud: 115200
    default_rate: 24000
    default_samples: 100000
    max_samples_per_capture: 262144
  
  - name: "sump_stream_capture.py"
    protocol: "custom_stream"
    commands: [0x03, 0x04]
    outputs: ["console"]
    features: ["live_binary_print"]
  
  - name: "tx_verify.py"
    protocol: "dual_port"
    ports: ["COM7(cmd)", "COM6(console)"]
    commands: [0x02]
    features: ["tx_trigger", "console_capture"]
  
  - name: "rssi_mon.py"
    protocol: "uart_console"
    port: "COM6"
    outputs: ["text_log"]
    features: ["timestamped_rssi"]
  
  - name: "capture_gdo.py"
    protocol: "custom_stream"
    commands: [0x03, 0x04]
    outputs: ["console", "transition_count"]
    features: ["quick_test", "binary_print"]
```

---

## Hardware Configuration

```yaml
hardware:
  target: "ESP32-S3 DevKitC-1"
  radio: "CC1101 (433/868/915 MHz module)"
  pinout:
    spi_bus: "SPI3_HOST"
    spi_mode: 0
    spi_speed_hz: 1000000
    cs: 5
    sck: 15
    mosi: 7
    miso: 6
    gdo0: 3
    gdo2: 4
  gdo_modes:
    gdo0:
      sync_word: 0x06
      async_data: 0x0D
      tx_fifo_thresh: 0x02
      high_z: 0x2E
    gdo2:
      sync_word: 0x06
      crc_ok: 0x07
      pll_lock: 0x08
      rssi_valid: 0x0B
      async_data: 0x0D
      high_z: 0x2E
  voltage: "3.3V"
  level_shifter_required: false
  antenna: "SMA / u.FL / PCB trace"
  usb_conflict: "GDO0/GDO2 (GPIO 3/4) share with USB Serial/JTAG"
```

---

## Dragon OS / HackRF Integration

```yaml
dragon_os:
  host: "192.168.1.101"
  user: "dragon"
  default_password: "dragon"
  ssh_key_recommended: true
  os: "Dragon OS Focal (Ubuntu-based)"
  hardware: "HackRF One (PortaPack H2/H4)"
  tools:
    - hackrf_transfer
    - hackrf_sweep
    - hackrf_info
    - hackrf_debug
    - inspectrum
    - URH
    - gnuradio-companion
  common_operations:
    rx_capture: "hackrf_transfer -r file.c8 -f FREQ -s RATE -n SAMPLES"
    tx_replay: "hackrf_transfer -t file.c8 -f FREQ -s RATE -x GAIN"
    sweep: "hackrf_sweep -f START:STOP -w BIN_WIDTH"
    sweep_single: "hackrf_sweep -f START:STOP -w BIN_WIDTH -1"
  coordinated_workflows:
    - "ESP32_TX_HackRF_RX: Validate ESP32 transmission"
    - "HackRF_TX_ESP32_RX: Validate ESP32 reception"
    - "Simultaneous_Spectrum_Narrowband: Wideband + narrowband"
    - "Signal_Replay: Capture → analyze → replay → verify"
```

---

## Build System

```yaml
build:
  framework: "ESP-IDF v6.0.2"
  target: "esp32s3"
  build_system: "CMake"
  config_system: "Kconfig (menuconfig)"
  partitions: "partitions_singleapp.csv (16MB flash)"
  presets:
    - name: "async_rx"
      file: "sdkconfig.async_rx"
      description: "Default main.c async capture"
    - name: "tx_2fsk"
      file: "sdkconfig.tx_2fsk"
      description: "2FSK beacon (rfuzz_tx_2fsk.c)"
    - name: "rx_packet"
      file: "sdkconfig.rx_packet"
      description: "Packet RX (RFuzz_RX.c)"
  key_kconfig_sections:
    - "CC1101 Radio Configuration"
    - "CC1101 Hardware (Board Support)"
    - "SUMP Capture"
    - "USB Interface"
    - "ESP Console Selection"
```

---

## Protocols

```yaml
protocols:
  custom_raw:
    name: "Custom Raw Streaming"
    host_to_device:
      - {cmd: 0x01, name: "START", args: "rate(u32le), count(u32le)"}
      - {cmd: 0x02, name: "TX_TRIGGER"}
      - {cmd: 0x03, name: "STREAM", args: "rate(u32le)"}
      - {cmd: 0x04, name: "STOP"}
      - {cmd: 0x05, name: "SWEEP", args: "start,end,step(u32le)"}
    device_to_host:
      boot: "0xAA"
      capture: "raw bytes (1 byte/sample)"
      stream: "continuous raw bytes"
      sweep: "RSSI per step"
    sample_format: "bit0=GDO0, bit1=GDO2"
  
  sump_ols:
    name: "SUMP/OLS Protocol"
    host_to_device:
      - {cmd: 0x00, name: "RESET"}
      - {cmd: 0x01, name: "RUN"}
      - {cmd: 0x02, name: "ID"}
      - {cmd: 0x03, name: "SELFTEST", args: "pattern(u8)"}
      - {cmd: 0x80, name: "SET_DIV", args: "divider(u24le)"}
      - {cmd: 0x81, name: "SET_COUNT", args: "read_count(u16), delay(u16)"}
      - {cmd: 0x82, name: "SET_FLAGS", args: "flags(u8)"}
      - {cmd: 0xC0, name: "TRIG_MASK0", args: "mask(u8)"}
      - {cmd: 0xC1, name: "TRIG_VALUE0", args: "value(u8)"}
      - {cmd: 0xC2, name: "TRIG_CONFIG0", args: "config(u8)"}
      - {cmd: 0x04, name: "GET_METADATA"}
    device_to_host:
      id: ["1ALS", "SUMP"]
      metadata: "TLV (name, version, mem, max_rate, proto, channels)"
      capture: "4-byte blocks (ch0, ch1, ch2, ch3)"
    limits:
      max_samples: 262144
      max_rate: 500000
  
  console_uart:
    name: "ESP-IDF Console"
    port: "UART0 (COM6)"
    baud: 115200
    format: "TAG (timestamp) MESSAGE"
```

---

## Documentation Structure

```yaml
docs:
  vault: "docs/obsidian"
  structure:
    00-overview:
      - index.md
    01-hardware:
      - pinout.md
      - board-support.md
    02-firmware:
      - architecture.md
      - cc1101-driver.md
      - sump-capture.md
    03-applications:
      - main-capture.md
      - tx-beacon.md
      - rx-packet.md
      - examples.md
    04-host-scripts:
      - index.md
      - capture_sump.py.md
    05-dragon-os:
      - ssh-setup.md
      - hackrf-ops.md
      - coordinated.md
    06-build-config:
      - kconfig.md
      - menuconfig-guide.md
    07-workflow:
      - development.md
      - testing.md
      - debugging.md
    08-reference:
      - register-map.md
      - protocol.md
      - troubleshooting.md
    _templates: []
    _attachments: []
```

---

## Quick Commands Reference

```yaml
quick_commands:
  build_flash_monitor: "idf.py build flash monitor"
  menuconfig: "idf.py menuconfig"
  apply_preset: "cp sdkconfig.preset sdkconfig"
  capture_simple: "python scripts/capture.py --port COM7 --rate 24000 --samples 100000"
  capture_sump: "python scripts/capture_sump.py --port COM7 --rate 24000 --samples 100000 --format sr"
  capture_vcd: "python scripts/capture_sump.py --port COM7 --rate 100000 --samples 50000 --format vcd"
  tx_verify: "python scripts/tx_verify.py"
  rssi_monitor: "python scripts/rssi_mon.py 30 rssi_log.txt"
  dragon_ssh: "ssh dragon@192.168.1.101"
  hackrf_rx: "ssh dragon 'hackrf_transfer -r cap.c8 -f 433920000 -s 2000000 -n 4000000'"
  hackrf_tx: "ssh dragon 'hackrf_transfer -t cap.c8 -f 433920000 -s 2000000 -x 20'"
  hackrf_sweep: "ssh dragon 'hackrf_sweep -f 430:440 -w 100000 -1' > sweep.csv"
  scp_download: "scp dragon:file.c8 ."
  scp_upload: "scp file.c8 dragon:~/"
```

---

## File Locations for AI Navigation

```yaml
key_files:
  main_app: "main/main.c"
  tx_beacon: "main/RFuzz_TX.c"
  rx_packet: "main/RFuzz_RX.c"
  tx_2fsk_example: "examples/rfuzz_tx_2fsk.c"
  cc1101_driver: "components/cc1101/src/cc1101.c"
  cc1101_header: "components/cc1101/include/cc1101.h"
  sump_capture: "components/sump_capture/src/sump_capture.c"
  hw_init: "components/hw_init/src/hw_init.c"
  spi_init: "components/hw_init/src/spi_init.c"
  capture_py: "scripts/capture.py"
  capture_sump_py: "scripts/capture_sump.py"
  tx_verify_py: "scripts/tx_verify.py"
  rssi_mon_py: "scripts/rssi_mon.py"
  sdkconfig: "sdkconfig"
  kconfig_cc1101: "components/cc1101/Kconfig"
  kconfig_sump: "components/sump_capture/Kconfig"
  kconfig_hw: "components/hw_init/Kconfig"
```

---

## Related

- [[00-overview/index|Main Index]]
- All other docs in vault