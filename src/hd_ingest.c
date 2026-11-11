#include "hd_ingest.h"

#include <openssl/sha.h>

#define BLK_IDX_BITS 23
#define BLK_IDX_SIZE (1u << BLK_IDX_BITS)
#define BLK_IDX_MASK (BLK_IDX_SIZE - 1)
#define BLK_BATCH_MAX_BYTES (512 * 1024)
#define BLK_BATCH_MAX_BLOCKS 4096

typedef struct {
    uint8_t  blk_sha[32];
    char     grp_sha[65];
    uint64_t grp_raw_len;
    uint32_t grp_crc;
    uint32_t offset;
    uint32_t len;
} blk_idx_entry_t;

typedef struct {
    blk_idx_entry_t *slots;
    size_t count;
} blk_index_t;

typedef struct {
    uint8_t  buf[BLK_BATCH_MAX_BYTES];
    uint32_t len;
    struct {
        uint8_t  sha[32];
        uint32_t offset;
        uint32_t blen;
        int      seg_idx;
    } blocks[BLK_BATCH_MAX_BLOCKS];
    int n_blocks;
} blk_batch_t;

static uint32_t blk_idx_hash(const uint8_t *sha) {
    uint32_t h;
    memcpy(&h, sha, 4);
    return (h * 2654435761u) & BLK_IDX_MASK;
}

static blk_idx_entry_t *blk_idx_find(blk_index_t *bi, const uint8_t *sha) {
    uint32_t h = blk_idx_hash(sha);
    uint32_t start = h;
    while (bi->slots[h].len != 0) {
        if (memcmp(bi->slots[h].blk_sha, sha, 32) == 0)
            return &bi->slots[h];
        h = (h + 1) & BLK_IDX_MASK;
        if (h == start) return NULL;
    }
    return NULL;
}

static void blk_idx_add(blk_index_t *bi, const uint8_t *blk_sha,
                        const char *grp_sha, uint64_t raw, uint32_t crc,
                        uint32_t off, uint32_t len) {
    uint32_t h = blk_idx_hash(blk_sha);
    while (bi->slots[h].len != 0) {
        if (memcmp(bi->slots[h].blk_sha, blk_sha, 32) == 0) return;
        h = (h + 1) & BLK_IDX_MASK;
    }
    memcpy(bi->slots[h].blk_sha, blk_sha, 32);
    strncpy(bi->slots[h].grp_sha, grp_sha, 64);
    bi->slots[h].grp_sha[64] = 0;
    bi->slots[h].grp_raw_len = raw;
    bi->slots[h].grp_crc = crc;
    bi->slots[h].offset = off;
    bi->slots[h].len = len;
    bi->count++;
}
#include "hd_fastcdc.h"
#include "hd_tokdec.h"
#include <zlib.h>
#include <openssl/sha.h>

void hd_ingest_cfg_default(hd_ingest_cfg *c) {
    c->sigma = 1024;
    c->theta = 64 * 1024;
    c->cdc_avg = 64 * 1024;
    c->level = 6;
    c->policy = HD_POLICY_HYBRID;
}

/* ---------- Object compression helper ---------- */
static uint8_t *zlib_def_raw(const uint8_t *in, size_t n, int level, size_t *outn) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (deflateInit2(&zs, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        hd_die("deflateInit2");
    size_t cap = deflateBound(&zs, n) + 64;
    uint8_t *out = malloc(cap);
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)n;
    zs.next_out = out;
    zs.avail_out = (uInt)cap;
    int rc = deflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) hd_die("deflate rc=%d", rc);
    *outn = cap - zs.avail_out;
    deflateEnd(&zs);
    return out;
}

static int put_obj(hd_store *st, hd_recipe *rec, hd_ingest_result *res,
                   const uint8_t *raw, size_t n, const hd_ingest_cfg *cfg) {
    char sha[65];
    uint32_t crc;
    size_t compn;
    double t0 = hd_now_ms();
    uint8_t *comp = zlib_def_raw(raw, n, cfg->level, &compn);
    res->t_compress_ms += hd_now_ms() - t0;
    int is_new = hd_store_put_object(st, raw, n, comp, compn, sha, &crc);
    if (is_new) {
        int maxb = (int)(compn / 4) + 8;
        hd_blkinfo *blks = malloc(maxb * sizeof(hd_blkinfo));
        int nb = hd_scan_blocks(comp, compn, blks, maxb);
        if (nb >= 0) hd_store_put_blockmap(st, sha, blks, nb);
        free(blks);
        res->n_new_objects++;
        res->new_obj_comp += compn;
        res->sum_ref_comp += compn;
    } else {
        res->n_hit_objects++;
        int64_t cs = hd_store_object_comp_size(st, sha);
        res->sum_ref_comp += (cs > 0 ? (uint64_t)cs : compn);
    }
    free(comp);
    return hd_recipe_add_obj(rec, sha, n, crc);
}

