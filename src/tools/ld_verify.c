 * Usage: ld_verify <file.gz> <expected_sha256_hex>
 * output: len=N sha256=... LD_VERIFY_OK/FAIL
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#include "libdeflate.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: ld_verify <file.gz> <sha256>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *in = malloc(sz);
    if (fread(in, 1, sz, f) != (size_t)sz) { return 2; }
    fclose(f);

    size_t cap = (size_t)sz * 4 + (1 << 22);
    unsigned char *out = NULL;
    size_t out_len = 0;
    struct libdeflate_decompressor *dc = libdeflate_alloc_decompressor();
    if (!dc) { fprintf(stderr, "alloc fail\n"); return 2; }

    for (int round = 0; round < 8; round++) {
        out = malloc(cap);
        enum libdeflate_result r = libdeflate_gzip_decompress(
            dc, in, sz, out, cap, &out_len);
        if (r == LIBDEFLATE_SUCCESS) goto done;
        free(out);
        out = NULL;
        if (r != LIBDEFLATE_INSUFFICIENT_SPACE) {
            fprintf(stderr, "libdeflate error %d\n", r);
            libdeflate_free_decompressor(dc);
            return 1;
        }
        cap *= 2;
    }
    fprintf(stderr, "cap exhausted\n");
    return 2;

done:
    libdeflate_free_decompressor(dc);
    unsigned char dg[32];
    SHA256(out, out_len, dg);
    static const char hx[] = "0123456789abcdef";
    char got[65];
    for (int i = 0; i < 32; i++) {
        got[i * 2] = hx[dg[i] >> 4];
        got[i * 2 + 1] = hx[dg[i] & 15];
    }
    got[64] = 0;
    printf("len=%zu sha256=%s\n", out_len, got);
    int ok = !strcmp(got, argv[2]);
    printf(ok ? "LD_VERIFY_OK\n" : "LD_VERIFY_FAIL\n");
    free(in);
    free(out);
    return ok ? 0 : 1;
}
