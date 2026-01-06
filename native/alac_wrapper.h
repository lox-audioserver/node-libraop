#pragma once

#include <stdint.h>
#include <stddef.h>

struct alac_codec_s;

struct alac_codec_s* alac_create_encoder(int frame_len, int sample_rate, int sample_size, int channels);
void alac_delete_encoder(struct alac_codec_s* codec);
void pcm_to_alac(struct alac_codec_s* codec, int16_t* sample, int frames, uint8_t** encoded, int* size);
void pcm_to_alac_raw(int16_t* sample, int frames, uint8_t** encoded, int* size, int chunk_len);
