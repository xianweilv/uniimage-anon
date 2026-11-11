#include "hd_recipe.h"

uint64_t hd_tar_parse_size(const uint8_t *h) {
    const uint8_t *f = h + 124;
    if (f[0] & 0x80) {            /* GNU base-256 */
        uint64_t v = f[0] & 0x7F;
        for (int i = 1; i < 12; i++) v = (v << 8) | f[i];
        return v;
    }
    uint64_t v = 0;
    for (int i = 0; i < 12; i++) {
        if (f[i] == 0 || f[i] == ' ') break;
        if (f[i] < '0' || f[i] > '7') return (uint64_t)-1;
        v = v * 8 + (uint64_t)(f[i] - '0');
    }
    return v;
}

int hd_tar_header_ok(const uint8_t *blk) {
    if (blk[0] == 0) return 0;
    const uint8_t *f = blk + 148;
    uint64_t want = 0;
    int got = 0;
    for (int i = 0; i < 8; i++) {
        if (f[i] >= '0' && f[i] <= '7') { want = want * 8 + (uint64_t)(f[i] - '0'); got = 1; }
        else if (f[i] == 0 || f[i] == ' ') { if (got) break; }
        else return 0;
    }
    if (!got) return 0;
    uint64_t sum = 0;
    for (int i = 0; i < 512; i++)
        sum += (i >= 148 && i < 156) ? (uint32_t)' ' : blk[i];
    uint64_t ssum = 0;
    for (int i = 0; i < 512; i++)
        ssum += (uint32_t)(int8_t)((i >= 148 && i < 156) ? ' ' : blk[i]);
    return sum == want || ssum == want;
}

void hd_recipe_init(hd_recipe *r) {
    memset(r, 0, sizeof *r);
    r->cap_segs = 1024;
    r->segs = malloc(r->cap_segs * sizeof(hd_seg));
    r->cap_objs = 1024;
    r->objs = malloc(r->cap_objs * sizeof(hd_objmeta));
    r->cap_bytes = 1 << 16;
    r->bytes = malloc(r->cap_bytes);
    if (!r->segs || !r->objs || !r->bytes) hd_die("OOM recipe");
}

void hd_recipe_free(hd_recipe *r) {
    free(r->segs);
    free(r->objs);
    free(r->bytes);
    memset(r, 0, sizeof *r);
}

static void ensure_segs(hd_recipe *r, int need) {
    if (r->n_segs + need <= r->cap_segs) return;
    while (r->cap_segs < r->n_segs + need) r->cap_segs *= 2;
    r->segs = realloc(r->segs, r->cap_segs * sizeof(hd_seg));
    if (!r->segs) hd_die("OOM segs");
}

static void ensure_bytes(hd_recipe *r, size_t need) {
    if (r->nbytes + need <= r->cap_bytes) return;
    while (r->cap_bytes < r->nbytes + need) r->cap_bytes *= 2;
    r->bytes = realloc(r->bytes, r->cap_bytes);
    if (!r->bytes) hd_die("OOM bytes");
}

void hd_recipe_add_meta(hd_recipe *r, const uint8_t *p, size_t n) {
    ensure_segs(r, 1);
    ensure_bytes(r, n);
    hd_seg *s = &r->segs[r->n_segs++];
    s->type = HD_SEG_META;
    s->a = (uint32_t)n;
    s->b = (uint32_t)r->nbytes;
    s->c = 0;
    memcpy(r->bytes + r->nbytes, p, n);
    r->nbytes += n;
    r->meta_bytes += n;
}

void hd_recipe_add_pad(hd_recipe *r, uint32_t z) {
    if (z == 0) return;
    ensure_segs(r, 1);
    hd_seg *s = &r->segs[r->n_segs++];
    s->type = HD_SEG_PAD;
    s->a = z;
    r->pad_bytes += z;
}

int hd_recipe_add_obj(hd_recipe *r, const char sha[65], uint64_t raw, uint32_t crc) {
    for (int i = r->n_objs - 1; i >= 0; i--)
        if (!memcmp(r->objs[i].sha, sha, 64)) return i;
    if (r->n_objs >= r->cap_objs) {
        r->cap_objs *= 2;
        r->objs = realloc(r->objs, r->cap_objs * sizeof(hd_objmeta));
        if (!r->objs) hd_die("OOM objs");
    }
    hd_objmeta *o = &r->objs[r->n_objs];
    memcpy(o->sha, sha, 65);
    o->raw_len = raw;
    o->crc = crc;
    return r->n_objs++;
}

