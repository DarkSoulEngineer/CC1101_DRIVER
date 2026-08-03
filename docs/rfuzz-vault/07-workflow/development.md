# Development Workflow

> Standard edit-build-flash-debug cycle for RFuzz firmware.

---

## Prerequisites

```bash
# ESP-IDF v5.0+ (tested with 6.0.2)
# Install: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/

# Python packages for host scripts
pip install pyserial

# Optional: Git hooks
pip install pre-commit
pre-commit install
```

---

## Daily Workflow

### 1. Pull Latest Changes
```bash
cd CC1101_DRIVER
git pull
git submodule update --init --recursive  # if any submodules
```

### 2. Configure (if needed)
```bash
idf.py menuconfig
# Or apply preset:
cp sdkconfig.async_rx sdkconfig
```

### 3. Build
```bash
idf.py build
```

**Incremental Build** (fast):
```bash
idf.py build          # Only changed files
```

**Clean Build**:
```bash
idf.py fullclean
idf.py build
```

### 4. Flash
```bash
# Auto-detect port
idf.py flash

# Specific port
idf.py -p COM6 flash

# Flash + monitor
idf.py flash monitor
```

### 5. Monitor
```bash
idf.py monitor
# Ctrl+] to exit
```

**Combined**:
```bash
idf.py flash monitor -p COM6
```

---

## Code Changes

### Modify Main Application
```bash
# Edit main.c (or RFuzz_TX.c, RFuzz_RX.c)
vim main/main.c

# Build & flash
idf.py build flash monitor
```

### Modify Component (cc1101, hw_init, sump_capture)
```bash
# Edit component source
vim components/cc1101/src/cc1101.c

# Build (components rebuild automatically)
idf.py build flash monitor
```

### Add New Component
```
components/
└── my_component/
    ├── CMakeLists.txt
    ├── include/
    │   └── my_component.h
    └── src/
        └── my_component.c
```
```cmake
# components/my_component/CMakeLists.txt
idf_component_register(SRCS "src/my_component.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos log)
```
```bash
idf.py reconfigure  # Re-run CMake
idf.py build
```

---

## Configuration Management

### Switch Presets
```bash
# List presets
ls sdkconfig.*

# Apply
cp sdkconfig.async_rx sdkconfig
idf.py build
```

### Temporary Override (One-off)
```bash
# Command line override
idf.py -DCC1101_DATARATE=9600 build flash monitor

# Environment variable
export CC1101_DATARATE=9600
idf.py build
```

### Save Current as Preset
```bash
cp sdkconfig sdkconfig.my_custom
git add sdkconfig.my_custom
```

---

## Git Workflow

### Feature Branch
```bash
git checkout -b feature/new-modulation
# Make changes
git add .
git commit -m "Add 4FSK modulation support"
git push origin feature/new-modulation
# Create PR
```

### Commit Message Format
```
<type>(<scope>): <subject>

<body>

<footer>
```

Types: `feat`, `fix`, `docs`, `refactor`, `test`, `chore`

Example:
```
feat(cc1101): add 4FSK modulation support

- Add CC1101_MOD_4FSK_E enum
- Update MDMCFG2 calculation
- Add Kconfig option

Closes #123
```

---

## Debugging Workflow

### 1. Serial Logs (Monitor)
```bash
idf.py monitor
# Look for:
# - ESP_LOGI/ESP_LOGE tags
# - MARCSTATE transitions
# - Register dumps
# - Assertion failures
```

### 2. GDB Debugging
```bash
# Start GDB server (OpenOCD)
idf.py openocd

# In another terminal
idf.py gdb

# Or with custom init
idf.py gdb -x gdbinit
```

**Common GDB Commands**:
```
(gdb) break cc1101_configure
(gdb) continue
(gdb) print cfg->modem.datarate_bps
(gdb) backtrace
(gdb) info registers
```

### 3. Logic Analyzer (SUMP)
```bash
# Capture GDO0/GDO2 during operation
python scripts/capture_sump.py --port COM7 --rate 100000 --samples 50000 --format vcd
# Open capture.vcd in GTKWave
```

### 4. JTAG Debug (ESP-Prog / FT2232)
```bash
# Hardware: ESP-Prog connected to JTAG pins
# OpenOCD config: board/esp32s3-builtin.cfg
openocd -f board/esp32s3-builtin.cfg
# Then: idf.py gdb
```

---

## Common Build Issues

| Error | Cause | Fix |
|-------|-------|-----|
| `sdkconfig not found` | First build | `idf.py menuconfig` |
| `Component "X" not found` | Missing CMakeLists.txt | Check component structure |
| `Multiple definitions` | Duplicate symbols | Check `REQUIRES`/`PRIV_REQUIRES` |
| `Flash size mismatch` | Wrong partition table | Check `CONFIG_ESPTOOLPY_FLASHSIZE` |
| `Guru Meditation` | Crash/exception | Check monitor for backtrace |

---

## Performance Optimization

### Build Speed
```bash
# Use all cores
idf.py build -j$(nproc)

# ccache (if installed)
export IDF_CCACHE_ENABLE=1
```

### Binary Size
```bash
# Check size
idf.py size

# Size breakdown
idf.py size-components
idf.py size-files
```

### Optimization Level
```bash
# In menuconfig:
→ Compiler options
  → Optimization Level: Debug (-Og) / Size (-Os) / Performance (-O2)
```

---

## CI/CD Integration

### GitHub Actions Example
```yaml
# .github/workflows/build.yml
name: Build
on: [push, pull_request]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Setup ESP-IDF
        uses: espressif/esp-idf-ci-action@v1
        with:
          esp_idf_version: v6.0.2
      - name: Build
        run: |
          cd CC1101_DRIVER
          cp sdkconfig.async_rx sdkconfig
          idf.py build
      - name: Upload artifacts
        uses: actions/upload-artifact@v4
        with:
          name: firmware
          path: CC1101_DRIVER/build/*.bin
```

---

## AI-Readable Workflow

```yaml
development_workflow:
  daily:
    - git pull
    - idf.py menuconfig (if needed)
    - idf.py build
    - idf.py flash monitor
  
  code_changes:
    main_app: "vim main/main.c → idf.py build flash monitor"
    component: "vim components/xxx/src/xxx.c → idf.py build flash monitor"
    new_component: "create structure → idf.py reconfigure → build"
  
  config_management:
    presets: "cp sdkconfig.preset sdkconfig"
    overrides: "idf.py -DVAR=value build"
    save_preset: "cp sdkconfig sdkconfig.name"
  
  git:
    branch: "feature/<description>"
    commit: "type(scope): subject"
    pr: "required for main"
  
  debugging:
    logs: "idf.py monitor"
    gdb: "idf.py openocd + idf.py gdb"
    logic_analyzer: "python capture_sump.py --format vcd"
    jtag: "ESP-Prog + openocd"
  
  common_issues:
    - sdkconfig_missing: "run menuconfig"
    - component_not_found: "check CMakeLists.txt"
    - flash_size: "check partition table"
    - crash: "check monitor backtrace"
  
  optimization:
    parallel_build: "idf.py build -j$(nproc)"
    ccache: "IDF_CCACHE_ENABLE=1"
    size_check: "idf.py size"
    opt_level: "menuconfig → Compiler options"
```

---

## Related

- [[07-workflow/testing|Testing Workflows]]
- [[07-workflow/debugging|Debugging Guide]]
- [[06-build-config/menuconfig-guide|Menuconfig Guide]]
- [[02-firmware/architecture|Firmware Architecture]]