#define BATCH_FILE_MAX 4096
typedef struct {
    uint8_t  buf[512 * 1024];
    uint32_t len;
    struct { uint32_t off, len; int seg_idx; } files[BATCH_FILE_MAX];
    int n_files;
} batch_t;

typedef enum { ST_HEADER = 0, ST_META_DATA, ST_CONTENT, ST_PAD } pstate_t;

typedef struct {
    hd_store *st;
    const hd_ingest_cfg *cfg;
    hd_recipe *rec;
    hd_ingest_result *res;

    pstate_t state;
    uint64_t need;
    uint64_t entry_size;
    uint64_t content_got;
    uint8_t  hdr[512];
    uint32_t hdr_got;
    uint64_t zero_run;
    int      cur_is_regular;
    uint8_t  cur_typeflag;
    int64_t  pax_size_pending;

    batch_t batch;
    int      batch_seg_pending;
    hd_buf  inline_buf;
    hd_buf  cdc_buf;
    hd_cdc_stream cdc;
    int      cdc_active;
    uint32_t fixed_bs;
    uint32_t carry_len;

    hd_buf  pad_buf;
    uint32_t pad_need, pad_got;

    blk_index_t blk_idx;
    blk_batch_t blk_batch;

    hd_buf  meta_buf;
} ing_t;

static void batch_flush(ing_t *g) {
    batch_t *b = &g->batch;
    if (b->len == 0) return;
    int idx = put_obj(g->st, g->rec, g->res, b->buf, b->len, g->cfg);
    for (int i = 0; i < b->n_files; i++)
        g->rec->segs[b->files[i].seg_idx].a = (uint32_t)idx;
    b->len = 0;
    b->n_files = 0;
}

static void batch_file_begin(ing_t *g, uint32_t fsize) {
    batch_t *b = &g->batch;
    if (b->n_files >= BATCH_FILE_MAX ||
        (b->len > 0 && (uint64_t)b->len + fsize > g->cfg->theta))
        batch_flush(g);
    hd_recipe_add_ref(g->rec, 0xFFFFFFFFu, b->len, fsize);
    b->files[b->n_files].off = b->len;
    b->files[b->n_files].len = fsize;
    b->files[b->n_files].seg_idx = g->rec->n_segs - 1;
    b->n_files++;
}

