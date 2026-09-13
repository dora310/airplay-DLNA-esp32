/**
 * Source-agnostic playback controller.
 *
 * For AirPlay:
 *   - Play/Pause: forwarded when a usable DACP service was discovered;
 *     otherwise a non-persistent software output gate provides local pause.
 *   - Next/Prev: forwarded via DACP when available (AirPlay 1 only).
 *   - Volume: adjusted locally (DAC + NVS persistence) and mirrored
 *     to the source via DACP when available.
 *
 * For Bluetooth:
 *   - All commands sent as AVRCP passthrough to source device
 *   - Source device controls playback and sends volume back
 */

#include "playback_control.h"

#include "dac.h"
#include "dacp_client.h"
#include "rtsp_events.h"
#include "rtsp_server.h"
#include "settings.h"
#include "dlna_renderer.h"

#include "esp_log.h"

#ifdef CONFIG_BT_A2DP_ENABLE
#include "a2dp_sink.h"
#endif

static const char *TAG = "playback_ctrl";

#define VOLUME_STEP_DB 3.0f
#define VOLUME_MIN_DB  (-30.0f)
#define VOLUME_MAX_DB  0.0f

static playback_source_t s_source = PLAYBACK_SOURCE_NONE;
static bool s_muted = false;

esp_err_t playback_control_init(void) {
  dacp_init();
  ESP_LOGI(TAG, "Playback control initialized");
  return ESP_OK;
}

void playback_control_set_source(playback_source_t source) {
  s_muted = false;
  airplay_set_output_muted(false);
  s_source = source;
  ESP_LOGI(TAG, "Source set to %d", source);
}

playback_source_t playback_control_get_source(void) {
  return s_source;
}

// ============================================================================
// AirPlay local volume helpers
// ============================================================================

static float clamp_volume(float db) {
  if (db < VOLUME_MIN_DB) {
    return VOLUME_MIN_DB;
  }
  if (db > VOLUME_MAX_DB) {
    return VOLUME_MAX_DB;
  }
  return db;
}

// Convert AirPlay dB (-30..0) to DACP percent (0..100)
static float db_to_dacp_percent(float db) {
  if (db <= VOLUME_MIN_DB) {
    return 0.0f;
  }
  if (db >= VOLUME_MAX_DB) {
    return 100.0f;
  }
  return ((db - VOLUME_MIN_DB) / (VOLUME_MAX_DB - VOLUME_MIN_DB)) * 100.0f;
}

static void airplay_adjust_volume(float step_db) {
  float current_db;
  if (settings_get_volume(&current_db) != ESP_OK) {
    current_db = -15.0f; // default 50 %
  }

  float new_db = clamp_volume(current_db + step_db);
  airplay_set_volume(new_db);

  // The final software mute gate remains at zero while muted, so the desired
  // post-unmute hardware level can still be updated safely here.
  dac_set_volume(new_db);

  ESP_LOGI(TAG, "AirPlay volume: %.1f -> %.1f dB%s", current_db, new_db,
           s_muted ? " (muted)" : "");

  // Notify AirPlay client via DACP (best-effort, don't block local action)
  dacp_send_volume(db_to_dacp_percent(new_db));
}

// ============================================================================
// Public API
// ============================================================================

static void set_airplay_local_mute(bool muted) {
  airplay_set_output_muted(muted);
  s_muted = muted;
  ESP_LOGI(TAG, "AirPlay software output %s (saved volume unchanged)",
           muted ? "muted" : "unmuted");
}

esp_err_t playback_control_play_pause(void) {
  switch (s_source) {
  case PLAYBACK_SOURCE_AIRPLAY: {
    // Always release a local fallback pause locally. Otherwise a DACP service
    // that appears later could leave the PCM output muted while toggling the
    // phone in the opposite direction.
    if (s_muted) {
      set_airplay_local_mute(false);
      rtsp_events_emit(RTSP_EVENT_PLAYING, NULL);
      return ESP_OK;
    }
    if (dacp_can_control()) {
      // Tell the source to toggle playback — it will FLUSH the stream
      // on pause and RECORD on resume, so we don't need local muting.
      // Signal the v1 grace period loop (if active) so it sends the
      // DACP command at the right time and keeps waiting for reconnect.
      // If not in a grace period, the flag is harmless.
      rtsp_server_request_resume();
      dacp_send_playpause();
      ESP_LOGI(TAG, "AirPlay play/pause sent via DACP");
      return ESP_OK;
    } else {
      // AirPlay 2 normally omits usable DACP remote control. Use the final
      // software gain gate so this works on a register-less PCM5102A.
      set_airplay_local_mute(true);
      rtsp_events_emit(RTSP_EVENT_PAUSED, NULL);
      return ESP_OK;
    }
  }
#ifdef CONFIG_BT_A2DP_ENABLE
  case PLAYBACK_SOURCE_BLUETOOTH:
    bt_a2dp_send_playpause();
    return ESP_OK;
#endif
  case PLAYBACK_SOURCE_DLNA:
    dlna_renderer_toggle_pause();
    return ESP_OK;
  default:
    ESP_LOGI(TAG, "Play/pause: no active source (source=%d)", s_source);
    return ESP_ERR_INVALID_STATE;
  }
}

