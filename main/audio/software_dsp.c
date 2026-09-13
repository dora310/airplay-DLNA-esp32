#include "software_dsp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "settings.h"
#include <math.h>
#include <string.h>

#define DSP_PI 3.14159265358979323846f

typedef struct {
  float b0, b1, b2, a1, a2;
  float z1[2], z2[2];
} biquad_t;

static software_dsp_config_t s_cfg;
static biquad_t s_eq[SOFTWARE_DSP_PEAK_BANDS];
static biquad_t s_crossover;
static uint32_t s_rate = 44100;
static uint32_t s_limiter_count;
static uint32_t s_clipping_count;
static bool s_speaker_protection;
static uint8_t s_speaker_threshold_percent = 90;
static float s_normalizer_gain = 1.0f;
static float s_eq_preamp_gain = 1.0f;
static float s_limiter_gain = 1.0f;
static SemaphoreHandle_t s_lock;

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static void sanitize_config(software_dsp_config_t *config) {
  config->balance = clampf(config->balance, -1.0f, 1.0f);
  config->normalization_target_dbfs =
      clampf(config->normalization_target_dbfs, -30.0f, -3.0f);
  if (config->channel < DSP_CHANNEL_STEREO ||
      config->channel > DSP_CHANNEL_SWAP) {
    config->channel = DSP_CHANNEL_STEREO;
  }
  if (config->crossover < DSP_CROSSOVER_OFF ||
      config->crossover > DSP_CROSSOVER_LOW_PASS) {
    config->crossover = DSP_CROSSOVER_OFF;
  }
  config->crossover_hz =
      clampf(config->crossover_hz, 30.0f, s_rate * 0.40f);
  bool boosted = false;
  for (int i = 0; i < SOFTWARE_DSP_PEAK_BANDS; i++) {
    config->bands[i].frequency_hz =
        clampf(config->bands[i].frequency_hz, 20.0f, s_rate * 0.45f);
    config->bands[i].gain_db =
        clampf(config->bands[i].gain_db, -12.0f, 12.0f);
    config->bands[i].q = clampf(config->bands[i].q, 0.1f, 12.0f);
    if (config->bands[i].gain_db > 0.01f) boosted = true;
  }
  if (boosted) config->limiter_enabled = true;
}

static void make_bypass(biquad_t *b) {
  memset(b, 0, sizeof(*b));
  b->b0 = 1.0f;
}

static void make_peak(biquad_t *b, float f, float gain, float q) {
  float w = 2.0f * DSP_PI * clampf(f, 20.0f, s_rate * 0.45f) / s_rate;
  float alpha = sinf(w) / (2.0f * clampf(q, 0.1f, 12.0f));
  float a = powf(10.0f, clampf(gain, -15.0f, 15.0f) / 40.0f);
  float a0 = 1.0f + alpha / a;
  b->b0 = (1.0f + alpha * a) / a0;
  b->b1 = (-2.0f * cosf(w)) / a0;
  b->b2 = (1.0f - alpha * a) / a0;
  b->a1 = b->b1;
  b->a2 = (1.0f - alpha / a) / a0;
  memset(b->z1, 0, sizeof(b->z1));
  memset(b->z2, 0, sizeof(b->z2));
}

static void make_crossover(biquad_t *b) {
  if (s_cfg.crossover == DSP_CROSSOVER_OFF) {
    make_bypass(b);
    return;
  }
  float w = 2.0f * DSP_PI * clampf(s_cfg.crossover_hz, 30.0f,
                                   s_rate * 0.40f) / s_rate;
  float c = cosf(w), alpha = sinf(w) / 1.41421356f;
  float a0 = 1.0f + alpha;
  if (s_cfg.crossover == DSP_CROSSOVER_LOW_PASS) {
    b->b0 = (1.0f - c) * 0.5f / a0;
    b->b1 = (1.0f - c) / a0;
    b->b2 = b->b0;
  } else {
    b->b0 = (1.0f + c) * 0.5f / a0;
    b->b1 = -(1.0f + c) / a0;
    b->b2 = b->b0;
  }
  b->a1 = (-2.0f * c) / a0;
  b->a2 = (1.0f - alpha) / a0;
  memset(b->z1, 0, sizeof(b->z1));
  memset(b->z2, 0, sizeof(b->z2));
}

