#pragma once

#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * OLED display module - Shows track metadata, playback position &
 * progress bar. Registers as an RTSP event observer to receive metadata
 * updates automatically.
 *
 * When CONFIG_DISPLAY_ENABLED is not set, display_init() is an inline no-op
 * and no display code is compiled or linked.
 */

#ifdef CONFIG_DISPLAY_ENABLED

/**
 * Initialize the OLED display and register for RTSP events.
 *
 * @param bus  Pre-initialised bus handle to share with the board:
 *             - I2C mode: pass an i2c_master_bus_handle_t
 *             - SPI mode: pass (void*)(intptr_t)spi_host_device_t
 *             Pass NULL to let the display component initialise its own bus
 *             (uses the GPIO pins from Kconfig).
 */
void display_init(void *bus);

/** Update now-playing text and timing from a non-RTSP source such as DLNA. */
void display_notify_metadata(const char *title, const char *artist,
                             const char *album, uint32_t duration_secs,
                             uint32_t position_secs);

/** Set the visible transport state for a non-RTSP source. */
void display_notify_playback(bool paused);

/** Return the display to its standby screen. */
void display_notify_stopped(void);

#else

static inline void display_init(void *bus) {
  (void)bus;
}

static inline void display_notify_metadata(const char *title,
                                           const char *artist,
                                           const char *album,
                                           uint32_t duration_secs,
                                           uint32_t position_secs) {
  (void)title;
  (void)artist;
  (void)album;
  (void)duration_secs;
  (void)position_secs;
}

static inline void display_notify_playback(bool paused) { (void)paused; }

static inline void display_notify_stopped(void) {}

#endif
