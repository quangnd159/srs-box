// Device-control protocol: lets ./dev and the web app drive the board over
// the same USB-Serial/JTAG link that carries ESP_LOG output, disambiguated
// by a leading '@'. Host side and full wire format: tools/devctl.py.
#pragma once

namespace devctl {

// Installs the USB-Serial/JTAG RX path, registers a virtual pointer input
// device for @tap/@swipe, and starts the command task. Call once, after the
// UI is up (it takes the LVGL lock briefly to register the indev).
// `on_cal`, if given, runs when the host sends @cal (used by the on-device
// touch calibration routine).
void init(void (*on_cal)() = nullptr);

}  // namespace devctl