static void rebuild(void) {
  float largest_boost_db = 0.0f;
  int boosted_bands = 0;
  for (int i = 0; i < SOFTWARE_DSP_PEAK_BANDS; i++) {
    make_peak(&s_eq[i], s_cfg.bands[i].frequency_hz,
              s_cfg.bands[i].gain_db, s_cfg.bands[i].q);
    if (s_cfg.bands[i].gain_db > 0.01f) {
      boosted_bands++;
      if (s_cfg.bands[i].gain_db > largest_boost_db) {
        largest_boost_db = s_cfg.bands[i].gain_db;
      }
    }
  }
  make_crossover(&s_crossover);

  // Positive EQ needs headroom before the biquads. Without this, a full-scale
  // DLNA decoder output clips as soon as a band is boosted. The extra 3 dB for
  // overlapping boosted bands is conservative without making EQ too quiet.
  float headroom_db = largest_boost_db;
  if (boosted_bands > 1) {
    headroom_db += 3.0f;
  }
  headroom_db = clampf(headroom_db, 0.0f, 18.0f);
  s_eq_preamp_gain = powf(10.0f, -headroom_db / 20.0f);
  s_limiter_gain = 1.0f;
}

esp_err_t software_dsp_init(uint32_t sample_rate) {
  if (s_lock) {
    software_dsp_set_sample_rate(sample_rate);
    return ESP_OK;
  }
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) return ESP_ERR_NO_MEM;
  s_rate = sample_rate ? sample_rate : 44100;
  memset(&s_cfg, 0, sizeof(s_cfg));
  s_cfg.limiter_enabled = true;
  s_cfg.normalization_target_dbfs = -16.0f;
  s_cfg.crossover_hz = 80.0f;
  const float defaults[SOFTWARE_DSP_PEAK_BANDS] = {60, 250, 1000, 4000, 12000};
  for (int i = 0; i < SOFTWARE_DSP_PEAK_BANDS; i++) {
    s_cfg.bands[i].frequency_hz = defaults[i];
    s_cfg.bands[i].q = 1.0f;
  }
  nvs_handle_t nvs;
  size_t saved_size = sizeof(s_cfg);
  if (nvs_open("airplay", NVS_READONLY, &nvs) == ESP_OK) {
    software_dsp_config_t saved;
    if (nvs_get_blob(nvs, "dsp_v3", &saved, &saved_size) == ESP_OK &&
        saved_size == sizeof(saved)) {
      s_cfg = saved;
    }
    nvs_close(nvs);
  }
  sanitize_config(&s_cfg);
  settings_maintenance_t maintenance;
  if (settings_get_maintenance(&maintenance) == ESP_OK) {
    s_speaker_protection = maintenance.speaker_protection_enabled;
    s_speaker_threshold_percent = maintenance.speaker_threshold_percent;
  }
  rebuild();
  return ESP_OK;
}

void software_dsp_set_sample_rate(uint32_t sample_rate) {
  if (!s_lock || !sample_rate) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_rate = sample_rate;
  rebuild();
  xSemaphoreGive(s_lock);
}

void software_dsp_get_config(software_dsp_config_t *config) {
  if (!config || !s_lock) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *config = s_cfg;
  xSemaphoreGive(s_lock);
}

esp_err_t software_dsp_set_config(const software_dsp_config_t *config) {
  if (!config || !s_lock) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_cfg = *config;
  sanitize_config(&s_cfg);
  rebuild();
  nvs_handle_t nvs;
  if (nvs_open("airplay", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_blob(nvs, "dsp_v3", &s_cfg, sizeof(s_cfg));
    nvs_commit(nvs);
    nvs_close(nvs);
  }
  xSemaphoreGive(s_lock);
  return ESP_OK;
}

static float run_biquad(biquad_t *b, float x, int ch) {
  float y = b->b0 * x + b->z1[ch];
  b->z1[ch] = b->b1 * x - b->a1 * y + b->z2[ch];
  b->z2[ch] = b->b2 * x - b->a2 * y;
  return y;
}

void software_dsp_reset_state(void) {
  if (!s_lock) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int i = 0; i < SOFTWARE_DSP_PEAK_BANDS; i++) {
    memset(s_eq[i].z1, 0, sizeof(s_eq[i].z1));
    memset(s_eq[i].z2, 0, sizeof(s_eq[i].z2));
  }
  memset(s_crossover.z1, 0, sizeof(s_crossover.z1));
  memset(s_crossover.z2, 0, sizeof(s_crossover.z2));
  s_normalizer_gain = 1.0f;
  s_limiter_gain = 1.0f;
  xSemaphoreGive(s_lock);
}

