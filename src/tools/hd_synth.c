 * Usage:
 *   hd_synth <store> synth    <recipe_sha> [--beta N] [--out F] [--verify]
 *   hd_synth <store> splice   <recipe_sha> [--out F] [--verify]
 */
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <zlib.h>
#include <openssl/sha.h>
#include "../hd_common.h"
#include "../hd_store.h"
#include "../hd_recipe.h"
#include "../hd_synth.h"
#include "../hd_splice.h"

static uint8_t *read_file(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz ? sz : 1);
    if (sz > 0 && fread(b, 1, sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static uint64_t assemble_tar(hd_store *st, const hd_recipe *rec,
                             uint8_t **out, char sha_hex[65], double *ms) {
    hd_buf b;
    hd_buf_init(&b, 1 << 20);
    SHA256_CTX sc;
    SHA256_Init(&sc);
    double t0 = hd_now_ms();
    int cur = -1;
    uint8_t *raw = NULL;
    for (int i = 0; i < rec->n_segs; i++) {
        const hd_seg *s = &rec->segs[i];
        if (s->type == HD_SEG_META) {
            hd_buf_push(&b, rec->bytes + s->b, s->a);
        } else if (s->type == HD_SEG_PAD) {
            static const uint8_t z[512] = {0};
            uint32_t r = s->a;
            while (r) { uint32_t t = r > 512 ? 512 : r; hd_buf_push(&b, z, t); r -= t; }
        } else {
            const hd_objmeta *o = &rec->objs[s->a];
            if (s->c == 0) continue;
            if (cur != (int)s->a) {
                free(raw);
                size_t clen;
                uint8_t *comp = hd_store_get_object(st, o->sha, &clen);
                if (!comp) hd_die("missing object %s", o->sha);
                raw = malloc(o->raw_len ? o->raw_len : 1);
                z_stream zs;
                memset(&zs, 0, sizeof zs);
                inflateInit2(&zs, -15);
                zs.next_in = comp;
                zs.avail_in = (uInt)clen;
                zs.next_out = raw;
                zs.avail_out = (uInt)o->raw_len;
                if (inflate(&zs, Z_FINISH) != Z_STREAM_END)
                    hd_die("obj inflate fail");
                inflateEnd(&zs);
                free(comp);
                cur = (int)s->a;
            }
            hd_buf_push(&b, raw + s->b, s->c);
        }
    }
    free(raw);
    SHA256_Update(&sc, b.data, b.len);
    uint8_t dg[32];
    SHA256_Final(dg, &sc);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        sha_hex[i * 2] = hx[dg[i] >> 4];
        sha_hex[i * 2 + 1] = hx[dg[i] & 15];
    }
    sha_hex[64] = 0;
    *ms = hd_now_ms() - t0;
    *out = b.data;
    return b.len;
}

static const char *g_comp = "z6";

typedef struct libdeflate_compressor libdeflate_compressor;
static struct {
    void *h;
    libdeflate_compressor *(*alloc)(int level);
    void (*free_c)(libdeflate_compressor *);
    size_t (*gzip)(const libdeflate_compressor *, const void *, size_t,
                   void *, size_t);
} g_ld;

static void ld_init(void) {
    if (g_ld.h) return;
    g_ld.h = dlopen("libdeflate.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!g_ld.h) g_ld.h = dlopen("libdeflate.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_ld.h) hd_die("libdeflate not loadable: %s", dlerror());
    g_ld.alloc = (libdeflate_compressor *(*)(int))
        dlsym(g_ld.h, "libdeflate_alloc_compressor");
    g_ld.free_c = (void (*)(libdeflate_compressor *))
        dlsym(g_ld.h, "libdeflate_free_compressor");
    g_ld.gzip = (size_t (*)(const libdeflate_compressor *, const void *,
                            size_t, void *, size_t))
        dlsym(g_ld.h, "libdeflate_gzip_compress");
    if (!g_ld.alloc || !g_ld.gzip)
        hd_die("libdeflate symbols missing");
}

static uint64_t gzip_l6(const uint8_t *in, size_t n, hd_buf *out, double *ms,
                        int omit_inflate) {
    (void)omit_inflate;
    double t0 = hd_now_ms();
    out->len = 0;
    if (g_comp[0] == 'l') {
        ld_init();
        int lvl = atoi(g_comp + 2);
        libdeflate_compressor *c = g_ld.alloc(lvl);
        if (!c) hd_die("libdeflate alloc(%d) failed", lvl);
        hd_buf_reserve(out, n + (n >> 6) + (1 << 20));
        size_t csz = g_ld.gzip(c, in, n, out->data, out->cap);
        if (csz == 0) hd_die("libdeflate gzip out of space");
        out->len = csz;
        g_ld.free_c(c);
        *ms = hd_now_ms() - t0;
        return csz;
    }
    int level = strcmp(g_comp, "z1") == 0 ? 1 : 6;
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    deflateInit2(&zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);  /* gzip */
    hd_buf_reserve(out, n + (n >> 6) + (1 << 20));
    size_t in_pos = 0;
    out->len = 0;
    int rc = Z_OK;
    while (rc == Z_OK || rc == Z_BUF_ERROR) {
        size_t chunk_in = n - in_pos;
        if (chunk_in > (1u << 30)) chunk_in = 1u << 30;
        zs.next_in = (Bytef *)in + in_pos;
        zs.avail_in = (uInt)chunk_in;
        int flush = (in_pos + chunk_in >= n) ? Z_FINISH : Z_NO_FLUSH;
        do {
            if (out->len == out->cap) hd_die("gzip_l6 out of space");
            size_t old_len = out->len;
            size_t room = out->cap - old_len;
            if (room > (1u << 30)) room = 1u << 30;
            zs.next_out = out->data + old_len;
            zs.avail_out = (uInt)room;
            rc = deflate(&zs, flush);
            out->len = old_len + (room - zs.avail_out);
            if (rc == Z_STREAM_END) break;
            if (rc != Z_OK && rc != Z_BUF_ERROR) hd_die("gzip_l6 rc=%d", rc);
        } while (zs.avail_out == 0);
        in_pos += chunk_in - zs.avail_in;
        if (rc == Z_STREAM_END) break;
        if (flush == Z_NO_FLUSH && zs.avail_in == 0 && in_pos >= n) flush = Z_FINISH;
        if (rc == Z_BUF_ERROR && zs.avail_in == 0 && in_pos < n) continue;
    }
    if (rc != Z_STREAM_END) hd_die("gzip_l6 final rc=%d", rc);
    deflateEnd(&zs);
    *ms = hd_now_ms() - t0;
    return out->len;
}

static int verify_gz(const uint8_t *gz, size_t gzlen, const char *want_sha) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return -1;
    SHA256_CTX sc;
    SHA256_Init(&sc);
    uint8_t *win = malloc(1 << 20);
    size_t fed = 0;
    int rc = Z_OK;
    for (;;) {
        if (zs.avail_in == 0 && fed < gzlen) {
            size_t chunk = gzlen - fed;
            if (chunk > (size_t)1 << 30) chunk = (size_t)1 << 30;
            zs.next_in = (Bytef *)(gz + fed);
            zs.avail_in = (uInt)chunk;
            fed += chunk;
        }
        zs.next_out = win;
        zs.avail_out = 1 << 20;
        rc = inflate(&zs, Z_NO_FLUSH);
        size_t got = (1 << 20) - zs.avail_out;
        if (got) SHA256_Update(&sc, win, got);
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK && rc != Z_BUF_ERROR) { inflateEnd(&zs); free(win); return -2; }
        if (rc == Z_BUF_ERROR && zs.avail_in == 0 && fed >= gzlen) break;
    }
    if (rc != Z_STREAM_END) { inflateEnd(&zs); free(win); return -2; }
    inflateEnd(&zs);
    free(win);
    uint8_t dg[32];
    SHA256_Final(dg, &sc);
    static const char hx[] = "0123456789abcdef";
    char hex[65];
    for (int i = 0; i < 32; i++) {
        hex[i * 2] = hx[dg[i] >> 4];
        hex[i * 2 + 1] = hx[dg[i] & 15];
    }
    hex[64] = 0;
    return strcmp(hex, want_sha) ? 1 : 0;
}

