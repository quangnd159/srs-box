# Hardware notes: `ostb-xiaozhi-3st`

Everything here was recovered from the device itself, since the vendor published no schematic. Nothing below is a guess; unknowns are marked as such.

## Identity

Sold as **DB MEGA3D 3ST**, a Vietnamese-localised xiaozhi AI voice assistant in a 3D-printed case, by Mega3D in Đà Nẵng, via Shopee. Listed under a smartwatch category with mostly nonsense attributes (3MP camera, 1GB storage, 42mm case) that describe no part of the actual product.

Three independent identifiers agree: the firmware's board class is `OSTB_XIAOZHI_3ST`, the reported SKU is `ostb-xiaozhi-3st`, and the application logs `3D-MEGA: starting` at boot. The OTA server baked into the firmware is `https://me.ai-box.vn/xiaozhi/ota/` alongside upstream `https://api.tenclass.net/xiaozhi/ota/`. The board does not exist in the upstream [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) registry.

The seller lists the screen as **2.0 inch** edge-to-edge touch, and advertises dual Type-C, one being an expansion port for external modules. No schematic, pinout, or documentation is provided; the listing points at a QR code that does not exist. Vendor support is a Shopee chat window.

Stock firmware: xiaozhi 3.1.4, ESP-IDF v5.5.1, built 2026-05-04.

## Silicon

```
Chip type    ESP32-S3 (QFN56) revision v0.2
Features     WiFi, BT 5 LE, dual core + LP core, 240MHz
PSRAM        8MB octal, embedded (AP_3v3)
Flash        16MB quad I/O, manufacturer boya (0x68), device 0x4018
Crystal      40MHz
USB          native USB-Serial/JTAG, VID 0x303a PID 0x1001
MAC          68:ee:8f:60:b1:7c
```

`N16R8` in the usual shorthand.

## Peripherals

Observed during boot of the stock firmware:

| Function | Part | Notes |
|---|---|---|
| Display | NV3023 | SPI, via the `78/esp_lcd_nv3023` managed component. Backlight is PWM, brightness 0–100. |
| Touch | CST816S/D | I2C, reports `IC id: 184`. Firmware includes a `cst816d_upgrade_entry()` bootmode/checksum path, so the vendor ships touch-controller firmware updates. |
| Audio out | ES8311 | Slave mode. |
| Audio in | ES7210 | Slave mode, 4 mics (`MIC1`–`MIC4`), TDM, 16-bit. |
| Audio bus | I2S | RX is TDM 4-slot; TX is standard stereo/mono. Runs at 24kHz in stock firmware. |
| Battery | ADC | Raw counts around 2300 map to ~68%; charge state is detected separately. |
| Buttons | at least 6 (stock firmware); 3 physical (power, plus, minus) on this board | `InitializeButtons()` registers six handlers in the stock firmware. The three physical buttons, confirmed on-device 2026-07-29 (see Pin map below): GPIO39 = minus, GPIO40 = plus, GPIO2 = power (short-press only — see below). |

Display resolution: confirmed 240 x 284 visible (240 x 320 panel memory, 36-row gap). See "Display configuration" below.

## Flash layout (stock)

| Partition | Type | Offset | Size |
|---|---|---|---|
| `nvs` | data/nvs | `0x009000` | 16K |
| `otadata` | data/ota | `0x00d000` | 8K |
| `phy_init` | data/phy | `0x00f000` | 4K |
| `ota_0` | app/ota_0 | `0x020000` | 4480K |
| `ota_1` | app/ota_1 | `0x480000` | 4480K |
| `assets` | data (0x82) | `0x8e0000` | 5760K |
| `data` | data (0x83) | `0xe80000` | 1504K |
| `nvs_user` | data/nvs | `0xff8000` | 32K |

Worth copying the shape of this: dual OTA slots plus a large asset area. Our layout will want one app partition, a LittleFS deck partition, and room for a CJK font and per-card audio.

## Pin map

**Recovered 2026-07-29** by reading the live GPIO matrix over JTAG while the stock firmware ran. Reproduce any time with `./dev pins`. Not guessed, not inferred from a similar board.

