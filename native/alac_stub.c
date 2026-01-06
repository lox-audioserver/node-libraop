#include <stdlib.h>
#include <stdint.h>
#include "alac.h"

alac_file *create_alac(int samplesize, int numchannels) {
  (void)samplesize;
  (void)numchannels;
  return NULL;
}

void delete_alac(alac_file *alac) {
  (void)alac;
}

void decode_frame(alac_file *alac, unsigned char *inbuffer, void *outbuffer, int *outputsize) {
  (void)alac;
  (void)inbuffer;
  (void)outbuffer;
  if (outputsize) *outputsize = 0;
}

void alac_set_info(alac_file *alac, char *inputbuffer) {
  (void)alac;
  (void)inputbuffer;
}

void allocate_buffers(alac_file *alac) {
  (void)alac;
}
