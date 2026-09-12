/**
 * Polling-based log streaming over HTTP.
 *
 * Intercepts ESP-IDF log output via esp_log_set_vprintf(), stores lines
 * in a ring buffer. The browser periodically drains new lines through
 * /api/logs/live. UART output is preserved.
 */

#include "log_stream.h"
#include "spiram_task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ring buffer size — must be power of two for masking. */
#define LOG_RING_SIZE 8192
#define LOG_RING_MASK (LOG_RING_SIZE - 1)
#define DIAG_RING_SIZE 4096
#define DIAG_RING_MASK (DIAG_RING_SIZE - 1)
#define DIAG_FILE "/spiffs/diagnostics.log"
#define DIAG_FILE_OLD "/spiffs/diagnostics.old.log"
#define DIAG_FILE_MAX 12288

#define LIVE_LOG_CHUNK 2048

static char *s_ring;
static volatile size_t s_head; /* next write position  */
static volatile size_t s_tail; /* next read position   */
static SemaphoreHandle_t s_mutex;
static char *s_diag_ring;
static volatile size_t s_diag_head;
static volatile size_t s_diag_tail;
static volatile uint32_t s_diag_dropped;

static vprintf_like_t s_orig_vprintf;

/* ------------------------------------------------------------------ */
/*  Ring buffer helpers (protected by s_mutex)                         */
/* ------------------------------------------------------------------ */

static inline size_t ring_used(void) {
  return (s_head - s_tail) & LOG_RING_MASK;
}

static void ring_write(const char *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    /* If head is about to overwrite tail, discard oldest byte. */
    if (((s_head + 1) & LOG_RING_MASK) == (s_tail & LOG_RING_MASK)) {
      s_tail = (s_tail + 1) & LOG_RING_MASK;
    }
    s_ring[s_head & LOG_RING_MASK] = data[i];
    s_head = (s_head + 1) & LOG_RING_MASK;
  }
}

static void diag_ring_write(const char *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    size_t next = (s_diag_head + 1) & DIAG_RING_MASK;
    if (next == (s_diag_tail & DIAG_RING_MASK)) {
      s_diag_dropped++;
      break;
    }
    s_diag_ring[s_diag_head & DIAG_RING_MASK] = data[i];
    s_diag_head = next;
  }
}

static size_t diag_ring_read(char *buf, size_t max) {
  size_t used = (s_diag_head - s_diag_tail) & DIAG_RING_MASK;
  if (used > max) used = max;
  for (size_t i = 0; i < used; i++) {
    buf[i] = s_diag_ring[s_diag_tail & DIAG_RING_MASK];
    s_diag_tail = (s_diag_tail + 1) & DIAG_RING_MASK;
  }
  return used;
}

static bool is_diagnostic_line(const char *line) {
  if (!line) return false;
  while (*line == '\r' || *line == '\n' || *line == ' ') line++;
  return ((line[0] == 'E' || line[0] == 'W') &&
          (line[1] == ' ' || line[1] == '(')) ||
         strstr(line, "E (") != NULL || strstr(line, "W (") != NULL;
}

static size_t ring_read(char *buf, size_t max) {
  size_t avail = ring_used();
  if (avail > max) {
    avail = max;
  }
  for (size_t i = 0; i < avail; i++) {
    buf[i] = s_ring[s_tail & LOG_RING_MASK];
    s_tail = (s_tail + 1) & LOG_RING_MASK;
  }
  return avail;
}

/* ------------------------------------------------------------------ */
/*  Log hook — called from any task/ISR-safe context by esp_log       */
/* ------------------------------------------------------------------ */

static int log_vprintf_hook(const char *fmt, va_list args) {
  /* A va_list is consumed by vprintf, so make independent copies for UART
     and capture. This also fixes truncated browser/persistent log lines. */
  va_list uart_args;
  va_list format_args;
  va_copy(uart_args, args);
  va_copy(format_args, args);
  int ret = s_orig_vprintf(fmt, uart_args);
  va_end(uart_args);

  char buf[256];
  int len = vsnprintf(buf, sizeof(buf), fmt, format_args);
  va_end(format_args);

  if (len > 0) {
    if ((size_t)len >= sizeof(buf)) {
      len = sizeof(buf) - 1;
    }
    if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
      ring_write(buf, (size_t)len);
      if (is_diagnostic_line(buf)) {
        diag_ring_write(buf, (size_t)len);
        if (buf[len - 1] != '\n') diag_ring_write("\n", 1);
      }
      xSemaphoreGive(s_mutex);
    }
    /* If the mutex is held we silently drop — better than blocking a log call.
     */
  }
  return ret;
}

