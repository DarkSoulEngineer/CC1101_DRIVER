# Example Projects

> **Source**: `examples/`

---

## Available Examples

| Example | Source | Description |
|---------|--------|-------------|
| **rfuzz_tx_2fsk** | `examples/rfuzz_tx_2fsk.c` | 2FSK beacon, fixed packet, easy URH decode |
| **usb_interface_test** | `examples/usb_interface_test/` | USB Serial/JTAG loopback test |

---

## rfuzz_tx_2fsk

**File**: `examples/rfuzz_tx_2fsk.c`

See [[03-applications/tx-beacon|TX Beacon Firmware]] for full documentation.

**Build**:
```bash
# As main application
cp examples/rfuzz_tx_2fsk.c main/main.c
idf.py build flash monitor

# Or as separate example (create CMakeLists.txt)
mkdir -p build_rfuzz_tx
cd build_rfuzz_tx
cmake ../examples/rfuzz_tx_2fsk
# ... or use idf.py with custom project
```

---

## usb_interface_test

**Directory**: `examples/usb_interface_test/`

**Purpose**: Test USB Serial/JTAG communication (echo test)

**Files**:
- `main/usb_interface_test.c` — Echoes received bytes
- `main/CMakeLists.txt`
- `CMakeLists.txt`
- `sdkconfig`, `sdkconfig.defaults`

**Source** (`usb_interface_test.c`):
```c
#include <stdio.h>
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "USB_TEST";

void app_main(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));

    ESP_LOGI(TAG, "USB Serial/JTAG echo test started");

    uint8_t buf[128];
    while (1) {
        int len = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len > 0) {
            usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "Echoed %d bytes", len);
        }
    }
}
```

**Build & Flash**:
```bash
cd examples/usb_interface_test
idf.py set-target esp32s3
idf.py build flash monitor
```

**Test**:
```bash
# Connect to USB Serial/JTAG port (typically COM7 on Windows)
# Send data - should be echoed back
python -c "import serial; s=serial.Serial('COM7', 115200); s.write(b'hello'); print(s.read(5))"
```

**Use Case**: Verify USB Serial/JTAG driver works before using SUMP capture over USB.

---

## Creating New Examples

### Template Structure
```
examples/my_example/
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   └── my_example.c
├── sdkconfig.defaults
└── README.md
```

### Example CMakeLists.txt (Root)
```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_example)
```

### Example CMakeLists.txt (Main)
```cmake
idf_component_register(SRCS "my_example.c"
                       INCLUDE_DIRS ""
                       REQUIRES hw_init cc1101 freertos log driver)
```

### Example sdkconfig.defaults
```ini
# CC1101 Pinout
CONFIG_CC1101_PIN_GDO0=3
CONFIG_CC1101_PIN_GDO2=4
CONFIG_CC1101_PIN_CS=5
CONFIG_CC1101_PIN_SCK=15
CONFIG_CC1101_PIN_MOSI=7
CONFIG_CC1101_PIN_MISO=6

# Radio Config
CONFIG_CC1101_FREQ_433=y
CONFIG_CC1101_FREQ_HZ=433920000
CONFIG_CC1101_MOD_2FSK=y
CONFIG_CC1101_DATARATE=2400
CONFIG_CC1101_DEVIATION=4
CONFIG_CC1101_CHANNEL_BW=12
CONFIG_CC1101_SYNC_MODE=2
CONFIG_CC1101_SYNC_WORD=57007
CONFIG_CC1101_PREAMBLE_BYTES=4
CONFIG_CC1101_PKT_FIXED=y
CONFIG_CC1101_CRC_ENABLE=n
CONFIG_CC1101_WHITENING=n
CONFIG_CC1101_APPEND_STATUS=n
CONFIG_CC1101_PA_10dBm=y
CONFIG_CC1101_ISR_ENABLE=n

# SUMP Capture (if used)
CONFIG_SUMP_TRANSPORT_USB=y
CONFIG_SUMP_MAX_SAMPLES=100000
CONFIG_SUMP_NUM_CHANNELS=2
CONFIG_SUMP_GDO2_MODE=13
CONFIG_SUMP_GDO2_PIN=4
```

### Build Example
```bash
cd examples/my_example
idf.py set-target esp32s3
idf.py build flash monitor
```

---

## AI-Readable Examples Index

```yaml
examples:
  - name: "rfuzz_tx_2fsk"
    path: "examples/rfuzz_tx_2fsk.c"
    type: "standalone_c"
    description: "2FSK beacon transmitter (2.4 kbps, fixed packet)"
    builds_as_main: true
    related_docs: ["03-applications/tx-beacon"]
  
  - name: "usb_interface_test"
    path: "examples/usb_interface_test/"
    type: "idf_project"
    description: "USB Serial/JTAG echo test"
    components: ["driver/usb_serial_jtag"]
    related_docs: ["01-hardware/board-support"]
```

---

## Related

- [[03-applications/tx-beacon|TX Beacon Firmware]]
- [[03-applications/rx-packet|RX Packet Firmware]]
- [[03-applications/main-capture|Async RX Capture]]
- [[01-hardware/board-support|Board Support]]
- [[06-build-config/menuconfig-guide|Menuconfig Guide]]