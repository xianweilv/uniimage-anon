/* hd_store.h - Content-addressed object and recipe store */
#ifndef HD_STORE_H
#define HD_STORE_H

#include "hd_common.h"
#include <zlib.h>

#define SHA_HEX 64

typedef struct {
    char root[1024];
    uint64_t n_obj_total;
    uint64_t n_obj_unique;
    uint64_t obj_bytes_raw;
    uint64_t obj_bytes_comp;
    uint64_t n_recipe_total;
    uint64_t n_recipe_unique;
    uint64_t recipe_bytes;
    uint64_t recipe_bytes_raw;
    double t_inflate_ms, t_tar_ms, t_hash_ms, t_compress_ms, t_io_ms;
    /* meta db file */
    int      metafd;
    void    *layerf;            /* FILE* */
    int write_meta;
} hd_store;

void hd_store_open(hd_store *s, const char *root, int write_meta);
void hd_store_close(hd_store *s);

int hd_store_put_object(hd_store *s, const uint8_t *raw, size_t rawlen,
                        const uint8_t *comp, size_t complen,
                        char sha_hex[SHA_HEX + 1], uint32_t *crc_out);

uint8_t *hd_store_get_object(hd_store *s, const char *sha_hex, size_t *len);

int hd_store_has_object(hd_store *s, const char *sha_hex);

int64_t hd_store_object_comp_size(hd_store *s, const char *sha_hex);

int hd_store_put_recipe(hd_store *s, const uint8_t *payload, size_t len,
                        char sha_hex[SHA_HEX + 1]);
uint8_t *hd_store_get_recipe(hd_store *s, const char *sha_hex, size_t *len);

void hd_store_log_layer(hd_store *s, const char *json_line);

struct hd_blkinfo;
int hd_store_put_blockmap(hd_store *s, const char *sha_hex,
                          const struct hd_blkinfo *blocks, int n);
struct hd_blkinfo *hd_store_get_blockmap(hd_store *s, const char *sha_hex, int *n);

void hd_sha256_hex(const uint8_t *data, size_t len, char out[SHA_HEX + 1]);
void hd_sha256_init_oneshot(void);

#endif
