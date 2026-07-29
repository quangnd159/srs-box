#!/usr/bin/env python3
"""Recover this board's pin map by reading the live GPIO matrix over JTAG.

The vendor published no schematic. But while the stock firmware is running,
the chip itself holds the answer: the GPIO matrix records which peripheral
signal is routed to each pad. This resets the board, lets the stock firmware
boot and configure everything, halts the CPU, and reads those registers.

    ./dev pins            # human-readable pin map
    ./dev pins --raw      # also dump the raw register values

Nothing is written to the device.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

# ESP32-S3 register file. TRM "IO MUX and GPIO Matrix".
GPIO_FUNC_OUT_SEL_CFG = 0x60004554  # + 4*gpio  -> out signal index in [8:0]
GPIO_FUNC_IN_SEL_CFG = 0x60004154   # + 4*signal -> source gpio in [5:0]
GPIO_ENABLE_REG = 0x60004020
GPIO_ENABLE1_REG = 0x60004024
IO_MUX_BASE = 0x60009004            # + 4*gpio

NUM_GPIO = 49
NUM_SIGNALS = 256

# 128 in OUT_SEL means "plain GPIO output", not a peripheral.
SIG_GPIO_OUT = 128

DEFAULT_OPENOCD = Path.home() / (
    ".espressif/tools/openocd-esp32/v0.12.0-esp32-20260703/openocd-esp32"
)
SIG_MAP_HEADER = Path.home() / (
    "esp/esp-idf/components/soc/esp32s3/include/soc/gpio_sig_map.h"
)

# Signals worth calling out, grouped by the peripheral we care about.
INTERESTING = {
    "display (SPI2/FSPI)": [
        "FSPICLK", "FSPID", "FSPIQ", "FSPICS0", "FSPICS1",
        "FSPIHD", "FSPIWP", "FSPIIO4", "FSPIIO5", "FSPIIO6", "FSPIIO7",
    ],
    "display (SPI3)": ["SPI3_CLK", "SPI3_D", "SPI3_Q", "SPI3_CS0"],
    "touch / codec (I2C)": [
        "I2CEXT0_SCL", "I2CEXT0_SDA", "I2CEXT1_SCL", "I2CEXT1_SDA",
    ],
    "audio (I2S)": [
        "I2S0O_BCK", "I2S0O_WS", "I2S0O_SD", "I2S0I_BCK", "I2S0I_WS",
        "I2S0I_SD", "I2S0_MCLK", "I2S1O_BCK", "I2S1O_WS", "I2S1O_SD",
    ],
    "backlight (LEDC)": [f"LEDC_LS_SIG_OUT{i}" for i in range(8)],
    "uart": ["U0TXD", "U0RXD", "U1TXD", "U1RXD"],
}


def load_signal_map() -> tuple[dict[int, list[str]], dict[str, int]]:
    """Parse gpio_sig_map.h into index -> names and name -> index."""
    if not SIG_MAP_HEADER.exists():
        sys.exit(f"missing {SIG_MAP_HEADER}; is ESP-IDF installed?")
    by_index: dict[int, list[str]] = {}
    by_name: dict[str, int] = {}
    pattern = re.compile(r"^#define\s+(\w+?)_IDX\s+(\d+)")
    for line in SIG_MAP_HEADER.read_text().splitlines():
        m = pattern.match(line.strip())
        if not m:
            continue
        name, idx = m.group(1), int(m.group(2))
        by_index.setdefault(idx, []).append(name)
        by_name[name] = idx
    return by_index, by_name


def run_openocd(openocd_dir: Path, boot_ms: int) -> str:
    binary = openocd_dir / "bin" / "openocd"
    scripts = openocd_dir / "share" / "openocd" / "scripts"
    if not binary.exists():
        sys.exit(f"openocd not found at {binary}")
    cmds = [
        "init",
        # Reset and let the stock firmware boot so it configures its pins.
        "reset run",
        f"sleep {boot_ms}",
        "halt",
        "echo {===OUTSEL===}",
        f"mdw {hex(GPIO_FUNC_OUT_SEL_CFG)} {NUM_GPIO}",
        "echo {===INSEL===}",
        f"mdw {hex(GPIO_FUNC_IN_SEL_CFG)} {NUM_SIGNALS}",
        "echo {===IOMUX===}",
        f"mdw {hex(IO_MUX_BASE)} {NUM_GPIO}",
        "echo {===ENABLE===}",
        f"mdw {hex(GPIO_ENABLE_REG)} 2",
        "echo {===END===}",
        "resume",
        "exit",
    ]
    argv = [str(binary), "-s", str(scripts), "-f", "board/esp32s3-builtin.cfg"]
    for c in cmds:
        argv += ["-c", c]
    proc = subprocess.run(argv, capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    if "===END===" not in out:
        print(out, file=sys.stderr)
        sys.exit(
            "openocd did not complete.\n"
            "If this says libusb_get_string_descriptor_ascii() failed, the board's\n"
            "USB stack is wedged: unplug and replug the cable, then retry."
        )
    return out


def parse_words(text: str, start: str, end: str) -> list[int]:
    """Collect the hex words openocd's mdw printed between two markers."""
    section = text.split(start, 1)[1].split(end, 1)[0]
    words: list[int] = []
    for line in section.splitlines():
        m = re.match(r"^0x[0-9a-fA-F]+:\s+(.*)$", line.strip())
        if not m:
            continue
        for tok in m.group(1).split():
            if re.fullmatch(r"[0-9a-fA-F]{8}", tok):
                words.append(int(tok, 16))
    return words


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--openocd", type=Path, default=DEFAULT_OPENOCD)
    ap.add_argument("--boot-ms", type=int, default=7000,
                    help="how long to let the stock firmware boot before halting")
    ap.add_argument("--raw", action="store_true")
    args = ap.parse_args()

    by_index, by_name = load_signal_map()
    out = run_openocd(args.openocd, args.boot_ms)

    outsel = parse_words(out, "===OUTSEL===", "===INSEL===")
    insel = parse_words(out, "===INSEL===", "===IOMUX===")
    iomux = parse_words(out, "===IOMUX===", "===ENABLE===")
    enable = parse_words(out, "===ENABLE===", "===END===")

    if len(outsel) < NUM_GPIO or len(insel) < NUM_SIGNALS:
        sys.exit(f"short read: {len(outsel)} outsel, {len(insel)} insel words")

    en = 0
    if len(enable) >= 2:
        en = enable[0] | (enable[1] << 32)

    if args.raw:
        print("=== raw OUT_SEL ===")
        for g in range(NUM_GPIO):
            print(f"  GPIO{g:<2} 0x{outsel[g]:08x}")
        print("=== raw IO_MUX ===")
        for g in range(NUM_GPIO):
            print(f"  GPIO{g:<2} 0x{iomux[g]:08x}")

    # --- outputs: which peripheral signal drives each pad ---------------------
    print("=== GPIO outputs (driven by a peripheral) ===")
    found_out: dict[str, int] = {}
    for g in range(NUM_GPIO):
        sig = outsel[g] & 0x1FF
        if sig == SIG_GPIO_OUT:
            continue
        names = by_index.get(sig, [])
        out_names = [n for n in names if n.endswith("_OUT")] or names
        label = "/".join(sorted(set(out_names))) or f"signal {sig}"
        driven = "out-enabled" if (en >> g) & 1 else "not enabled"
        print(f"  GPIO{g:<3} <- {label}  ({driven})")
        for n in out_names:
            found_out.setdefault(n[:-4] if n.endswith("_OUT") else n, g)

    # --- inputs: which pad feeds each peripheral signal -----------------------
    found_in: dict[str, int] = {}
    for sig in range(NUM_SIGNALS):
        val = insel[sig]
        if not (val & 0x80):  # bit 7: route through the matrix
            continue
        gpio = val & 0x3F
        if gpio >= NUM_GPIO:
            continue
        for n in by_index.get(sig, []):
            if n.endswith("_IN"):
                found_in.setdefault(n[:-3], gpio)

    # --- the summary that actually matters -----------------------------------
    print()
    print("=== recovered pin map ===")
    for group, sigs in INTERESTING.items():
        lines = []
        for s in sigs:
            g_out = found_out.get(s)
            g_in = found_in.get(s)
            if g_out is None and g_in is None:
                continue
            if g_out is not None and g_in is not None and g_out != g_in:
                lines.append(f"    {s:<16} GPIO{g_out} (out), GPIO{g_in} (in)")
            else:
                g = g_out if g_out is not None else g_in
                direction = "out" if g_out is not None else "in"
                lines.append(f"    {s:<16} GPIO{g} ({direction})")
        if lines:
            print(f"  {group}:")
            print("\n".join(lines))

    print()
    print("Pins with no peripheral routing (plain GPIO: buttons, resets, "
          "chip-selects driven in software) will not appear above.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
