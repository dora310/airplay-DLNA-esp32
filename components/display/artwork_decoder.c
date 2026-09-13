#include "artwork_decoder.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tjpgd/tjpgd.h"

#include <string.h>

static const char *TAG = "artwork_decode";

typedef struct {
  uint32_t red;
  uint32_t green;
  uint32_t blue;
  uint32_t count;
} pixel_accumulator_t;

typedef struct {
  const uint8_t *input;
  size_t input_len;
  size_t input_pos;
  uint16_t *output;
  uint16_t output_width;
  uint16_t output_height;
  uint16_t source_width;
  uint16_t source_height;
  pixel_accumulator_t *accumulators;
  unsigned callback_count;
} decode_context_t;

static size_t jpeg_input(JDEC *decoder, uint8_t *destination,
                         size_t requested) {
  decode_context_t *ctx = (decode_context_t *)decoder->device;
  size_t remaining = ctx->input_len - ctx->input_pos;
  size_t count = requested < remaining ? requested : remaining;

  if (destination && count) {
    memcpy(destination, ctx->input + ctx->input_pos, count);
  }
  ctx->input_pos += count;
  return count;
}

static int jpeg_output(JDEC *decoder, void *bitmap, JRECT *rect) {
  decode_context_t *ctx = (decode_context_t *)decoder->device;
  if (!bitmap || !rect || rect->right < rect->left ||
      rect->bottom < rect->top || rect->right >= ctx->source_width ||
      rect->bottom >= ctx->source_height) {
    return 0;
  }

  const uint16_t block_width = rect->right - rect->left + 1;
  const uint16_t block_height = rect->bottom - rect->top + 1;
  const uint16_t *source = (const uint16_t *)bitmap;

  /* Decode at native JPEG resolution and box-filter into the final display
   * size. TJpgDec's MCU descaling path produces visible repeated blocks for
   * some Apple Music covers, especially at 1/4 and 1/8 scale. Accumulating
   * every native pixel avoids those artifacts and gives smoother thumbnails. */
  for (uint16_t row = 0; row < block_height; ++row) {
    uint32_t source_y = rect->top + row;
    uint32_t target_y =
        source_y * ctx->output_height / ctx->source_height;
    for (uint16_t column = 0; column < block_width; ++column) {
      uint32_t source_x = rect->left + column;
      uint32_t target_x =
          source_x * ctx->output_width / ctx->source_width;
      uint16_t pixel = source[(size_t)row * block_width + column];
      pixel_accumulator_t *acc =
          &ctx->accumulators[(size_t)target_y * ctx->output_width + target_x];
      acc->red += (pixel >> 11) & 0x1fU;
      acc->green += (pixel >> 5) & 0x3fU;
      acc->blue += pixel & 0x1fU;
      ++acc->count;
    }
  }

  /* Artwork has no real-time deadline. Yield periodically so decoding cannot
   * monopolise a core needed by Wi-Fi, RTSP or audio-buffer maintenance. */
  if ((++ctx->callback_count & 0x0fU) == 0) {
    vTaskDelay(1);
  }
  return 1;
}

