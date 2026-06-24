"""
extra_scripts.py — PlatformIO custom targets for the EPUB reader project.

Registered targets
------------------
clear_cache
    Sends "!CLEARCACHE\\n" over the configured serial port to the running
    firmware.  The ESP32 deletes /cache/meta.bin and restarts, forcing a full
    page-cache rebuild on next boot.

    Usage:
        pio run -t clear_cache
"""

Import("env")  # noqa: F821  (PlatformIO injects this)
import os
import subprocess
import sys


def clear_cache(source, target, env):  # noqa: ARG001
    """PlatformIO build action: wipe the SD page cache via serial."""
    script = os.path.join(env.subst("$PROJECT_DIR"), "tools", "clear_cache.py")

    # Honour the port set in platformio.ini / environment
    port = env.subst("$UPLOAD_PORT") or ""

    cmd = [sys.executable, script]
    if port:
        cmd += ["--port", port]

    print(f"\n>>> Running: {' '.join(cmd)}\n")
    result = subprocess.call(cmd)
    if result != 0:
        env.Exit(result)


env.AddCustomTarget(          # noqa: F821
    name        = "clear_cache",
    dependencies = None,
    actions     = clear_cache,
    title       = "Clear SD page cache",
    description = "Send !CLEARCACHE over serial; ESP32 deletes /cache/meta.bin and restarts.",
)
