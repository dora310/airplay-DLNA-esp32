/**
 * @file display_st7789.c
 * @brief ST7789 TFT display driver using esp_lcd + LVGL 9 (esp_lvgl_port)
 *
 * Implements the display_init() API for ST7789-based TFT displays.
 * Display: GMT020-02 ST7789V, 240x320 native / 320x240 landscape, SPI.
 *
 * Background image is loaded at startup from SPIFFS (/spiffs/bg/background.bin)
 * — a raw RGB565 little-endian binary file. If the file is absent, the display
 * initialises normally with a blank (black) background. The background can be
 * updated without reflashing by uploading a new file via the HTTP file API.
 *
 * GPIO assignments (configured via sdkconfig / menuconfig):
 *   CLK  -> CONFIG_DISPLAY_SPI_CLK  (default 18)
 *   MOSI -> CONFIG_DISPLAY_SPI_MOSI (default 17)
 *   CS   -> CONFIG_DISPLAY_SPI_CS   (default 15)
 *   DC   -> CONFIG_DISPLAY_SPI_DC   (default 16)
 *   RST  -> CONFIG_DISPLAY_SPI_RST  (default 21)
 *   BL   -> not present on GMT020-02; CONFIG_DISPLAY_BL_GPIO must be -1
 */

#include "display.h"
#include "audio_output.h"
#include "board_common.h"
#include "playback_control.h"
#include "rtsp_events.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "display_st7789";

// Generated 16 px UTF-8 font containing Latin/Spanish, Persian/Arabic and
// common Japanese glyphs. LVGL's bidi and Arabic/Persian shaping options are
// enabled by sdkconfig.defaults.esp32s3.
LV_FONT_DECLARE(lv_font_international_16);

// Fail the build instead of silently drawing missing/broken international
// glyphs if the required LVGL options are lost from sdkconfig defaults.
#if !LV_USE_FONT_COMPRESSED
#error "lv_font_international_16 requires LV_USE_FONT_COMPRESSED=1"
#endif
#if LV_TXT_ENC != LV_TXT_ENC_UTF8
#error "The ST7789 metadata display requires LV_TXT_ENC_UTF8"
#endif

// ============================================================================
// Hardware configuration
// ============================================================================

#define DISPLAY_WIDTH      CONFIG_DISPLAY_ST7789_WIDTH
#define DISPLAY_HEIGHT     CONFIG_DISPLAY_ST7789_HEIGHT
#define LCD_HOST           SPI2_HOST
/* The GMT020-02 breakout can remain blank at 40 MHz with Dupont leads.
 * Start at a conservative 10 MHz. This is still fast enough for the
 * now-playing interface and leaves substantial margin for wiring quality. */
#define LCD_PIXEL_CLOCK_HZ (10 * 1000 * 1000)
#define DRAW_BUF_LINES     10

// Background image path on SPIFFS
#define BG_SPIFFS_PATH   "/spiffs/bg/background.bin"
#define BG_EXPECTED_SIZE ((long)DISPLAY_WIDTH * DISPLAY_HEIGHT * 2) // RGB565

// ============================================================================
// Layout constants
// ============================================================================

/* The 2.25-inch ST7789P3 is an unusually short 284x76 landscape panel. Keep
 * its essential now-playing information visible and hide the lower-priority
 * album/status row. Larger ST7789 panels retain the original layout. */
#define DISPLAY_COMPACT_STRIP (DISPLAY_HEIGHT <= 100)

#if CONFIG_DISPLAY_ST7789_HEIGHT <= 100
#define X_MARGIN     6
#define X_MARGIN_R   (-6)
#define Y_TITLE      2
#define Y_ARTIST     21
#define Y_ALBUM      0
#define Y_PROGRESS   43
#define Y_TIME       51
#define Y_STATUS     0
#define BAR_HEIGHT   6
#define ARTWORK_SIZE 48
#define ARTWORK_X    4
#define ARTWORK_Y    4
#define TEXT_X       X_MARGIN
#define TEXT_RIGHT   X_MARGIN
#else
#define X_MARGIN   22
#define X_MARGIN_R (-22)
#define Y_TITLE    10
#define Y_ARTIST   44
#define Y_ALBUM    69
// Keep the status controls on-screen for both the original 320x170 ST7789
// layout and the Waveshare 240x240 panel.
#define Y_PROGRESS ((DISPLAY_HEIGHT >= 220) ? 148 : 114)
#define Y_TIME     (Y_PROGRESS + 18)
#define Y_STATUS   ((DISPLAY_HEIGHT >= 220) ? 188 : (DISPLAY_HEIGHT - 18))
#define BAR_HEIGHT 12
#define ARTWORK_SIZE 112
#define ARTWORK_X    10
#define ARTWORK_Y    10
#define TEXT_X       X_MARGIN
#define TEXT_RIGHT   X_MARGIN
#endif

// ============================================================================
// Display state
// ============================================================================

typedef enum {
  DISPLAY_STATE_STANDBY,
  DISPLAY_STATE_CONNECTED,
  DISPLAY_STATE_PLAYING,
  DISPLAY_STATE_PAUSED,
} display_state_t;

static struct {
  char title[METADATA_STRING_MAX];
  char artist[METADATA_STRING_MAX];
  char album[METADATA_STRING_MAX];
  uint32_t duration_secs;
  uint32_t position_secs;
  display_state_t state;
  volatile bool dirty;  // written by RTSP callback, polled by display_task
  int64_t sync_time_us; // 64-bit — torn reads would cause position jumps
} s_display;

// Protects s_display against concurrent access from the RTSP event callback
// thread and the display_task. Must be held around any multi-field read or
// write (strings, sync_time_us, state+position together). Dirty is volatile
// so the polling loop picks up the flag without holding the mutex.
static SemaphoreHandle_t s_state_mutex = NULL;

#define STATE_LOCK()   xSemaphoreTake(s_state_mutex, portMAX_DELAY)
#define STATE_UNLOCK() xSemaphoreGive(s_state_mutex)

