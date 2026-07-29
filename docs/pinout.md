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
| Buttons | at least 6 | `InitializeButtons()` registers six handlers. Exact GPIOs unknown. |

Display resolution is **not yet confirmed**. The stock firmware never logs it, and NV3023 panels ship in several sizes.

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

**The display is on SPI3, not SPI2/FSPI**, and uses a single data line — ordinary 4-wire SPI, not QSPI. Anything written against an FSPI example will need adjusting.

**GPIO18 is the one to be careful with.** It is driven high roughly 200ms after reset, earlier than the display, the codec, or anything else. On a battery-powered board that pattern means a power latch or peripheral rail enable. Drive it high early in our firmware too. If a first boot dies immediately or the screen never lights, this is the first thing to check.

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
Landscape orientation needs a different approach.

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

### Still unknown

Inputs do not appear in the output routing, so these remain open:

- **The three physical buttons** (power, minus, plus). JTAG sampling of `GPIO_IN` was inconclusive: the halt/resume cycle samples too slowly, and GPIO26–37 dominate the results because on an N16R8 they carry the SPI flash and octal PSRAM, so their traffic looks like activity. Candidates after excluding those are GPIO2, GPIO39, GPIO40, plus GPIO0 (BOOT). The bring-up firmware scans all plausible pins at 20ms and logs presses directly, which is far faster than doing this over JTAG.
- Touch interrupt and touch reset lines for the CST816.
- The battery ADC channel.
- **Display resolution.** The stock firmware never logs it and NV3023 panels ship in several sizes; the seller says 2.0 inch. Assumed 240×320 for now. The bring-up firmware draws a 4px red border at the assumed edges plus 20px green ticks, so one look at the panel settles it.

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
