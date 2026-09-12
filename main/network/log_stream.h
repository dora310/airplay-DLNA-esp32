#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Polling-based live log transport.
 *
 * Hooks into esp_log via esp_log_set_vprintf() to capture all log output
 * into a ring buffer. The Logs page periodically drains new lines from
 * /api/logs/live. Logs continue to go to UART as normal.
 */

/**
 * Initialize the log capture ring buffer and hook esp_log output.
 * Call before web_server_start() so early logs are captured.
 */
esp_err_t log_stream_init(void);

/**
 * Register the /api/logs/live polling handler on the HTTP server.
 */
esp_err_t log_stream_register(httpd_handle_t server);

/** Copy retained warning/error history from SPIFFS. Returns bytes copied. */
size_t log_stream_read_persistent(char *buffer, size_t capacity);

/** Erase retained warning/error history. */
esp_err_t log_stream_clear_persistent(void);

/** Number of diagnostic bytes dropped because the non-blocking ring was full. */
uint32_t log_stream_persistent_dropped(void);
