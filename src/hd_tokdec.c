#include "hd_tokdec.h"

 */
#define TD_ROOT_BITS 9
#define TD_ROOT_SIZE (1 << TD_ROOT_BITS)

typedef struct {
    uint32_t root[TD_ROOT_SIZE];
    uint32_t *arena;
    uint16_t subbase[TD_ROOT_SIZE];
    uint16_t submask[TD_ROOT_SIZE];
    int      two_level;
} td_table;

static void td_init(td_table *t) {
    if (!t->arena)
        t->arena = malloc((1u << HD_MAX_BITS) * sizeof(uint32_t));
    if (!t->arena) hd_die("OOM td_table");
    t->two_level = 0;
}

static uint32_t td_rev(uint32_t c, int l) {
    uint32_t r = 0;
    for (int b = 0; b < l; b++) { r = (r << 1) | (c & 1); c >>= 1; }
    return r;
}

static int td_build(td_table *t, const uint8_t *lens, int n, int allow_incomplete) {
    int count[HD_MAX_BITS + 1] = {0};
    int maxlen = 0, used = 0;
    for (int i = 0; i < n; i++) {
        int l = lens[i];
        if (l < 0 || l > HD_MAX_BITS) return -1;
        if (l) { count[l]++; used++; if (l > maxlen) maxlen = l; }
    }
    if (used == 0) return -1;
    if (used == 1) {
        int sym = -1;
        for (int i = 0; i < n; i++) if (lens[i]) { sym = i; break; }
        t->two_level = 0;
        uint32_t entry = ((uint32_t)sym << 8) | 1;
        for (int p = 0; p < TD_ROOT_SIZE; p++) t->root[p] = entry;
        return 0;
    }
    int64_t left = 1;
    for (int l = 1; l <= HD_MAX_BITS; l++) {
        left <<= 1;
        left -= count[l];
        if (left < 0) return -2;
    }
    if (left > 0 && !allow_incomplete) return -3;

    uint32_t next_code[HD_MAX_BITS + 1] = {0};
    uint32_t code = 0;
    for (int l = 1; l <= maxlen; l++) {
        code = (code + count[l - 1]) << 1;
        next_code[l] = code;
    }

    memset(t->root, 0xFF, sizeof t->root);

    if (maxlen <= TD_ROOT_BITS) {
        t->two_level = 0;
        for (int i = 0; i < n; i++) {
            int l = lens[i];
            if (!l) continue;
            uint32_t r = td_rev(next_code[l]++, l);
            uint32_t entry = ((uint32_t)i << 8) | (uint32_t)l;
            uint32_t step = 1u << l;
            for (uint32_t j = r; j < TD_ROOT_SIZE; j += step)
                t->root[j] = entry;
        }
        return 0;
    }
    if (left > 0) return -3;

    t->two_level = 1;
    int sublen[TD_ROOT_SIZE];
    memset(sublen, 0, sizeof sublen);

    uint32_t codes[HD_NLITLEN + HD_DIST_SYMS];
    for (int i = 0; i < n; i++) {
        int l = lens[i];
        codes[i] = l ? next_code[l]++ : 0;
    }
    for (int i = 0; i < n; i++) {
        int l = lens[i];
        if (l <= TD_ROOT_BITS) continue;
        uint32_t rootidx = td_rev(codes[i] >> (l - TD_ROOT_BITS), TD_ROOT_BITS);
        int need = l - TD_ROOT_BITS;
        if (need > sublen[rootidx]) sublen[rootidx] = need;
    }
    int arena_next = 0;
    for (int p = 0; p < TD_ROOT_SIZE; p++) {
        if (!sublen[p]) continue;
        int sz = 1 << sublen[p];
        if (arena_next + sz > (1 << HD_MAX_BITS)) return -2;
        t->subbase[p] = (uint16_t)arena_next;
        t->submask[p] = (uint16_t)(sz - 1);
        memset(t->arena + arena_next, 0xFF, (size_t)sz * sizeof(uint32_t));
        arena_next += sz;
        t->root[p] = ((uint32_t)p << 8) | 0;
    }
    for (int i = 0; i < n; i++) {
        int l = lens[i];
        if (!l) continue;
        uint32_t entry = ((uint32_t)i << 8) | (uint32_t)l;
        if (l <= TD_ROOT_BITS) {
            uint32_t r = td_rev(codes[i], l);
            uint32_t step = 1u << l;
            for (uint32_t j = r; j < TD_ROOT_SIZE; j += step)
                t->root[j] = entry;
        } else {
            uint32_t rootidx = td_rev(codes[i] >> (l - TD_ROOT_BITS), TD_ROOT_BITS);
            int sl = sublen[rootidx];
            int lp = l - TD_ROOT_BITS;
            uint32_t sub = td_rev(codes[i] & ((1u << lp) - 1), lp);
            uint32_t base = t->subbase[rootidx];
            uint32_t step = 1u << lp;
            uint32_t sz = 1u << sl;
            for (uint32_t j = sub; j < sz; j += step)
                t->arena[base + j] = entry;
        }
    }
    return 0;
}

