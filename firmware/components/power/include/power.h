// Battery status: a tiny, deliberately honest API.
//
// The battery ADC channel is not yet identified (see docs/pinout.md, "Still
// unknown"), so there is nothing real to read yet. This stub exists so the
// UI can be built against a stable interface now and wired to real readings
// later without touching ui/ again -- returning -1 / false is the contract,
// not a placeholder for "some day this returns a real number", so the UI
// must treat -1 as "unknown", never as "assume some percentage".
#pragma once

namespace power {

// -1 means unknown (no ADC channel wired up yet). Never fake a percentage:
// see CLAUDE.md and docs/pinout.md for why guessing hardware facts here has
// cost real debugging time before.
int battery_percent();

// Always false until charge-state detection is wired up.
bool charging();

}  // namespace power
