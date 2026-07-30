#!/usr/bin/env python3
"""Host-side control for the SRS Stick.

The device speaks normal ESP_LOG text on USB CDC. Control traffic rides the
same link, distinguished by a leading '@' so it never collides with log output.

  host -> device   @shot
                   @tap <x> <y>
                   @swipe <x1> <y1> <x2> <y2> <ms>
                   @ping
                   @time <unix_epoch_seconds>
                   @gap <y>

  device -> host   @ok <text> | @err <text>
                   @shot <w> <h> <fmt> <nbytes>\n followed by nbytes raw pixels

There is no RTC on this board, so the device has no idea what time it is
until the host tells it: `time` sends the host's own clock over as
@time <epoch>, which the firmware feeds to settimeofday() and persists to
flash immediately (see docs/deck-format.md and CLAUDE.md).

Usage:
  devctl.py monitor
  devctl.py shot [out.png]
  devctl.py tap X Y
  devctl.py swipe X1 Y1 X2 Y2 [ms]
  devctl.py time
  devctl.py gap Y
  devctl.py reset
"""
import argparse
import sys
import time

import serial
from serial.tools import list_ports

BAUD = 115200


def find_port() -> str:
    candidates = [
        p.device
        for p in list_ports.comports()
        # 0x303a is Espressif's USB vendor id
        if (p.vid == 0x303A) or "usbmodem" in p.device
    ]
    if not candidates:
        sys.exit("no ESP32 serial port found; is the device plugged in?")
    if len(candidates) > 1:
        sys.exit(f"multiple candidate ports, pass --port: {candidates}")
    return candidates[0]


def open_port(port: str | None, timeout: float = 1.0) -> serial.Serial:
    return serial.Serial(port or find_port(), BAUD, timeout=timeout)


def hard_reset(ser: serial.Serial) -> None:
    """Pulse DTR/RTS to reboot the chip, as esptool does."""
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setDTR(True)
    ser.setRTS(False)
    time.sleep(0.1)
    ser.setDTR(False)


def cmd_monitor(args) -> None:
    ser = open_port(args.port, timeout=0.1)
    if args.reset:
        hard_reset(ser)
    ser.reset_input_buffer()
    deadline = time.time() + args.seconds if args.seconds else None
    while deadline is None or time.time() < deadline:
        data = ser.read(4096)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()


def _read_line(ser: serial.Serial, timeout: float) -> bytes:
    end = time.time() + timeout
    buf = bytearray()
    while time.time() < end:
        b = ser.read(1)
        if not b:
            continue
        if b == b"\n":
            return bytes(buf).rstrip(b"\r")
        buf += b
    raise TimeoutError("timed out waiting for a line from the device")


def _read_exact(ser: serial.Serial, n: int, timeout: float) -> bytes:
    end = time.time() + timeout
    buf = bytearray()
    while len(buf) < n and time.time() < end:
        chunk = ser.read(n - len(buf))
        if chunk:
            buf += chunk
    if len(buf) != n:
        raise TimeoutError(f"expected {n} bytes, got {len(buf)}")
    return bytes(buf)


def cmd_shot(args) -> None:
    from PIL import Image

    ser = open_port(args.port, timeout=0.1)
    ser.reset_input_buffer()
    ser.write(b"@shot\n")

    # Log lines may interleave; scan until the screenshot header shows up.
    end = time.time() + args.timeout
    header = None
    while time.time() < end:
        line = _read_line(ser, timeout=max(0.1, end - time.time()))
        if line.startswith(b"@shot "):
            header = line.decode()
            break
        if line.startswith(b"@err"):
            sys.exit(line.decode())
        if args.verbose and line:
            print(line.decode(errors="replace"), file=sys.stderr)
    if header is None:
        sys.exit("device never sent a screenshot header")

    _, w, h, fmt, nbytes = header.split()
    w, h, nbytes = int(w), int(h), int(nbytes)
    raw = _read_exact(ser, nbytes, timeout=args.timeout)

    if fmt == "rgb565":
        img = Image.frombytes("RGB", (w, h), raw, "raw", "BGR;16")
    elif fmt == "rgb888":
        img = Image.frombytes("RGB", (w, h), raw)
    else:
        sys.exit(f"unsupported pixel format {fmt}")

    img.save(args.out)
    print(f"{args.out}  {w}x{h} {fmt}")


def _simple(args, payload: bytes) -> None:
    ser = open_port(args.port)
    ser.reset_input_buffer()
    ser.write(payload)
    end = time.time() + 3
    while time.time() < end:
        line = _read_line(ser, timeout=max(0.1, end - time.time()))
        if line.startswith((b"@ok", b"@err")):
            print(line.decode())
            return
    print("no acknowledgement from device", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", help="serial port (auto-detected when omitted)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("monitor", help="stream device logs")
    m.add_argument("--reset", action="store_true", help="reboot before listening")
    m.add_argument("--seconds", type=float, default=0, help="stop after N seconds")
    m.set_defaults(func=cmd_monitor)

    s = sub.add_parser("shot", help="capture the screen to a PNG")
    s.add_argument("out", nargs="?", default="shot.png")
    s.add_argument("--timeout", type=float, default=10)
    s.add_argument("--verbose", action="store_true")
    s.set_defaults(func=cmd_shot)

    t = sub.add_parser("tap", help="inject a touch at x,y")
    t.add_argument("x", type=int)
    t.add_argument("y", type=int)
    t.set_defaults(func=lambda a: _simple(a, f"@tap {a.x} {a.y}\n".encode()))

    w = sub.add_parser("swipe", help="inject a swipe gesture")
    for name in ("x1", "y1", "x2", "y2"):
        w.add_argument(name, type=int)
    w.add_argument("ms", nargs="?", type=int, default=200)
    w.set_defaults(
        func=lambda a: _simple(
            a, f"@swipe {a.x1} {a.y1} {a.x2} {a.y2} {a.ms}\n".encode()
        )
    )

    tm = sub.add_parser("time", help="send the host's current clock to the device")
    tm.set_defaults(func=lambda a: _simple(a, f"@time {int(time.time())}\n".encode()))

    g = sub.add_parser("gap", help="set the panel y-offset live (alignment calibration)")
    g.add_argument("y", type=int)
    g.set_defaults(func=lambda a: _simple(a, f"@gap {a.y}\n".encode()))

    r = sub.add_parser("reset", help="reboot the device")
    r.set_defaults(func=lambda a: hard_reset(open_port(a.port)))

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