bool artwork_decoder_decode_jpeg(const uint8_t *jpeg, size_t jpeg_len,
                                 uint16_t max_dimension,
                                 artwork_rgb565_t *out) {
  if (!out) {
    return false;
  }
  memset(out, 0, sizeof(*out));

  if (!jpeg || jpeg_len < 4 || max_dimension == 0 || jpeg[0] != 0xff ||
      jpeg[1] != 0xd8) {
    ESP_LOGW(TAG, "Rejected invalid JPEG input");
    return false;
  }

  decode_context_t ctx = {
      .input = jpeg,
      .input_len = jpeg_len,
  };
  JDEC decoder;

  /* TJpgDec fast-decode level 1 normally needs only a few KiB. A bounded
   * 12 KiB internal pool avoids consuming the audio PSRAM buffer and cleanly
   * rejects unusual JPEGs that need more workspace. */
  const size_t work_size = 12U * 1024U;
  void *work = heap_caps_malloc(work_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!work) {
    ESP_LOGW(TAG, "No internal memory for JPEG workspace");
    return false;
  }

  JRESULT result = jd_prepare(&decoder, jpeg_input, work, work_size, &ctx);
  if (result != JDR_OK || decoder.width == 0 || decoder.height == 0) {
    ESP_LOGW(TAG, "JPEG prepare failed: %d", (int)result);
    heap_caps_free(work);
    return false;
  }

  /* Bound CPU time as well as memory. AirPlay normally supplies square covers
   * well below this limit; an extreme image is rejected without affecting
   * text metadata or playback. */
  if (decoder.width > 1200 || decoder.height > 1200) {
    ESP_LOGW(TAG, "JPEG source dimensions exceed safe limit: %ux%u",
             (unsigned)decoder.width, (unsigned)decoder.height);
    heap_caps_free(work);
    return false;
  }

  ctx.source_width = decoder.width;
  ctx.source_height = decoder.height;
  uint16_t longest =
      decoder.width > decoder.height ? decoder.width : decoder.height;
  uint16_t target_longest =
      longest > max_dimension ? max_dimension : longest;
  if (decoder.width >= decoder.height) {
    ctx.output_width = target_longest;
    ctx.output_height = (uint16_t)(((uint32_t)decoder.height * target_longest +
                                    decoder.width / 2U) /
                                   decoder.width);
  } else {
    ctx.output_height = target_longest;
    ctx.output_width = (uint16_t)(((uint32_t)decoder.width * target_longest +
                                   decoder.height / 2U) /
                                  decoder.height);
  }
  if (ctx.output_width == 0) ctx.output_width = 1;
  if (ctx.output_height == 0) ctx.output_height = 1;

  const size_t pixel_count =
      (size_t)ctx.output_width * (size_t)ctx.output_height;
  if (pixel_count > SIZE_MAX / sizeof(uint16_t)) {
    heap_caps_free(work);
    return false;
  }

  const size_t output_size = pixel_count * sizeof(uint16_t);
  ctx.output = heap_caps_calloc(pixel_count, sizeof(uint16_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ctx.output) {
    ESP_LOGW(TAG, "No PSRAM for %ux%u decoded artwork",
             (unsigned)ctx.output_width, (unsigned)ctx.output_height);
    heap_caps_free(work);
    return false;
  }

  ctx.accumulators = heap_caps_calloc(
      pixel_count, sizeof(pixel_accumulator_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ctx.accumulators) {
    ESP_LOGW(TAG, "No PSRAM for artwork downsampling workspace");
    heap_caps_free(ctx.output);
    heap_caps_free(work);
    return false;
  }

  result = jd_decomp(&decoder, jpeg_output, 0);
  heap_caps_free(work);
  if (result != JDR_OK) {
    ESP_LOGW(TAG, "JPEG decode failed: %d", (int)result);
    heap_caps_free(ctx.accumulators);
    heap_caps_free(ctx.output);
    return false;
  }

  for (size_t i = 0; i < pixel_count; ++i) {
    const pixel_accumulator_t *acc = &ctx.accumulators[i];
    if (acc->count == 0) {
      ctx.output[i] = 0;
      continue;
    }
    uint16_t red = (uint16_t)(acc->red / acc->count);
    uint16_t green = (uint16_t)(acc->green / acc->count);
    uint16_t blue = (uint16_t)(acc->blue / acc->count);
    ctx.output[i] = (uint16_t)((red << 11) | (green << 5) | blue);
  }
  heap_caps_free(ctx.accumulators);

  out->pixels = (uint8_t *)ctx.output;
  out->data_size = output_size;
  out->width = ctx.output_width;
  out->height = ctx.output_height;
  ESP_LOGI(TAG, "Decoded artwork to RGB565: %ux%u (%zu bytes)",
           (unsigned)out->width, (unsigned)out->height, out->data_size);
  return true;
}

void artwork_decoder_free(artwork_rgb565_t *image) {
  if (!image) {
    return;
  }
  if (image->pixels) {
    heap_caps_free(image->pixels);
  }
  memset(image, 0, sizeof(*image));
}