static int decode_block(hd_br *r, hd_tokstream *ts) {
    uint32_t bfinal = hd_br_take(r, 1);
    uint32_t btype  = hd_br_take(r, 2);

    if (btype == 0) {
        hd_br_align(r);
        uint32_t len = hd_br_take(r, 16);
        uint32_t nlen = hd_br_take(r, 16);
        if ((len ^ nlen) != 0xFFFF) return HD_TD_ERR_DATA;
        for (uint32_t i = 0; i < len; i++) {
            uint8_t b = (uint8_t)hd_br_take(r, 8);
            hd_ts_push(ts, b, 0);
        }
        return (int)bfinal ? 2 : 1;
    }

    static td_table lt_tab, dt_tab;
    static int tabs_init = 0;
    if (!tabs_init) { td_init(&lt_tab); td_init(&dt_tab); tabs_init = 1; }
    int have_dist = 1;

    if (btype == 1) {
        if (td_build(&lt_tab, HD_FIXED_LITLEN_LEN, HD_LITLEN_SYMS, 0))
            return HD_TD_ERR_DATA;
        if (td_build(&dt_tab, HD_FIXED_DIST_LEN, HD_DIST_SYMS, 1))
            return HD_TD_ERR_DATA;
    } else if (btype == 2) {
        uint32_t hlit  = hd_br_take(r, 5) + 257;
        uint32_t hdist = hd_br_take(r, 5) + 1;
        uint32_t hclen = hd_br_take(r, 4) + 4;
        if (hlit > HD_NLITLEN || hdist > HD_DIST_SYMS) return HD_TD_ERR_DATA;

        uint8_t cl_lens[HD_CLEN_SYMS] = {0};
        for (uint32_t i = 0; i < hclen; i++)
            cl_lens[HD_CLEN_ORDER[i]] = (uint8_t)hd_br_take(r, 3);

        hd_huffdec cl;
        if (hd_huffdec_build(&cl, cl_lens, HD_CLEN_SYMS)) return HD_TD_ERR_DATA;

        uint8_t lens[HD_NLITLEN + HD_DIST_SYMS];
        uint32_t total = hlit + hdist, i = 0;
        while (i < total) {
            uint32_t bits = hd_br_peek(r, cl.nbits);
            int sym = hd_huff_decode(&cl, bits);
            int len = hd_huff_decodelen(&cl, bits);
            if (len == 0) return HD_TD_ERR_DATA;
            hd_br_take(r, len);
            if (sym < 16) {
                lens[i++] = (uint8_t)sym;
            } else if (sym == 16) {
                if (i == 0) return HD_TD_ERR_DATA;
                uint32_t rep = 3 + hd_br_take(r, 2);
                uint8_t prev = lens[i - 1];
                while (rep-- && i < total) lens[i++] = prev;
            } else if (sym == 17) {
                uint32_t rep = 3 + hd_br_take(r, 3);
                while (rep-- && i < total) lens[i++] = 0;
            } else {
                uint32_t rep = 11 + hd_br_take(r, 7);
                while (rep-- && i < total) lens[i++] = 0;
            }
        }
        if (i != total) return HD_TD_ERR_DATA;
        if (lens[HD_EOB] == 0) return HD_TD_ERR_DATA;

        if (td_build(&lt_tab, lens, (int)hlit, 0)) return HD_TD_ERR_DATA;
        int any_dist = 0;
        for (uint32_t k = hlit; k < total; k++)
            if (lens[k]) { any_dist = 1; break; }
        if (any_dist) {
            if (td_build(&dt_tab, lens + hlit, (int)hdist, 0)) return HD_TD_ERR_DATA;
        } else have_dist = 0;
        hd_huffdec_free(&cl);
    } else {
        return HD_TD_ERR_DATA;
    }

    const uint8_t *in = r->in;
    size_t inlen = r->inlen;
    size_t inpos = r->bytepos;
    uint64_t over = (uint64_t)r->overrun;
    uint64_t bb = r->bitbuf;
    int bc = r->bitcnt;
    const uint32_t *lroot = lt_tab.root;
    const uint32_t *droot = dt_tab.root;
    const uint32_t *laren = lt_tab.arena;
    const uint32_t *daren = dt_tab.arena;
    const uint16_t *lbase = lt_tab.subbase, *lmask = lt_tab.submask;
    const uint16_t *dbase = dt_tab.subbase, *dmask = dt_tab.submask;
    int rc = HD_TD_OK;
    uint64_t guard = 0;
    uint64_t real_bits_end = (uint64_t)inlen * 8;

#define TD_NEED(n) do { \
    while (bc < (n)) { \
        if (inpos < inlen) { bb |= (uint64_t)in[inpos] << bc; inpos++; } \
        else { over += 8; } \
        bc += 8; \
    } \
} while (0)

    for (;;) {
        if ((++guard & 2047) == 0 &&
            ((uint64_t)inpos * 8 + over - (uint64_t)bc) > real_bits_end) {
            rc = HD_TD_ERR_DATA;
            break;
        }
        TD_NEED(15);
        uint32_t e = lroot[bb & 511];
        uint32_t len = e & 0xFF;
        uint32_t sym;
        if (len) {
            sym = e >> 8;
        } else {
            uint32_t id = e >> 8;
            uint32_t e2 = laren[lbase[id] + ((bb >> 9) & lmask[id])];
            len = e2 & 0xFF;
            sym = e2 >> 8;
        }
        if (len == 0 || len > HD_MAX_BITS) { rc = HD_TD_ERR_DATA; break; }
        bb >>= len;
        bc -= len;
        if (sym < 256) {
            hd_ts_push(ts, sym, 0);
        } else if (sym == HD_EOB) {
            break;
        } else {
            int li = (int)sym - 257;
            if (li >= 29) { rc = HD_TD_ERR_DATA; break; }
            uint32_t length = HD_LEN_BASE[li];
            int lex = HD_LEN_EXTRA[li];
            if (lex) {
                TD_NEED(lex);
                length += (uint32_t)(bb & ((1u << lex) - 1));
                bb >>= lex; bc -= lex;
            }
            if (!have_dist) { rc = HD_TD_ERR_DATA; break; }
            TD_NEED(15);
            uint32_t de = droot[bb & 511];
            uint32_t dlen = de & 0xFF;
            uint32_t dsym;
            if (dlen) {
                dsym = de >> 8;
            } else {
                uint32_t id = de >> 8;
                uint32_t e2 = daren[dbase[id] + ((bb >> 9) & dmask[id])];
                dlen = e2 & 0xFF;
                dsym = e2 >> 8;
            }
            if (dlen == 0 || dlen > HD_MAX_BITS) { rc = HD_TD_ERR_DATA; break; }
            bb >>= dlen;
            bc -= dlen;
            if (dsym >= HD_DIST_SYMS) { rc = HD_TD_ERR_DATA; break; }
            uint32_t dist = HD_DIST_BASE[dsym];
            int dex = HD_DIST_EXTRA[dsym];
            if (dex) {
                TD_NEED(dex);
                dist += (uint32_t)(bb & ((1u << dex) - 1));
                bb >>= dex; bc -= dex;
            }
            hd_ts_push(ts, length, dist);
        }
    }
