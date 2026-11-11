 *
 */
#include <zlib.h>
#include "../hd_common.h"
#include "../hd_tokdec.h"
#include "../hd_tokenc.h"
#include "../hd_huffman.h"

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz ? sz : 1);
    if (sz > 0 && fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

static uint8_t *zlib_compress(const uint8_t *in, size_t inlen, int level, size_t *outlen) {
    uLongf cap = compressBound(inlen);
    uint8_t *out = malloc(cap);
    if (compress2(out, &cap, in, inlen, level) != Z_OK) hd_die("compress2 failed");
    *outlen = cap;
    return out;
}
static uint8_t *zlib_compress_keep(const uint8_t *in, size_t inlen, size_t *outlen) {
    return zlib_compress(in, inlen, 6, outlen);
}
static uint8_t *(*zlib_compress_ref)(const uint8_t *, size_t, size_t *) = zlib_compress_keep;

static uint8_t *zlib_inflate_raw(const uint8_t *in, size_t inlen, size_t *outlen) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -15) != Z_OK) hd_die("inflateInit2");
    size_t cap = 1 << 20;
    uint8_t *out = malloc(cap);
    size_t have = 0;
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)inlen;
    int rc;
    do {
        if (have == cap) { cap *= 2; out = realloc(out, cap); }
        zs.next_out = out + have;
        zs.avail_out = (uInt)(cap - have);
        rc = inflate(&zs, Z_NO_FLUSH);
        have = cap - zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) hd_die("inflate failed rc=%d", rc);
    } while (zs.avail_out == 0);
    inflateEnd(&zs);
    *outlen = have;
    return out;
}

static uint8_t *zlib_deflate_raw(const uint8_t *in, size_t inlen, int level, size_t *outlen) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        hd_die("deflateInit2");
    size_t cap = deflateBound(&zs, inlen);
    uint8_t *out = malloc(cap);
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)inlen;
    zs.next_out = out;
    zs.avail_out = (uInt)cap;
    int rc = deflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) hd_die("deflate rc=%d", rc);
    *outlen = cap - zs.avail_out;
    deflateEnd(&zs);
    return out;
}

/* ---------- selftest ---------- */
static int selftest_one(const uint8_t *data, size_t len, uint32_t beta, int level) {
    /* 1) zlib raw deflate compression */
    size_t clen;
    uint8_t *comp = zlib_deflate_raw(data, len, level, &clen);

    hd_tokstream ts;
    hd_ts_init(&ts);
    int rc = hd_tokens_from_deflate(comp, clen, &ts, NULL);
    if (rc) { fprintf(stderr, "  decode rc=%d\n", rc); return 1; }
    if (hd_ts_out_bytes(&ts) != len) {
        fprintf(stderr, "  size mismatch: %llu != %zu\n",
                (unsigned long long)hd_ts_out_bytes(&ts), len);
        return 1;
    }

    size_t outcap = clen * 4 + 65536;
    uint8_t *out = malloc(outcap);
    int64_t n = hd_deflate_from_tokens(ts.tok, ts.ntok, beta, out, outcap);
    if (n <= 0) { fprintf(stderr, "  encode failed %lld\n", (long long)n); return 1; }

    size_t rlen;
    uint8_t *rt = zlib_inflate_raw(out, (size_t)n, &rlen);
    if (rlen != len || memcmp(rt, data, len) != 0) {
        fprintf(stderr, "  ROUNDTRIP MISMATCH rlen=%zu len=%zu\n", rlen, len);
        return 1;
    }

    printf("  ok len=%7zu zlib=%7zu(%4.2f%%) reenc=%7lld(%4.2f%%) tokens=%6zu "
           "lit=%3.1f%% match=%3.1f%%\n",
           len, clen, 100.0 * clen / len, (long long)n, 100.0 * n / len,
           ts.ntok,
           100.0 * ts.nliterals / (double)ts.ntok,
           100.0 * ts.nmatches / (double)ts.ntok);

    free(comp); free(out); free(rt);
    hd_ts_free(&ts);
    return 0;
}

