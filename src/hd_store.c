#include "hd_store.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/sha.h>

void hd_sha256_hex(const uint8_t *data, size_t len, char out[SHA_HEX + 1]) {
    uint8_t d[32];
    SHA256(data, len, d);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hex[d[i] >> 4];
        out[i * 2 + 1] = hex[d[i] & 15];
    }
    out[SHA_HEX] = 0;
}

static void path_of(char *dst, size_t cap, const char *root,
                    const char *sub, const char *sha) {
    snprintf(dst, cap, "%s/%s/%.2s/%s", root, sub, sha, sha);
}

static void mkdirs(const char *path) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void mkdirs_for_file(const char *path) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *slash = strrchr(tmp, '/');
    if (slash == tmp) return;
    if (slash) *slash = 0;
    mkdirs(tmp);
}

void hd_store_open(hd_store *s, const char *root, int write_meta) {
    memset(s, 0, sizeof *s);
    snprintf(s->root, sizeof s->root, "%s", root);
    char p[1200];
    snprintf(p, sizeof p, "%s/objects", root); mkdirs(p);
    snprintf(p, sizeof p, "%s/recipes", root); mkdirs(p);
    s->write_meta = write_meta;
    s->metafd = -1;
    if (write_meta) {
        snprintf(p, sizeof p, "%s/objmeta.jsonl", root);
        s->metafd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
        snprintf(p, sizeof p, "%s/layers.jsonl", root);
        s->layerf = fopen(p, "a");
    }
}

void hd_store_close(hd_store *s) {
    if (s->metafd >= 0) close(s->metafd);
    if (s->layerf) fclose((FILE *)s->layerf);
    s->metafd = -1;
    s->layerf = NULL;
}

int hd_store_put_object(hd_store *s, const uint8_t *raw, size_t rawlen,
                        const uint8_t *comp, size_t complen,
                        char sha_hex[SHA_HEX + 1], uint32_t *crc_out) {
    hd_sha256_hex(raw, rawlen, sha_hex);
    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, raw, (uInt)rawlen);
    if (crc_out) *crc_out = crc;

    s->n_obj_total++;
    char p[1200];
    path_of(p, sizeof p, s->root, "objects", sha_hex);
    struct stat st;
    if (stat(p, &st) == 0) return 0;

    mkdirs_for_file(p);
    FILE *f = fopen(p, "wb");
    if (!f) hd_die("cannot create %s", p);
    fwrite(comp, 1, complen, f);
    fclose(f);
    s->n_obj_unique++;
    s->obj_bytes_raw += rawlen;
    s->obj_bytes_comp += complen;
    if (s->metafd >= 0) {
        char line[256];
        int n = snprintf(line, sizeof line,
                         "{\"sha\":\"%s\",\"raw\":%zu,\"comp\":%zu,\"crc\":%u}\n",
                         sha_hex, rawlen, complen, crc);
        ssize_t rc = write(s->metafd, line, (size_t)n);
        (void)rc;
    }
    return 1;
}


