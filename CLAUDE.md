# CLAUDE.md

An Anki-style spaced-repetition flashcard device on an ESP32-S3 handheld. Primary use is Chinese vocabulary, so CJK text and per-card audio are first-class, not afterthoughts.

Q is new to embedded work. Explain the *why* in plain terms when it matters, lead on technical decisions rather than asking him to arbitrate them, and keep learning notes in Vietnamese in `~/vaults/dqnotes/Personal/SRS Stick.md`.

## The device

An unbranded ESP32-S3 board sold pre-flashed with a Vietnamese build of [xiaozhi](https://github.com/78/xiaozhi-esp32). It reports `SKU=ostb-xiaozhi-3st`, is **not** in the upstream xiaozhi board registry, and shipped with no documentation or schematic.

| | |
|---|---|
| MCU | ESP32-S3 (QFN56) rev v0.2, dual core, 240MHz |
| Flash | 16MB, quad I/O, `boya` |
| PSRAM | 8MB octal (`N16R8`) |
| USB | native USB-Serial/JTAG, VID `0x303a` PID `0x1001` |
| Display | NV3023 SPI LCD, ~1.8", driven through LVGL |
| Touch | CST816S/D capacitive over I2C (reports IC id 184) |
| Audio out | ES8311 codec |
| Audio in | ES7210, 4-mic array, TDM |
| Power | battery voltage via ADC, charge-state detect |

The stock firmware used ESP-IDF v5.5.1 and the `78/esp_lcd_nv3023` component, so both are known-good choices here.

**Pin map is not yet known.** The vendor never published it. It is being recovered by halting the running stock firmware over USB-JTAG with `openocd-esp32` and reading the ESP32-S3 GPIO matrix (`GPIO_FUNCn_OUT_SEL_CFG`) and IO MUX registers, which record exactly which GPIO each FSPI/I2C/I2S/LEDC signal is routed to. Record results in `docs/pinout.md` as they are confirmed. Do not guess pins into the firmware.

## Two hazards, both learned the hard way

**Never kill esptool mid-operation.** `pkill`-ing it during a flash read leaves the board's USB stack unresponsive: serial goes silent *and* ep0 control transfers start failing, which makes OpenOCD fail too with a misleading `libusb_get_string_descriptor_ascii() failed with -1`. The only recovery is a physical unplug/replug, which needs the user. Let long reads finish; `tools/dumpflash.sh` is chunked and resumable precisely so it never has to be interrupted.

Also note the board is **battery-backed**, so unplugging USB does not reset it, and DTR/RTS toggling does not reset it either. A wedged USB stack still requires a physical replug.

**Use a current `openocd-esp32`, not the one ESP-IDF v5.5.1 pins.** v5.5.1 pins `v0.12.0-esp32-20250707`, which cannot talk to this board on macOS 27 at all. `v0.12.0-esp32-20251215` and later work; `v0.12.0-esp32-20260703` is installed at `~/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260703/`.

The underlying cause is libusb on macOS 27 (Tahoe): every ep0 control transfer that macOS has not already cached at enumeration fails with `kIOReturnNotResponding`, including the LANGID table that `libusb_get_string_descriptor_ascii` reads first. It is not a permissions problem and `sudo` does not help. Espressif closed the matching report ([openocd-esp32#263](https://github.com/espressif/openocd-esp32/issues/263)) as "Won't Do"; newer builds fix it silently by bundling a newer libusb. The bulk JTAG data path was never affected.

Vanilla Homebrew `openocd` will not work either — it lacks the `esp_usb_jtag` interface entirely.

## Working on it

Everything goes through `./dev`, which auto-detects the serial port and bootstraps its own Python venv:

```
./dev doctor          # first thing to run: device present? toolchain ok? chip info
./dev flash           # build, flash, then stream logs
./dev logs            # stream logs
./dev shot out.png    # capture the screen as a PNG
./dev tap X Y         # inject a touch
./dev swipe X1 Y1 X2 Y2
./dev backup          # dump all 16MB to backup/
./dev restore FILE    # write an image back
```

`shot` and `tap` are the point of the whole setup: they close the loop so a change to the UI can be verified without a human looking at the hardware. Build them early and keep them working.

### Device control protocol

Control traffic shares the USB CDC link with normal `ESP_LOG` output and is disambiguated by a leading `@`. Implemented host-side in `tools/devctl.py`; the firmware must match.

```
host -> device   @shot | @tap <x> <y> | @swipe <x1> <y1> <x2> <y2> <ms> | @ping
device -> host   @ok <text> | @err <text>
                 @shot <w> <h> <fmt> <nbytes>\n  then nbytes of raw pixels
```

Screenshots come from `lv_snapshot_take` on the active screen into PSRAM, not from the LVGL draw buffer, which only holds a partial frame.

## Design decisions

**ESP-IDF, not Arduino.** The NV3023 driver and TinyUSB composite device both live in the IDF ecosystem; Arduino would fight us on each.

**FSRS-6 scheduling, implemented on-device.** Per-card stability and difficulty, updated locally so the device works fully offline.

**The review log is the source of truth.** Every grade appends `(card_id, timestamp, rating)`. Due dates are derived, never synced. This is how Anki models it, so pushing raw revlog entries to a desktop collection stays correct and lets Anki's own FSRS recompute.

**Sync over serial, driven by a web app. Not USB Mass Storage.** MSC was the original plan and was rejected: macOS cannot read LittleFS, so MSC would force a second FAT filesystem into the firmware, and a mounted host writing the FAT while the device appends a review corrupts study history. Its one advantage — no software to install — a web app has anyway. Decks move over the same `@`-prefixed CDC protocol as screenshots, with two clients: the web app over WebSerial, and `./dev` from a terminal. Revisit MSC only if bulk audio transfer proves slow; native USB CDC does roughly 500KB/s, so it almost certainly will not.

**The web app is a deck compiler, not a form.** Chinese cards need pinyin, a gloss, and audio, none of which the ESP32 can produce. Input is a word list; output is a bundle of card records, a font subset, and audio. Building that bundle host-side is the whole reason the web app exists.

**Storage is a compact binary deck format on LittleFS**, not SQLite: fixed-size scheduling records plus a text blob.

**Font subsetting makes CJK cheap.** Shipping all of Unicode CJK is heavy; shipping exactly the glyphs present in the user's decks is not. Around 2,500 hanzi as a 32px bitmap font is roughly 250KB. The compiler emits the subset alongside the deck. Pinyin is rendered with tone colouring.

**Audio is the scarce resource, not text.** Opus at roughly 3KB per word — a codec already proven on this chip by the stock xiaozhi firmware — means a ~6MB budget holds about 2,000 words. The compiler prioritises audio for new and due cards and omits it for long-mature ones. The deck format must therefore treat audio as optional per card.

**Anki remains authoritative for history.** The device exports raw `(card_id, timestamp, rating)` rows; desktop Anki recomputes its own schedule from them. Never sync due dates, or the two schedules will disagree.

## Display debugging, learned expensively

**Never write a row wider than the panel.** Out-of-range writes wrap and
overwrite earlier pixels, so a test pattern can erase itself. Worse, the
corrupted output then looks like a mirror, a transpose, or a wrong resolution,
and every conclusion drawn from it is wrong. This single mistake produced a
long chain of confident false diagnoses. Verify write geometry before
interpreting anything on the glass.

**Get geometry from the firmware, not from photographs.** The stock firmware's
splash JPEG is embedded in flash and its dimensions are the panel's visible
size: `grep` for the JPEG magic, read the SOF header, done. The vendored
driver's `CASET`/`RASET` init values say the same thing. Both are unambiguous
and need no hardware round-trip. Photographing test patterns is slow, depends
on the user interpreting bezels and reflections, and confuses reference frames.

**Agree a reference frame before asking about orientation.** "Left" and "top"
mean nothing unless the device's orientation is fixed first, and photos arrive
in whatever rotation they were shot. Better: make the test frame-independent,
e.g. anchor everything to a uniquely coloured marker.

**Put calibration on the device.** Cycling settings under the physical buttons,
with the active setting shown on screen, converges in one flash. Guessing one
combination per flash does not, especially when settings interact.

**The simulator cannot see these bugs.** It renders the framebuffer, which is
correct; the fault is downstream in the panel. Never report a display fix as
verified on the strength of a simulator render.

## Borrowed from chat-stick

[steveruizok/chat-stick](https://github.com/steveruizok/chat-stick) targets different hardware with Arduino and Arduino_GFX, so none of its code ports directly. Two of its designs are worth reimplementing.

**A staged idle-power cascade.** Active → Dimmed → ScreenOff → LightSleep → PowerOff, each with its own timeout, all reset by one `registerActivity()` call. Right for a device that gets put down mid-session constantly. Its `setTimeouts` clamps the thresholds into a valid order so screen-off can never precede dim; copy that, since an inverted config is otherwise a confusing bug.

**Zero double-click delay on the grading buttons.** Its button state machine treats `doubleClickMs = 0` as "emit the click immediately on release". Double-click detection inherently costs latency, because the code must wait to see whether a second click follows. On a grade button that latency is felt on every single card. Grades must fire on release with no debounce window; reserve held-press for secondary actions and do not put a double-click gesture anywhere in the review loop.

Its repo structure is also worth keeping: shared logic in `common/`, board specifics isolated. That is why `fsrs`, `deck`, and `session` here are hardware-free and host-testable.

## Layout

```
dev              entry point for every task
tools/           host-side scripts (devctl.py)
firmware/        ESP-IDF project
web/             deck-authoring web app (Next.js), pushes decks over WebSerial
decks/           sample and exported decks
backup/          flash dumps, including the stock xiaozhi image
docs/            pinout.md and hardware notes
```
