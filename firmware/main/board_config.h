// Pin map for the DB MEGA3D 3ST (ostb-xiaozhi-3st).
//
// Recovered by reading the live GPIO matrix over JTAG while the stock
// firmware ran; see docs/pinout.md. Reproduce with `./dev pins`.
// These are measured, not guessed. Do not "fix" them against a similar board.

#pragma once

#include <driver/gpio.h>

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------
// Driven high ~200ms after reset by the stock firmware, before the display or
// codec come up. On a battery-powered board that is a power latch or rail
// enable. Drive it high first thing; if the board dies on boot or the panel
// never lights, suspect this before anything else.
#define PIN_POWER_ENABLE     GPIO_NUM_18

// ---------------------------------------------------------------------------
// Display: NV3023 over SPI3, 4-wire (single data line, not QSPI)
// ---------------------------------------------------------------------------
#define PIN_LCD_SCLK         GPIO_NUM_9
#define PIN_LCD_MOSI         GPIO_NUM_10
#define PIN_LCD_CS           GPIO_NUM_14
#define PIN_LCD_DC           GPIO_NUM_8
#define PIN_LCD_RST          GPIO_NUM_4
#define PIN_LCD_BACKLIGHT    GPIO_NUM_13   // PWM via LEDC channel 0

#define LCD_SPI_HOST         SPI3_HOST
#define LCD_PIXEL_CLOCK_HZ   (40 * 1000 * 1000)

// The NV3023 maxes out at 132x162 -- it has 396 source channels (132 columns)
// and 162 gate lines. Its GM0/GM1/GM2 strap pins select one of six geometries:
// 132x162, 128x128, 120x160, 128x160, 130x130, 132x132.
//
// Driving it wider than the strapped width makes writes wrap, which looks
// exactly like a mirrored or rotated image. If the display ever looks
// scrambled, suspect this before any orientation flag.
#define LCD_H_RES            240
#define LCD_V_RES            284

// The controller has 320 rows of memory but the panel shows only some of
// them. The original probes (splash JPEG 284x240; lines at y=296/308 visible,
// y=320 not; nothing at y=0) put the visible window at the LAST 284 rows,
// i.e. gap 36 -- but their ~12-row granularity hid an 8-row error that
// surfaced as a 1mm dead band above the UI on the glass.
//
// Remeasured 2026-07-30 with the @gap edge markers (magenta line on the
// first framebuffer row, cyan on the last, stepped live via `./dev gap`):
// the bezel opening actually exposes ~288 rows, and 28 is the offset that
// CENTERS the 284-row window in it, leaving a symmetric ~2-row black margin
// under each bezel edge.
//
// 36 is nevertheless the deliberate choice, tried both ways on the glass:
// the panel is edge-lit from the bottom and has a ~1mm backlight glow band
// there, which 28 exposes as a white bar under the grade buttons (the
// framebuffer margin covering it is black, and no drawn pixel can darken
// backlight bleed). 36 parks the bright buttons over the glow so it is
// invisible, and the ~8 unwritten rows this leaves at the TOP read as clean
// black bezel thanks to the full-GRAM clear at init. The bottom ~5px of the
// framebuffer (the under-button margin) lands off-glass and is sacrificed
// knowingly. Do not "correct" this to 28 without re-looking at the glow.
#define LCD_OFFSET_X         0
#define LCD_OFFSET_Y         36

// The panel is 240 x 284 visible (see LCD_H_RES/LCD_V_RES and
// LCD_OFFSET_Y above), used portrait, straight through with no rotation.
// swap_xy stays false (see LCD_SWAP_XY), so the UI canvas is 240x284, not a
// swapped 284x240.
#define UI_H_RES             240
#define UI_V_RES             284