// ============================================================================
// LVGL handles and widgets
// ============================================================================

static lv_display_t *s_lvgl_disp = NULL;

static uint8_t *s_bg_buf = NULL;
static lv_image_dsc_t s_bg_dsc;

static lv_obj_t *s_label_title = NULL;
static lv_obj_t *s_label_artist = NULL;
static lv_obj_t *s_label_album = NULL;
static lv_obj_t *s_label_muted = NULL;
static lv_obj_t *s_label_status = NULL;
static lv_obj_t *s_bar_progress = NULL;
static lv_obj_t *s_label_time_elapsed = NULL;
static lv_obj_t *s_label_time_remaining = NULL;
static lv_obj_t *s_label_battery = NULL;
static lv_obj_t *s_label_volume = NULL;
static lv_obj_t *s_theme_layer = NULL;
static lv_obj_t *s_artwork_image = NULL;
static lv_obj_t *s_artwork_placeholder = NULL;

// R29 changes the colour theme from track metadata only. Album-art reception
// and JPEG rendering remain disabled. A later revision can feed a colour
// extracted from decoded artwork into the same apply_track_theme() function.
typedef struct {
  uint8_t bg_r;
  uint8_t bg_g;
  uint8_t bg_b;
  uint8_t accent_r;
  uint8_t accent_g;
  uint8_t accent_b;
} display_palette_t;

static const display_palette_t s_track_palettes[] = {
    {3, 16, 38, 30, 144, 255},    // blue
    {27, 12, 48, 191, 90, 242},   // purple
    {4, 37, 39, 45, 212, 191},    // teal
    {48, 10, 25, 255, 69, 100},   // rose
    {49, 25, 7, 255, 159, 10},    // amber
    {10, 24, 52, 94, 92, 230},    // indigo
    {5, 39, 24, 48, 209, 88},     // green
    {46, 13, 8, 255, 99, 72},     // coral
    {9, 32, 48, 100, 210, 255},   // cyan
    {37, 15, 37, 255, 55, 150},   // magenta
};

static uint32_t s_applied_theme_key = UINT32_MAX;

// Compressed JPEG ownership is transferred from the RTSP callback to the
// display task. Both pending fields are protected by s_state_mutex. Active
// fields are touched only while the LVGL lock is held.
static uint8_t *s_artwork_pending = NULL;
static size_t s_artwork_pending_len = 0;
static bool s_artwork_pending_ready = false;
static bool s_artwork_clear_requested = false;
static uint8_t *s_artwork_active = NULL;
static lv_image_dsc_t s_artwork_dsc;

// ============================================================================
// Background loading
// ============================================================================

static bool bg_load_from_spiffs(void) {
  FILE *f = fopen(BG_SPIFFS_PATH, "rb");
  if (!f) {
    ESP_LOGI(TAG, "No background file at %s — using blank background",
             BG_SPIFFS_PATH);
    return false;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size != BG_EXPECTED_SIZE) {
    ESP_LOGW(TAG, "Background file wrong size: %ld (expected %ld) — skipping",
             size, BG_EXPECTED_SIZE);
    fclose(f);
    return false;
  }

  s_bg_buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (!s_bg_buf) {
    ESP_LOGE(TAG, "Failed to allocate %ld bytes in PSRAM for background", size);
    fclose(f);
    return false;
  }

  if (fread(s_bg_buf, 1, size, f) != (size_t)size) {
    ESP_LOGE(TAG, "Failed to read background file");
    fclose(f);
    heap_caps_free(s_bg_buf);
    s_bg_buf = NULL;
    return false;
  }

  fclose(f);

  memset(&s_bg_dsc, 0, sizeof(s_bg_dsc));
  s_bg_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  s_bg_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  s_bg_dsc.header.w = DISPLAY_WIDTH;
  s_bg_dsc.header.h = DISPLAY_HEIGHT;
  s_bg_dsc.data_size = BG_EXPECTED_SIZE;
  s_bg_dsc.data = s_bg_buf;

  ESP_LOGI(TAG, "Background loaded from SPIFFS (%ld bytes, PSRAM)", size);
  return true;
}

// ============================================================================
// Helpers
// ============================================================================

static uint32_t get_estimated_position(void) {
  uint32_t pos = s_display.position_secs;
  if (s_display.state == DISPLAY_STATE_PLAYING && s_display.sync_time_us > 0) {
    int64_t elapsed_us = esp_timer_get_time() - s_display.sync_time_us;
    uint32_t elapsed_secs = (uint32_t)(elapsed_us / 1000000);
    pos += elapsed_secs;
    if (s_display.duration_secs > 0 && pos > s_display.duration_secs) {
      pos = s_display.duration_secs;
    }
  }
  return pos;
}

static void format_time(uint32_t secs, char *buf, size_t len) {
  snprintf(buf, len, "%lu:%02lu", secs / 60, secs % 60);
}

static void format_remaining(uint32_t remaining_secs, char *buf, size_t len) {
  snprintf(buf, len, "-%lu:%02lu", remaining_secs / 60, remaining_secs % 60);
}

static uint32_t theme_hash_append(uint32_t hash, const char *text) {
  // FNV-1a deliberately hashes raw UTF-8 bytes. Spanish, Persian and Japanese
  // titles therefore select palettes just as reliably as ASCII titles.
  if (!text) {
    return hash;
  }
  for (const uint8_t *p = (const uint8_t *)text; *p; ++p) {
    hash ^= *p;
    hash *= 16777619u;
  }
  hash ^= 0xffu; // field separator
  hash *= 16777619u;
  return hash;
}