void playback_control_volume_up(void) {
  switch (s_source) {
  case PLAYBACK_SOURCE_AIRPLAY:
    airplay_adjust_volume(VOLUME_STEP_DB);
    break;
#ifdef CONFIG_BT_A2DP_ENABLE
  case PLAYBACK_SOURCE_BLUETOOTH:
    bt_a2dp_send_volume_up();
    break;
#endif
  case PLAYBACK_SOURCE_DLNA:
    dlna_renderer_volume_step(5);
    break;
  default:
    break;
  }
}

void playback_control_volume_down(void) {
  switch (s_source) {
  case PLAYBACK_SOURCE_AIRPLAY:
    airplay_adjust_volume(-VOLUME_STEP_DB);
    break;
#ifdef CONFIG_BT_A2DP_ENABLE
  case PLAYBACK_SOURCE_BLUETOOTH:
    bt_a2dp_send_volume_down();
    break;
#endif
  case PLAYBACK_SOURCE_DLNA:
    dlna_renderer_volume_step(-5);
    break;
  default:
    break;
  }
}

esp_err_t playback_control_next(void) {
  switch (s_source) {
  case PLAYBACK_SOURCE_AIRPLAY:
    if (!dacp_can_control()) {
      ESP_LOGW(TAG, "AirPlay next unavailable: source has no DACP service");
      return ESP_ERR_NOT_SUPPORTED;
    }
    dacp_send_next();
    ESP_LOGI(TAG, "AirPlay next track via DACP");
    return ESP_OK;
#ifdef CONFIG_BT_A2DP_ENABLE
  case PLAYBACK_SOURCE_BLUETOOTH:
    bt_a2dp_send_next();
    return ESP_OK;
#endif
  default:
    return ESP_ERR_NOT_SUPPORTED;
  }
}

esp_err_t playback_control_prev(void) {
  switch (s_source) {
  case PLAYBACK_SOURCE_AIRPLAY:
    if (!dacp_can_control()) {
      ESP_LOGW(TAG, "AirPlay previous unavailable: source has no DACP service");
      return ESP_ERR_NOT_SUPPORTED;
    }
    dacp_send_prev();
    ESP_LOGI(TAG, "AirPlay prev track via DACP");
    return ESP_OK;
#ifdef CONFIG_BT_A2DP_ENABLE
  case PLAYBACK_SOURCE_BLUETOOTH:
    bt_a2dp_send_prev();
    return ESP_OK;
#endif
  default:
    return ESP_ERR_NOT_SUPPORTED;
  }
}

esp_err_t playback_control_toggle_mute(void) {
  switch (s_source) {
  case PLAYBACK_SOURCE_AIRPLAY:
    set_airplay_local_mute(!s_muted);
    return ESP_OK;
#ifdef CONFIG_BT_A2DP_ENABLE
  case PLAYBACK_SOURCE_BLUETOOTH:
    // Bluetooth uses AVRCP absolute volume — no dedicated mute. Log.
    ESP_LOGI(TAG, "Bluetooth: mute toggle not supported (use source device)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
  case PLAYBACK_SOURCE_DLNA:
    dlna_renderer_toggle_mute();
    return ESP_OK;
  default:
    ESP_LOGI(TAG, "Toggle mute: no active source (source=%d)", s_source);
    return ESP_ERR_INVALID_STATE;
  }
}

bool playback_control_is_muted(void) {
  if (s_source == PLAYBACK_SOURCE_DLNA) {
    return dlna_renderer_is_muted();
  }
  return s_source == PLAYBACK_SOURCE_AIRPLAY ? airplay_output_is_muted()
                                             : s_muted;
}

int playback_control_get_volume_percent(void) {
  if (s_muted) {
    return 0;
  }
  float db;
  if (settings_get_volume(&db) != ESP_OK) {
    db = -15.0f;
  }
  return (int)(db_to_dacp_percent(clamp_volume(db)) + 0.5f);
}