static void batch_append(ing_t *g, const uint8_t *p, uint32_t n) {
    batch_t *b = &g->batch;
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

static void cdc_flush(ing_t *g) {
    if (g->cdc_buf.len == 0) return;
    int idx = put_obj(g->st, g->rec, g->res, g->cdc_buf.data, g->cdc_buf.len, g->cfg);
    hd_recipe_add_ref(g->rec, idx, 0, (uint32_t)g->cdc_buf.len);
    g->cdc_buf.len = 0;
    g->cdc.chunk_len = 0;
    g->cdc.h = 0;
}


static void blk_batch_flush_impl(ing_t *g) {
    blk_batch_t *bb = &g->blk_batch;
    if (bb->len == 0) return;
    char grp_sha[65];
    uint32_t crc;
    size_t compn;
    uint8_t *comp = zlib_def_raw(bb->buf, bb->len, g->cfg->level, &compn);
    int is_new = hd_store_put_object(g->st, bb->buf, bb->len, comp, compn,
                                     grp_sha, &crc);
    free(comp);
    if (is_new) {
        g->res->n_new_objects++;
        g->res->new_obj_comp += compn;
    }
    g->res->sum_ref_comp += compn;
    int obj_idx = hd_recipe_add_obj(g->rec, grp_sha, bb->len, crc);
    for (int i = 0; i < bb->n_blocks; i++) {
        blk_idx_add(&g->blk_idx, bb->blocks[i].sha,
                    grp_sha, bb->len, crc,
                    bb->blocks[i].offset, bb->blocks[i].blen);
        if (bb->blocks[i].seg_idx >= 0)
            g->rec->segs[bb->blocks[i].seg_idx].a = (uint32_t)obj_idx;
    }
    bb->len = 0;
    bb->n_blocks = 0;
}

static void emit_block_dedup(ing_t *g, const uint8_t *blk, uint32_t len) {
    uint8_t sha[32];
    SHA256(blk, len, sha);
    blk_idx_entry_t *e = blk_idx_find(&g->blk_idx, sha);
    if (e) {
        int obj_idx = -1;
        for (int i = g->rec->n_objs - 1; i >= 0; i--) {
            if (strcmp(g->rec->objs[i].sha, e->grp_sha) == 0) {
                obj_idx = i;
                break;
            }
        }
        if (obj_idx < 0)
            obj_idx = hd_recipe_add_obj(g->rec, e->grp_sha,
                                        e->grp_raw_len, e->grp_crc);
        hd_recipe_add_ref(g->rec, obj_idx, e->offset, len);
        g->res->n_hit_objects++;
        int64_t cs = hd_store_object_comp_size(g->st, e->grp_sha);
        g->res->sum_ref_comp += (cs > 0 ? (uint64_t)cs : 0);
        return;
    }
    if (g->blk_batch.len > 0 && g->blk_batch.len + len > g->cfg->theta)
        blk_batch_flush_impl(g);
    if (g->blk_batch.n_blocks >= BLK_BATCH_MAX_BLOCKS)
        blk_batch_flush_impl(g);
    memcpy(g->blk_batch.buf + g->blk_batch.len, blk, len);
    memcpy(g->blk_batch.blocks[g->blk_batch.n_blocks].sha, sha, 32);
    g->blk_batch.blocks[g->blk_batch.n_blocks].offset = g->blk_batch.len;
    g->blk_batch.blocks[g->blk_batch.n_blocks].blen = len;
    hd_recipe_add_ref(g->rec, 0xFFFFFFFFu, g->blk_batch.len, len);
    g->blk_batch.blocks[g->blk_batch.n_blocks].seg_idx = g->rec->n_segs - 1;
    g->blk_batch.n_blocks++;
    g->blk_batch.len += len;
}

static void route_content(ing_t *g, const uint8_t *p, size_t n, int is_last) {
    const hd_ingest_cfg *cfg = g->cfg;
    uint64_t fsize = g->entry_size;

    if (cfg->policy == HD_POLICY_FILE) {
        hd_buf_push(&g->cdc_buf, p, n);
        if (is_last) {
            int idx = put_obj(g->st, g->rec, g->res,
                              g->cdc_buf.data, g->cdc_buf.len, g->cfg);
            hd_recipe_add_ref(g->rec, idx, 0, (uint32_t)g->cdc_buf.len);
            g->cdc_buf.len = 0;
        }
        return;
    }
    if (cfg->policy == HD_POLICY_CHUNK8K || cfg->policy == HD_POLICY_CHUNK16K ||
        cfg->policy == HD_POLICY_FIXBLOCK) {
        size_t off = 0;
        while (off < n) {
            uint32_t need = g->fixed_bs - g->carry_len;
            size_t take = (n - off) < need ? (n - off) : need;
            memcpy(g->cdc_buf.data + g->carry_len, p + off, take);
            g->carry_len += (uint32_t)take;
            off += take;
            if (g->carry_len == g->fixed_bs) {
                int idx = put_obj(g->st, g->rec, g->res,
                                  g->cdc_buf.data, g->fixed_bs, g->cfg);
                hd_recipe_add_ref(g->rec, idx, 0, g->fixed_bs);
                g->carry_len = 0;
            }
        }
        if (is_last && g->carry_len > 0) {
            int idx = put_obj(g->st, g->rec, g->res,
                              g->cdc_buf.data, g->carry_len, g->cfg);
            hd_recipe_add_ref(g->rec, idx, 0, g->carry_len);
            g->carry_len = 0;
        }
        return;
    }

    if (cfg->policy == HD_POLICY_CHUNK8KB || cfg->policy == HD_POLICY_CHUNK16KB) {
        size_t off = 0;
        while (off < n) {
            uint32_t need = g->fixed_bs - g->carry_len;
            size_t take = (n - off) < need ? (n - off) : need;
            memcpy(g->cdc_buf.data + g->carry_len, p + off, take);
            g->carry_len += (uint32_t)take;
            off += take;
            if (g->carry_len == g->fixed_bs) {
                emit_block_dedup(g, g->cdc_buf.data, g->fixed_bs);
                g->carry_len = 0;
            }
        }
        if (is_last && g->carry_len > 0) {
            emit_block_dedup(g, g->cdc_buf.data, g->carry_len);
            g->carry_len = 0;
        }
        return;
    }

    /* HYBRID */
    if (fsize < cfg->sigma) {
        hd_buf_push(&g->inline_buf, p, n);
        if (is_last) {
            hd_recipe_add_meta(g->rec, g->inline_buf.data, g->inline_buf.len);
            g->rec->n_files_inlined++;
            g->inline_buf.len = 0;
        }
        return;
    }
    if (fsize < cfg->theta) {
        if (!g->batch_seg_pending) {
            batch_file_begin(g, (uint32_t)fsize);
            g->batch_seg_pending = 1;
        }
        batch_append(g, p, (uint32_t)n);
        if (is_last) {
            g->rec->n_files_batched++;
            g->batch_seg_pending = 0;
        }
        return;
    }
    /* CDC */
    if (!g->cdc_active) {
        hd_cdc_begin(&g->cdc, cfg->cdc_avg);
        g->cdc_active = 1;
    }
    size_t off = 0;
    while (off < n) {
        size_t cut = hd_cdc_feed(&g->cdc, p + off, n - off, 0);
        if (cut == 0) {
            hd_buf_push(&g->cdc_buf, p + off, n - off);
            break;
        }
        hd_buf_push(&g->cdc_buf, p + off, cut);
        off += cut;
        cdc_flush(g);
    }
    if (is_last) {
        cdc_flush(g);
        g->cdc_active = 0;
        g->rec->n_files_chunked++;
    }
}

static void finish_empty_file(ing_t *g) {
    if (g->cfg->policy == HD_POLICY_FILE) {
        int idx = put_obj(g->st, g->rec, g->res, (const uint8_t *)"", 0, g->cfg);
        hd_recipe_add_ref(g->rec, idx, 0, 0);
    } else if (g->cfg->policy == HD_POLICY_HYBRID) {
        g->rec->n_files_inlined++;
    }
}

static void enter_pad(ing_t *g) {
    g->state = ST_PAD;
    g->pad_need = (uint32_t)((512 - g->entry_size % 512) % 512);
    g->pad_got = 0;
    g->pad_buf.len = 0;
}

static void pax_parse(hd_buf *b, int64_t *size_out) {
    *size_out = -1;
    size_t i = 0;
    while (i < b->len) {
        size_t j = i;
        while (j < b->len && b->data[j] >= '0' && b->data[j] <= '9') j++;
        if (j == i || j >= b->len || b->data[j] != ' ') break;
        size_t reclen = 0;
        for (size_t k = i; k < j; k++) reclen = reclen * 10 + (b->data[k] - '0');
        if (reclen == 0 || i + reclen > b->len) break;
        size_t ks = j + 1;
        size_t ke = ks;
        while (ke < i + reclen && b->data[ke] != '=' && b->data[ke] != '\n') ke++;
        if (ke < i + reclen && b->data[ke] == '=') {
            size_t vs = ke + 1;
            size_t ve = i + reclen - 1;
            if (ve > vs && ke - ks == 4 && !memcmp(b->data + ks, "size", 4)) {
                uint64_t v = 0;
                int ok = 1;
                for (size_t k = vs; k < ve; k++) {
                    if (b->data[k] < '0' || b->data[k] > '9') { ok = 0; break; }
                    v = v * 10 + (b->data[k] - '0');
                }
                if (ok) *size_out = (int64_t)v;
            }
        }
        i += reclen;
    }
}

static void feed(ing_t *g, const uint8_t *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t avail = n - off;
        switch (g->state) {
        case ST_HEADER: {
            size_t want = 512 - g->hdr_got;
            size_t take = avail < want ? avail : want;
            memcpy(g->hdr + g->hdr_got, p + off, take);
            g->hdr_got += (uint32_t)take;
            off += take;
            if (g->hdr_got < 512) return;
            g->hdr_got = 0;

            int allz = 1;
            for (int i = 0; i < 512; i++)
                if (g->hdr[i]) { allz = 0; break; }
            if (allz) { g->zero_run++; break; }
            if (g->zero_run) {
                hd_recipe_add_pad(g->rec, (uint32_t)(g->zero_run * 512));
                g->zero_run = 0;
            }

            if (!hd_tar_header_ok(g->hdr)) {
                hd_recipe_add_meta(g->rec, g->hdr, 512);
                break;
            }
            uint8_t tf = g->hdr[156];
            uint64_t size = hd_tar_parse_size(g->hdr);
            if (size == (uint64_t)-1) {
                hd_recipe_add_meta(g->rec, g->hdr, 512);
                break;
            }
            hd_recipe_add_meta(g->rec, g->hdr, 512);
            g->cur_typeflag = tf;

            if (tf == 'x' || tf == 'g' || tf == 'L' || tf == 'K') {
                g->cur_is_regular = 0;
                g->entry_size = size;
                if (size == 0) {
                    enter_pad(g);
                } else {
                    g->state = ST_META_DATA;
                    g->need = size;
                    g->meta_buf.len = 0;
                }
                break;
            }
            if (tf == '0' || tf == 0 || tf == '7') {
                g->cur_is_regular = 1;
                g->rec->n_files++;
                uint64_t fsize = size;
                if (g->pax_size_pending >= 0) {
                    fsize = (uint64_t)g->pax_size_pending;
                    g->pax_size_pending = -1;
                }
                g->entry_size = fsize;
                if (fsize == 0) {
                    finish_empty_file(g);
                    enter_pad(g);
                } else {
                    g->state = ST_CONTENT;
                    g->need = fsize;
                    g->content_got = 0;
                    g->batch_seg_pending = 0;
                }
                break;
            }
            g->cur_is_regular = 0;
            g->entry_size = size;
            if (size > 0) {
                g->state = ST_META_DATA;
                g->need = size;
                g->meta_buf.len = 0;
            } else {
                enter_pad(g);
            }
            break;
        }
        case ST_META_DATA: {
            size_t take = avail < g->need ? avail : (size_t)g->need;
            hd_buf_push(&g->meta_buf, p + off, take);
            off += take;
            g->need -= take;
            if (g->need == 0) {
                hd_recipe_add_meta(g->rec, g->meta_buf.data, g->meta_buf.len);
                if (g->cur_typeflag == 'x') {
                    int64_t psz;
                    pax_parse(&g->meta_buf, &psz);
                    if (psz >= 0) g->pax_size_pending = psz;
                }
                g->meta_buf.len = 0;
                enter_pad(g);
            }
            break;
        }
        case ST_CONTENT: {
            size_t take = avail < g->need ? avail : (size_t)g->need;
            g->content_got += take;
            g->rec->content_bytes += take;
            int is_last = (g->content_got >= g->entry_size);
            route_content(g, p + off, take, is_last);
            off += take;
            g->need -= take;
            if (g->need == 0) enter_pad(g);
            break;
        }
        case ST_PAD: {
            size_t want = g->pad_need - g->pad_got;
            size_t take = avail < want ? avail : want;
            hd_buf_push(&g->pad_buf, p + off, take);
            g->pad_got += (uint32_t)take;
            off += take;
            if (g->pad_got >= g->pad_need) {
                if (g->pad_buf.len) {
                    int allz = 1;
                    for (size_t i = 0; i < g->pad_buf.len; i++)
                        if (g->pad_buf.data[i]) { allz = 0; break; }
                    if (allz)
                        hd_recipe_add_pad(g->rec, (uint32_t)g->pad_buf.len);
                    else
                        hd_recipe_add_meta(g->rec, g->pad_buf.data, g->pad_buf.len);
                    g->pad_buf.len = 0;
                }
                g->state = ST_HEADER;
            }
            break;
        }
        }
    }
}