// Called only while the LVGL lock is held. It performs no allocation and does
// nothing when the selected theme is already active.
static void apply_track_theme(const char *title, const char *artist,
                              const char *album, display_state_t state) {
  bool has_track = state != DISPLAY_STATE_STANDBY && title && title[0];
  uint32_t key = 0;
  size_t palette_index = 0;

  if (has_track) {
    key = 2166136261u;
    key = theme_hash_append(key, title);
    key = theme_hash_append(key, artist);
    key = theme_hash_append(key, album);
    if (key == 0) {
      key = 1;
    }
    palette_index = key %
                    (sizeof(s_track_palettes) / sizeof(s_track_palettes[0]));
  }

  if (key == s_applied_theme_key) {
    return;
  }
  s_applied_theme_key = key;

  const display_palette_t *palette = &s_track_palettes[palette_index];
  lv_color_t background = lv_color_make(palette->bg_r, palette->bg_g,
                                        palette->bg_b);
  lv_color_t accent = lv_color_make(palette->accent_r, palette->accent_g,
                                    palette->accent_b);

  if (s_theme_layer) {
    lv_obj_set_style_bg_color(s_theme_layer, background, 0);
  }
  if (s_bar_progress) {
    lv_obj_set_style_bg_color(s_bar_progress, accent, LV_PART_INDICATOR);
  }
  if (s_label_status) {
    lv_obj_set_style_text_color(s_label_status, accent, 0);
  }
  if (s_artwork_placeholder) {
    lv_obj_set_style_border_color(s_artwork_placeholder, accent, 0);
  }

  ESP_LOGI(TAG, "Track colour theme: %u", (unsigned)palette_index);
}

static bool jpeg_get_dimensions(const uint8_t *data, size_t len,
                                uint16_t *width, uint16_t *height) {
  if (!data || len < 4 || data[0] != 0xff || data[1] != 0xd8) {
    return false;
  }

  size_t pos = 2;
  while (pos + 4 <= len) {
    while (pos < len && data[pos] != 0xff) {
      pos++;
    }
    while (pos < len && data[pos] == 0xff) {
      pos++;
    }
    if (pos >= len) {
      break;
    }

    uint8_t marker = data[pos++];
    if (marker == 0xd8 || marker == 0xd9 || marker == 0x01 ||
        (marker >= 0xd0 && marker <= 0xd7)) {
      continue;
    }
    if (pos + 2 > len) {
      break;
    }

    size_t segment_len = ((size_t)data[pos] << 8) | data[pos + 1];
    if (segment_len < 2 || pos + segment_len > len) {
      break;
    }

    bool is_sof = (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 &&
                   marker != 0xc8 && marker != 0xcc);
    if (is_sof && segment_len >= 7) {
      *height = (uint16_t)(((uint16_t)data[pos + 3] << 8) | data[pos + 4]);
      *width = (uint16_t)(((uint16_t)data[pos + 5] << 8) | data[pos + 6]);
      return *width > 0 && *height > 0;
    }
    pos += segment_len;
  }
  return false;
}

// Must be called with the LVGL lock held. Takes ownership of jpeg_data when
// successful; on failure the caller remains responsible for freeing it.
static bool artwork_widget_set(uint8_t *jpeg_data, size_t jpeg_len) {
  if (!s_artwork_placeholder) {
    return false;
  }

  uint16_t width = 0, height = 0;
  if (!jpeg_get_dimensions(jpeg_data, jpeg_len, &width, &height)) {
    ESP_LOGW(TAG, "Rejected artwork with invalid JPEG dimensions");
    return false;
  }

  if (s_artwork_image) {
    lv_obj_delete(s_artwork_image);
    s_artwork_image = NULL;
  }
  if (s_artwork_active) {
    lv_image_cache_drop(&s_artwork_dsc);
    heap_caps_free(s_artwork_active);
    s_artwork_active = NULL;
  }

  memset(&s_artwork_dsc, 0, sizeof(s_artwork_dsc));
  s_artwork_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  s_artwork_dsc.header.cf = LV_COLOR_FORMAT_RAW;
  s_artwork_dsc.header.w = width;
  s_artwork_dsc.header.h = height;
  s_artwork_dsc.data_size = jpeg_len;
  s_artwork_dsc.data = jpeg_data;

  s_artwork_image = lv_image_create(lv_screen_active());
  lv_image_set_src(s_artwork_image, &s_artwork_dsc);
  uint16_t longest = width > height ? width : height;
  uint32_t scale = ((uint32_t)ARTWORK_SIZE * 256U) / longest;
  if (scale > 256U) {
    scale = 256U; // Do not enlarge small covers.
  }
  if (scale == 0) {
    scale = 1;
  }
  lv_image_set_scale(s_artwork_image, scale);
  lv_obj_align(s_artwork_image, LV_ALIGN_TOP_LEFT, ARTWORK_X, ARTWORK_Y);
  lv_obj_t *parent = lv_obj_get_parent(s_artwork_image);
  lv_obj_move_to_index(s_artwork_image, lv_obj_get_child_count(parent) - 1);
  lv_obj_add_flag(s_artwork_placeholder, LV_OBJ_FLAG_HIDDEN);
  s_artwork_active = jpeg_data;
  ESP_LOGI(TAG, "Displaying JPEG artwork: %ux%u, %zu bytes", width, height,
           jpeg_len);
  return true;
}

// Must be called with the LVGL lock held.
static void artwork_widget_clear(void) {
  if (s_artwork_image) {
    lv_obj_delete(s_artwork_image);
    s_artwork_image = NULL;
  }
  if (s_artwork_active) {
    lv_image_cache_drop(&s_artwork_dsc);
    heap_caps_free(s_artwork_active);
    s_artwork_active = NULL;
  }
  if (s_artwork_placeholder) {
    lv_obj_clear_flag(s_artwork_placeholder, LV_OBJ_FLAG_HIDDEN);
  }
}

// ============================================================================
// UI creation - called once after LVGL init, with lock held
// ============================================================================

