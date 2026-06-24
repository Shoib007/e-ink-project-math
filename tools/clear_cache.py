#!/usr/bin/env python3
"""
clear_cache.py — EPUB reader cache wiper
=========================================

Sends the "!CLEARCACHE" command over serial to the ESP32.
The firmware deletes /cache/meta.bin and restarts, forcing a full
page-cache rebuild on the next boot.

Usage (from the project root):
    pio run -t clear_cache

Or manually:
    python tools/clear_cache.py --port COM12
"""

import argparse
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial not found. Run:  pip install pyserial")
    sys.exit(1)


COMMAND   = b"!CLEARCACHE\n"
BAUDRATE  = 115200
BOOT_WAIT = 3.0   # seconds to wait after opening port (ESP32 USB-CDC resets on connect)
TIMEOUT   = 8.0   # seconds to wait for confirmation after sending command
RETRIES   = 3     # number of times to re-send command if no response


def find_port() -> str | None:
    """Auto-detect the first likely ESP32 / USB-serial port."""
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").upper()
        if any(k in desc for k in ("USB", "UART", "CP210", "CH340", "FTDI",
                                    "SILABS", "XIAO", "ESP")):
            return p.device
    return None


def run(port: str) -> int:
    print(f"Opening {port} at {BAUDRATE} baud ...")
    try:
        ser = serial.Serial(
            port,
            BAUDRATE,
            timeout=0.5,          # non-blocking reads
            dsrdtr=False,         # don't toggle DSR (avoids spurious reset on some boards)
            rtscts=False,
        )
    except serial.SerialException as e:
        print(f"ERROR opening port: {e}")
        return 1

    with ser:
        # Drain any bootloader noise produced by opening the port
        print(f"Waiting {BOOT_WAIT:.0f}s for ESP32 to finish booting ...")
        deadline = time.time() + BOOT_WAIT
        while time.time() < deadline:
            line = ser.readline().decode(errors="replace").strip()
            if line:
                print(f"  [boot] {line}")
        ser.reset_input_buffer()

        for attempt in range(1, RETRIES + 1):
            print(f"Sending command (attempt {attempt}/{RETRIES}): {COMMAND.strip().decode()}")
            ser.write(COMMAND)
            ser.flush()

            deadline = time.time() + TIMEOUT
            while time.time() < deadline:
                line = ser.readline().decode(errors="replace").strip()
                if line:
                    print(f"  ESP32: {line}")
                if "[Cache]" in line and ("Deleted" in line or "clear" in line or "Restart" in line):
                    print("\n✓ Cache cleared — ESP32 is restarting.")
                    return 0
            print(f"  No response on attempt {attempt}.\n")

    print("✗ Command not acknowledged after all attempts.")
    print()
    print("  Possible causes:")
    print("  1. Firmware with serial-handler was NOT uploaded yet.")
    print("     → Run:  pio run -t upload   then try again.")
    print("  2. The ESP32 is currently building cache (Core 1 busy).")
    print("     → Wait for cache build to finish, then try again.")
    print("  3. Wrong COM port. Detected:", port)
    print("     → Override with:  python tools/clear_cache.py --port COMx")
    return 1


def main():
    parser = argparse.ArgumentParser(
        description="Clear the EPUB page cache on the ESP32 via serial.")
    parser.add_argument("--port", "-p",
                        help="Serial port (e.g. COM12 or /dev/ttyUSB0). "
                             "Auto-detected if omitted.")
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        print("ERROR: No serial port found. Specify one with --port.")
        return 1

    return run(port)


if __name__ == "__main__":
    sys.exit(main())
