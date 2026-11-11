/* hd_recipe.h - Tar stream parsing + recipe (Meta/Pad/Ref segment) construction and serialization */
#ifndef HD_RECIPE_H
#define HD_RECIPE_H

#include "hd_common.h"

#define HD_SEG_META 0
#define HD_SEG_PAD  1
#define HD_SEG_REF  2

#define HD_MAX_OBJS_PER_LAYER (1 << 20)

typedef struct {
    uint8_t  type;
    uint32_t a, b, c;
} hd_seg;

typedef struct {
    char     sha[65];
    uint64_t raw_len;
    uint32_t crc;
} hd_objmeta;

typedef struct {
    hd_objmeta *objs;
    int        n_objs, cap_objs;
    hd_seg    *segs;
    int        n_segs, cap_segs;
    uint8_t  *bytes;
    size_t    nbytes, cap_bytes;
    uint64_t  tar_size;
    uint64_t  content_bytes;
    uint64_t  meta_bytes;
    uint64_t  pad_bytes;
    uint64_t  ref_bytes;
    int       n_files;
    int       n_files_inlined;
    int       n_files_batched;
    int       n_files_chunked;      /* >= theta CDC */
    int       n_files_whole;
} hd_recipe;

void hd_recipe_init(hd_recipe *r);
void hd_recipe_free(hd_recipe *r);
void hd_recipe_add_meta(hd_recipe *r, const uint8_t *p, size_t n);
void hd_recipe_add_pad(hd_recipe *r, uint32_t z);
int  hd_recipe_add_obj(hd_recipe *r, const char sha[65], uint64_t raw, uint32_t crc);
void hd_recipe_add_ref(hd_recipe *r, int obj_idx, uint32_t off, uint32_t len);

void hd_recipe_serialize(const hd_recipe *r, hd_buf *out);

int hd_recipe_deserialize(hd_recipe *r, const uint8_t *payload, size_t len);

uint64_t hd_tar_parse_size(const uint8_t *hdr);
int      hd_tar_header_ok(const uint8_t *blk);    /* checksumverification */

#endif
