#include "hd_fastcdc.h"

static uint8_t GEAR[256];
static int gear_init = 0;

static void gear_init_fn(void) {
    uint64_t h = 88172645463325252ULL;
    for (int i = 0; i < 256; i++) {
        h = h * 6364136223846793005ULL + 1442695040888963407ULL;
        GEAR[i] = (uint8_t)(h >> 33);
    }
    gear_init = 1;
}

void hd_cdc_params(uint32_t avg, uint32_t *min_sz, uint32_t *max_sz) {
    *min_sz = avg / 4;
    if (*min_sz < 512) *min_sz = 512;
    *max_sz = avg * 8;
    if (*max_sz > 1024 * 1024) *max_sz = 1024 * 1024;
    if (*max_sz < *min_sz * 2) *max_sz = *min_sz * 2;
}

int hd_fastcdc(const uint8_t *in, size_t inlen,
               uint32_t min_sz, uint32_t avg_sz, uint32_t max_sz,
               size_t *cuts, int cutcap) {
    if (!gear_init) gear_init_fn();

    uint32_t bits = 1;
    while ((1u << bits) < avg_sz) bits++;
    uint32_t mask_hard = (1u << (bits + 1)) - 1;   /* p = 2^-(bits+1) */
    uint32_t mask_easy = (1u << (bits - 1)) - 1;   /* p = 2^-(bits-1) */

    int n = 0;
    size_t pos = 0;
    while (pos < inlen) {
        size_t remain = inlen - pos;
        if (remain <= min_sz) break;
        uint32_t maxlen = (remain < max_sz) ? (uint32_t)remain : max_sz;
        uint64_t h = 0;
        size_t cut = pos + maxlen;
        for (size_t i = 0; i < maxlen; i++) {
            h = ((h << 1) + GEAR[in[pos + i]]) & 0xFFFFFFFFULL;
            if (i + 1 < min_sz) continue;
            uint32_t m = (i + 1 <= avg_sz - min_sz / 2) ? mask_hard : mask_easy;
            if (((uint32_t)h & m) == m) { cut = pos + i + 1; break; }
        }
        if (n >= cutcap) return -1;
        cuts[n++] = cut;
        pos = cut;
    }
    return n;
}

void hd_cdc_begin(hd_cdc_stream *cs, uint32_t avg) {
    if (!gear_init) gear_init_fn();
    hd_cdc_params(avg, &cs->min_sz, &cs->max_sz);
    cs->avg_sz = avg;
    uint32_t bits = 1;
    while ((1u << bits) < avg) bits++;
    cs->mask_hard = (1u << (bits + 1)) - 1;
    cs->mask_easy = (1u << (bits - 1)) - 1;
    cs->h = 0;
    cs->chunk_len = 0;
}

size_t hd_cdc_feed(hd_cdc_stream *cs, const uint8_t *p, size_t n, int is_last) {
    for (size_t i = 0; i < n; i++) {
        cs->h = ((cs->h << 1) + GEAR[p[i]]) & 0xFFFFFFFFULL;
        cs->chunk_len++;
        if (cs->chunk_len >= cs->max_sz) return i + 1;
        if (cs->chunk_len >= cs->min_sz) {
            uint32_t m = (cs->chunk_len <= cs->avg_sz) ? cs->mask_hard : cs->mask_easy;
            if (((uint32_t)cs->h & m) == m) return i + 1;
        }
    }
    (void)is_last;
    return 0;
}