static void ui_create(void) {
  lv_obj_t *scr = lv_screen_active();

  // A dark-blue background makes the first successful panel refresh obvious.
  // It also remains readable when no optional background image is installed.
  lv_obj_set_style_bg_color(scr, lv_color_make(3, 16, 38), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // Background image — loaded from SPIFFS at display_init() time.
  // If no file was found, s_bg_buf is NULL and we skip the image widget,
  // leaving the screen black. All other widgets render normally on top.
  if (s_bg_buf) {
    lv_obj_t *bg = lv_image_create(scr);
    lv_image_set_src(bg, &s_bg_dsc);
    lv_obj_align(bg, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  // Full-screen colour layer. It is created before every text/control widget,
  // so it cannot cover metadata. With a custom SPIFFS background it acts as a
  // strong tint; without one it is the complete screen background.
  s_theme_layer = lv_obj_create(scr);
  lv_obj_remove_style_all(s_theme_layer);
  lv_obj_set_size(s_theme_layer, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  lv_obj_align(s_theme_layer, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_color(s_theme_layer, lv_color_make(3, 16, 38), 0);
  lv_obj_set_style_bg_opa(s_theme_layer,
                          s_bg_buf ? LV_OPA_90 : LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_theme_layer, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s_theme_layer, LV_OBJ_FLAG_CLICKABLE);

#ifdef CONFIG_ENABLE_AIRPLAY_ARTWORK
  // Album-art area. This is compiled only when JPEG artwork is explicitly
  // enabled. It is disabled in R24 because direct compressed-JPEG rendering
  // produced a moving white rectangle on this ST7789/LVGL combination.
  s_artwork_placeholder = lv_obj_create(scr);
  lv_obj_set_size(s_artwork_placeholder, ARTWORK_SIZE, ARTWORK_SIZE);
  lv_obj_align(s_artwork_placeholder, LV_ALIGN_TOP_LEFT, ARTWORK_X, ARTWORK_Y);
  lv_obj_set_style_bg_color(s_artwork_placeholder, lv_color_make(18, 18, 28),
                            0);
  lv_obj_set_style_bg_opa(s_artwork_placeholder, LV_OPA_80, 0);
  lv_obj_set_style_border_color(s_artwork_placeholder,
                                lv_color_make(55, 55, 75), 0);
  lv_obj_set_style_border_width(s_artwork_placeholder, 1, 0);
  lv_obj_set_style_radius(s_artwork_placeholder, 8, 0);
  lv_obj_clear_flag(s_artwork_placeholder, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *artwork_text = lv_label_create(s_artwork_placeholder);
  lv_label_set_text(artwork_text, DISPLAY_COMPACT_STRIP ? "ART" : "ALBUM\nART");
  lv_obj_set_style_text_align(artwork_text, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(artwork_text, lv_color_make(100, 105, 125), 0);
  lv_obj_set_style_text_font(artwork_text, &lv_font_montserrat_14, 0);
  lv_obj_center(artwork_text);
#endif

  // Muted indicator — top-right corner, red, hidden by default
  s_label_muted = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_muted, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_label_muted, lv_color_make(255, 50, 50), 0);
  lv_obj_align(s_label_muted, LV_ALIGN_TOP_RIGHT, X_MARGIN_R, Y_TITLE);
  lv_obj_add_flag(s_label_muted, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(s_label_muted, "MUTED");

  // Metadata uses the international font. LV_BASE_DIR_AUTO enables Persian
  // right-to-left layout while Latin and Japanese remain left-to-right.
  s_label_title = lv_label_create(scr);
  lv_obj_set_width(s_label_title,
                   DISPLAY_WIDTH - TEXT_X - TEXT_RIGHT -
                       (DISPLAY_COMPACT_STRIP ? 0 : 55));
  lv_label_set_long_mode(s_label_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_font(s_label_title, &lv_font_international_16, 0);
  lv_obj_set_style_base_dir(s_label_title, LV_BASE_DIR_AUTO, 0);
  lv_obj_set_style_text_color(s_label_title, lv_color_white(), 0);
  lv_obj_align(s_label_title, LV_ALIGN_TOP_LEFT, TEXT_X, Y_TITLE);
  lv_label_set_text(s_label_title, "AirPlay Ready");

  // Artist — medium font, light grey, scrolling
  s_label_artist = lv_label_create(scr);
  lv_obj_set_width(s_label_artist, DISPLAY_WIDTH - TEXT_X - TEXT_RIGHT);
  lv_label_set_long_mode(s_label_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_font(s_label_artist, &lv_font_international_16, 0);
  lv_obj_set_style_base_dir(s_label_artist, LV_BASE_DIR_AUTO, 0);
  lv_obj_set_style_text_color(s_label_artist, lv_color_make(180, 180, 180), 0);
  lv_obj_align(s_label_artist, LV_ALIGN_TOP_LEFT, TEXT_X, Y_ARTIST);
  lv_label_set_text(s_label_artist, "");

  // Album — small font, dimmer grey, scrolling
  s_label_album = lv_label_create(scr);
  lv_obj_set_width(s_label_album,
                   DISPLAY_WIDTH - TEXT_X - TEXT_RIGHT -
                       (DISPLAY_COMPACT_STRIP ? 0 : 70));
  lv_label_set_long_mode(s_label_album, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_font(s_label_album, &lv_font_international_16, 0);
  lv_obj_set_style_base_dir(s_label_album, LV_BASE_DIR_AUTO, 0);
  lv_obj_set_style_text_color(s_label_album, lv_color_make(140, 140, 140), 0);
  lv_obj_align(s_label_album, LV_ALIGN_TOP_LEFT, TEXT_X, Y_ALBUM);
  lv_label_set_text(s_label_album, "");
  if (DISPLAY_COMPACT_STRIP) {
    lv_obj_add_flag(s_label_album, LV_OBJ_FLAG_HIDDEN);
  }

  // Playback status indicator — right side at album row, amber
  s_label_status = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_status, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_label_status, lv_color_make(255, 200, 0), 0);
  lv_obj_align(s_label_status, LV_ALIGN_TOP_RIGHT, X_MARGIN_R, Y_ALBUM);
  lv_label_set_text(s_label_status, "");
  if (DISPLAY_COMPACT_STRIP) {
    lv_obj_add_flag(s_label_status, LV_OBJ_FLAG_HIDDEN);
  }

  // Progress bar — inset from border on both sides, rounded
  s_bar_progress = lv_bar_create(scr);
  lv_obj_set_size(s_bar_progress, DISPLAY_WIDTH - (X_MARGIN * 2), BAR_HEIGHT);
  lv_obj_align(s_bar_progress, LV_ALIGN_TOP_LEFT, X_MARGIN, Y_PROGRESS);
  lv_bar_set_range(s_bar_progress, 0, 100);
  lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(s_bar_progress, lv_color_make(20, 20, 40), 0);
  lv_obj_set_style_bg_opa(s_bar_progress, LV_OPA_80, 0);
  lv_obj_set_style_bg_color(s_bar_progress, lv_color_make(30, 144, 255),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_bar_progress, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(s_bar_progress, 3, 0);
  lv_obj_set_style_radius(s_bar_progress, 3, LV_PART_INDICATOR);

  // Elapsed time — below bar, left aligned
  s_label_time_elapsed = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_time_elapsed, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_label_time_elapsed,
                              lv_color_make(150, 150, 150), 0);
  lv_obj_align(s_label_time_elapsed, LV_ALIGN_TOP_LEFT, X_MARGIN, Y_TIME);
  lv_label_set_text(s_label_time_elapsed, "");

  // Remaining time — below bar, right aligned
  s_label_time_remaining = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_time_remaining, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_label_time_remaining,
                              lv_color_make(150, 150, 150), 0);
  lv_obj_align(s_label_time_remaining, LV_ALIGN_TOP_RIGHT, X_MARGIN_R, Y_TIME);
  lv_label_set_text(s_label_time_remaining, "");

  // Volume — status row, bottom-left
  s_label_volume = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_volume, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_label_volume, lv_color_make(150, 150, 150), 0);
  lv_obj_align(s_label_volume, LV_ALIGN_TOP_LEFT, X_MARGIN, Y_STATUS);
  lv_label_set_text(s_label_volume, "");
  if (DISPLAY_COMPACT_STRIP) {
    lv_obj_add_flag(s_label_volume, LV_OBJ_FLAG_HIDDEN);
  }

  // Battery — status row, bottom-right
  s_label_battery = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_battery, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_label_battery, lv_color_make(150, 150, 150), 0);
  lv_obj_align(s_label_battery, LV_ALIGN_TOP_RIGHT, X_MARGIN_R, Y_STATUS);
  lv_label_set_text(s_label_battery, "");
  if (DISPLAY_COMPACT_STRIP) {
    lv_obj_add_flag(s_label_battery, LV_OBJ_FLAG_HIDDEN);
  }
}

// Update the battery + volume status row. Called from ui_update() with the
// LVGL port lock already held.
static void ui_update_status_row(void) {
  // Volume — show as percentage, with the channel mode (L+R / L / R)
  const char *chan;
  switch (audio_output_get_channel_mode()) {
  case AUDIO_CHANNEL_LEFT:
    chan = "L";
    break;
  case AUDIO_CHANNEL_RIGHT:
    chan = "R";
    break;
  default:
    chan = "L+R";
    break;
  }
  char vol_str[24];
  snprintf(vol_str, sizeof(vol_str), LV_SYMBOL_VOLUME_MAX " %d%%  %s",
           playback_control_get_volume_percent(), chan);
  lv_label_set_text(s_label_volume, vol_str);

  // Battery — only if the board reports one
  int pct = 0;
  bool charging = false;
  if (board_battery_read(&pct, &charging)) {
    const char *icon;
    if (charging) {
      icon = LV_SYMBOL_USB; // plugged into USB / charging
    } else if (pct >= 80) {
      icon = LV_SYMBOL_BATTERY_FULL;
    } else if (pct >= 60) {
      icon = LV_SYMBOL_BATTERY_3;
    } else if (pct >= 40) {
      icon = LV_SYMBOL_BATTERY_2;
    } else if (pct >= 20) {
      icon = LV_SYMBOL_BATTERY_1;
    } else {
      icon = LV_SYMBOL_BATTERY_EMPTY;
    }

    char bat_str[16];
    snprintf(bat_str, sizeof(bat_str), "%s %d%%", icon, pct);
    lv_label_set_text(s_label_battery, bat_str);

    // Tint red when low and not charging
    lv_color_t color = (!charging && pct <= 20) ? lv_color_make(255, 60, 60)
                                                : lv_color_make(150, 150, 150);
    lv_obj_set_style_text_color(s_label_battery, color, 0);
  } else {
    lv_label_set_text(s_label_battery, "");
  }
}

// ============================================================================
// UI update
// ============================================================================

static void ui_update(void) {
  // Snapshot shared state under s_state_mutex. This avoids torn reads of
  // sync_time_us (int64, two-word on Xtensa) and guarantees consistent
  // strings vs. state. The snapshot is then rendered without the state
  // mutex held, so the RTSP callback can't be blocked by LVGL rendering.
  char title[METADATA_STRING_MAX];
  char artist[METADATA_STRING_MAX];
  char album[METADATA_STRING_MAX];
  uint32_t duration_secs;
  uint32_t position_secs;
  int64_t sync_time_us;
  display_state_t state;
  uint8_t *pending_artwork = NULL;
  size_t pending_artwork_len = 0;
  bool clear_artwork = false;

  STATE_LOCK();
  memcpy(title, s_display.title, sizeof(title));
  memcpy(artist, s_display.artist, sizeof(artist));
  memcpy(album, s_display.album, sizeof(album));
  duration_secs = s_display.duration_secs;
  position_secs = s_display.position_secs;
  sync_time_us = s_display.sync_time_us;
  state = s_display.state;
  clear_artwork = s_artwork_clear_requested;
  s_artwork_clear_requested = false;
  if (s_artwork_pending_ready) {
    pending_artwork = s_artwork_pending;
    pending_artwork_len = s_artwork_pending_len;
    s_artwork_pending = NULL;
    s_artwork_pending_len = 0;
    s_artwork_pending_ready = false;
  }
  STATE_UNLOCK();

  // Defensive NUL termination — if the RTSP producer ever fills all
  // METADATA_STRING_MAX bytes without a terminator, lv_label_set_text
  // would read off the end.
  title[METADATA_STRING_MAX - 1] = '\0';
  artist[METADATA_STRING_MAX - 1] = '\0';
  album[METADATA_STRING_MAX - 1] = '\0';

  if (!lvgl_port_lock(100)) {
    ESP_LOGW(TAG, "ui_update: lock timeout");
    // Return ownership to the pending slot so a temporary LVGL lock timeout
    // cannot lose the cover image.
    STATE_LOCK();
    s_artwork_clear_requested |= clear_artwork;
    if (pending_artwork) {
      if (!s_artwork_pending_ready) {
        s_artwork_pending = pending_artwork;
        s_artwork_pending_len = pending_artwork_len;
        s_artwork_pending_ready = true;
        pending_artwork = NULL;
      }
    }
    STATE_UNLOCK();
    if (pending_artwork) {
      heap_caps_free(pending_artwork);
    }
    return;
  }

  if (clear_artwork) {
    artwork_widget_clear();
  }
  if (pending_artwork &&
      !artwork_widget_set(pending_artwork, pending_artwork_len)) {
    heap_caps_free(pending_artwork);
  }

  apply_track_theme(title, artist, album, state);

  switch (state) {
  case DISPLAY_STATE_STANDBY:
    lv_label_set_text(s_label_title, "AirPlay Ready");
    lv_label_set_text(s_label_artist, "");
    lv_label_set_text(s_label_album, "");
    lv_label_set_text(s_label_status, "");
    lv_label_set_text(s_label_time_elapsed, "");
    lv_label_set_text(s_label_time_remaining, "");
    lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
    lv_obj_add_flag(s_label_muted, LV_OBJ_FLAG_HIDDEN);
    break;

  case DISPLAY_STATE_CONNECTED:
    // Metadata can arrive before RECORD/SETRATEANCHORTIME changes the state
    // to PLAYING. Render it immediately instead of hiding it behind the fixed
    // Connected/Ready message.
    lv_label_set_text(s_label_title, title[0] ? title : "Connected");
    lv_label_set_text(s_label_artist, artist[0] ? artist : "");
    lv_label_set_text(s_label_album, album[0] ? album : "");
    lv_label_set_text(s_label_status, "");
    lv_label_set_text(s_label_time_elapsed, "");
    lv_label_set_text(s_label_time_remaining, "");
    lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
    lv_obj_add_flag(s_label_muted, LV_OBJ_FLAG_HIDDEN);
    break;

  case DISPLAY_STATE_PLAYING:
  case DISPLAY_STATE_PAUSED: {
    lv_label_set_text(s_label_title, title[0] ? title : "---");
    lv_label_set_text(s_label_artist, artist[0] ? artist : "");
    lv_label_set_text(s_label_album, album[0] ? album : "");
    lv_label_set_text(s_label_status,
                      state == DISPLAY_STATE_PAUSED ? "PAUSED" : "PLAYING");

    // Muted indicator
    if (playback_control_is_muted()) {
      lv_obj_clear_flag(s_label_muted, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_label_muted, LV_OBJ_FLAG_HIDDEN);
    }

    uint32_t pos = position_secs;
    if (state == DISPLAY_STATE_PLAYING && sync_time_us > 0) {
      int64_t elapsed_us = esp_timer_get_time() - sync_time_us;
      uint32_t elapsed_secs = (uint32_t)(elapsed_us / 1000000);
      pos += elapsed_secs;
      if (duration_secs > 0 && pos > duration_secs) {
        pos = duration_secs;
      }
    }

    if (duration_secs > 0) {
      int pct = (int)((uint64_t)pos * 100 / duration_secs);
      lv_bar_set_value(s_bar_progress, pct, LV_ANIM_OFF);

      char elapsed_str[12];
      format_time(pos, elapsed_str, sizeof(elapsed_str));
      lv_label_set_text(s_label_time_elapsed, elapsed_str);

      uint32_t remaining = (pos <= duration_secs) ? duration_secs - pos : 0;
      char remaining_str[16];
      format_remaining(remaining, remaining_str, sizeof(remaining_str));
      lv_label_set_text(s_label_time_remaining, remaining_str);
    } else {
      lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
      lv_label_set_text(s_label_time_elapsed, "");
      lv_label_set_text(s_label_time_remaining, "");
    }
    break;
  }
  }

  ui_update_status_row();

  lvgl_port_unlock();
}

// ============================================================================
// RTSP event callback
// ============================================================================

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)user_data;

#ifdef CONFIG_ENABLE_AIRPLAY_ARTWORK
  if (event == RTSP_EVENT_METADATA && data && data->metadata.has_artwork &&
      data->metadata.artwork_data && data->metadata.artwork_len > 0 &&
      data->metadata.artwork_format == RTSP_ARTWORK_JPEG) {
    uint8_t *copy = heap_caps_malloc(data->metadata.artwork_len,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
      ESP_LOGW(TAG, "No PSRAM for %zu-byte artwork",
               data->metadata.artwork_len);
      return;
    }
    memcpy(copy, data->metadata.artwork_data, data->metadata.artwork_len);

    STATE_LOCK();
    uint8_t *old_pending = s_artwork_pending;
    s_artwork_pending = copy;
    s_artwork_pending_len = data->metadata.artwork_len;
    s_artwork_pending_ready = true;
    s_display.dirty = true;
    STATE_UNLOCK();
    if (old_pending) {
      heap_caps_free(old_pending);
    }
    return;
  }
#endif

  // All mutations of s_display happen under the state mutex so reads in
  // display_task see a consistent snapshot. The callback runs in the RTSP
  // event thread (not an ISR), so blocking on a FreeRTOS mutex is safe.
  STATE_LOCK();

  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
    if (s_artwork_pending) {
      heap_caps_free(s_artwork_pending);
      s_artwork_pending = NULL;
    }
    s_artwork_pending_len = 0;
    s_artwork_pending_ready = false;
    s_artwork_clear_requested = true;
    s_display.state = DISPLAY_STATE_CONNECTED;
    memset(s_display.title, 0, sizeof(s_display.title));
    memset(s_display.artist, 0, sizeof(s_display.artist));
    memset(s_display.album, 0, sizeof(s_display.album));
    s_display.duration_secs = 0;
    s_display.position_secs = 0;
    s_display.sync_time_us = 0;
    s_display.dirty = true;
    break;

  case RTSP_EVENT_PLAYING:
    s_display.state = DISPLAY_STATE_PLAYING;
    s_display.sync_time_us = esp_timer_get_time();
    s_display.dirty = true;
    break;

  case RTSP_EVENT_PAUSED:
    s_display.position_secs = get_estimated_position();
    s_display.sync_time_us = 0;
    s_display.state = DISPLAY_STATE_PAUSED;
    s_display.dirty = true;
    break;

  case RTSP_EVENT_DISCONNECTED:
    if (s_artwork_pending) {
      heap_caps_free(s_artwork_pending);
      s_artwork_pending = NULL;
    }
    s_artwork_pending_len = 0;
    s_artwork_pending_ready = false;
    s_artwork_clear_requested = true;
    s_display.state = DISPLAY_STATE_STANDBY;
    memset(s_display.title, 0, sizeof(s_display.title));
    memset(s_display.artist, 0, sizeof(s_display.artist));
    memset(s_display.album, 0, sizeof(s_display.album));
    s_display.duration_secs = 0;
    s_display.position_secs = 0;
    s_display.sync_time_us = 0;
    s_display.dirty = true;
    break;

  case RTSP_EVENT_METADATA:
    if (data) {
      bool track_changed = data->metadata.title[0] &&
                           strcmp(data->metadata.title, s_display.title) != 0;

      if (data->metadata.title[0]) {
        memcpy(s_display.title, data->metadata.title, METADATA_STRING_MAX);
        s_display.title[METADATA_STRING_MAX - 1] = '\0';
      }
      if (data->metadata.artist[0]) {
        memcpy(s_display.artist, data->metadata.artist, METADATA_STRING_MAX);
        s_display.artist[METADATA_STRING_MAX - 1] = '\0';
      }
      if (data->metadata.album[0]) {
        memcpy(s_display.album, data->metadata.album, METADATA_STRING_MAX);
        s_display.album[METADATA_STRING_MAX - 1] = '\0';
      }
      if (data->metadata.duration_secs) {
        s_display.duration_secs = data->metadata.duration_secs;
      }

      if (track_changed || data->metadata.position_secs ||
          s_display.position_secs == 0) {
        s_display.position_secs = data->metadata.position_secs;
      }

      s_display.sync_time_us = esp_timer_get_time();
      s_display.dirty = true;
    }
    break;

  }

  STATE_UNLOCK();
}

void display_notify_metadata(const char *title, const char *artist,
                             const char *album, uint32_t duration_secs,
                             uint32_t position_secs) {
  if (!s_state_mutex) {
    return;
  }
  rtsp_event_data_t data = {0};
  if (title) {
    strlcpy(data.metadata.title, title, sizeof(data.metadata.title));
  }
  if (artist) {
    strlcpy(data.metadata.artist, artist, sizeof(data.metadata.artist));
  }
  if (album) {
    strlcpy(data.metadata.album, album, sizeof(data.metadata.album));
  }
  data.metadata.duration_secs = duration_secs;
  data.metadata.position_secs = position_secs;
  on_rtsp_event(RTSP_EVENT_METADATA, &data, NULL);
}

void display_notify_playback(bool paused) {
  if (!s_state_mutex) {
    return;
  }
  on_rtsp_event(paused ? RTSP_EVENT_PAUSED : RTSP_EVENT_PLAYING, NULL, NULL);
}

void display_notify_stopped(void) {
  if (!s_state_mutex) {
    return;
  }
  on_rtsp_event(RTSP_EVENT_DISCONNECTED, NULL, NULL);
}

// ============================================================================
// Display task
// ============================================================================

static void display_task(void *pvParameters) {
  (void)pvParameters;

  while (1) {
    // Consume dirty under the state mutex so a concurrent set in the
    // RTSP callback is never lost (clear-after-set ordering).
    bool need_update = false;
    display_state_t state;
    STATE_LOCK();
    if (s_display.dirty) {
      s_display.dirty = false;
      need_update = true;
    }
    state = s_display.state;
    STATE_UNLOCK();

    if (need_update) {
      ui_update();
    }

    static TickType_t last_progress_update = 0;
    TickType_t now = xTaskGetTickCount();
    if (state == DISPLAY_STATE_PLAYING &&
        (now - last_progress_update) >= pdMS_TO_TICKS(1000)) {
      last_progress_update = now;
      ui_update();
    }

    // Refresh battery + volume + channel mode periodically even when idle.
    // These are not tied to RTSP events; poll at 1s so an interactive change
    // (volume, double-click channel toggle) shows up promptly.
    static TickType_t last_status_update = 0;
    if (!need_update && (now - last_status_update) >= pdMS_TO_TICKS(1000)) {
      last_status_update = now;
      if (lvgl_port_lock(100)) {
        ui_update_status_row();
        lvgl_port_unlock();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

// ============================================================================
// Initialization
// ============================================================================

void display_init(void *bus) {
  s_state_mutex = xSemaphoreCreateMutex();
  assert(s_state_mutex != NULL);

  // The main/main.c contract (from PR #59) is:
  //   bus != NULL → a pre-initialised spi_host_device_t passed as
  //                 (void*)(intptr_t)host. Use it and skip our own
  //                 spi_bus_initialize() to share the board's SPI bus.
  //   bus == NULL → fall back to initialising our own SPI bus from the
  //                 GPIO pins in Kconfig. Used by boards that don't
  //                 expose a shared SPI bus for the display.
  spi_host_device_t spi_host =
      (bus != NULL) ? (spi_host_device_t)(intptr_t)bus : LCD_HOST;

  ESP_LOGI(TAG,
           "Initializing ST7789 (%dx%d landscape) host=%d "
           "CLK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d",
           DISPLAY_WIDTH, DISPLAY_HEIGHT, (int)spi_host, CONFIG_DISPLAY_SPI_CLK,
           CONFIG_DISPLAY_SPI_MOSI, CONFIG_DISPLAY_SPI_CS,
           CONFIG_DISPLAY_SPI_DC, CONFIG_DISPLAY_SPI_RST,
           CONFIG_DISPLAY_BL_GPIO);

  // Backlight GPIO is optional — a value of -1 means "not wired / always on",
  // which matches the Kconfig default. BIT64(-1) is undefined, and
  // gpio_set_level(-1, ...) returns ESP_ERR_INVALID_ARG, so skip the config
  // entirely when the pin is not set.
#if CONFIG_DISPLAY_BL_GPIO >= 0
  {
    gpio_config_t bl_cfg = {
        .pin_bit_mask = BIT64(CONFIG_DISPLAY_BL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(CONFIG_DISPLAY_BL_GPIO, 0);
  }
#endif

  bg_load_from_spiffs();

  // Only initialise the SPI bus ourselves if the caller didn't pass a
  // pre-initialised one. Calling spi_bus_initialize() on a host that is
  // already initialised returns ESP_ERR_INVALID_STATE.
  if (bus == NULL) {
    spi_bus_config_t buscfg = {
        .mosi_io_num = CONFIG_DISPLAY_SPI_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_DISPLAY_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO));
  }

  esp_lcd_panel_io_handle_t io_handle = NULL;
  esp_lcd_panel_io_spi_config_t io_cfg = {
      .dc_gpio_num = CONFIG_DISPLAY_SPI_DC,
      .cs_gpio_num = CONFIG_DISPLAY_SPI_CS,
      .pclk_hz = LCD_PIXEL_CLOCK_HZ,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .spi_mode = 0,
      .trans_queue_depth = 4,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host,
                                           &io_cfg, &io_handle));

  esp_lcd_panel_handle_t panel_handle = NULL;
  esp_lcd_panel_dev_config_t panel_cfg = {
      .reset_gpio_num = CONFIG_DISPLAY_SPI_RST,
      .rgb_endian = LCD_RGB_ENDIAN_RGB,
      .bits_per_pixel = 16,
  };
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle));
  ESP_LOGI(TAG, "ST7789 panel handle created; resetting controller");
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
  vTaskDelay(pdMS_TO_TICKS(120));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
  vTaskDelay(pdMS_TO_TICKS(120));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
  vTaskDelay(pdMS_TO_TICKS(100));
  ESP_LOGI(TAG, "ST7789 controller initialized at %d MHz",
           LCD_PIXEL_CLOCK_HZ / 1000000);

  // Pin the LVGL task to Core 0. Default task_affinity=-1 allows migration
  // to Core 1 where it interferes with the audio task (priority 7).
  const lvgl_port_cfg_t lvgl_cfg = {
      .task_priority = 4,
      .task_stack = 6144,
      .task_affinity = 0,
      .task_max_sleep_ms = 500,
      .timer_period_ms = 5,
  };
  ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

  // buff_dma=true, buff_spiram=false: draw buffers in DMA-capable internal
  // SRAM. esp_lvgl_port passes the buffer pointer directly to
  // esp_lcd_panel_draw_bitmap — PSRAM buffers cause SPI master to allocate
  // a private DMA buffer at runtime, which fails under memory pressure.
  const lvgl_port_display_cfg_t disp_cfg = {
      .io_handle = io_handle,
      .panel_handle = panel_handle,
      .buffer_size = DISPLAY_WIDTH * DRAW_BUF_LINES,
      .double_buffer = true,
      .trans_size = 0,
      .hres = DISPLAY_WIDTH,
      .vres = DISPLAY_HEIGHT,
      .monochrome = false,
      .flags =
          {
              .buff_dma = true,
              .buff_spiram = false,
              .swap_bytes = true,
          },
  };
  s_lvgl_disp = lvgl_port_add_disp(&disp_cfg);
  assert(s_lvgl_disp != NULL);

  // Rotation MUST be applied after lvgl_port_add_disp() — the port resets
  // the ST7789 MADCTL register during display registration.
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
  ESP_ERROR_CHECK(esp_lcd_panel_set_gap(
      panel_handle, CONFIG_DISPLAY_ST7789_GAP_X, CONFIG_DISPLAY_ST7789_GAP_Y));

  // Acquire the LVGL port mutex before touching LVGL widgets. The port task
  // is already running at this point, so we must wait on the lock rather
  // than pass 0 (non-waiting try). If the lock cannot be acquired within a
  // generous timeout, init has gone wrong — abort rather than corrupt LVGL
  // state by calling ui_create() unlocked.
  if (lvgl_port_lock(1000)) {
    ui_create();
    /* Do not wait for the periodic LVGL timer for the first transfer. This
     * immediately sends the dark-blue startup screen and the Ready labels. */
    lv_refr_now(s_lvgl_disp);
    lvgl_port_unlock();
  } else {
    ESP_LOGE(TAG, "Failed to acquire LVGL lock during init — UI not built");
    abort();
  }

#if CONFIG_DISPLAY_BL_GPIO >= 0
  {
    gpio_set_level(CONFIG_DISPLAY_BL_GPIO, 1);
  }
#endif

  s_display.state = DISPLAY_STATE_STANDBY;
  s_display.dirty = true;

  if (rtsp_events_register(on_rtsp_event, NULL) != 0) {
    ESP_LOGE(TAG, "Could not register RTSP metadata listener");
  } else {
    ESP_LOGI(TAG, "RTSP metadata listener registered");
  }

  // Pinned to Core 0 — audio runs on Core 1
  xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 3, NULL, 0);

  ESP_LOGI(TAG, "ST7789 display initialized (LVGL 9 + esp_lvgl_port)");
}
