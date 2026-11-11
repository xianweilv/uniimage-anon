#include "hd_tokenc.h"

typedef struct {
    uint8_t  sym;
    uint16_t extra;
    uint8_t  ebits;
} cl_item;

static size_t cl_rle_encode(const uint8_t *lens, int n, cl_item *out, int outmax) {
    size_t m = 0;
    int i = 0;
    while (i < n) {
        uint8_t cur = lens[i];
        int run = 1;
        while (i + run < n && lens[i + run] == cur) run++;
        i += run;
        if (cur == 0) {
            while (run >= 11) {
                int r = run > 138 ? 138 : run;
                if (m >= (size_t)outmax) return 0;
                out[m++] = (cl_item){18, (uint16_t)(r - 11), 7};
                run -= r;
            }
            if (run >= 3) {
                if (m >= (size_t)outmax) return 0;
                out[m++] = (cl_item){17, (uint16_t)(run - 3), 3};
                run = 0;
            }
            while (run-- > 0) {
                if (m >= (size_t)outmax) return 0;
                out[m++] = (cl_item){0, 0, 0};
            }
        } else {
            if (m >= (size_t)outmax) return 0;
            out[m++] = (cl_item){cur, 0, 0};
            run--;
            while (run >= 3) {
                int r = run > 6 ? 6 : run;
                if (m >= (size_t)outmax) return 0;
                out[m++] = (cl_item){16, (uint16_t)(r - 3), 2};
                run -= r;
            }
            while (run-- > 0) {
                if (m >= (size_t)outmax) return 0;
                out[m++] = (cl_item){cur, 0, 0};
            }
        }
    }
    return m;
}

static uint8_t dist_tab[512];
static int dist_tab_init = 0;

static void dist_tab_build(void) {
    for (int d = 1; d < 256; d++) {
        int ds = 0;
        while (ds + 1 < HD_DIST_SYMS && HD_DIST_BASE[ds + 1] <= d) ds++;
        dist_tab[d] = (uint8_t)ds;
    }
    dist_tab[0] = 0;
    for (int idx = 0; idx < 256; idx++) {
        int d = (idx << 7) + 1;
        if (d > 32768) d = 32768;
        int ds = 0;
        while (ds + 1 < HD_DIST_SYMS && HD_DIST_BASE[ds + 1] <= d) ds++;
        dist_tab[256 + idx] = (uint8_t)ds;
    }
    dist_tab_init = 1;
}

static inline int hd_dist_sym(uint32_t d) {
    if (!dist_tab_init) dist_tab_build();
    int ds = (d < 256) ? dist_tab[d] : dist_tab[256 + ((d - 1) >> 7)];
    while (ds + 1 < HD_DIST_SYMS && HD_DIST_BASE[ds + 1] <= d) ds++;
    return ds;
}

static int write_dyn_header(hd_bw *w,
                            const uint8_t *ll_lens, int hlit,
                            const uint8_t *d_lens, int hdist) {
    cl_item items[HD_NLITLEN + HD_DIST_SYMS + 8];
    uint8_t all[HD_NLITLEN + HD_DIST_SYMS];
    memcpy(all, ll_lens, hlit);
    memcpy(all + hlit, d_lens, hdist);
    size_t nitems = cl_rle_encode(all, hlit + hdist, items,
                                   (int)(sizeof(items) / sizeof(items[0])));
    if (nitems == 0) return -1;

    uint32_t clfreq[HD_CLEN_SYMS] = {0};
    for (size_t k = 0; k < nitems; k++) clfreq[items[k].sym]++;
    uint8_t cl_lens[HD_CLEN_SYMS] = {0};
    hd_huff_lengths(clfreq, HD_CLEN_SYMS, 7, cl_lens);
    hd_huffenc clenc;
    hd_huffenc_build(&clenc, cl_lens, HD_CLEN_SYMS);

    int hclen = HD_CLEN_SYMS;
    while (hclen > 4 && cl_lens[HD_CLEN_ORDER[hclen - 1]] == 0) hclen--;

    hd_bw_put(w, (uint32_t)(hlit - 257), 5);
    hd_bw_put(w, (uint32_t)(hdist - 1), 5);
    hd_bw_put(w, (uint32_t)(hclen - 4), 4);
    for (int k = 0; k < hclen; k++)
        hd_bw_put(w, cl_lens[HD_CLEN_ORDER[k]], 3);
    for (size_t k = 0; k < nitems; k++) {
        hd_bw_put(w, clenc.code[items[k].sym], clenc.len[items[k].sym]);
        if (items[k].ebits) hd_bw_put(w, items[k].extra, items[k].ebits);
    }
    return 0;
}

