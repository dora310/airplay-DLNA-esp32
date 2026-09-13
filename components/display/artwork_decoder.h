#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t *pixels;
  size_t data_size;
  uint16_t width;
  uint16_t height;
} artwork_rgb565_t;

/**
 * Decode a baseline JPEG into a bounded RGB565 PSRAM buffer.
 *
 * The decoder selects a hardware-independent 1/1, 1/2, 1/4 or 1/8 scale so
 * neither output dimension exceeds max_dimension. The caller owns out->pixels
 * on success and must release it with artwork_decoder_free().
 */
bool artwork_decoder_decode_jpeg(const uint8_t *jpeg, size_t jpeg_len,
                                 uint16_t max_dimension,
                                 artwork_rgb565_t *out);

void artwork_decoder_free(artwork_rgb565_t *image);
