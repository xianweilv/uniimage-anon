 * Usage: hd_ingest <store> <policy> [--sigma N] [--theta N] [--level N] <blob>...
 *   policy: hybrid | file | chunk8k | chunk16k | fixblock
 */
#include <string.h>
#include <sys/stat.h>
#include "../hd_common.h"
#include "../hd_store.h"
#include "../hd_ingest.h"

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

int main(int argc, char **argv) {
    hd_common_init();
    if (argc < 3) {
        fprintf(stderr, "usage: hd_ingest <store> <policy> [opts] <blob>...\n");
        return 1;
    }
    hd_store st;
    hd_store_open(&st, argv[1], 1);

    hd_ingest_cfg cfg;
    hd_ingest_cfg_default(&cfg);
    const char *pol = argv[2];
    if (!strcmp(pol, "hybrid")) cfg.policy = HD_POLICY_HYBRID;
    else if (!strcmp(pol, "file")) cfg.policy = HD_POLICY_FILE;
    else if (!strcmp(pol, "chunk8k")) cfg.policy = HD_POLICY_CHUNK8K;
    else if (!strcmp(pol, "chunk16k")) cfg.policy = HD_POLICY_CHUNK16K;
    else if (!strcmp(pol, "fixblock")) cfg.policy = HD_POLICY_FIXBLOCK;
    else if (!strcmp(pol, "chunk8kb")) cfg.policy = HD_POLICY_CHUNK8KB;
    else if (!strcmp(pol, "chunk16kb")) cfg.policy = HD_POLICY_CHUNK16KB;
    else { fprintf(stderr, "bad policy %s\n", pol); return 1; }

    int i = 3;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "--sigma")) { cfg.sigma = (uint32_t)atoll(argv[i+1]); i += 2; }
        else if (!strcmp(argv[i], "--theta")) { cfg.theta = (uint32_t)atoll(argv[i+1]); i += 2; }
        else if (!strcmp(argv[i], "--avg")) { cfg.cdc_avg = (uint32_t)atoll(argv[i+1]); i += 2; }
        else if (!strcmp(argv[i], "--level")) { cfg.level = atoi(argv[i+1]); i += 2; }
        else { fprintf(stderr, "bad opt %s\n", argv[i]); return 1; }
    }
    if (cfg.cdc_avg == 64 * 1024) cfg.cdc_avg = cfg.theta;

    for (; i < argc; i++) {
        size_t blen;
        uint8_t *blob = read_file(argv[i], &blen);
        if (!blob) { fprintf(stderr, "read fail %s\n", argv[i]); return 1; }
        hd_recipe rec;
        hd_ingest_result res;
        char rsha[65];
        double t0 = hd_now_ms();
        int rc = hd_ingest_layer(&st, &cfg, blob, blen, &rec, &res, rsha);
        double wall = hd_now_ms() - t0;
        if (rc) {
            printf("{\"file\":\"%s\",\"error\":%d}\n", argv[i], rc);
            free(blob);
            continue;
        }
        int64_t recsz = -1;
        char rp[1200];
        snprintf(rp, sizeof rp, "%s/recipes/%.2s/%s", st.root, rsha, rsha);
        struct stat sst;
        if (stat(rp, &sst) == 0) recsz = sst.st_size;

        printf("{\"file\":\"%s\",\"blob_sha\":\"%s\",\"diffid\":\"%s\","
               "\"comp\":%llu,\"tar\":%llu,\"new_obj\":%d,\"hit_obj\":%d,"
               "\"sum_ref_comp\":%llu,\"new_obj_comp\":%llu,"
               "\"nfiles\":%d,\"inlined\":%d,\"batched\":%d,\"chunked\":%d,"
               "\"nseg\":%d,\"nobj\":%d,\"meta_bytes\":%llu,\"pad_bytes\":%llu,"
               "\"content_bytes\":%llu,"
               "\"recipe\":\"%s\",\"recipe_raw\":%llu,\"recipe_gz\":%lld,"
               "\"t_wall_ms\":%.3f,\"t_inflate_ms\":%.3f,\"t_route_ms\":%.3f,"
               "\"t_compress_ms\":%.3f,"
               "\"store_obj_unique\":%llu,\"store_obj_raw\":%llu,"
               "\"store_obj_comp\":%llu,\"store_recipes\":%llu,\"store_recipe_bytes\":%llu}\n",
               argv[i], res.blob_sha, res.diffid,
               (unsigned long long)res.blob_comp_size,
               (unsigned long long)res.tar_size,
               res.n_new_objects, res.n_hit_objects,
               (unsigned long long)res.sum_ref_comp,
               (unsigned long long)res.new_obj_comp,
               rec.n_files, rec.n_files_inlined, rec.n_files_batched, rec.n_files_chunked,
               rec.n_segs, rec.n_objs,
               (unsigned long long)rec.meta_bytes, (unsigned long long)rec.pad_bytes,
               (unsigned long long)rec.content_bytes,
               rsha, (unsigned long long)res.recipe_raw_size, recsz,
               wall, res.t_inflate_ms, res.t_route_ms, res.t_compress_ms,
               (unsigned long long)st.n_obj_unique,
               (unsigned long long)st.obj_bytes_raw,
               (unsigned long long)st.obj_bytes_comp,
               (unsigned long long)st.n_recipe_unique,
               (unsigned long long)st.recipe_bytes);
        fflush(stdout);
        hd_recipe_free(&rec);
        free(blob);
    }
    hd_store_close(&st);
    return 0;
}
