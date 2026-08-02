#!/usr/bin/env python3
"""Host-side control for the SRS Box.

The device speaks normal ESP_LOG text on USB CDC. Control traffic rides the
same link, distinguished by a leading '@' so it never collides with log output.

  host -> device   @shot
                   @tap <x> <y>
                   @swipe <x1> <y1> <x2> <y2> <ms>
                   @ping
                   @time <unix_epoch_seconds>
                   @gap <y>
                   @fput <path> <nbytes> <crc32>
                   @fget <path>
                   @fls
                   @fdel <path>
                   @reboot
                   @adc
                   @gpin

  device -> host   @ok <text> | @err <text>
                   @shot <w> <h> <fmt> <nbytes>\n followed by nbytes raw pixels
                   @fget <nbytes> <crc32>\n followed by nbytes raw file bytes

File transfer (@fput/@fget/@fls/@fdel/@reboot) is the sync protocol described
in docs/sync-protocol.md: paths are relative to the device's /data mount and
restricted to decks/, fonts/, and revlog.bin. See that doc for the exact wire
format; this module mirrors it, including muting expectations around @fget's
binary payload the same way @shot does.

There is no RTC on this board, so the device has no idea what time it is
until the host tells it: `time` sends the host's own clock over as
@time <epoch>, which the firmware feeds to settimeofday() and persists to
flash immediately (see docs/deck-format.md and CLAUDE.md). Every sync-protocol
command (fput/fget/fls/fdel/reboot) also re-anchors the clock automatically
before doing its own work, so a device left unplugged for a while gets a
fresh @time on the next `push`/`pull`/`ls`/`rm`/`sync`/`reboot`, not just an
explicit `devctl.py time`. That auto-sync is best-effort: a failure only
prints a warning, it never blocks the requested operation.

Usage:
  devctl.py monitor
  devctl.py shot [out.png]
  devctl.py tap X Y
  devctl.py swipe X1 Y1 X2 Y2 [ms]
  devctl.py time
  devctl.py gap Y
  devctl.py reset
  devctl.py fput LOCAL_FILE DEVICE_PATH
  devctl.py fget DEVICE_PATH [LOCAL_FILE]
  devctl.py fls
  devctl.py fdel DEVICE_PATH
  devctl.py reboot
  devctl.py adc
  devctl.py gpin
"""
import argparse
import sys
import time
import zlib
from pathlib import Path

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


def _await_reply(ser: serial.Serial, timeout: float) -> bytes:
    """Next @ok/@err line, skipping any interleaved ESP_LOG output.

    The CDC link carries log lines between protocol replies, so waiting for
    "the next line" races against the logger; every ack must tolerate noise.
    """
    end = time.time() + timeout
    while time.time() < end:
        line = _read_line(ser, timeout=max(0.1, end - time.time()))
        if line.startswith(b"@ok") or line.startswith(b"@err"):
            return line
    raise TimeoutError("timed out waiting for an @ok/@err reply")


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


def _sync_time(ser: serial.Serial) -> None:
    """Hands the device the host's clock before a sync-protocol operation.

    There is no RTC on this board (see CLAUDE.md), so every connection should
    re-anchor its idea of the time. Best-effort: a failure here is logged as
    a warning and must not block the operation the user actually asked for.
    """
    try:
        ser.write(f"@time {int(time.time())}\n".encode())
        line = _await_reply(ser, timeout=3)
        if not line.startswith(b"@ok"):
            print(f"warning: @time sync failed: {line.decode(errors='replace')}", file=sys.stderr)
    except TimeoutError:
        print("warning: @time sync timed out", file=sys.stderr)


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


def _progress(label: str, done: int, total: int) -> None:
    pct = 100 * done // total if total else 100
    end = "\n" if done >= total else ""
    print(f"\r{label} {pct:3d}% ({done}/{total} bytes)", end=end, file=sys.stderr, flush=True)


def cmd_fput(args) -> None:
    data = Path(args.local_file).read_bytes()
    crc = zlib.crc32(data) & 0xFFFFFFFF

    ser = open_port(args.port, timeout=0.1)
    ser.reset_input_buffer()
    _sync_time(ser)
    ser.write(f"@fput {args.device_path} {len(data)} {crc:08x}\n".encode())

    line = _await_reply(ser, timeout=5)
    if line != b"@ok send":
        sys.exit(line.decode(errors="replace") or "device did not ack @fput")

    sent = 0
    chunk_size = 4096
    while sent < len(data):
        n = ser.write(data[sent : sent + chunk_size])
        sent += n
        _progress(f"push {args.device_path}", sent, len(data))

    # Generous: the device tolerates LittleFS GC stalls up to 30s before its
    # own timeout, so the host must outwait that plus the write itself.
    line = _await_reply(ser, timeout=60)
    print(line.decode(errors="replace"))
    if not line.startswith(b"@ok"):
        sys.exit(1)


