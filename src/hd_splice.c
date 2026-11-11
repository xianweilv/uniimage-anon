#include "hd_splice.h"
#include "hd_tokdec.h"
#include <zlib.h>

static hd_huffenc fixed_enc;
static int fixed_init = 0;

static void init_fixed(void) {
    if (fixed_init) return;
    hd_huffenc_build(&fixed_enc, HD_FIXED_LITLEN_LEN, HD_LITLEN_SYMS);
    fixed_init = 1;
}

static inline void put_fixed_dist(hd_bw *w, int dsym, uint32_t d) {
    uint32_t code = (uint32_t)dsym;
    uint32_t rev = 0;
    for (int b = 0; b < 5; b++) { rev = (rev << 1) | (code & 1); code >>= 1; }
    hd_bw_put(w, rev, 5);
    if (HD_DIST_EXTRA[dsym]) {
        int ds = 0;
        while (ds + 1 < HD_DIST_SYMS && HD_DIST_BASE[ds + 1] <= d) ds++;
        hd_bw_put(w, d - HD_DIST_BASE[ds], HD_DIST_EXTRA[ds]);
    }
}

static void put_fixed_literal_block(hd_bw *w, const uint8_t *p, size_t n, int bfinal) {
    hd_bw_put(w, bfinal ? 1 : 0, 1);
    hd_bw_put(w, 1, 2);
    for (size_t i = 0; i < n; i++)
        hd_bw_put(w, fixed_enc.code[p[i]], fixed_enc.len[p[i]]);
    hd_bw_put(w, fixed_enc.code[HD_EOB], fixed_enc.len[HD_EOB]);
}

/* stored block（chunking <= 65535） */
static void put_stored(hd_bw *w, const uint8_t *p, size_t n, int bfinal) {
    while (n > 0) {
        size_t take = n > 65535 ? 65535 : n;
        int last = (take == n);
        hd_bw_put(w, (bfinal && last) ? 1 : 0, 1);
        hd_bw_put(w, 0, 2);
        hd_bw_align(w);
        hd_bw_put(w, (uint32_t)(take & 0xFFFF), 16);
        hd_bw_put(w, (uint32_t)((~take) & 0xFFFF), 16);
        for (size_t i = 0; i < take; i++) hd_bw_put(w, p[i], 8);
        n -= take;
        p += take;
    }
}

static void bitcopy(hd_bw *w, const uint8_t *src, uint64_t bit_off, uint64_t nbits) {
    while (nbits >= 64) {
        size_t by = (size_t)(bit_off >> 3);
        int sh = (int)(bit_off & 7);
        uint64_t v;
        memcpy(&v, src + by, 8);
        v >>= sh;
        if (sh) v |= (uint64_t)src[by + 8] << (64 - sh);
        hd_bw_put(w, (uint32_t)(v & 0xFFFFFFFFu), 32);
        hd_bw_put(w, (uint32_t)(v >> 32), 32);
        bit_off += 64;
        nbits -= 64;
    }
    while (nbits > 0) {
        uint32_t take = nbits > 32 ? 32 : (uint32_t)nbits;
        size_t by = (size_t)(bit_off >> 3);
        int sh = (int)(bit_off & 7);
        uint64_t v;
        memcpy(&v, src + by, 8);
        v >>= sh;
        hd_bw_put(w, (uint32_t)(v & ((take == 32) ? 0xFFFFFFFFu : ((1u << take) - 1))), take);
        bit_off += take;
        nbits -= take;
    }
}

