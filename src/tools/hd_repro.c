 * Usage: hd_repro <tokens.bin> [beta]
 */
#include <string.h>
#include <zlib.h>
#include "../hd_common.h"
#include "../hd_tokenc.h"

static void expand(const hd_token *tok, size_t n, hd_buf *out) {
    for (size_t i = 0; i < n; i++) {
        if (tok[i].d == 0) {
            hd_buf_push_byte(out, (uint8_t)tok[i].v);
        } else {
            for (uint32_t j = 0; j < tok[i].v; j++)
                hd_buf_push_byte(out, out->data[out->len - tok[i].d]);
        }
    }
}

static int try_encode(const hd_token *tok, size_t n, uint32_t beta,
                      hd_buf *raw_want) {
    size_t cap = raw_want->len + (raw_want->len >> 3) + (1 << 20);
    uint8_t *out = malloc(cap);
    int64_t n_enc = hd_deflate_from_tokens(tok, n, beta, out, cap);
    if (n_enc <= 0) { free(out); return -1; }
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    inflateInit2(&zs, -15);
    uint8_t *dec = malloc(raw_want->len + 64);
    zs.next_in = out;
    zs.avail_in = (uInt)n_enc;
    zs.next_out = dec;
    zs.avail_out = (uInt)raw_want->len + 64;
    int rc = inflate(&zs, Z_FINISH);
    size_t got = (raw_want->len + 64) - zs.avail_out;
    inflateEnd(&zs);
    int ok = (rc == Z_STREAM_END) && got == raw_want->len &&
             memcmp(dec, raw_want->data, raw_want->len) == 0;
    free(out); free(dec);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    hd_common_init();
    if (argc < 2) return 1;
    uint32_t beta = argc > 2 ? (uint32_t)atoll(argv[2]) : 16384;
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    hd_token *tok = malloc(sz);
    if (fread(tok, 1, sz, f) != (size_t)sz) return 2;
    fclose(f);
    size_t ntok = (size_t)sz / sizeof(hd_token);
    printf("tokens=%zu beta=%u\n", ntok, beta);

    size_t lo = 1, hi = ntok;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        hd_buf want;
        hd_buf_init(&want, 1 << 16);
        expand(tok, mid, &want);
        int r = try_encode(tok, mid, beta, &want);
        free(want.data);
        if (r == 0) lo = mid + 1;
        else hi = mid;
    }
    printf("minimal failing prefix = %zu tokens\n", lo);
    hd_buf want;
    hd_buf_init(&want, 1 << 16);
    expand(tok, lo, &want);
    printf("verify [0,%zu): %s\n", lo - 1,
           try_encode(tok, lo - 1, beta, &want) == 0 ? "OK" : "FAIL");
    printf("verify [0,%zu]: FAIL expected\n", lo);
    for (size_t j = lo > 12 ? lo - 12 : 0; j < lo; j++)
        printf("  tok[%zu] %s v=%u d=%u\n", j, tok[j].d ? "M" : "L",
               tok[j].v, tok[j].d);
    printf("single-token encodes:\n");
    for (size_t j = lo > 6 ? lo - 6 : 0; j < lo; j++) {
        hd_buf w1;
        hd_buf_init(&w1, 4096);
        expand(tok + j, 1, &w1);
        printf("  tok[%zu] alone: %s\n", j,
               try_encode(tok + j, 1, beta, &w1) == 0 ? "OK" : "FAIL");
        free(w1.data);
    }
    return 0;
}
