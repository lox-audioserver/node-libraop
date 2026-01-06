#include "alac_wrapper.h"

#include <stdlib.h>
#include <string.h>

struct alac_codec_s {
  int placeholder;
};

struct alac_codec_s* alac_create_encoder(int frame_len, int sample_rate, int sample_size, int channels) {
  (void)frame_len;
  (void)sample_rate;
  (void)sample_size;
  (void)channels;
  return calloc(1, sizeof(struct alac_codec_s));
}

void alac_delete_encoder(struct alac_codec_s* codec) {
  free(codec);
}

void pcm_to_alac(struct alac_codec_s* codec, int16_t* sample, int frames, uint8_t** encoded, int* size) {
  (void)codec;
  if (!encoded || !size) return;
  size_t bytes = (size_t)frames * 4;  // assume 16-bit stereo
  uint8_t* out = NULL;
  if (sample && bytes) {
    out = (uint8_t*)malloc(bytes);
    if (out) memcpy(out, sample, bytes);
  }
  *encoded = out;
  *size = out ? (int)bytes : 0;
}

void pcm_to_alac_raw(int16_t* sample, int frames, uint8_t** encoded, int* size, int chunk_len) {
  (void)chunk_len;
  pcm_to_alac(NULL, sample, frames, encoded, size);
}
