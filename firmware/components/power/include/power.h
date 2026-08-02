// Battery status: a tiny, deliberately honest API.
//
// The battery ADC line is confirmed: GPIO17 / ADC_UNIT_2 / ADC_CHANNEL_6, 12dB
// attenuation (see docs/pinout.md, confirmed 2026-08-02). battery_percent()
// reads it lazily on first call and converts raw counts to a percentage via
// an assumed 2:1 resistor divider and a LiPo open-circuit-voltage table; see
// power.cpp for the calibration observation backing the divider assumption.
//
// Charge status is GPIO47, confirmed 2026-08-02 by an unplug/replug diff
// (devctl's @gpinhist): active-high while the charger IC is actively topping
// off the battery. See power.cpp and docs/pinout.md for the full story,
// including the caveat that this hasn't been distinguished from a separate
// "charge complete" signal (only ever observed at high battery level so far).
#pragma once

namespace power {

// -1 means "no reading yet" -- either battery_percent() hasn't been called
// yet, or every ADC read so far has failed (e.g. ESP_ERR_TIMEOUT, which ADC2
// can legitimately return). Once a real reading has landed, transient
// failures fall back to the last good value instead of flapping to -1.
// Never fake a percentage: see CLAUDE.md and docs/pinout.md for why guessing
// hardware facts here has cost real debugging time before.
int battery_percent();

// True while the charger IC is actively charging the battery (GPIO47,
// active-high). See power.cpp for the confirmation story and its caveat.
bool charging();

}  // namespace power
