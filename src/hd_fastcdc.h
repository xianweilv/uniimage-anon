/* hd_fastcdc.h - FastCDC content-defined chunking (gear hash, dual-mask normalization) */
#ifndef HD_FASTCDC_H
#define HD_FASTCDC_H

#include "hd_common.h"

int hd_fastcdc(const uint8_t *in, size_t inlen,
               uint32_t min_sz, uint32_t avg_sz, uint32_t max_sz,
               size_t *cuts, int cutcap);

void hd_cdc_params(uint32_t avg, uint32_t *min_sz, uint32_t *max_sz);

typedef struct {
    uint64_t h;
    uint32_t min_sz, avg_sz, max_sz;
    uint32_t mask_hard, mask_easy;
    uint32_t chunk_len;
} hd_cdc_stream;

void hd_cdc_begin(hd_cdc_stream *cs, uint32_t avg);
size_t hd_cdc_feed(hd_cdc_stream *cs, const uint8_t *p, size_t n, int is_last);

#endif