| Function | GPIO | How it was identified |
|---|---|---|
| LCD SCLK | **9** | `SPI3_CLK_OUT` in the GPIO matrix |
| LCD MOSI | **10** | `SPI3_D_OUT` |
| LCD CS | **14** | `SPI3_CS0_OUT` |
| LCD DC | **8** | plain GPIO, toggles erratically through panel init |
| LCD RESET | **4** | plain GPIO, held low then released mid-init |
| LCD backlight | **13** | `LEDC_LS_SIG_OUT0`, PWM |
| Power / peripheral enable | **18** | plain GPIO, driven high ~200ms after reset, before all else |
| I2C SCL | **11** | `I2CEXT0_SCL_OUT` |
| I2C SDA | **12** | `I2CEXT0_SDA_OUT` |
| I2S MCLK | **5** | `I2S0_MCLK_OUT` |
| I2S BCK | **15** | `I2S0O_BCK_OUT` |
| I2S WS | **16** | `I2S0O_WS_OUT` |
| I2S DOUT (to ES8311) | **6** | `I2S0O_SD_OUT` |
| I2S DIN (from ES7210) | **7** | `I2S0I_SD_IN` |
| Button: power (short press only) | **2** | plain GPIO, confirmed 2026-07-29 by an on-device GPIO scan with the user pressing the button |
| Button: plus / Good | **40** | plain GPIO, confirmed 2026-07-29 by an on-device GPIO scan with the user pressing the button |
| Button: minus / Again | **39** | plain GPIO, confirmed 2026-07-29 by an on-device GPIO scan with the user pressing the button |

**The display is on SPI3, not SPI2/FSPI**, and uses a single data line — ordinary 4-wire SPI, not QSPI. Anything written against an FSPI example will need adjusting.

**GPIO18 is the one to be careful with.** It is driven high roughly 200ms after reset, earlier than the display, the codec, or anything else. On a battery-powered board that pattern means a power latch or peripheral rail enable. Drive it high early in our firmware too. If a first boot dies immediately or the screen never lights, this is the first thing to check.

**A hard reset on battery can drop power entirely, and this is expected, not a new bug.** During any reset all GPIOs float briefly, including whatever drives the GPIO18 latch, and the latch does not always survive that gap. Observed twice during this session's flashing (2026-07-29): `./dev flash` completes, the post-flash hard reset runs, and the board vanishes from USB — not wedged, genuinely powered off. Recovery is pressing the power button (a short press is enough; see the buttons section above).

