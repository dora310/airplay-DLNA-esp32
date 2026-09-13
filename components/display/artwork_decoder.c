#include "artwork_decoder.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tjpgd/tjpgd.h"

#include <string.h>

static const char *TAG = "artwork_decode";

typedef struct {
  const uint8_t *input;
  size_t input_len;
  size_t input_pos;
  uint16_t *output;
  uint16_t output_width;
  uint16_t output_height;
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
      rect->bottom < rect->top || rect->right >= ctx->output_width ||
      rect->bottom >= ctx->output_height) {
    return 0;
  }

  const uint16_t block_width = rect->right - rect->left + 1;
  const uint16_t block_height = rect->bottom - rect->top + 1;
  const uint16_t *source = (const uint16_t *)bitmap;

  for (uint16_t row = 0; row < block_height; ++row) {
    uint16_t *destination =
        ctx->output + (size_t)(rect->top + row) * ctx->output_width +
        rect->left;
    memcpy(destination, source + (size_t)row * block_width,
           (size_t)block_width * sizeof(uint16_t));
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

  uint8_t scale = 0;
  uint32_t longest =
      decoder.width > decoder.height ? decoder.width : decoder.height;
  while (scale < 3 && (longest >> scale) > max_dimension) {
    ++scale;
  }

  ctx.output_width = (uint16_t)(decoder.width >> scale);
  ctx.output_height = (uint16_t)(decoder.height >> scale);

  /* A JPEG larger than 8 * max_dimension cannot be bounded by TJpgDec's
   * maximum 1/8 scale. Reject it rather than allocating an unexpected image. */
  if (ctx.output_width > max_dimension || ctx.output_height > max_dimension ||
      ctx.output_width == 0 || ctx.output_height == 0) {
    ESP_LOGW(TAG, "JPEG dimensions exceed safe limit: %ux%u",
             (unsigned)decoder.width, (unsigned)decoder.height);
    heap_caps_free(work);
    return false;
  }

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

  result = jd_decomp(&decoder, jpeg_output, scale);
  heap_caps_free(work);
  if (result != JDR_OK) {
    ESP_LOGW(TAG, "JPEG decode failed: %d", (int)result);
    heap_caps_free(ctx.output);
    return false;
  }

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
