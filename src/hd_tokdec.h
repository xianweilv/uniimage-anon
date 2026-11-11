/* hd_tokdec.h - Deflate bitstream -> LZ77 token stream (Huffman decoding only) */
#ifndef HD_TOKDEC_H
#define HD_TOKDEC_H

#include "hd_common.h"
#include "hd_bitio.h"
#include "hd_huffman.h"

typedef struct {
    hd_token *tok;
    size_t    ntok, cap;
    uint64_t  nliterals;
    uint64_t  nmatches;
    uint64_t  matched_bytes;
} hd_tokstream;

static inline void hd_ts_init(hd_tokstream *ts) {
    ts->cap = 4096;
    ts->tok = malloc(ts->cap * sizeof(hd_token));
    if (!ts->tok) hd_die("OOM tokstream");
    ts->ntok = 0;
    ts->nliterals = ts->nmatches = ts->matched_bytes = 0;
}
static inline void hd_ts_free(hd_tokstream *ts) {
    free(ts->tok); ts->tok = NULL; ts->ntok = ts->cap = 0;
}
static inline void hd_ts_reset(hd_tokstream *ts) {
    ts->ntok = 0;
    ts->nliterals = ts->nmatches = ts->matched_bytes = 0;
}
static inline void hd_ts_push(hd_tokstream *ts, uint32_t v, uint32_t d) {
    if (ts->ntok == ts->cap) {
        ts->cap *= 2;
        ts->tok = realloc(ts->tok, ts->cap * sizeof(hd_token));
        if (!ts->tok) hd_die("OOM tokstream grow");
    }
    ts->tok[ts->ntok].v = v;
    ts->tok[ts->ntok].d = d;
    ts->ntok++;
    if (d) { ts->nmatches++; ts->matched_bytes += v; }
    else   { ts->nliterals++; }
}

#define HD_TD_OK          0
#define HD_TD_ERR_DATA   (-1)
#define HD_TD_ERR_MEM    (-2)

 * parameter：
int hd_tokens_from_deflate(const uint8_t *in, size_t inlen,
                           hd_tokstream *ts, uint64_t *stream_bits);


int hd_scan_blocks(const uint8_t *in, size_t inlen, hd_blkinfo *blocks, int cap);

static inline uint64_t hd_ts_out_bytes(const hd_tokstream *ts) {
    return ts->nliterals + ts->matched_bytes;
}

#endif