void hd_recipe_add_ref(hd_recipe *r, int obj_idx, uint32_t off, uint32_t len) {
    ensure_segs(r, 1);
    hd_seg *s = &r->segs[r->n_segs++];
    s->type = HD_SEG_REF;
    s->a = (uint32_t)obj_idx;
    s->b = off;
    s->c = len;
    r->ref_bytes += len;
}

 *   magic "UIR1" | u32 n_objs | u32 n_segs
 *     PAD : u8 type | u32 z
 *     REF : u8 type | u32 obj_idx | u32 off | u32 len
 */
#define REC_MAGIC 0x31524955   /* "UIR1" LE */

void hd_recipe_serialize(const hd_recipe *r, hd_buf *out) {
    hd_buf_push(out, "UIR1", 4);
    uint32_t n_objs = (uint32_t)r->n_objs;
    uint32_t n_segs = (uint32_t)r->n_segs;
    hd_buf_push(out, &n_objs, 4);
    hd_buf_push(out, &n_segs, 4);
    for (int i = 0; i < r->n_objs; i++) {
        uint8_t bin[32];
        for (int k = 0; k < 32; k++) {
            unsigned v;
            sscanf(r->objs[i].sha + k * 2, "%2x", &v);
            bin[k] = (uint8_t)v;
        }
        hd_buf_push(out, bin, 32);
        hd_buf_push(out, &r->objs[i].raw_len, 8);
        hd_buf_push(out, &r->objs[i].crc, 4);
    }
    for (int i = 0; i < r->n_segs; i++) {
        const hd_seg *s = &r->segs[i];
        uint8_t t = s->type;
        hd_buf_push(out, &t, 1);
        if (t == HD_SEG_META) {
            hd_buf_push(out, &s->a, 4);
            hd_buf_push(out, r->bytes + s->b, s->a);
        } else if (t == HD_SEG_PAD) {
            hd_buf_push(out, &s->a, 4);
        } else {
            hd_buf_push(out, &s->a, 4);
            hd_buf_push(out, &s->b, 4);
            hd_buf_push(out, &s->c, 4);
        }
    }
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd_u64(const uint8_t *p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}

int hd_recipe_deserialize(hd_recipe *r, const uint8_t *p, size_t len) {
    hd_recipe_init(r);
    if (len < 12 || memcmp(p, "UIR1", 4)) return -1;
    size_t off = 4;
    uint32_t n_objs = rd_u32(p + off); off += 4;
    uint32_t n_segs = rd_u32(p + off); off += 4;
    if (n_objs > HD_MAX_OBJS_PER_LAYER) return -1;
    if (n_objs > (uint32_t)r->cap_objs) {
        r->cap_objs = (int)n_objs;
        r->objs = realloc(r->objs, r->cap_objs * sizeof(hd_objmeta));
        if (!r->objs) return -1;
    }
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n_objs; i++) {
        if (off + 44 > len) return -1;
        hd_objmeta *o = &r->objs[r->n_objs++];
        for (int k = 0; k < 32; k++) {
            o->sha[k * 2] = hex[p[off + k] >> 4];
            o->sha[k * 2 + 1] = hex[p[off + k] & 15];
        }
        o->sha[64] = 0;
        off += 32;
        o->raw_len = rd_u64(p + off); off += 8;
        o->crc = rd_u32(p + off); off += 4;
    }
    ensure_segs(r, (int)n_segs);
    for (uint32_t i = 0; i < n_segs; i++) {
        if (off + 1 > len) return -1;
        uint8_t t = p[off++];
        if (t == HD_SEG_META) {
            if (off + 4 > len) return -1;
            uint32_t n = rd_u32(p + off); off += 4;
            if (off + n > len) return -1;
            hd_recipe_add_meta(r, p + off, n);
            off += n;
        } else if (t == HD_SEG_PAD) {
            if (off + 4 > len) return -1;
            hd_recipe_add_pad(r, rd_u32(p + off)); off += 4;
        } else if (t == HD_SEG_REF) {
            if (off + 12 > len) return -1;
            int idx = (int)rd_u32(p + off);
            uint32_t o2 = rd_u32(p + off + 4), l = rd_u32(p + off + 8);
            off += 12;
            hd_recipe_add_ref(r, idx, o2, l);
        } else return -1;
    }
    return 0;
}
