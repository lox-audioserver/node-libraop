#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "encoder.h"

struct encoder_s {
  uint32_t sample_rate;
  uint8_t channels;
  uint8_t sample_size;
  size_t max_frames;
  char mimetype[16];
};

struct encoder_s* encoder_create(char* codec, uint32_t sample_rate, uint8_t channels,
                                 uint8_t sample_size, size_t max_frames, size_t* icy_interval) {
  (void)codec;
  (void)icy_interval;
  struct encoder_s* enc = (struct encoder_s*)calloc(1, sizeof(struct encoder_s));
  if (!enc) return NULL;
  enc->sample_rate = sample_rate;
  enc->channels = channels;
  enc->sample_size = sample_size ? sample_size : 2;
  enc->max_frames = max_frames ? max_frames : ENCODER_MAX_FRAMES;
  strncpy(enc->mimetype, "audio/L16", sizeof(enc->mimetype) - 1);
  return enc;
}

char* encoder_mimetype(struct encoder_s* encoder) {
  return encoder ? encoder->mimetype : "audio/L16";
}

bool encoder_open(struct encoder_s* encoder) {
  (void)encoder;
  return true;
}

void encoder_close(struct encoder_s* encoder) {
  (void)encoder;
}

size_t encoder_space(struct encoder_s* encoder) {
  if (!encoder) return 0;
  return encoder->max_frames * encoder->channels;
}

uint8_t* encoder_encode(struct encoder_s* encoder, int16_t* pcm, size_t frames, size_t* bytes) {
  if (!encoder || !pcm || !bytes) return NULL;
  size_t b = frames * encoder->channels * encoder->sample_size;
  uint8_t* out = (uint8_t*)malloc(b);
  if (!out) return NULL;
  memcpy(out, pcm, b);
  *bytes = b;
  return out;
}

void encoder_delete(struct encoder_s* encoder) {
  free(encoder);
}