typedef struct {
    uint32_t lit[256];
    uint32_t len[259];
    uint32_t dist[30];
    uint32_t lex[29];       /* li -> base|nbits<<16 */
    uint32_t dex[30];       /* ds -> base|nbits<<16 */
} enc_tabs;

static void enc_tabs_build(enc_tabs *t, const hd_huffenc *lle, const hd_huffenc *de) {
    for (int v = 0; v < 256; v++)
        t->lit[v] = lle->code[v] | ((uint32_t)lle->len[v] << 16);
    for (int v = 3; v <= 258; v++) {
        uint32_t ls = HD_LEN2SYM[v];
        int li = (int)ls - 257;
        t->len[v] = lle->code[ls] | ((uint32_t)lle->len[ls] << 16)
                    | ((uint32_t)li << 20);
    }
    for (int ds = 0; ds < HD_DIST_SYMS; ds++) {
        t->dist[ds] = de->code[ds] | ((uint32_t)de->len[ds] << 16);
        t->dex[ds] = HD_DIST_BASE[ds] | ((uint32_t)HD_DIST_EXTRA[ds] << 16);
    }
    for (int li = 0; li < 29; li++)
        t->lex[li] = HD_LEN_BASE[li] | ((uint32_t)HD_LEN_EXTRA[li] << 16);
}

static hd_huffenc fx_lit, fx_dist;
static int fx_init = 0;

static void fx_init_fn(void) {
    if (fx_init) return;
    hd_huffenc_build(&fx_lit, HD_FIXED_LITLEN_LEN, HD_LITLEN_SYMS);
    hd_huffenc_build(&fx_dist, HD_FIXED_DIST_LEN, HD_DIST_SYMS);
    fx_init = 1;
}

void hd_tokenc_init(void) {
    dist_tab_build();
    fx_init_fn();
}

