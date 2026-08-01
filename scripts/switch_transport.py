#!/usr/bin/env python3
"""
Switch RFuzz SUMP transport mode between USB and UART.

  USB mode:  OLS protocol on USB Serial JTAG (COM7) — use with capture_sump.py
  UART mode: OLS protocol on UART0 (COM6) — use with PulseView OLS driver

After switching, rebuild and flash:
    python switch_transport.py usb && idf.py build && idf.py flash
    python switch_transport.py uart && idf.py build && idf.py flash
"""

import os
import re
import sys

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))
SDKCONFIG = os.path.join(PROJECT_ROOT, "sdkconfig")
SDKCONFIG_DEFAULTS = os.path.join(PROJECT_ROOT, "sdkconfig.defaults")

UART_DEFAULTS = """\
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"

# OLS transport: UART0 (COM6) for PulseView, USB Serial JTAG (COM7) for console
CONFIG_SUMP_TRANSPORT_UART=y
CONFIG_SUMP_UART_BAUD=921600
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y

# Enable IDF logs on USB Serial JTAG (COM7) for debugging
CONFIG_BOOTLOADER_LOG_LEVEL_INFO=y
CONFIG_LOG_DEFAULT_LEVEL_INFO=y

# USB Serial JTAG for IDF console (debug logs)
CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=y
"""

USB_DEFAULTS = """\
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"

# OLS transport: USB Serial JTAG (COM7) for Python capture, UART0 (COM6) for console
CONFIG_SUMP_TRANSPORT_USB=y
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_UART_NUM=0
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y

# Enable IDF logs on UART0 (COM6) for debugging
CONFIG_BOOTLOADER_LOG_LEVEL_INFO=y
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
"""


def switch(mode):
    if mode == "uart":
        content = UART_DEFAULTS
        info = "UART mode: PulseView on COM6, debug on COM7"
    elif mode == "usb":
        content = USB_DEFAULTS
        info = "USB mode: capture_sump.py on COM7, debug on COM6"
    else:
        print(f"Unknown mode: {mode}")
        print("Usage: python switch_transport.py [uart|usb]")
        sys.exit(1)

    with open(SDKCONFIG_DEFAULTS, "w", newline="\n") as f:
        f.write(content)

    if os.path.exists(SDKCONFIG):
        os.remove(SDKCONFIG)
        print(f"[*] Removed sdkconfig (will regenerate on next build)")

    print(f"[+] Switched to {info}")
    print(f"    Now run: idf.py build && idf.py flash")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__.strip())
        sys.exit(0)
    switch(sys.argv[1].lower())