int main(int argc, char **argv) {
    hd_common_init();
    if (argc < 4) {
        fprintf(stderr, "usage: hd_synth <store> <mode> <recipe_sha|gzfile> [opts]\n");
        return 1;
    }
    hd_store st;
    hd_store_open(&st, argv[1], 0);
    const char *mode = argv[2];
    const char *arg = argv[3];
    const char *outf = NULL;
    int verify = 0;
    uint32_t beta = 16384;
    int nthreads = 1;
    int refsonly = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--out")) outf = argv[++i];
        else if (!strcmp(argv[i], "--comp")) g_comp = argv[++i];
        else if (!strcmp(argv[i], "--verify")) verify = 1;
        else if (!strcmp(argv[i], "--beta")) beta = (uint32_t)atoll(argv[++i]);
        else if (!strcmp(argv[i], "--threads")) nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--refsonly")) refsonly = 1;
    }

    if (!strcmp(mode, "inflate")) {
        size_t n;
        uint8_t *gz = read_file(arg, &n);
        if (!gz) hd_die("read %s", arg);
        z_stream zs;
        memset(&zs, 0, sizeof zs);
        inflateInit2(&zs, 15 + 32);
        uint8_t *win = malloc(1 << 20);
        uint64_t total = 0;
        double t0 = hd_now_ms();
        zs.next_in = gz;
        zs.avail_in = (uInt)n;
        zs.next_out = win;
        zs.avail_out = 1 << 20;
        int rc;
        while ((rc = inflate(&zs, Z_NO_FLUSH)) != Z_STREAM_END) {
            if (rc != Z_OK && rc != Z_BUF_ERROR) hd_die("inflate rc=%d", rc);
            size_t got = (1 << 20) - zs.avail_out;
            total += got;
            zs.next_out = win;
            zs.avail_out = 1 << 20;
            if (rc == Z_BUF_ERROR && zs.avail_in == 0) break;
        }
        total += (1 << 20) - zs.avail_out;
        double ms = hd_now_ms() - t0;
        inflateEnd(&zs);
        printf("{\"mode\":\"inflate\",\"file\":\"%s\",\"comp\":%llu,\"tar\":%llu,"
               "\"ms\":%.3f,\"mbps\":%.1f}\n",
               arg, (unsigned long long)n, (unsigned long long)total,
               ms, total / 1024.0 / 1024.0 / (ms / 1000.0));
        return 0;
    }

    size_t rlen;
    uint8_t *rpayload = hd_store_get_recipe(&st, arg, &rlen);
    if (!rpayload) hd_die("recipe %s not found", arg);
    hd_recipe rec;
    if (hd_recipe_deserialize(&rec, rpayload, rlen)) hd_die("recipe parse fail");
    free(rpayload);

    uint8_t *tar = NULL;
    char want_sha[65];
    double asm_ms;
    uint64_t tar_size = assemble_tar(&st, &rec, &tar, want_sha, &asm_ms);

    hd_buf out;
    hd_buf_init(&out, 1 << 20);
    double ms = 0;
    int vres = -99;

    if (!strcmp(mode, "assemble")) {
        printf("{\"mode\":\"assemble\",\"recipe\":\"%s\",\"tar\":%llu,"
               "\"diffid\":\"%s\",\"ms\":%.3f}\n",
               arg, (unsigned long long)tar_size, want_sha, asm_ms);
        free(tar);
        return 0;
    } else if (!strcmp(mode, "synth")) {
        hd_synth_cfg cfg;
        hd_synth_cfg_default(&cfg);
        cfg.beta_tokens = beta;
        cfg.threads = nthreads;
        hd_synth_result res;
        hd_synth_refsonly_flag = refsonly;
        int rc = hd_synth_layer(&st, &cfg, &rec, &out, &res);
        if (rc) hd_die("synth rc=%d", rc);
        ms = res.t_total_ms;
        if (verify) vres = verify_gz(out.data, out.len, want_sha);
        printf("{\"mode\":\"synth\",\"recipe\":\"%s\",\"tar\":%llu,\"out\":%llu,"
               "\"ms\":%.3f,\"mbps\":%.1f,"
               "\"t_dec_ms\":%.3f,\"t_enc_ms\":%.3f,\"t_misc_ms\":%.3f,"
               "\"t_load_ms\":%.3f,\"t_loop_ms\":%.3f,"
               "\"t_reserve_ms\":%.3f,\"t_pass1_ms\":%.3f,"
               "\"n_tokens\":%llu,\"tok_obj\":%llu,\"tok_lit\":%llu,"
               "\"crc_ok\":%s,\"verify\":\"%s\",\"beta\":%u}\n",
               arg, (unsigned long long)tar_size,
               (unsigned long long)res.out_comp_size,
               ms, tar_size / 1024.0 / 1024.0 / (ms / 1000.0),
               res.t_decode_ms, res.t_encode_ms, res.t_misc_ms,
               res.t_load_ms, res.t_loop_ms, res.t_reserve_ms, res.t_pass1_ms,
               (unsigned long long)res.n_tokens,
               (unsigned long long)res.tokens_from_obj,
               (unsigned long long)res.tokens_literalized,
               res.crc ? "true" : "false",
               vres == 0 ? "OK" : (vres == -99 ? "SKIP" : "FAIL"), beta);
    } else if (!strcmp(mode, "splice")) {
        hd_splice_result res;
        int rc = hd_splice_layer(&st, &rec, &out, &res);
        if (rc) hd_die("splice rc=%d", rc);
        ms = res.t_total_ms;
        if (verify) vres = verify_gz(out.data, out.len, want_sha);
        printf("{\"mode\":\"splice\",\"recipe\":\"%s\",\"tar\":%llu,\"out\":%llu,"
               "\"ms\":%.3f,\"mbps\":%.1f,"
               "\"t_bitcopy_ms\":%.3f,\"verify\":\"%s\"}\n",
               arg, (unsigned long long)tar_size,
               (unsigned long long)res.out_comp_size,
               ms, tar_size / 1024.0 / 1024.0 / (ms / 1000.0),
               res.t_bitcopy_ms,
               vres == 0 ? "OK" : (vres == -99 ? "SKIP" : "FAIL"));
    } else if (!strcmp(mode, "b4") || !strcmp(mode, "b2")) {
        double gz_ms;
        uint64_t csz = gzip_l6(tar, tar_size, &out, &gz_ms, 0);
        ms = gz_ms;
        if (verify) vres = verify_gz(out.data, out.len, want_sha);
        printf("{\"mode\":\"%s-%s\",\"recipe\":\"%s\",\"tar\":%llu,\"out\":%llu,"
               "\"ms\":%.3f,\"mbps\":%.1f,\"asm_ms\":%.3f,\"verify\":\"%s\"}\n",
               mode, g_comp, arg, (unsigned long long)tar_size,
               (unsigned long long)csz,
               ms, tar_size / 1024.0 / 1024.0 / (ms / 1000.0),
               asm_ms,
               vres == 0 ? "OK" : (vres == -99 ? "SKIP" : "FAIL"));
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 1;
    }

    if (outf && out.len) {
        FILE *f = fopen(outf, "wb");
        fwrite(out.data, 1, out.len, f);
        fclose(f);
    }
    free(tar);
    hd_buf_free(&out);
    hd_recipe_free(&rec);
    return 0;
}
