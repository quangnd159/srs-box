// Device-control protocol: lets ./dev and the web app drive the board over
// the same USB-Serial/JTAG link that carries ESP_LOG output, disambiguated
// by a leading '@'. Host side and full wire format: tools/devctl.py.
#pragma once

#include <cstdint>

namespace devctl {

// Installs the USB-Serial/JTAG RX path, registers a virtual pointer input
// device for @tap/@swipe, and starts the command task. Call once, after the
// UI is up (it takes the LVGL lock briefly to register the indev).
// `on_cal`, if given, runs when the host sends @cal (used by the on-device
// touch calibration routine).
// `on_time_set`, if given, runs after @time successfully calls
// settimeofday(), with the unix-epoch seconds that were just set. There is
// no RTC on this board (see CLAUDE.md), so main.cpp uses this hook to
// persist the value immediately rather than waiting for the next periodic
// tick, in case power is lost soon after.
// `on_gap`, if given, runs when the host sends @gap <y>: it applies a new
// panel y-offset live, for aligning the write window to the glass without
// reflashing (the "put calibration on the device" lesson).
//
// Also implements the file-transfer half of the protocol (@fput, @fget,
// @fls, @fdel, @reboot), documented in full in docs/sync-protocol.md, plus
// self-contained hardware diagnostics with no callback of their own:
// @adc (one-shot survey of GPIO1/GPIO3 on ADC1 and GPIO17 on ADC2 -- the
// battery ADC line, now confirmed on GPIO17, see docs/pinout.md), @gpin
// (digital snapshot of the free GPIOs being swept for the still-unknown
// charge-detect line), and @gpinhist (start/stop/dump a background sampler
// over the same pins, for diffing levels across a USB unplug/replug window).
void init(void (*on_cal)() = nullptr, void (*on_time_set)(int64_t epoch_seconds) = nullptr,
          void (*on_gap)(int y_gap) = nullptr);

}  // namespace devctl
