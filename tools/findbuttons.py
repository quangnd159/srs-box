#!/usr/bin/env python3
"""Find which GPIOs the physical buttons are wired to.

Buttons are inputs, so they never appear in the GPIO output routing that
`readpins.py` recovers. Instead this samples GPIO_IN over a window while the
user presses each button, and reports any pin that changed state.

    ./dev buttons --seconds 40

Press each button several times during the window. Keep power-button presses
short; a long press will power the board off.
"""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

GPIO_IN_REG = 0x6000403C   # GPIO 0..31
GPIO_IN1_REG = 0x60004040  # GPIO 32..48

DEFAULT_OPENOCD = Path.home() / (
    ".espressif/tools/openocd-esp32/v0.12.0-esp32-20260703/openocd-esp32"
)

# Pins already accounted for; changes on these are expected, not buttons.
KNOWN = {
    4: "LCD RESET", 5: "I2S MCLK", 6: "I2S DOUT", 7: "I2S DIN",
    8: "LCD DC", 9: "LCD SCLK", 10: "LCD MOSI", 11: "I2C SCL",
    12: "I2C SDA", 13: "LCD backlight", 14: "LCD CS", 15: "I2S BCK",
    16: "I2S WS", 18: "power/periph enable",
    19: "USB D-", 20: "USB D+",
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--openocd", type=Path, default=DEFAULT_OPENOCD)
    ap.add_argument("--seconds", type=float, default=40.0)
    args = ap.parse_args()

    binary = args.openocd / "bin" / "openocd"
    scripts = args.openocd / "share" / "openocd" / "scripts"
    if not binary.exists():
        sys.exit(f"openocd not found at {binary}")

    # Estimate iterations from the sampling window; each halt/resume cycle
    # costs roughly 60ms in practice.
    iterations = max(20, int(args.seconds / 0.06))

    argv = [str(binary), "-s", str(scripts), "-f", "board/esp32s3-builtin.cfg", "-c", "init"]
    for _ in range(iterations):
        argv += ["-c", "halt",
                 "-c", f"mdw {hex(GPIO_IN_REG)} 1",
                 "-c", f"mdw {hex(GPIO_IN1_REG)} 1",
                 "-c", "resume"]
    argv += ["-c", "exit"]

    print(f"sampling GPIO_IN for ~{args.seconds:.0f}s — press every button now, "
          "several times each", flush=True)
    started = time.time()
    proc = subprocess.run(argv, capture_output=True, text=True)
    text = proc.stdout + proc.stderr

    lo, hi = [], []
    for line in text.splitlines():
        m = re.match(r"^0x(6000403c|60004040):\s+([0-9a-f]{8})", line.strip().lower())
        if m:
            (lo if m.group(1) == "6000403c" else hi).append(int(m.group(2), 16))

    n = min(len(lo), len(hi))
    if n < 5:
        print(text, file=sys.stderr)
        sys.exit("too few samples; if openocd errored, replug the device and retry")

    print(f"collected {n} samples over {time.time()-started:.0f}s\n")

    states: dict[int, list[int]] = {}
    for i in range(n):
        combined = lo[i] | (hi[i] << 32)
        for g in range(49):
            states.setdefault(g, []).append((combined >> g) & 1)

    candidates = []
    for g, bits in states.items():
        if len(set(bits)) < 2:
            continue
        transitions = sum(1 for a, b in zip(bits, bits[1:]) if a != b)
        candidates.append((g, transitions, sum(bits), len(bits)))

    if not candidates:
        print("no pin changed state. Either no button was pressed during the "
              "window, or the buttons are not on plain GPIOs.")
        return 1

    candidates.sort(key=lambda c: c[1])
    print("pins that changed state:")
    for g, transitions, ones, total in candidates:
        note = KNOWN.get(g, "")
        idle = "idle high" if ones > total / 2 else "idle low"
        tag = f"  <- {note}" if note else "  <- BUTTON CANDIDATE"
        print(f"  GPIO{g:<3} transitions={transitions:<4} {idle}{tag}")

    print("\nButtons are typically idle high and active low, with few "
          "transitions. Busy pins are the display and codec doing their job.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