static void persistent_log_task(void *arg) {
  (void)arg;
  char chunk[1024];
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    size_t len = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      len = diag_ring_read(chunk, sizeof(chunk));
      xSemaphoreGive(s_mutex);
    }
    if (!len) continue;

    FILE *existing = fopen(DIAG_FILE, "rb");
    long size = 0;
    if (existing) {
      if (fseek(existing, 0, SEEK_END) == 0) size = ftell(existing);
      fclose(existing);
    }
    if (size >= DIAG_FILE_MAX) {
      remove(DIAG_FILE_OLD);
      rename(DIAG_FILE, DIAG_FILE_OLD);
    }
    FILE *file = fopen(DIAG_FILE, "ab");
    if (file) {
      fwrite(chunk, 1, len, file);
      fclose(file);
    } else {
      s_diag_dropped += (uint32_t)len;
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Short HTTP polling handler                                         */
/* ------------------------------------------------------------------ */

static esp_err_t live_log_get(httpd_req_t *req) {
  char buffer[LIVE_LOG_CHUNK];

  size_t len = 0;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    len = ring_read(buffer, LIVE_LOG_CHUNK);
    xSemaphoreGive(s_mutex);
  }

  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  return httpd_resp_send(req, buffer, len);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t log_stream_init(void) {
  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) {
    return ESP_ERR_NO_MEM;
  }

#ifdef CONFIG_SPIRAM
  s_ring = heap_caps_malloc(LOG_RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if (!s_ring) {
    s_ring = malloc(LOG_RING_SIZE);
  }
  if (!s_ring) {
    return ESP_ERR_NO_MEM;
  }
  s_diag_ring = heap_caps_malloc(DIAG_RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_diag_ring) s_diag_ring = malloc(DIAG_RING_SIZE);
  if (!s_diag_ring) return ESP_ERR_NO_MEM;

  s_head = s_tail = 0;
  s_diag_head = s_diag_tail = 0;

  /* Hook into esp_log — keep the original so UART output continues. */
  s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);

  task_create_spiram(persistent_log_task, "diag_log", 3072, NULL, 2,
                     NULL, NULL);

  return ESP_OK;
}

static size_t read_file_into(const char *path, char *buffer, size_t capacity,
                             size_t offset) {
  if (offset >= capacity) return offset;
  FILE *file = fopen(path, "rb");
  if (!file) return offset;
  offset += fread(buffer + offset, 1, capacity - offset - 1, file);
  fclose(file);
  buffer[offset] = '\0';
  return offset;
}

size_t log_stream_read_persistent(char *buffer, size_t capacity) {
  if (!buffer || capacity < 2) return 0;
  buffer[0] = '\0';
  size_t offset = read_file_into(DIAG_FILE_OLD, buffer, capacity, 0);
  return read_file_into(DIAG_FILE, buffer, capacity, offset);
}

esp_err_t log_stream_clear_persistent(void) {
  remove(DIAG_FILE);
  remove(DIAG_FILE_OLD);
  if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    s_diag_head = s_diag_tail = 0;
    s_diag_dropped = 0;
    xSemaphoreGive(s_mutex);
  }
  return ESP_OK;
}

uint32_t log_stream_persistent_dropped(void) { return s_diag_dropped; }

esp_err_t log_stream_register(httpd_handle_t server) {
  httpd_uri_t live_uri = {
      .uri = "/api/logs/live",
      .method = HTTP_GET,
      .handler = live_log_get,
  };
  esp_err_t err = httpd_register_uri_handler(server, &live_uri);
  if (err != ESP_OK) {
    ESP_LOGE("log_stream", "Failed to register /api/logs/live: %s",
             esp_err_to_name(err));
    return err;
  }

  ESP_LOGI("log_stream", "Log polling available on /api/logs/live");
  return ESP_OK;
}