int hd_encode_block(hd_bw *w, const hd_token *tok, size_t n, int final) {
    if (n == 0) {
        if (!final) return 0;
        hd_bw_put(w, 1, 1);          /* BFINAL */
        hd_bw_put(w, 1, 2);          /* BTYPE=01 fixed */
        hd_bw_put(w, 0, 7);
        return 0;
    }
    fx_init_fn();

    uint32_t llfreq[HD_NLITLEN] = {0};
    uint32_t dfreq[HD_DIST_SYMS] = {0};
    uint64_t out_bytes = 0;
    int all_lit = 1;
    for (size_t i = 0; i < n; i++) {
        if (tok[i].d == 0) {
            llfreq[tok[i].v]++;
            out_bytes++;
        } else {
            all_lit = 0;
            llfreq[HD_LEN2SYM[tok[i].v]]++;
            dfreq[hd_dist_sym(tok[i].d)]++;
            out_bytes += tok[i].v;
        }
    }
    llfreq[HD_EOB]++;

    uint8_t ll_lens[HD_NLITLEN] = {0};
    uint8_t d_lens[HD_DIST_SYMS] = {0};
    int nll = hd_huff_lengths(llfreq, HD_NLITLEN, HD_MAX_BITS, ll_lens);
    int nd  = hd_huff_lengths(dfreq, HD_DIST_SYMS, HD_MAX_BITS, d_lens);
    if (nll < 2) hd_die("encode_block: litlen table degenerate");

    int hlit = HD_NLITLEN;
    while (hlit > 257 && ll_lens[hlit - 1] == 0) hlit--;
    int hdist;
    if (nd == 0) hdist = 1;
    else {
        hdist = HD_DIST_SYMS;
        while (hdist > 1 && d_lens[hdist - 1] == 0) hdist--;
    }
    uint64_t dyn_bits = 3 + (uint64_t)hd_dyn_header_bits(ll_lens, hlit,
                                                          d_lens, hdist);
    uint64_t extra_bits = 0;
    for (size_t i = 0; i < n; i++) {
        if (tok[i].d == 0) {
            dyn_bits += ll_lens[tok[i].v];
        } else {
            uint32_t ls = HD_LEN2SYM[tok[i].v];
            dyn_bits += ll_lens[ls];
            int li = (int)ls - 257;
            extra_bits += HD_LEN_EXTRA[li];
            int ds = hd_dist_sym(tok[i].d);
            dyn_bits += d_lens[ds];
            extra_bits += HD_DIST_EXTRA[ds];
        }
    }
    dyn_bits += ll_lens[HD_EOB];
    dyn_bits += extra_bits;

    uint64_t fix_bits = 3;
    for (int s = 0; s < HD_NLITLEN; s++)
        if (llfreq[s]) fix_bits += (uint64_t)llfreq[s] * HD_FIXED_LITLEN_LEN[s];
    for (int ds = 0; ds < HD_DIST_SYMS; ds++)
        if (dfreq[ds]) fix_bits += (uint64_t)dfreq[ds] * 5;
    fix_bits += extra_bits;

    uint64_t store_bits = UINT64_MAX;
    int can_store = 0;
    if (can_store) {
        int pad = (8 - (w->bitcnt & 7)) & 7;
        store_bits = 3 + pad + 32 + 8 * (uint64_t)n;
    }

    if (can_store && store_bits < dyn_bits && store_bits < fix_bits) {
        /* stored block */
        hd_bw_put(w, final ? 1 : 0, 1);
        hd_bw_put(w, 0, 2);              /* BTYPE=00 */
        hd_bw_align(w);
        hd_bw_put(w, (uint32_t)n, 16);
        hd_bw_put(w, (uint32_t)((~n) & 0xFFFF), 16);
        for (size_t i = 0; i < n; i++)
            hd_bw_put(w, tok[i].v, 8);
        return 0;
    }

    if (fix_bits < dyn_bits) {
        hd_bw_put(w, final ? 1 : 0, 1);
        hd_bw_put(w, 1, 2);              /* BTYPE=01 */
        for (size_t i = 0; i < n; i++) {
            uint32_t v = tok[i].v, d = tok[i].d;
            if (d == 0) {
                hd_bw_put(w, fx_lit.code[v], fx_lit.len[v]);
            } else {
                uint32_t ls = HD_LEN2SYM[v];
                hd_bw_put(w, fx_lit.code[ls], fx_lit.len[ls]);
                int li = (int)ls - 257;
                if (HD_LEN_EXTRA[li])
                    hd_bw_put(w, v - HD_LEN_BASE[li], HD_LEN_EXTRA[li]);
                int ds = hd_dist_sym(d);
                hd_bw_put(w, fx_dist.code[ds], fx_dist.len[ds]);
                if (HD_DIST_EXTRA[ds])
                    hd_bw_put(w, d - HD_DIST_BASE[ds], HD_DIST_EXTRA[ds]);
            }
        }
        hd_bw_put(w, fx_lit.code[HD_EOB], fx_lit.len[HD_EOB]);
        return 0;
    }

    hd_huffenc lle, de;
    hd_huffenc_build(&lle, ll_lens, HD_NLITLEN);
    if (nd) hd_huffenc_build(&de, d_lens, HD_DIST_SYMS);

    hd_bw_put(w, final ? 1 : 0, 1);
    hd_bw_put(w, 2, 2);              /* BTYPE=10 dynamic */
    if (write_dyn_header(w, ll_lens, hlit, d_lens, hdist)) return -1;

    enc_tabs et;
    enc_tabs_build(&et, &lle, &de);
    const uint32_t *elit = et.lit, *elen = et.len, *edist = et.dist;
    const uint32_t *elex = et.lex, *edex = et.dex;
    uint64_t bb = w->bitbuf;
    int bc = w->bitcnt;
    uint8_t *out = w->out;
    size_t olen = w->len, ocap = w->cap;

    for (size_t i = 0; i < n; i++) {
        uint32_t v = tok[i].v, d = tok[i].d;
        if (d == 0) {
            uint32_t e = elit[v];
            bb |= (uint64_t)(e & 0xFFFF) << bc;
            bc += (int)(e >> 16);
        } else {
            uint32_t e = elen[v];
            bb |= (uint64_t)(e & 0xFFFF) << bc;
            bc += (int)((e >> 16) & 0xF);
            uint32_t li = e >> 20;
            uint32_t x = elex[li];
            int nb = (int)(x >> 16);
            if (nb) {
                bb |= (uint64_t)(v - (x & 0xFFFF)) << bc;
                bc += nb;
            }
            int ds = (d < 256) ? dist_tab[d] : dist_tab[256 + ((d - 1) >> 7)];
            while (ds + 1 < HD_DIST_SYMS && HD_DIST_BASE[ds + 1] <= d) ds++;
            e = edist[ds];
            bb |= (uint64_t)(e & 0xFFFF) << bc;
            bc += (int)(e >> 16);
            x = edex[ds];
            nb = (int)(x >> 16);
            if (nb) {
                bb |= (uint64_t)(d - (x & 0xFFFF)) << bc;
                bc += nb;
            }
        }
        while (bc >= 8) {
            if (olen < ocap) out[olen] = (uint8_t)(bb & 0xFF);
            olen++;
            bb >>= 8;
            bc -= 8;
        }
    }
    /* EOB */
    bb |= (uint64_t)lle.code[HD_EOB] << bc;
    bc += lle.len[HD_EOB];
    while (bc >= 8) {
        if (olen < ocap) out[olen] = (uint8_t)(bb & 0xFF);
        olen++;
        bb >>= 8;
        bc -= 8;
    }
    w->bitbuf = bb;
    w->bitcnt = bc;
    w->len = olen;
    return 0;
}