int hd_splice_layer(hd_store *st, const hd_recipe *rec,
                    hd_buf *out_buf, hd_splice_result *res) {
    init_fixed();
    memset(res, 0, sizeof *res);
    double t0 = hd_now_ms();

    size_t est = rec->tar_size / 2 + (rec->tar_size >> 6) + (1 << 20);
    hd_buf_reserve(out_buf, est + 64);
    hd_bw w;
    hd_bw_init(&w, out_buf->data, out_buf->cap);

    static const uint8_t hdr[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255};
    for (int k = 0; k < 10; k++) hd_bw_put(&w, hdr[k], 8);

    uint32_t crc = crc32(0L, Z_NULL, 0);
    uint64_t isize = 0;
    uint8_t zeros[65536];
    memset(zeros, 0, sizeof zeros);

    int cur_obj = -1;
    uint8_t *obj_comp = NULL;
    size_t obj_comp_len = 0;
    hd_blkinfo *blks = NULL;
    int nblks = 0;
    uint8_t *obj_raw = NULL;

    for (int i = 0; i < rec->n_segs; i++) {
        const hd_seg *seg = &rec->segs[i];
        if (seg->type == HD_SEG_META) {
            const uint8_t *p = rec->bytes + seg->b;
            put_stored(&w, p, seg->a, 0);
            crc = crc32(crc, p, seg->a);
            isize += seg->a;
        } else if (seg->type == HD_SEG_PAD) {
            uint32_t z = seg->a;
            if (z) {
                hd_bw_put(&w, 0, 1);
                hd_bw_put(&w, 1, 2);
                hd_bw_put(&w, fixed_enc.code[0], fixed_enc.len[0]);
                uint32_t rem = z - 1;
                while (rem >= 3) {
                    uint32_t l = rem > 258 ? 258 : rem;
                    hd_bw_put(&w, fixed_enc.code[HD_LEN2SYM[l]], fixed_enc.len[HD_LEN2SYM[l]]);
                    int li = HD_LEN2SYM[l] - 257;
                    if (HD_LEN_EXTRA[li])
                        hd_bw_put(&w, l - HD_LEN_BASE[li], HD_LEN_EXTRA[li]);
                    put_fixed_dist(&w, 0, 1);
                    rem -= l;
                }
                while (rem > 0) {
                    hd_bw_put(&w, fixed_enc.code[0], fixed_enc.len[0]);
                    rem--;
                }
                hd_bw_put(&w, fixed_enc.code[HD_EOB], fixed_enc.len[HD_EOB]);
                while (z > 0) {
                    uint32_t take = z > (uint32_t)sizeof zeros ? (uint32_t)sizeof zeros : z;
                    crc = crc32(crc, zeros, take);
                    z -= take;
                }
                isize += seg->a;
            }
        } else {
            int idx = (int)seg->a;
            uint32_t off = seg->b, len = seg->c;
            if (idx < 0 || idx >= rec->n_objs) goto fail;
            const hd_objmeta *o = &rec->objs[idx];
            if (len == 0) continue;
            int full = (off == 0 && len == o->raw_len);

            if (cur_obj != idx) {
                free(obj_comp); free(blks); free(obj_raw);
                obj_comp = blks = NULL; obj_raw = NULL;
                obj_comp = hd_store_get_object(st, o->sha, &obj_comp_len);
                if (!obj_comp) { fprintf(stderr, "splice: no obj %.16s\n", o->sha); goto fail; }
                blks = hd_store_get_blockmap(st, o->sha, &nblks);
                if (!blks) { fprintf(stderr, "splice: no blkmap %.16s\n", o->sha); goto fail; }
                cur_obj = idx;
            }

            if (full) {
                double tc0 = hd_now_ms();
                for (int b = 0; b < nblks; b++) {
                    hd_bw_put(&w, 0, 1);
                    hd_bw_put(&w, blks[b].btype, 2);
                    if (blks[b].btype == 0) {
                        uint64_t pbit = blks[b].start_bit + 3;
                        uint64_t abit = (pbit + 7) & ~7ULL;
                        hd_bw_align(&w);
                        uint64_t src_byte = abit >> 3;
                        /* LEN */
                        uint32_t LEN = obj_comp[src_byte] | (obj_comp[src_byte + 1] << 8);
                        for (int k = 0; k < 2 + 2; k++)
                            hd_bw_put(&w, obj_comp[src_byte + k], 8);
                        for (uint32_t k = 0; k < LEN; k++)
                            hd_bw_put(&w, obj_comp[src_byte + 4 + k], 8);
                    } else {
                        bitcopy(&w, obj_comp, blks[b].start_bit + 3,
                                blks[b].end_bit - blks[b].start_bit - 3);
                    }
                }
                res->t_bitcopy_ms += hd_now_ms() - tc0;
                crc = crc32_combine(crc, o->crc, (z_off_t)o->raw_len);
                isize += o->raw_len;
            } else {
                double tc0 = hd_now_ms();
                if (!obj_raw) {
                    obj_raw = malloc(o->raw_len ? o->raw_len : 1);
                    z_stream zs;
                    memset(&zs, 0, sizeof zs);
                    inflateInit2(&zs, -15);
                    zs.next_in = obj_comp;
                    zs.avail_in = (uInt)obj_comp_len;
                    zs.next_out = obj_raw;
                    zs.avail_out = (uInt)o->raw_len;
                    int irc2 = inflate(&zs, Z_FINISH);
                    if (irc2 != Z_STREAM_END) {
                        fprintf(stderr, "splice: partial inflate rc=%d\n", irc2);
                        inflateEnd(&zs);
                        goto fail;
                    }
                    inflateEnd(&zs);
                }
                uint32_t done = 0;
                while (done < len) {
                    uint32_t take = len - done > 262144 ? 262144 : len - done;
                    put_fixed_literal_block(&w, obj_raw + off + done, take, 0);
                    done += take;
                }
                res->t_misc_ms += hd_now_ms() - tc0;
                crc = crc32(crc, obj_raw + off, len);
                isize += len;
            }
        }
        if (w.len > w.cap) {
            size_t need = w.len + 64;
            free(obj_comp); free(blks); free(obj_raw);
            hd_buf_reserve(out_buf, need);
            out_buf->len = 0;
            hd_splice_result r2;
            int rc = hd_splice_layer(st, rec, out_buf, &r2);
            *res = r2;
            return rc;
        }
    }

    hd_bw_put(&w, 1, 1);
    hd_bw_put(&w, 1, 2);
    hd_bw_put(&w, fixed_enc.code[HD_EOB], fixed_enc.len[HD_EOB]);
    hd_bw_align(&w);

    uint32_t crcv = crc;
    uint32_t isz = (uint32_t)(isize & 0xFFFFFFFF);
    for (int k = 0; k < 4; k++) hd_bw_put(&w, (uint32_t)((crcv >> (8 * k)) & 0xFF), 8);
    for (int k = 0; k < 4; k++) hd_bw_put(&w, (uint32_t)((isz >> (8 * k)) & 0xFF), 8);

    if (w.len > w.cap) { fprintf(stderr, "splice: cap %zu > %zu\n", w.len, w.cap); goto fail; }
    out_buf->len = w.len;
    res->out_comp_size = w.len;
    res->tar_size = isize;
    res->crc = crcv;
    res->t_total_ms = hd_now_ms() - t0;

    free(obj_comp); free(blks); free(obj_raw);
    return 0;
fail:
    free(obj_comp); free(blks); free(obj_raw);
    return -1;
}