def cmd_fget(args) -> None:
    ser = open_port(args.port, timeout=0.1)
    ser.reset_input_buffer()
    _sync_time(ser)
    ser.write(f"@fget {args.device_path}\n".encode())

    end = time.time() + args.timeout
    header = None
    while time.time() < end:
        line = _read_line(ser, timeout=max(0.1, end - time.time()))
        if line.startswith(b"@fget "):
            header = line.decode()
            break
        if line.startswith(b"@err"):
            sys.exit(line.decode())
        if args.verbose and line:
            print(line.decode(errors="replace"), file=sys.stderr)
    if header is None:
        sys.exit("device never sent a file header")

    _, nbytes, crc_hex = header.split()
    nbytes = int(nbytes)

    out = args.local_file or Path(args.device_path).name
    received = bytearray()
    chunk_size = 4096
    while len(received) < nbytes:
        want = min(chunk_size, nbytes - len(received))
        received += _read_exact(ser, want, timeout=args.timeout)
        _progress(f"pull {args.device_path}", len(received), nbytes)

    crc = zlib.crc32(bytes(received)) & 0xFFFFFFFF
    if f"{crc:08x}" != crc_hex:
        sys.exit(f"crc mismatch: device said {crc_hex}, got {crc:08x}")

    Path(out).write_bytes(received)
    print(f"{out}  {nbytes:,} bytes")


def cmd_fls(args) -> None:
    ser = open_port(args.port)
    ser.reset_input_buffer()
    _sync_time(ser)
    ser.write(b"@fls\n")
    line = _await_reply(ser, timeout=5)
    if not line.startswith(b"@ok fls"):
        sys.exit(line.decode(errors="replace") or "device did not ack @fls")
    entries = line.decode()[len("@ok fls") :].strip().split()
    if not entries:
        print("(no files)")
        return
    for entry in entries:
        if entry == "...":
            print("... (listing truncated)")
            continue
        path, _, size = entry.rpartition("=")
        print(f"  {path:<40} {int(size):>10,} bytes")


def cmd_fdel(args) -> None:
    _simple(args, f"@fdel {args.device_path}\n".encode(), sync_time=True)


def cmd_reboot(args) -> None:
    _simple(args, b"@reboot\n", sync_time=True)


def _simple(args, payload: bytes, sync_time: bool = False) -> None:
    ser = open_port(args.port)
    ser.reset_input_buffer()
    if sync_time:
        _sync_time(ser)
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

    # DTR/RTS pulsing is known to wedge THIS board's USB stack (recovery needs
    # a physical replug — see CLAUDE.md). Kept only behind an explicit flag;
    # use `reboot` (@reboot, a clean esp_restart) instead.
    r = sub.add_parser("reset", help="DTR/RTS reset — WEDGES this board, use 'reboot'")
    r.add_argument("--i-know-this-wedges-the-board", action="store_true")
    r.set_defaults(
        func=lambda a: hard_reset(open_port(a.port))
        if a.i_know_this_wedges_the_board
        else sys.exit("refusing: DTR/RTS wedges this board's USB. Use 'reboot' instead.")
    )

    fp = sub.add_parser("fput", help="push a local file to the device (@fput)")
    fp.add_argument("local_file")
    fp.add_argument("device_path", help="path relative to /data, e.g. decks/hsk1.srs")
    fp.set_defaults(func=cmd_fput)

    fg = sub.add_parser("fget", help="pull a file from the device (@fget)")
    fg.add_argument("device_path")
    fg.add_argument("local_file", nargs="?")
    fg.add_argument("--timeout", type=float, default=30)
    fg.add_argument("--verbose", action="store_true")
    fg.set_defaults(func=cmd_fget)

    fl = sub.add_parser("fls", help="list files on the device (@fls)")
    fl.set_defaults(func=cmd_fls)

    fd = sub.add_parser("fdel", help="delete a file on the device (@fdel)")
    fd.add_argument("device_path")
    fd.set_defaults(func=cmd_fdel)

    rb = sub.add_parser("reboot", help="apply pushed content by restarting (@reboot)")
    rb.set_defaults(func=cmd_reboot)

    # Battery ADC / charge-detect hunting: see docs/pinout.md's "Still
    # unknown" section. Both are one-shot hardware surveys, safe to re-run.
    ad = sub.add_parser("adc", help="one-shot ADC survey of the battery-sense candidates (@adc)")
    ad.set_defaults(func=lambda a: _simple(a, b"@adc\n"))

    gp = sub.add_parser("gpin", help="digital snapshot of free GPIOs, for charge-detect (@gpin)")
    gp.set_defaults(func=lambda a: _simple(a, b"@gpin\n"))

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