int64_t hd_deflate_from_tokens(const hd_token *tok, size_t ntok,
                               uint32_t beta_tokens,
                               uint8_t *out, size_t outcap) {
    if (beta_tokens < 64) beta_tokens = 64;
    hd_bw w;
    hd_bw_init(&w, out, outcap);
    size_t i = 0;
    do {
        size_t n = ntok - i;
        if (n > beta_tokens) n = beta_tokens;
        int final = (i + n >= ntok);
        if (hd_encode_block(&w, tok + i, n, final)) return -1;
        i += n;
    } while (i < ntok);
    hd_bw_align(&w);
    if (w.len > outcap) return -(int64_t)w.len;
    return (int64_t)w.len;
}

int64_t hd_gzip_from_tokens(const hd_token *tok, size_t ntok,
                            uint32_t beta_tokens,
                            uint32_t crc32_val, uint32_t isize,
                            uint8_t *out, size_t outcap) {
    static const uint8_t hdr[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255};
    if (outcap < 10 + 8) return -1;
    memcpy(out, hdr, 10);
    int64_t n = hd_deflate_from_tokens(tok, ntok, beta_tokens, out + 10, outcap - 18);
    if (n < 0) return n;
    size_t end = 10 + (size_t)n;
    for (int k = 0; k < 4; k++) out[end + k] = (uint8_t)(crc32_val >> (8 * k));
    for (int k = 0; k < 4; k++) out[end + 4 + k] = (uint8_t)(isize >> (8 * k));
    return (int64_t)(end + 8);
}

int hd_dyn_header_bits(const uint8_t *ll_lens, int hlit,
                       const uint8_t *dist_lens, int hdist) {
    cl_item items[HD_NLITLEN + HD_DIST_SYMS + 8];
    uint8_t all[HD_NLITLEN + HD_DIST_SYMS];
    memcpy(all, ll_lens, hlit);
    memcpy(all + hlit, dist_lens, hdist);
    size_t nitems = cl_rle_encode(all, hlit + hdist, items,
                                   (int)(sizeof(items) / sizeof(items[0])));
    if (nitems == 0) return -1;
    uint32_t clfreq[HD_CLEN_SYMS] = {0};
    for (size_t k = 0; k < nitems; k++) clfreq[items[k].sym]++;
    uint8_t cl_lens[HD_CLEN_SYMS] = {0};
    hd_huff_lengths(clfreq, HD_CLEN_SYMS, 7, cl_lens);
    hd_huffenc clenc;
    hd_huffenc_build(&clenc, cl_lens, HD_CLEN_SYMS);
    int hclen = HD_CLEN_SYMS;
    while (hclen > 4 && cl_lens[HD_CLEN_ORDER[hclen - 1]] == 0) hclen--;
    int bits = 14 + hclen * 3;
    for (size_t k = 0; k < nitems; k++)
        bits += clenc.len[items[k].sym] + items[k].ebits;
    return bits;
}
