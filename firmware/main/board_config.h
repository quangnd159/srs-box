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

// The controller has 320 rows of memory but the panel only shows 284 of them,
// and the visible window is the LAST 284 rows -- so the gap is 320-284 = 36.
//
// Confirmed three ways: the stock firmware's splash JPEG is 284x240; probe
// lines at y=296 and y=308 were visible while y=320 was not; and a border at
// y=0 never appeared while only the y>=36 sliver of a 50px origin block did.
#define LCD_OFFSET_X         0
#define LCD_OFFSET_Y         36

// The device is used in landscape (short edge vertical), so the UI is rotated
// 90 degrees. LVGL then presents a 280x240 canvas.
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
// Landscape therefore needs a different approach than a hardware axis swap.
#define LCD_MIRROR_X         false
#define LCD_MIRROR_Y         true
#define LCD_SWAP_XY          false

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
// Buttons: not yet identified
// ---------------------------------------------------------------------------
// The board has power, minus and plus buttons. JTAG sampling of GPIO_IN
// narrowed them to these candidates. GPIO26-37 were excluded because on an
// N16R8 they carry the SPI flash and octal PSRAM, so their activity is memory
// traffic rather than input.
#define BUTTON_CANDIDATES    {GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_17, \
                              GPIO_NUM_21, GPIO_NUM_38, GPIO_NUM_39,           \
                              GPIO_NUM_40, GPIO_NUM_41, GPIO_NUM_42,           \
                              GPIO_NUM_45, GPIO_NUM_46, GPIO_NUM_47, GPIO_NUM_48}

// GPIO0 is the strapping/BOOT pin and is almost always a button on these
// boards, but it is scanned separately so a press cannot be confused with the
// bootloader entry condition.
#define PIN_BOOT_BUTTON      GPIO_NUM_0