**Telling power-off from a wedge (see below) apart:** check whether the board still enumerates.
- Powered off: the board disappears from the USB device tree entirely (e.g. it drops out of `ioreg`/`system_profiler SPUSBDataTree`, and the serial port node itself vanishes). Fix: press the power button.
- Wedged: the port stays present in the device tree but every operation on it fails (serial silent, OpenOCD's `libusb_get_string_descriptor_ascii()` failing). Fix: physical unplug/replug (see below).

Distinguishing DC from RESET was done by sampling `GPIO_OUT_REG` and `GPIO_ENABLE_REG` repeatedly across a reset: DC toggles constantly while the panel init sequence alternates command and data, whereas RESET makes a single clean low-to-high transition and then holds.

## Display configuration (confirmed on hardware)

| Setting | Value |
|---|---|
| Panel memory | 240 x 320 |
| Visible area | 240 x 284 |
| `y` gap | 36 |
| `mirror_x` | false |
| `mirror_y` | true |
| `swap_xy` | **false** |
| Colour order | **BGR** |

**Mirror and gap are not independent.** `mirror_y` reverses the row scan
direction, which moves the 36-row dead zone from one end of the panel to the
other, so the correct gap flips with it. Tuning them separately cannot
converge; always vary them as a pair. Finding this took an on-device
calibration mode driven by the physical buttons, which was far faster than
reflashing per combination.

**Apply mirror and gap AFTER `lvgl_port_add_disp()`.** The port applies its
own rotation settings (all false by default) while setting up, silently
undoing anything configured before it. This is subtle: the display looks
correct while an interactive calibration routine is running (because that runs
after LVGL is up) and reverts to mirrored the moment the same values are moved
into the init path. If the panel is mirrored despite the config being right,
check the ordering first.

**Do not enable `swap_xy`.** It puts the panel into a geometry the driver's
gap handling does not transform alongside, and the screen fills with static.
The UI is portrait already, matching the panel directly, so there is no
reason to reach for it.

**Colour order is BGR.** Under RGB, red and blue swap: amber renders blue and
blue renders orange, while green is unaffected.

### How the geometry was actually found

The fast route, in hindsight: the stock firmware's splash JPEG is embedded in
flash and its dimensions are the panel's visible size. Extracting it is a
`grep` for the JPEG magic plus a read of the SOF header, needs no hardware,
and is unambiguous.

The vendored `esp_lcd_nv3023` driver also ships `CASET 0x00..0xEF` and
`RASET 0x00..0x11B` in its init sequence, i.e. 240 x 284, which was sitting in
the repo the whole time.

Both beat photographing test patterns. The slow route failed largely because
writing rows **wider than the panel wraps**, silently corrupting the very
patterns being used as evidence, which produced a long chain of confident
wrong conclusions: a phantom transpose, a phantom MADCTL fault, and estimates
of the width ranging from 132 to 400 pixels.

### The three physical buttons (confirmed 2026-07-29)

JTAG sampling of `GPIO_IN` was inconclusive: the halt/resume cycle samples too
slowly, and GPIO26-37 dominate the results because on an N16R8 they carry the
SPI flash and octal PSRAM, so their traffic looks like activity. That
narrowed things to candidates (GPIO2, GPIO39, GPIO40, plus GPIO0/BOOT) but no
further.

What settled it, finally, on 2026-07-29: an on-device GPIO scan with the user
pressing each physical button in turn while the scan logged edges directly
over serial. This is the first time the mapping was actually verified against
a live button press rather than inferred — an earlier "identified" claim in
this doc predates this session and was not actually confirmed. The result is
unambiguous: **GPIO39 = minus, GPIO40 = plus, GPIO2 = power**. See the pin map
above.

**The power button is dual-function, and that matters for firmware design.**
A short press produces a clean, readable edge on GPIO2 (pulse width roughly
20-150ms) — that part behaves like an ordinary button. But a *long* press cuts
power at the hardware latch: the board switches off and drops off USB
entirely, with no chance for firmware to log or react. This means GPIO2
**cannot be relied on for UI input** — any firmware that requires the user to
tell short-press from long-press on this pin is gambling on the hardware
latch's timing, not the firmware's. Because of this, `main.cpp`'s button
mapping treats the power button as inert during review: when the answer is
hidden, any button (including power) reveals it; once revealed, only minus
(Again) and plus (Good) do anything, and the entire review loop is drivable
without ever touching the power button. See `firmware/main/main.cpp`.

### Still unknown

Inputs do not appear in the output routing, so these remain open:

- Touch reset line for the CST816 (touch INT is now known — see "Touch" below: it is unwired).
- The battery ADC channel.

## Touch (confirmed 2026-07-29)

### INT is not wired

A scan of every free candidate GPIO showed zero edges while the glass was
being touched. The CST816's INT line is not connected to the ESP32 at all;
the firmware must poll over I2C instead of waiting on an interrupt. The touch
reset line is still unknown.

### The CST816 must have auto-sleep disabled, or polling misses most touches

Polling without INT ran into the CST816's own auto-sleep: after a few seconds
idle it stops updating its coordinate registers, so a poll loop reads stale
data and touches appear to do nothing. Symptom, if this write is missing:
touch is "mostly dead, occasionally works" — not a wiring or geometry problem,
an auto-sleep problem. Fix: write register `0xFE = 0x01` (disable auto-sleep)
right after `esp_lcd_touch_new_i2c_cst816s`, at init. Implemented in
`firmware/main/main.cpp`, `touch_init()`.

### The touch film's coordinate frame is swapped and inverted relative to the panel

The film's native frame is 284x240 (note the axes are swapped versus the
240x284 visible panel): its x axis runs along the panel's long side (vertical
when the device is held with USB at the bottom), and its y axis runs along
the short side, inverted.

This was measured, not guessed, with an on-device calibration routine: send
`@cal` over the devctl protocol (the same `@`-prefixed CDC link used for
`@shot`/`@tap`). A red dot is shown at five known positions in turn; raw
touch coordinates are logged per tap (`RAWTOUCH` lines appear in the log only
while calibration is active); an affine transform is then fit host-side
against the five (known position, raw reading) pairs.