int64_t hd_store_object_comp_size(hd_store *s, const char *sha_hex) {
    char p[1200];
    path_of(p, sizeof p, s->root, "objects", sha_hex);
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

uint8_t *hd_store_get_object(hd_store *s, const char *sha_hex, size_t *len) {
    char p[1200];
    path_of(p, sizeof p, s->root, "objects", sha_hex);
    FILE *f = fopen(p, "rb");
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

int hd_store_has_object(hd_store *s, const char *sha_hex) {
    char p[1200];
    path_of(p, sizeof p, s->root, "objects", sha_hex);
    struct stat st;
    return stat(p, &st) == 0;
}

int hd_store_put_recipe(hd_store *s, const uint8_t *payload, size_t len,
                        char sha_hex[SHA_HEX + 1]) {
    hd_sha256_hex(payload, len, sha_hex);
    s->n_recipe_total++;
    char p[1200];
    path_of(p, sizeof p, s->root, "recipes", sha_hex);
    struct stat st;
    if (stat(p, &st) == 0) return 0;

    /* gzip compressionstorage（level 9） */
    uLongf cap = compressBound(len);
    uint8_t *gz = malloc(cap);
    if (compress2(gz, &cap, payload, len, 9) != Z_OK) hd_die("recipe gzip");
    mkdirs_for_file(p);
    FILE *f = fopen(p, "wb");
    if (!f) hd_die("cannot create %s", p);
    fwrite(gz, 1, cap, f);
    fclose(f);
    free(gz);
    s->n_recipe_unique++;
    s->recipe_bytes += cap;
    s->recipe_bytes_raw += len;
    return 1;
}

uint8_t *hd_store_get_recipe(hd_store *s, const char *sha_hex, size_t *len) {
    char p[1200];
    path_of(p, sizeof p, s->root, "recipes", sha_hex);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *gz = malloc(sz ? sz : 1);
    if (sz > 0 && fread(gz, 1, sz, f) != (size_t)sz) { fclose(f); free(gz); return NULL; }
    fclose(f);

    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, 15 + 32) != Z_OK) { free(gz); return NULL; }
    size_t cap2 = sz * 4 + 65536;
    uint8_t *out = malloc(cap2);
    zs.next_in = gz;
    zs.avail_in = (uInt)sz;
    zs.next_out = out;
    zs.avail_out = (uInt)cap2;
    int rc = inflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) {
        while (rc == Z_OK || rc == Z_BUF_ERROR) {
            size_t have = cap2 - zs.avail_out;
            cap2 *= 2;
            out = realloc(out, cap2);
            zs.next_out = out + have;
            zs.avail_out = (uInt)(cap2 - have);
            rc = inflate(&zs, Z_FINISH);
            if (rc == Z_STREAM_END) break;
            if (rc != Z_OK && rc != Z_BUF_ERROR) { inflateEnd(&zs); free(out); free(gz); return NULL; }
            if (rc == Z_BUF_ERROR && zs.avail_in == 0) { inflateEnd(&zs); free(out); free(gz); return NULL; }
        }
    }
    *len = cap2 - zs.avail_out;
    inflateEnd(&zs);
    free(gz);
    return out;
}

void hd_store_log_layer(hd_store *s, const char *json_line) {
    if (s->layerf) fprintf((FILE *)s->layerf, "%s\n", json_line);
}

int hd_store_put_blockmap(hd_store *s, const char *sha_hex,
                          const struct hd_blkinfo *blocks, int n) {
    char p[1200];
    snprintf(p, sizeof p, "%s/objects/%.2s/%s.blk", s->root, sha_hex, sha_hex);
    mkdirs_for_file(p);
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    uint32_t nn = (uint32_t)n;
    fwrite(&nn, 4, 1, f);
    for (int i = 0; i < n; i++) {
        fwrite(&blocks[i].start_bit, 8, 1, f);
        fwrite(&blocks[i].end_bit, 8, 1, f);
        fwrite(&blocks[i].btype, 1, 1, f);
    }
    fclose(f);
    return 0;
}

struct hd_blkinfo *hd_store_get_blockmap(hd_store *s, const char *sha_hex, int *n) {
    char p[1200];
    snprintf(p, sizeof p, "%s/objects/%.2s/%s.blk", s->root, sha_hex, sha_hex);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    uint32_t nn = 0;
    if (fread(&nn, 4, 1, f) != 1) { fclose(f); return NULL; }
    struct hd_blkinfo *b = malloc(nn * sizeof(struct hd_blkinfo));
    for (uint32_t i = 0; i < nn; i++) {
        if (fread(&b[i].start_bit, 8, 1, f) != 1 ||
            fread(&b[i].end_bit, 8, 1, f) != 1 ||
            fread(&b[i].btype, 1, 1, f) != 1) {
            fclose(f); free(b); return NULL;
        }
    }
    fclose(f);
    *n = (int)nn;
    return b;
}