// Panel orientation. The NV3023 can be wired with either scan direction and
// the driver cannot know which; this is set by looking at the screen.
// Text renders mirrored without it, which is unmistakable.
// Matches all three upstream xiaozhi NV3023 boards. The driver writes
// MADCTL=0x00 during panel_init and its mirror() both sets and clears bits,
// so these are honoured exactly as given.
// Confirmed on the device: text readable, nothing clipped, no static band.
// Mirror and gap are NOT independent -- mirror_y reverses the scan direction,
// which moves the 36-row dead zone to the other end of the panel, so the
// correct gap changes with it. They must always be chosen as a pair.
//
// swap_xy must stay false: enabling it put the panel into a geometry the
// driver's gap handling does not transform alongside, which produced static.
// The UI is portrait already (see UI_H_RES/UI_V_RES above), so this is moot
// in practice -- noted here only so nobody re-tries it while chasing some
// other bug.
#define LCD_MIRROR_X         false
#define LCD_MIRROR_Y         true
#define LCD_SWAP_XY          false

// ---------------------------------------------------------------------------
// Battery ADC: confirmed 2026-08-02
// ---------------------------------------------------------------------------
// ADC2_CHANNEL_6, 12dB attenuation. Confirmed by stock-firmware strings
// showing its PowerManager polling ADC_CHANNEL_6, by ruling out ADC1's copy
// of that channel index (GPIO7, already the ES7210 mic data line), and by a
// live @adc read giving a stable battery-shaped raw ~2449 with USB plugged.
// See docs/pinout.md. Not consumed from here: no component besides main.cpp
// includes this header, so firmware/components/power/power.cpp defines the
// same pin/unit/channel locally, pointing back at docs/pinout.md.
#define PIN_BATTERY_ADC      GPIO_NUM_17
#define BATTERY_ADC_UNIT     ADC_UNIT_2
#define BATTERY_ADC_CHANNEL  ADC_CHANNEL_6

// Charge status: confirmed 2026-08-02 by an @gpinhist unplug/replug diff --
// active-high while the charger IC is actively topping off the battery. Read
// 1 for ~3s while plugged and topping off, dropped to 0 at the unplug
// instant, and stayed 0 after replug (battery was already above the
// recharge threshold, so charging didn't resume) and on a fresh plugged/full
// @gpin -- ruling out plain VBUS-present. Not yet distinguished from a
// separate "charge complete" pin; that would need a drained-battery
// observation. See docs/pinout.md. Same caveat as PIN_BATTERY_ADC above:
// power.cpp defines this pin locally rather than including this header.
#define PIN_CHARGE_STATUS    GPIO_NUM_47

// ---------------------------------------------------------------------------
// I2C: CST816 touch controller, ES8311 and ES7210 audio codecs
// ---------------------------------------------------------------------------
#define PIN_I2C_SCL          GPIO_NUM_11
#define PIN_I2C_SDA          GPIO_NUM_12
#define I2C_FREQ_HZ          400000

#define I2C_ADDR_CST816      0x15
#define I2C_ADDR_ES8311      0x18
#define I2C_ADDR_ES7210      0x40

// ---------------------------------------------------------------------------
// Audio (unused: this build has no audio, but the pins are recorded)
// ---------------------------------------------------------------------------
#define PIN_I2S_MCLK         GPIO_NUM_5
#define PIN_I2S_BCK          GPIO_NUM_15
#define PIN_I2S_WS           GPIO_NUM_16
#define PIN_I2S_DOUT         GPIO_NUM_6
#define PIN_I2S_DIN          GPIO_NUM_7

// ---------------------------------------------------------------------------
// Buttons: confirmed on-device 2026-07-29
// ---------------------------------------------------------------------------
// An on-device GPIO scan logged presses directly over serial while the user
// pressed each physical button in turn, which settled these three
// unambiguously. See docs/pinout.md.
//
// PIN_BTN_POWER is dual-function and cannot be used as ordinary UI input: a
// short press gives a clean, readable edge (~20-150ms), but a long press cuts
// power at the hardware latch and the board drops off USB immediately, with
// no chance for firmware to react. main.cpp treats it as inert once the
// answer is revealed; the review loop is fully drivable with PIN_BTN_MINUS
// and PIN_BTN_PLUS alone. See docs/pinout.md for the full story.
#define PIN_BTN_POWER        GPIO_NUM_2   // power; short-press only, see above
#define PIN_BTN_PLUS         GPIO_NUM_40  // plus / Good
#define PIN_BTN_MINUS        GPIO_NUM_39  // minus / Again