#undef TD_NEED

    r->bytepos = inpos;
    r->bitbuf = bb;
    r->bitcnt = bc;
    r->overrun = (uint32_t)over;

    if (rc) return rc;
    return bfinal ? 2 : 1;
}

int hd_tokens_from_deflate(const uint8_t *in, size_t inlen,
                           hd_tokstream *ts, uint64_t *stream_bits) {
    hd_br r;
    hd_br_init(&r, in, inlen);
    for (;;) {
        int rc = decode_block(&r, ts);
        if (rc == 2) break;
        if (rc < 0) return rc;
    }
    if (hd_br_bitpos(&r) > (uint64_t)inlen * 8) return HD_TD_ERR_DATA;
    if (stream_bits) *stream_bits = hd_br_bitpos(&r);
    return HD_TD_OK;
}

int hd_scan_blocks(const uint8_t *in, size_t inlen, hd_blkinfo *blocks, int cap) {
    hd_br r;
    hd_br_init(&r, in, inlen);
    int n = 0;
    for (;;) {
        uint64_t start = hd_br_bitpos(&r);
        uint32_t bfinal = hd_br_take(&r, 1);
        uint32_t btype = hd_br_take(&r, 2);
        if (btype == 3) return -1;
        if (btype == 0) {
            hd_br_align(&r);
            uint32_t len = hd_br_take(&r, 16);
            uint32_t nlen = hd_br_take(&r, 16);
            if ((len ^ nlen) != 0xFFFF) return -1;
            while (len > 0) {
                uint32_t take = len;
                hd_br_fill(&r, 1);
                size_t whole = (size_t)r.bitcnt / 8;
                if (whole == 0) {
                    if (r.bytepos >= r.inlen) return -1;
                    hd_br_fill(&r, 8);
                    whole = 1;
                }
                if (take > whole) take = (uint32_t)whole;
                r.bitbuf >>= take * 8;
                r.bitcnt -= (int)(take * 8);
                len -= take;
            }
        } else {
            hd_huffdec lt, dt;
            int have_dist = 1;
            if (btype == 1) {
                if (hd_huffdec_build2(&lt, HD_FIXED_LITLEN_LEN, HD_LITLEN_SYMS, 0)) return -1;
                if (hd_huffdec_build2(&dt, HD_FIXED_DIST_LEN, HD_DIST_SYMS, 1)) return -1;
            } else {
                uint32_t hlit = hd_br_take(&r, 5) + 257;
                uint32_t hdist = hd_br_take(&r, 5) + 1;
                uint32_t hclen = hd_br_take(&r, 4) + 4;
                if (hlit > HD_NLITLEN || hdist > HD_DIST_SYMS) return -1;
                uint8_t cl_lens[HD_CLEN_SYMS] = {0};
                for (uint32_t i = 0; i < hclen; i++)
                    cl_lens[HD_CLEN_ORDER[i]] = (uint8_t)hd_br_take(&r, 3);
                hd_huffdec cl;
                if (hd_huffdec_build(&cl, cl_lens, HD_CLEN_SYMS)) return -1;
                uint8_t lens[HD_NLITLEN + HD_DIST_SYMS];
                uint32_t total = hlit + hdist, i = 0;
                while (i < total) {
                    uint32_t bits = hd_br_peek(&r, cl.nbits);
                    int sym = hd_huff_decode(&cl, bits);
                    int len = hd_huff_decodelen(&cl, bits);
                    if (len == 0) return -1;
                    hd_br_take(&r, len);
                    if (sym < 16) lens[i++] = (uint8_t)sym;
                    else if (sym == 16) {
                        if (i == 0) return -1;
                        uint32_t rep = 3 + hd_br_take(&r, 2);
                        uint8_t prev = lens[i - 1];
                        while (rep-- && i < total) lens[i++] = prev;
                    } else if (sym == 17) {
                        uint32_t rep = 3 + hd_br_take(&r, 3);
                        while (rep-- && i < total) lens[i++] = 0;
                    } else {
                        uint32_t rep = 11 + hd_br_take(&r, 7);
                        while (rep-- && i < total) lens[i++] = 0;
                    }
                }
                if (i != total) return -1;
                if (lens[HD_EOB] == 0) return -1;
                if (hd_huffdec_build(&lt, lens, (int)hlit)) return -1;
                int any = 0;
                for (uint32_t k = hlit; k < total; k++) if (lens[k]) { any = 1; break; }
                if (any) {
                    if (hd_huffdec_build(&dt, lens + hlit, (int)hdist)) return -1;
                } else have_dist = 0;
                hd_huffdec_free(&cl);
            }
            uint64_t guard = 0;
            for (;;) {
                if ((++guard & 1023) == 0 &&
                    hd_br_bitpos(&r) > (uint64_t)inlen * 8) return -1;
                hd_br_fill(&r, lt.nbits);
                uint32_t entry = lt.tab[(uint32_t)(r.bitbuf & lt.mask)];
                int len = (int)(entry & 0xFF);
                if (len == 0) return -1;
                r.bitbuf >>= len;
                r.bitcnt -= len;
                int sym = (int)(entry >> 16);
                if (sym == HD_EOB) break;
                if (sym > 256) {
                    int li = sym - 257;
                    if (li >= 29) return -1;
                    if (HD_LEN_EXTRA[li]) {
                        r.bitbuf >>= HD_LEN_EXTRA[li];
                        r.bitcnt -= HD_LEN_EXTRA[li];
                        hd_br_fill(&r, 1);
                    }
                    if (!have_dist) return -1;
                    hd_br_fill(&r, dt.nbits);
                    uint32_t dentry = dt.tab[(uint32_t)(r.bitbuf & dt.mask)];
                    int dn = (int)(dentry & 0xFF);
                    if (dn == 0) return -1;
                    r.bitbuf >>= dn;
                    r.bitcnt -= dn;
                    int dsym = (int)(dentry >> 16);
                    if (dsym >= HD_DIST_SYMS) return -1;
                    if (HD_DIST_EXTRA[dsym]) {
                        r.bitbuf >>= HD_DIST_EXTRA[dsym];
                        r.bitcnt -= HD_DIST_EXTRA[dsym];
                    }
                }
            }
            hd_huffdec_free(&lt);
            if (have_dist) hd_huffdec_free(&dt);
        }
        if (n >= cap) return -1;
        blocks[n].start_bit = start;
        blocks[n].end_bit = hd_br_bitpos(&r);
        blocks[n].btype = (uint8_t)btype;
        n++;
        if (bfinal) break;
    }
    if (hd_br_bitpos(&r) > (uint64_t)inlen * 8) return -1;
    return n;
}
