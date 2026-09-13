#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Start the AirPlay RTSP server on port 7000
 * Handles initial connection requests from iOS devices
 */
esp_err_t rtsp_server_start(void);

/**
 * Stop the RTSP server
 */
void rtsp_server_stop(void);

/**
 * Set volume from AirPlay (in dB, range -144 to 0)
 * @param volume_db Volume in dB (0 = max, -144 = mute)
 */
void airplay_set_volume(float volume_db);

/**
 * Apply or remove a software mute at the final AirPlay PCM output.
 *
 * This does not change the saved volume. It is required for I2S-only DACs
 * such as PCM5102A, where dac_set_volume() is intentionally a no-op.
 */
void airplay_set_output_muted(bool muted);

/** Return true while the final AirPlay PCM output is software-muted. */
bool airplay_output_is_muted(void);

/**
 * Get current volume as Q15 scale factor for audio processing
 * @return Q15 fixed-point multiplier (0 = mute, 32768 = unity)
 */
int32_t airplay_get_volume_q15(void);

/**
 * Request resume during the AirPlay v1 grace period.
 * Called from the play/pause button when the source is still AirPlay
 * but the RTSP connection has been torn down (paused). Sends a DACP
 * playpause to the phone so it reconnects.
 */
void rtsp_server_request_resume(void);
