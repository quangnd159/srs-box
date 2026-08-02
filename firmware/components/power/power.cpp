// Battery percentage from the confirmed battery-sense ADC line.
//
// GPIO17 / ADC_UNIT_2 / ADC_CHANNEL_6, 12dB attenuation. Confirmed on
// hardware 2026-08-02: stock-firmware strings show its PowerManager polling
// ADC_CHANNEL_6 every 3s, ADC1's copy of that channel index (GPIO7) is
// already the ES7210 mic data line so it can't be the battery, and a live
// @adc read of GPIO17 gave a stable raw ~2448-2450 with USB plugged and the
// battery presumably full. See docs/pinout.md.
//
// No other component in this firmware includes board_config.h -- pins there
// are only ever consumed directly by main.cpp -- so the pin is defined here
// instead, pointing back at docs/pinout.md rather than duplicating it there.

#include "power.h"

#include <cmath>
#include <cstdint>

#include <driver/gpio.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_err.h>
#include <esp_log.h>

namespace power {
namespace {

const char* TAG = "power";

// GPIO17, see docs/pinout.md "Pin map" (battery ADC, confirmed 2026-08-02).
constexpr adc_unit_t kAdcUnit = ADC_UNIT_2;
constexpr adc_channel_t kAdcChannel = ADC_CHANNEL_6;
constexpr adc_atten_t kAdcAtten = ADC_ATTEN_DB_12;
constexpr adc_bitwidth_t kAdcBitwidth = ADC_BITWIDTH_DEFAULT;

// GPIO47, see docs/pinout.md "Pin map" (charge status, confirmed 2026-08-02
// by the @gpinhist unplug/replug diff). Active-high while the charger IC is
// actively topping off the battery; reads clean with no pulls enabled.
constexpr gpio_num_t kChargePin = GPIO_NUM_47;

// ---------------------------------------------------------------------------
// Lazy one-time init. Callers poll roughly once a minute from the UI tick,
// so there's no need for a background task; the unit is created once and
// kept open for the life of the process (unlike devctl's @adc, which is a
// one-shot diagnostic and tears its unit down after every call).

bool g_init_attempted = false;
bool g_channel_ok = false;
adc_oneshot_unit_handle_t g_unit = nullptr;
adc_cali_handle_t g_cali = nullptr;
bool g_have_cali = false;

void init_once() {
  if (g_init_attempted) return;
  g_init_attempted = true;

  adc_oneshot_unit_init_cfg_t unit_cfg = {};
  unit_cfg.unit_id = kAdcUnit;
  if (adc_oneshot_new_unit(&unit_cfg, &g_unit) != ESP_OK) {
    ESP_LOGE(TAG, "adc_oneshot_new_unit failed");
    return;
  }

  adc_oneshot_chan_cfg_t chan_cfg = {};
  chan_cfg.atten = kAdcAtten;
  chan_cfg.bitwidth = kAdcBitwidth;
  if (adc_oneshot_config_channel(g_unit, kAdcChannel, &chan_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "adc_oneshot_config_channel failed");
    return;
  }
  g_channel_ok = true;

  // Best-effort: curve-fitting calibration needs eFuse bits that may not be
  // burnt on every chip revision. Fall back to a linear raw->mV approximation
  // if unsupported; either way battery_percent() keeps working.
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t cali_cfg = {};
  cali_cfg.unit_id = kAdcUnit;
  cali_cfg.atten = kAdcAtten;
  cali_cfg.bitwidth = kAdcBitwidth;
  g_have_cali = adc_cali_create_scheme_curve_fitting(&cali_cfg, &g_cali) == ESP_OK;
#endif
  if (!g_have_cali) {
    ESP_LOGW(TAG, "adc_cali unavailable, using linear raw->mV approximation");
  }
}

int raw_to_mv(int raw) {
  int mv = 0;
  if (g_have_cali && adc_cali_raw_to_voltage(g_cali, raw, &mv) == ESP_OK) {
    return mv;
  }
  // Linear approximation: 12dB attenuation reads the full 0-3.3V range across
  // ADC_BITWIDTH_DEFAULT's 12-bit (0-4095) range on the S3.
  return static_cast<int>((static_cast<int64_t>(raw) * 3300) / 4095);
}

// Standard LiPo open-circuit-voltage curve, millivolts -> percent, highest
// voltage first. Linearly interpolated between points, clamped to [0, 100].
struct OcvPoint {
  int mv;
  int percent;
};
constexpr OcvPoint kOcvTable[] = {
    {4200, 100}, {4060, 90}, {3980, 80}, {3920, 70}, {3870, 60}, {3820, 50},
    {3790, 40},  {3770, 30}, {3740, 20}, {3680, 10}, {3450, 5},  {3300, 0},
};
constexpr int kOcvTableLen = sizeof(kOcvTable) / sizeof(kOcvTable[0]);

int voltage_to_percent(int vbat_mv) {
  if (vbat_mv >= kOcvTable[0].mv) return 100;
  if (vbat_mv <= kOcvTable[kOcvTableLen - 1].mv) return 0;
  for (int i = 0; i < kOcvTableLen - 1; ++i) {
    const OcvPoint& hi = kOcvTable[i];
    const OcvPoint& lo = kOcvTable[i + 1];
    if (vbat_mv <= hi.mv && vbat_mv >= lo.mv) {
      const double t = static_cast<double>(vbat_mv - lo.mv) / (hi.mv - lo.mv);
      const double p = lo.percent + t * (hi.percent - lo.percent);
      return static_cast<int>(std::lround(p));
    }
  }
  return 0;  // unreachable given the bounds checks above
}

// ---------------------------------------------------------------------------
// Smoothing: a simple exponential moving average keeps the displayed percent
// from jittering read to read without any state machine. No attempt is made
// to force monotonicity (a real battery can dip under load); the EMA alone
// is enough to stop single-sample noise from being visible.

bool g_have_percent = false;
double g_smoothed_percent = 0.0;
int g_last_good_percent = -1;
constexpr double kSmoothingAlpha = 0.3;  // higher = follows new readings faster

// ---------------------------------------------------------------------------
// Charge status: GPIO47, lazily configured on first charging() call, same
// pattern as the ADC's init_once().

bool g_charge_pin_init = false;

void init_charge_pin_once() {
  if (g_charge_pin_init) return;
  g_charge_pin_init = true;

  gpio_config_t cfg = {};
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.pin_bit_mask = 1ULL << kChargePin;
  const esp_err_t err = gpio_config(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gpio_config(charge pin) failed: %s", esp_err_to_name(err));
  }
}

}  // namespace

int battery_percent() {
  init_once();
  if (!g_channel_ok) return g_last_good_percent;

  constexpr int kSamples = 16;
  int64_t sum = 0;
  int ok_count = 0;
  for (int i = 0; i < kSamples; ++i) {
    int raw = 0;
    const esp_err_t err = adc_oneshot_read(g_unit, kAdcChannel, &raw);
    if (err == ESP_OK) {
      sum += raw;
      ++ok_count;
    } else if (err != ESP_ERR_TIMEOUT) {
      ESP_LOGW(TAG, "adc_oneshot_read failed: %s", esp_err_to_name(err));
    }
    // ESP_ERR_TIMEOUT is expected occasionally (see docs/pinout.md) and is
    // silently skipped rather than logged on every poll.
  }
  if (ok_count == 0) return g_last_good_percent;

  const int raw_avg = static_cast<int>(sum / ok_count);
  const int mv = raw_to_mv(raw_avg);
  // Assumed 2:1 resistor divider: a live @adc read on 2026-08-02 gave raw
  // ~2449 at 12dB with USB plugged (battery presumably full), which is
  // ~2.1V pre-divider -> ~4.2V post-divider, matching a full LiPo. Nothing
  // about the divider itself (resistor values, exact ratio) is confirmed
  // beyond this one consistency check.
  const int vbat_mv = mv * 2;

  const int percent = voltage_to_percent(vbat_mv);
  if (!g_have_percent) {
    g_smoothed_percent = percent;
    g_have_percent = true;
  } else {
    g_smoothed_percent += kSmoothingAlpha * (percent - g_smoothed_percent);
  }
  g_last_good_percent = static_cast<int>(std::lround(g_smoothed_percent));
  if (g_last_good_percent < 0) g_last_good_percent = 0;
  if (g_last_good_percent > 100) g_last_good_percent = 100;
  return g_last_good_percent;
}

bool charging() {
  // GPIO47, confirmed 2026-08-02 by the @gpinhist unplug/replug diff: it read
  // 1 for the first ~3s of the sampling window (plugged, battery full,
  // charger topping off), dropped to 0 exactly at the unplug moment, and
  // stayed 0 after replug (recharge didn't resume because the battery was
  // already above the charger IC's recharge threshold). A fresh @gpin while
  // plugged and full also reads 0, consistent with "actively charging" and
  // ruling out plain VBUS-present. See docs/pinout.md.
  //
  // Caveat: this distinguishes "actively charging" from "not currently
  // charging", not necessarily "charging" from "charge complete" -- that
  // would need an observation starting from a drained battery. Revisit if
  // charging a drained battery ever shows this pin behaving differently.
  init_charge_pin_once();
  return gpio_get_level(kChargePin) == 1;
}

}  // namespace power