static int cmd_selftest(int argc, char **argv) {
    int fail = 0;
    printf("[selftest] synthetic patterns (beta=1024, level 6)\n");
    {
        size_t n = 1 << 20;
        uint8_t *buf = malloc(n);
        memset(buf, 0, n);
        fail |= selftest_one(buf, n, 1024, 6);
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(rand() & 0xFF);
        fail |= selftest_one(buf, n, 1024, 6);
        for (size_t i = 0; i < n; i++)
            buf[i] = (i < n / 2) ? (uint8_t)(rand() & 0xFF)
                                 : (uint8_t)(buf[(size_t)(rand() % (n / 2))]);
        fail |= selftest_one(buf, n, 1024, 6);
        for (size_t i = 0; i < 100; i++) buf[i] = (uint8_t)('a' + i % 26);
        fail |= selftest_one(buf, 100, 1024, 6);
        fail |= selftest_one(buf, 1, 1024, 6);
        fail |= selftest_one(buf, 5000, 64, 6);
        for (int lv = 1; lv <= 9; lv++) {
            for (size_t i = 0; i < n; i++)
                buf[i] = (uint8_t)((rand() & 0xFF) ^ (i / 97));
            fail |= selftest_one(buf, n, 4096, lv);
        }
        free(buf);
    }
    for (int i = 2; i < argc; i++) {
        size_t len;
        uint8_t *data = read_file(argv[i], &len);
        if (!data) { fprintf(stderr, "read %s failed\n", argv[i]); fail = 1; continue; }
        printf("[selftest] %s (beta=16384, level 6)\n", argv[i]);
        if (len > 64 << 20) len = 64 << 20;
        fail |= selftest_one(data, len, 16384, 6);
        free(data);
    }
    printf(fail ? "SELFTEST FAIL\n" : "SELFTEST PASS\n");
    return fail;
}

static int cmd_debug_roundtrip(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: hd_tool debug-rt <in-file> [beta] [level] [out-prefix]\n");
        return 1;
    }
    size_t len;
    uint8_t *data = read_file(argv[2], &len);
    if (!data) hd_die("read %s", argv[2]);
    uint32_t beta = argc > 3 ? (uint32_t)atoi(argv[3]) : 1024;
    int level = argc > 4 ? atoi(argv[4]) : 6;
    const char *pfx = argc > 5 ? argv[5] : "/tmp/hdrt";

    size_t clen;
    uint8_t *comp = zlib_deflate_raw(data, len, level, &clen);
    char p[512];
    snprintf(p, sizeof p, "%s.zlib", pfx);
    FILE *f = fopen(p, "wb"); fwrite(comp, 1, clen, f); fclose(f);

    hd_tokstream ts;
    hd_ts_init(&ts);
    int rc = hd_tokens_from_deflate(comp, clen, &ts, NULL);
    printf("decode rc=%d ntok=%zu out_bytes=%llu (len=%zu)\n",
           rc, ts.ntok, (unsigned long long)hd_ts_out_bytes(&ts), len);
    if (rc) return 1;
    snprintf(p, sizeof p, "%s.tok", pfx);
    f = fopen(p, "wb"); fwrite(ts.tok, sizeof(hd_token), ts.ntok, f); fclose(f);

    size_t outcap = clen * 4 + 65536;
    uint8_t *out = malloc(outcap);
    int64_t n = hd_deflate_from_tokens(ts.tok, ts.ntok, beta, out, outcap);
    printf("encode n=%lld (zlib=%zu)\n", (long long)n, clen);
    if (n <= 0) return 1;
    snprintf(p, sizeof p, "%s.reenc", pfx);
    f = fopen(p, "wb"); fwrite(out, 1, (size_t)n, f); fclose(f);

    hd_tokstream ts2;
    hd_ts_init(&ts2);
    int rc2 = hd_tokens_from_deflate(out, (size_t)n, &ts2, NULL);
    printf("self-decode rc=%d ntok=%zu out_bytes=%llu\n",
           rc2, ts2.ntok, (unsigned long long)hd_ts_out_bytes(&ts2));
    int tok_eq = (rc2 == 0 && ts2.ntok == ts.ntok &&
                  memcmp(ts2.tok, ts.tok, ts.ntok * sizeof(hd_token)) == 0);
    printf("token streams equal: %s\n", tok_eq ? "YES" : "NO");

    size_t rlen;
    uint8_t *rt = zlib_inflate_raw(out, (size_t)n, &rlen);
    printf("zlib inflate: %s (rlen=%zu)\n",
           (rlen == len && memcmp(rt, data, len) == 0) ? "OK" : "FAIL", rlen);
    return 0;
}