int hd_ingest_layer(hd_store *st, const hd_ingest_cfg *cfg,
                    const uint8_t *blob, size_t bloblen,
                    hd_recipe *rec_out, hd_ingest_result *res,
                    char recipe_sha[65]) {
    memset(res, 0, sizeof *res);
    hd_ingest_cfg cfgd;
    if (!cfg) { hd_ingest_cfg_default(&cfgd); cfg = &cfgd; }

    ing_t g;
    memset(&g, 0, sizeof g);
    g.st = st;
    g.cfg = cfg;
    g.rec = rec_out;
    g.res = res;
    g.state = ST_HEADER;
    g.pax_size_pending = -1;
    g.fixed_bs = (cfg->policy == HD_POLICY_CHUNK8K ||
                cfg->policy == HD_POLICY_CHUNK8KB) ? 8192
               : (cfg->policy == HD_POLICY_CHUNK16K ||
                  cfg->policy == HD_POLICY_CHUNK16KB) ? 16384
               : cfg->theta;   /* FIXBLOCK */
    if (cfg->policy == HD_POLICY_CHUNK8KB || cfg->policy == HD_POLICY_CHUNK16KB) {
        g.blk_idx.slots = calloc(BLK_IDX_SIZE, sizeof(blk_idx_entry_t));
        g.blk_idx.count = 0;
        g.blk_batch.len = 0;
        g.blk_batch.n_blocks = 0;
        if (!g.blk_idx.slots) {
            fprintf(stderr, "FATAL: blk_idx calloc failed");
            exit(1);
        }
    }
    hd_recipe_init(rec_out);
    hd_buf_init(&g.inline_buf, 4096);
    hd_buf_init(&g.cdc_buf, 512 * 1024);
    hd_buf_init(&g.pad_buf, 4096);
    hd_buf_init(&g.meta_buf, 4096);
    if (cfg->policy == HD_POLICY_CHUNK8K || cfg->policy == HD_POLICY_CHUNK16K ||
        cfg->policy == HD_POLICY_FIXBLOCK)
        hd_buf_reserve(&g.cdc_buf, g.fixed_bs);

    SHA256_CTX sha;
    SHA256_Init(&sha);
    hd_sha256_hex(blob, bloblen, res->blob_sha);

    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, 15 + 32) != Z_OK) { hd_recipe_free(rec_out); return -1; }

    uint8_t *win = malloc(1 << 20);
    zs.next_in = (Bytef *)blob;
    zs.avail_in = (uInt)bloblen;
    zs.next_out = win;
    zs.avail_out = 1 << 20;
    double t0 = hd_now_ms();
    int rc;
    int done = 0;
    while (!done) {
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) done = 1;
        else if (rc != Z_OK && rc != Z_BUF_ERROR) {
            inflateEnd(&zs); free(win);
            fprintf(stderr, "ingest: inflate rc=%d\n", rc);
            hd_recipe_free(rec_out);
            return -2;
        }
        size_t got = (1 << 20) - zs.avail_out;
        if (got) {
            double t1 = hd_now_ms();
            res->t_inflate_ms += t1 - t0;
            SHA256_Update(&sha, win, got);
            res->tar_size += got;
            feed(&g, win, got);
            res->t_route_ms += hd_now_ms() - t1;
            t0 = hd_now_ms();
            zs.next_out = win;
            zs.avail_out = 1 << 20;
        } else {
            if (rc == Z_BUF_ERROR && zs.avail_in == 0) break;
            if (done) break;
            zs.next_out = win;
            zs.avail_out = 1 << 20;
        }
    }
    inflateEnd(&zs);
    free(win);

    uint8_t dg[32];
    SHA256_Final(dg, &sha);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        res->diffid[i * 2] = hex[dg[i] >> 4];
        res->diffid[i * 2 + 1] = hex[dg[i] & 15];
    }
    res->diffid[64] = 0;
    res->blob_comp_size = bloblen;

    if (g.zero_run)
        hd_recipe_add_pad(rec_out, (uint32_t)(g.zero_run * 512));
    batch_flush(&g);
    if (g.cdc_buf.len) cdc_flush(&g);
    if (g.carry_len) {
        int idx = put_obj(st, rec_out, res, g.cdc_buf.data, g.carry_len, cfg);
        hd_recipe_add_ref(rec_out, idx, 0, g.carry_len);
        g.carry_len = 0;
    }
    if (g.inline_buf.len)
        hd_recipe_add_meta(rec_out, g.inline_buf.data, g.inline_buf.len);
    if (g.meta_buf.len)
        hd_recipe_add_meta(rec_out, g.meta_buf.data, g.meta_buf.len);

    rec_out->tar_size = res->tar_size;
    res->t_total_ms = res->t_inflate_ms + res->t_route_ms + res->t_compress_ms;

    hd_buf ser;
    hd_buf_init(&ser, 1 << 16);
    hd_recipe_serialize(rec_out, &ser);
    res->recipe_raw_size = ser.len;
    hd_store_put_recipe(st, ser.data, ser.len, recipe_sha);
    hd_buf_free(&ser);

    hd_buf_free(&g.inline_buf);
    hd_buf_free(&g.cdc_buf);
    hd_buf_free(&g.pad_buf);
    hd_buf_free(&g.meta_buf);
    return 0;
}
