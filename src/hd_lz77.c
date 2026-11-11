#include "hd_lz77.h"

typedef struct {
    uint16_t good_length;
    uint16_t max_lazy;
    uint16_t nice_length;
    uint16_t max_chain;
    int      lazy;
} lz_conf;

static const lz_conf LZ_CONF[10] = {
    {0, 0, 0, 0, 0},
    {4, 4, 8, 4, 0},
    {4, 5, 16, 8, 0},
    {4, 6, 32, 32, 0},
    {4, 4, 16, 16, 0},
    {8, 16, 32, 32, 0},
    {8, 16, 128, 128, 1},   /* 6 default */
    {8, 32, 128, 256, 1},
    {32, 128, 258, 1024, 1},
    {32, 258, 258, 4096, 1},
};

#define LZ_WSIZE     32768
#define LZ_MIN_MATCH 3
#define LZ_MAX_MATCH 258
#define LZ_HASH_BITS 15
#define LZ_HASH_SIZE (1 << LZ_HASH_BITS)
#define LZ_WMASK     (LZ_WSIZE - 1)

static inline uint32_t lz_h3(const uint8_t *p) {
    return ((uint32_t)p[0] << 10 ^ (uint32_t)p[1] << 5 ^ p[2]) & (LZ_HASH_SIZE - 1);
}

typedef struct {
    const uint8_t *in;
    size_t inlen;
    int32_t head[LZ_HASH_SIZE];
    int32_t chain[LZ_WSIZE];
} lz_ctx;

static inline void lz_insert(lz_ctx *c, size_t pos) {
    if (pos + LZ_MIN_MATCH > c->inlen) return;
    uint32_t h = lz_h3(c->in + pos);
    c->chain[pos & LZ_WMASK] = c->head[h];
    c->head[h] = (int32_t)pos;
}

static inline int lz_longest(lz_ctx *c, size_t pos, int max_chain, int nice,
                             uint32_t *out_dist) {
    int best_len = 0;
    uint32_t best_dist = 0;
    if (pos + LZ_MIN_MATCH > c->inlen) return 0;
    uint32_t h = lz_h3(c->in + pos);
    int32_t cur = c->head[h];
    size_t limit = (pos > LZ_WSIZE) ? pos - LZ_WSIZE : 0;
    size_t maxlen = c->inlen - pos;
    if (maxlen > LZ_MAX_MATCH) maxlen = LZ_MAX_MATCH;
    while (cur >= 0 && (size_t)cur >= limit && max_chain-- > 0) {
        const uint8_t *m = c->in + cur, *p = c->in + pos;
        int ml = 0;
        if (best_len == 0 || m[best_len] == p[best_len]) {
            while (ml < (int)maxlen && m[ml] == p[ml]) ml++;
            if (ml > best_len) {
                best_len = ml;
                best_dist = (uint32_t)(pos - (size_t)cur);
                if (ml >= nice) break;
            }
        }
        cur = c->chain[cur & LZ_WMASK];
    }
    if (best_len >= LZ_MIN_MATCH) { *out_dist = best_dist; return best_len; }
    return 0;
}

int hd_lz77_encode(const uint8_t *in, size_t inlen, int level, hd_tokstream *ts) {
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    const lz_conf *cf = &LZ_CONF[level];

    lz_ctx *c = malloc(sizeof(lz_ctx));
    if (!c) hd_die("OOM lz77 ctx");
    c->in = in;
    c->inlen = inlen;
    for (uint32_t i = 0; i < LZ_HASH_SIZE; i++) c->head[i] = -1;

    size_t pos = 0;
    while (pos < inlen) {
        uint32_t dist = 0;
        int len = lz_longest(c, pos, cf->max_chain, cf->nice_length, &dist);
        lz_insert(c, pos);

        if (cf->lazy && len >= LZ_MIN_MATCH && len < cf->max_lazy &&
            pos + 4 <= inlen) {
            uint32_t ndist = 0;
            int nlen = lz_longest(c, pos + 1, cf->max_chain > 32 ? 32 : cf->max_chain,
                                  cf->nice_length, &ndist);
            if (nlen > len) {
                hd_ts_push(ts, in[pos], 0);
                pos++;
                continue;
            }
        }

        if (len >= LZ_MIN_MATCH) {
            hd_ts_push(ts, (uint32_t)len, dist);
            for (size_t k = 1; k < (size_t)len; k++)
                lz_insert(c, pos + k);
            pos += (size_t)len;
        } else {
            hd_ts_push(ts, in[pos], 0);
            pos++;
        }
    }
    free(c);
    return 0;
}