void software_dsp_process(int16_t *pcm, size_t frames, int channels) {
  if (!pcm || frames == 0 || channels != 2 || !s_lock) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (!s_cfg.enabled && !s_speaker_protection) {
    xSemaphoreGive(s_lock);
    return;
  }

  float peak = 1.0f;
  for (size_t i = 0; i < frames * 2; i++) {
    float a = fabsf((float)pcm[i]);
    if (a > peak) peak = a;
  }
  if (s_cfg.enabled && s_cfg.normalization_enabled) {
    float target = 32767.0f * powf(10.0f, s_cfg.normalization_target_dbfs / 20.0f);
    float wanted = clampf(target / peak, 0.25f, 4.0f);
    s_normalizer_gain += (wanted - s_normalizer_gain) * 0.0025f;
  } else {
    s_normalizer_gain = 1.0f;
  }

  float left_gain = s_cfg.enabled && s_cfg.balance > 0
                        ? 1.0f - s_cfg.balance : 1.0f;
  float right_gain = s_cfg.enabled && s_cfg.balance < 0
                         ? 1.0f + s_cfg.balance : 1.0f;
  for (size_t i = 0; i < frames; i++) {
    float l = pcm[i * 2], r = pcm[i * 2 + 1];
    if (s_cfg.enabled) {
      switch (s_cfg.channel) {
      case DSP_CHANNEL_MONO: l = r = (l + r) * 0.5f; break;
      case DSP_CHANNEL_LEFT: r = l; break;
      case DSP_CHANNEL_RIGHT: l = r; break;
      case DSP_CHANNEL_SWAP: { float t = l; l = r; r = t; break; }
      default: break;
      }
    }
    float preamp = s_cfg.enabled ? s_eq_preamp_gain : 1.0f;
    l *= left_gain * s_normalizer_gain * preamp;
    r *= right_gain * s_normalizer_gain * preamp;
    if (s_cfg.enabled) {
      for (int b = 0; b < SOFTWARE_DSP_PEAK_BANDS; b++) {
        l = run_biquad(&s_eq[b], l, 0);
        r = run_biquad(&s_eq[b], r, 1);
      }
      l = run_biquad(&s_crossover, l, 0);
      r = run_biquad(&s_crossover, r, 1);
    }
    if (!isfinite(l) || !isfinite(r)) {
      // A corrupt decoder block or invalid transient must never be converted
      // into a full-scale burst.
      l = 0.0f;
      r = 0.0f;
      s_clipping_count++;
    } else if (fabsf(l) > 32767.0f || fabsf(r) > 32767.0f) {
      s_clipping_count++;
    }
    if ((s_cfg.enabled && s_cfg.limiter_enabled) || s_speaker_protection) {
      const float threshold = s_speaker_protection
          ? 32767.0f * ((float)s_speaker_threshold_percent / 100.0f)
          : 32112.0f;
      float sample_peak = fmaxf(fabsf(l), fabsf(r));
      float wanted_gain = sample_peak > threshold ? threshold / sample_peak
                                                   : 1.0f;
      if (wanted_gain < 1.0f) {
        s_limiter_count++;
      }
      // Immediate attack prevents clipping. Slow recovery avoids gain chatter
      // and replaces two expensive/distorting tanhf() calls per audio frame.
      if (wanted_gain < s_limiter_gain) {
        s_limiter_gain = wanted_gain;
      } else {
        s_limiter_gain += (1.0f - s_limiter_gain) * 0.0005f;
      }
      l *= s_limiter_gain;
      r *= s_limiter_gain;
    }
    pcm[i * 2] = (int16_t)clampf(l, -32768.0f, 32767.0f);
    pcm[i * 2 + 1] = (int16_t)clampf(r, -32768.0f, 32767.0f);
  }
  xSemaphoreGive(s_lock);
}

uint32_t software_dsp_limiter_count(void) { return s_limiter_count; }

uint32_t software_dsp_clipping_count(void) { return s_clipping_count; }

bool software_dsp_limiter_active(void) {
  return (s_cfg.enabled && s_cfg.limiter_enabled) || s_speaker_protection;
}

bool software_dsp_speaker_protection_enabled(void) {
  return s_speaker_protection;
}

uint8_t software_dsp_speaker_threshold_percent(void) {
  return s_speaker_threshold_percent;
}

void software_dsp_set_speaker_protection(bool enabled,
                                         uint8_t threshold_percent) {
  if (threshold_percent < 50) threshold_percent = 50;
  if (threshold_percent > 98) threshold_percent = 98;
  if (!s_lock) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_speaker_protection = enabled;
  s_speaker_threshold_percent = threshold_percent;
  xSemaphoreGive(s_lock);
}

void software_dsp_reset_protection_counters(void) {
  s_limiter_count = 0;
  s_clipping_count = 0;
}
