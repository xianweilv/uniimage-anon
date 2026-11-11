 *   hd_bench decompose <file> [file...] [--level N] [--repeat N]
 */
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>
#include "../hd_common.h"
#include "../hd_tokdec.h"
#include "../hd_tokenc.h"
#include "../hd_lz77.h"

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
    if (deflate(&zs, Z_FINISH) != Z_STREAM_END) hd_die("deflate");
    *outn = cap - zs.avail_out;
    deflateEnd(&zs);
    return out;
}

static double bench_full_deflate(const uint8_t *in, size_t n, int level, size_t *outn, int rep) {
    double best = 1e18;
    for (int r = 0; r < rep; r++) {
        size_t cn;
        double t0 = hd_now_ms();
        uint8_t *c = zlib_def_raw(in, n, level, &cn);
        double ms = hd_now_ms() - t0;
        free(c);
        if (ms < best) best = ms;
        *outn = cn;
    }
    return best;
}

int main(int argc, char **argv) {
    hd_common_init();
    if (argc < 3) {
        fprintf(stderr, "usage: hd_bench decompose <file>... [--level N] [--repeat N]\n");
        return 1;
    }
    if (strcmp(argv[1], "decompose")) {
        fprintf(stderr, "unknown subcommand\n");
        return 1;
    }
    int level = 6, rep = 3;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--level")) level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--repeat")) rep = atoi(argv[++i]);
    }

    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        size_t n;
        uint8_t *data = read_file(argv[i], &n);
        if (!data) { fprintf(stderr, "read fail %s\n", argv[i]); continue; }
        if (n < 4096) { free(data); continue; }

        size_t zc = 0;
        double t_zfull = bench_full_deflate(data, n, level, &zc, rep);

        double t_lz = 0, best_lz = 1e18;
        hd_tokstream ts;
        hd_ts_init(&ts);
        size_t lz_tok = 0;
        for (int r = 0; r < rep; r++) {
            hd_ts_reset(&ts);
            double t0 = hd_now_ms();
            hd_lz77_encode(data, n, level, &ts);
            double ms = hd_now_ms() - t0;
            if (ms < best_lz) { best_lz = ms; lz_tok = ts.ntok; }
        }
        t_lz = best_lz;

        size_t cn;
        uint8_t *comp = zlib_def_raw(data, n, level, &cn);
        hd_tokstream ts2;
        hd_ts_init(&ts2);
        hd_tokens_from_deflate(comp, cn, &ts2, NULL);
        double best_he = 1e18;
        size_t he_out = 0;
        for (int r = 0; r < rep; r++) {
            size_t cap = cn * 4 + 65536;
            uint8_t *out = malloc(cap);
            double t0 = hd_now_ms();
            int64_t got = hd_deflate_from_tokens(ts2.tok, ts2.ntok, 16384, out, cap);
            double ms = hd_now_ms() - t0;
            if (ms < best_he) { best_he = ms; he_out = got > 0 ? (size_t)got : 0; }
            free(out);
        }

        double best_hd = 1e18;
        for (int r = 0; r < rep; r++) {
            hd_tokstream ts3;
            hd_ts_init(&ts3);
            double t0 = hd_now_ms();
            hd_tokens_from_deflate(comp, cn, &ts3, NULL);
            double ms = hd_now_ms() - t0;
            if (ms < best_hd) best_hd = ms;
            hd_ts_free(&ts3);
        }

        double best_inf = 1e18;
        for (int r = 0; r < rep; r++) {
            z_stream zs;
            memset(&zs, 0, sizeof zs);
            inflateInit2(&zs, -15);
            static uint8_t win[1 << 20];
            double t0 = hd_now_ms();
            zs.next_in = comp;
            zs.avail_in = (uInt)cn;
            zs.next_out = win;
            zs.avail_out = sizeof win;
            int rc;
            while ((rc = inflate(&zs, Z_NO_FLUSH)) != Z_STREAM_END) {
                if (rc != Z_OK && rc != Z_BUF_ERROR) break;
                zs.next_out = win;
                zs.avail_out = sizeof win;
                if (rc == Z_BUF_ERROR && zs.avail_in == 0) break;
            }
            double ms = hd_now_ms() - t0;
            if (ms < best_inf) best_inf = ms;
            inflateEnd(&zs);
        }

        printf("{\"file\":\"%s\",\"bytes\":%llu,\"level\":%d,"
               "\"zlib_comp\":%zu,\"zlib_ratio\":%.4f,"
               "\"t_zfull_ms\":%.3f,\"t_lz77_ms\":%.3f,\"t_huffenc_ms\":%.3f,"
               "\"t_huffdec_ms\":%.3f,\"t_inflate_ms\":%.3f,"
               "\"lz_tokens\":%zu,\"reenc_size\":%zu,"
               "\"mbps_zfull\":%.1f,\"mbps_lz\":%.1f,\"mbps_huffenc\":%.1f,"
               "\"mbps_huffdec\":%.1f,\"mbps_inflate\":%.1f}\n",
               argv[i], (unsigned long long)n, level,
               zc, (double)zc / n,
               t_zfull, t_lz, best_he, best_hd, best_inf,
               lz_tok, he_out,
               n / 1024.0 / 1024.0 / (t_zfull / 1000.0),
               n / 1024.0 / 1024.0 / (t_lz / 1000.0),
               n / 1024.0 / 1024.0 / (best_he / 1000.0),
               n / 1024.0 / 1024.0 / (best_hd / 1000.0),
               n / 1024.0 / 1024.0 / (best_inf / 1000.0));
        fflush(stdout);
        free(data);
        free(comp);
        hd_ts_free(&ts);
        hd_ts_free(&ts2);
    }
    return 0;
}
