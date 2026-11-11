/* hd_bitio.h - Deflate bit-level I/O (LSB-first within bytes, RFC 1951) */
#ifndef HD_BITIO_H
#define HD_BITIO_H

#include "hd_common.h"

/* ---------------- Bit reader ---------------- */
typedef struct {
    const uint8_t *in;      /* input buffer */
    size_t   inlen;
    size_t   bytepos;
    uint64_t bitbuf;
    int      bitcnt;
    int      overrun;
} hd_br;

static inline void hd_br_init(hd_br *r, const uint8_t *in, size_t inlen) {
    r->in = in; r->inlen = inlen;
    r->bytepos = 0; r->bitbuf = 0; r->bitcnt = 0; r->overrun = 0;
}

static inline void hd_br_fill(hd_br *r, int n) {
    while (r->bitcnt < n) {
        if (r->bytepos < r->inlen) {
            r->bitbuf |= (uint64_t)r->in[r->bytepos++] << r->bitcnt;
        } else {
            r->overrun += 8;
        }
        r->bitcnt += 8;
    }
}

static inline uint32_t hd_br_peek(hd_br *r, int n) {
    hd_br_fill(r, n);
    return (uint32_t)(r->bitbuf & ((1ULL << n) - 1));
}

static inline uint32_t hd_br_take(hd_br *r, int n) {
    hd_br_fill(r, n);
    uint32_t v = (uint32_t)(r->bitbuf & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1)));
    r->bitbuf >>= n;
    r->bitcnt -= n;
    return v;
}

static inline int hd_br_align(hd_br *r) {
    int k = r->bitcnt & 7;
    r->bitbuf >>= k;
    r->bitcnt -= k;
    return k;
}

static inline uint64_t hd_br_bitpos(const hd_br *r) {
    return (uint64_t)r->bytepos * 8 + (uint64_t)r->overrun - (uint64_t)r->bitcnt;
}

/* ---------------- Bit writer ---------------- */
typedef struct {
    uint8_t *out;
    size_t   len, cap;
    uint64_t bitbuf;
    int      bitcnt;
} hd_bw;

static inline void hd_bw_init(hd_bw *w, uint8_t *out, size_t cap) {
    w->out = out; w->len = 0; w->cap = cap;
    w->bitbuf = 0; w->bitcnt = 0;
}

static inline void hd_bw_flush_bytes(hd_bw *w) {
    while (w->bitcnt >= 8) {
        if (w->len < w->cap) w->out[w->len] = (uint8_t)(w->bitbuf & 0xFF);
        w->len++;
        w->bitbuf >>= 8;
        w->bitcnt -= 8;
    }
}

static inline void hd_bw_put(hd_bw *w, uint32_t v, int n) {
    if (n == 0) return;
    w->bitbuf |= ((uint64_t)(v & ((n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1)))) << w->bitcnt;
    w->bitcnt += n;
    hd_bw_flush_bytes(w);
}

static inline void hd_bw_align(hd_bw *w) {
    int k = (8 - (w->bitcnt & 7)) & 7;
    if (k) hd_bw_put(w, 0, k);
}

static inline uint64_t hd_bw_bitpos(const hd_bw *w) {
    return (uint64_t)w->len * 8 + (uint64_t)w->bitcnt;
}

#endif