#include "../hd_store.h"
#include "../hd_recipe.h"
#include "../hd_splice.h"
#include "../hd_synth.h"

static uint8_t *inflate_store_obj(hd_store *st, const char *sha, size_t *len) {
    size_t cl;
    uint8_t *c = hd_store_get_object(st, sha, &cl);
    if (!c) return NULL;
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    inflateInit2(&zs, -15);
    size_t cap = 1 << 20;
    uint8_t *out = malloc(cap);
    size_t have = 0;
    zs.next_in = c;
    zs.avail_in = (uInt)cl;
    for (;;) {
        if (have == cap) { cap *= 2; out = realloc(out, cap); }
        zs.next_out = out + have;
        zs.avail_out = (uInt)(cap - have);
        int rc = inflate(&zs, Z_FINISH);
        have = cap - zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK && rc != Z_BUF_ERROR) { inflateEnd(&zs); free(out); free(c); return NULL; }
        if (rc == Z_BUF_ERROR && zs.avail_in == 0) break;
    }
    inflateEnd(&zs);
    free(c);
    *len = have;
    return out;
}

static int cmd_splicetest(int argc, char **argv) {
    hd_store st;
    int use_real = (argc >= 5);
    if (use_real) {
        hd_store_open(&st, argv[2], 0);
        hd_recipe rec;
        hd_recipe_init(&rec);
        uint8_t *r1 = NULL, *r2 = NULL;
        size_t l1, l2;
        r1 = inflate_store_obj(&st, argv[3], &l1);
        r2 = inflate_store_obj(&st, argv[4], &l2);
        if (!r1 || !r2) { printf("object load fail\n"); return 1; }
        char sh1[65], sh2[65];
        strcpy(sh1, argv[3]); strcpy(sh2, argv[4]);
        uint32_t c1_ = crc32(0L, Z_NULL, 0); c1_ = crc32(c1_, r1, (uInt)l1);
        uint32_t c2_ = crc32(0L, Z_NULL, 0); c2_ = crc32(c2_, r2, (uInt)l2);
        int i1 = hd_recipe_add_obj(&rec, sh1, l1, c1_);
        int i2 = hd_recipe_add_obj(&rec, sh2, l2, c2_);
        hd_recipe_add_ref(&rec, i1, 0, (uint32_t)l1);
        hd_recipe_add_ref(&rec, i2, 0, (uint32_t)l2);
        rec.tar_size = l1 + l2;
        hd_buf out;
        hd_buf_init(&out, 1 << 16);
        hd_splice_result sres;
        int rc = hd_splice_layer(&st, &rec, &out, &sres);
        FILE *sf = fopen("/tmp/e2e/iso_splice.gz", "wb");
        if (sf) { fwrite(out.data, 1, out.len, sf); fclose(sf); }
        printf("splice rc=%d out=%zu (raw %zu + %zu)\n", rc, out.len, l1, l2);
        uint8_t *cat = malloc(l1 + l2);
        memcpy(cat, r1, l1);
        memcpy(cat + l1, r2, l2);
        char want[65];
        hd_sha256_hex(cat, l1 + l2, want);
        char *got = hd_synth_verify(&st, &rec, out.data, out.len);
        printf("want=%s\ngot =%s\n%s\n", want, got ? got : "(null)",
               (got && !strcmp(got, want)) ? "PASS" : "FAIL");
        return (got && !strcmp(got, want)) ? 0 : 1;
    }
    hd_store_open(&st, "/tmp/splicetest_store", 1);
    size_t n1 = 40000, n2 = 30685;
    uint8_t *b1 = malloc(n1), *b2 = malloc(n2);
    srand(42);
    for (size_t i = 0; i < n1; i++)
        b1[i] = (i < n1 / 3) ? (uint8_t)(rand() & 0xFF) : (uint8_t)(b1[rand() % (n1 / 3)] + i);
    for (size_t i = 0; i < n2; i++)
        b2[i] = (i < n2 / 3) ? (uint8_t)(rand() & 0xFF) : (uint8_t)(b2[rand() % (n2 / 3)] ^ i);

    hd_recipe rec;
    hd_recipe_init(&rec);
    char sha1[65], sha2[65];
    /* objectcompression（zlib L6 raw） */
    size_t c1, c2;
    uint8_t *z1 = zlib_deflate_raw(b1, n1, 6, &c1);
    uint8_t *z2 = zlib_deflate_raw(b2, n2, 6, &c2);
    uint32_t crc1, crc2;
    hd_store_put_object(&st, b1, n1, z1, c1, sha1, &crc1);
    hd_store_put_object(&st, b2, n2, z2, c2, sha2, &crc2);
    hd_blkinfo blks[4096];
    int nb = hd_scan_blocks(z1, c1, blks, 4096);
    hd_store_put_blockmap(&st, sha1, blks, nb);
    nb = hd_scan_blocks(z2, c2, blks, 4096);
    hd_store_put_blockmap(&st, sha2, blks, nb);

    int i1 = hd_recipe_add_obj(&rec, sha1, n1, crc1);
    int i2 = hd_recipe_add_obj(&rec, sha2, n2, crc2);
    hd_recipe_add_ref(&rec, i1, 0, (uint32_t)n1);
    hd_recipe_add_ref(&rec, i2, 0, (uint32_t)n2);
    rec.tar_size = n1 + n2;

    /* splice */
    hd_buf out;
    hd_buf_init(&out, 1 << 16);
    hd_splice_result sres;
    int rc = hd_splice_layer(&st, &rec, &out, &sres);
    printf("splice rc=%d out=%zu\n", rc, out.len);
    if (rc) return 1;
    char *got = hd_synth_verify(&st, &rec, out.data, out.len);
    uint8_t *cat = malloc(n1 + n2);
    memcpy(cat, b1, n1);
    memcpy(cat + n1, b2, n2);
    char want[65];
    hd_sha256_hex(cat, n1 + n2, want);
    printf("want=%s\ngot =%s\n%s\n", want, got ? got : "(null)",
           (got && !strcmp(got, want)) ? "SPLICETEST PASS" : "SPLICETEST FAIL");
    int pass = got && !strcmp(got, want);
    free(got);
    free(cat);
    free(b1); free(b2); free(z1); free(z2);
    hd_buf_free(&out);
    hd_recipe_free(&rec);
    hd_store_close(&st);
    return pass ? 0 : 1;
}

int main(int argc, char **argv) {
    hd_common_init();
    if (argc < 2) {
        fprintf(stderr, "usage: hd_tool <selftest|debug-rt|bench-stage> [args]\n");
        return 1;
    }
    if (!strcmp(argv[1], "selftest")) return cmd_selftest(argc, argv);
    if (!strcmp(argv[1], "debug-rt")) return cmd_debug_roundtrip(argc, argv);
    if (!strcmp(argv[1], "splicetest")) return cmd_splicetest(argc, argv);
    fprintf(stderr, "unknown subcommand %s\n", argv[1]);
    return 1;
}