The fitted transform, implemented in `process_coordinates()` inside
`touch_init()` in `firmware/main/main.cpp`:

```
ui_x = 223.8 - 0.9231 * raw_y
ui_y = -16.8 + 0.9739 * raw_x
```

Residuals were 2-5px on all five calibration targets.

**Leave the touch driver's `swap_xy`/`mirror` flags false.** The affine
above already encodes the real swap, scale, and offset; the driver's flags
would additionally try to swap/mirror against the wrong axis lengths (they
assume the touch frame matches the panel frame, which it does not here), so
enabling them on top of the affine breaks it again.

**Never copy the panel's `mirror_y` to the touch config.** The LCD's
`mirror_y` (see "Display configuration" above) compensates the panel's own
scan direction and has nothing to do with the touch film's frame. Copying it
into the touch config was the original bug: taps landed in the wrong half of
the screen and the grade buttons were unreachable. The touch frame and the
panel's scan direction are independent facts and must be calibrated
independently.

**`@cal` is permanent tooling, not a one-off diagnostic.** Send `@cal\n` over
the CDC link (e.g. from a short Python one-liner using the existing
`devctl.py` transport) any time the touch film is replaced or the mapping is
suspected to have drifted; it cycles a dot through 5 positions (6s each) and
logs the raw readings needed to refit the transform.

A full hardware-verification pass on 2026-07-29 confirmed all four grade
buttons plus reveal work by touch, with taps landing within a few pixels of
target and ratings 1-4 each firing correctly on the first try.

## The board wedges during long USB sessions

Symptom: flash reads start failing part-way through a dump and then *every* read fails, at any offset and any baud rate, with or without the esptool stub. Serial output stops. OpenOCD starts failing at `libusb_get_string_descriptor_ascii()`.

This looks exactly like a defective flash region, and was misdiagnosed as one: chunks `0x200000`–`0x400000` failed repeatedly, so the range was assumed bad. The control test that settled it was re-reading `0x0`, a region that had already dumped successfully. It failed too. Nothing is wrong with the flash; the board's USB stack stops responding, and everything after that point fails regardless of address.

**Always run that control before blaming an address range.** Read a known-good offset; if it also fails, the device is wedged, not the flash.

Recovery is a physical unplug and replug, which requires the user. The board is battery-backed, so unplugging USB does not reset the SoC, and DTR/RTS toggling does not either.

Triggers seen so far: killing esptool mid-read, and long unbroken sequences of USB operations. `tools/dumpflash.sh` is chunked and resumable so a wedge costs only the chunks not yet fetched.

The approach: the stock firmware initialises the display, touch, and audio at boot, which means the ESP32-S3's GPIO matrix and IO MUX registers currently hold the answer. Halting the running chip over USB-JTAG and reading them gives the routing directly.

Registers of interest:

- `GPIO_FUNCn_OUT_SEL_CFG` at `0x60004554 + 4n` — for GPIO *n*, bits `[8:0]` give the peripheral output signal driving it. Reverse-lookup against `soc/gpio_sig_map.h` yields which pin carries `FSPICLK_OUT`, `FSPID_OUT`, `FSPICS0_OUT`, `I2CEXT0_SCL_OUT`, `I2S0O_BCK_OUT`, `LEDC_LS_SIG_OUT*`, and so on.
- `GPIO_FUNCn_IN_SEL_CFG` at `0x60004154 + 4n` — indexed by *signal*, giving the source GPIO. This is the useful direction for inputs such as `FSPIQ_IN` (display MISO) and `I2S0I_SD_IN` (mic data).
- IO MUX at `0x60009000 + 4(n+1)` — pins 10–14 can bypass the matrix and use the fast FSPI path, so these must be checked too or those pins will look unrouted.

Homebrew's `openocd` 0.12.0 cannot do this: it ships `target/esp32s3.cfg` but no `esp_usb_jtag` interface. Use the `openocd-esp32` build that `~/esp/esp-idf/install.sh` installs, with `board/esp32s3-builtin.cfg`.

Cross-check once recovered: the `BoxAudioCodec` class name means the vendor followed the ESP32-S3-BOX reference audio design, so the ES8311/ES7210 pins are likely to match it. Treat that as confirmation, never as the source.
