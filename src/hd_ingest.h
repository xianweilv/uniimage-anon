/* hd_ingest.h - Layer ingest pipeline: gzip layer -> recipe + content-addressed compressed objects */
#ifndef HD_INGEST_H
#define HD_INGEST_H

#include "hd_common.h"
#include "hd_store.h"
#include "hd_recipe.h"

typedef enum {
    HD_POLICY_HYBRID = 0,
    HD_POLICY_FILE,
    HD_POLICY_CHUNK8K,
    HD_POLICY_CHUNK16K,
    HD_POLICY_FIXBLOCK,
    HD_POLICY_CHUNK8KB,
    HD_POLICY_CHUNK16KB,
} hd_policy;

typedef struct {
    uint32_t sigma;
    uint32_t theta;
    uint32_t cdc_avg;
    int      level;
    hd_policy policy;
} hd_ingest_cfg;

typedef struct {
    uint64_t blob_comp_size;
    uint64_t tar_size;
    uint64_t blob_comp_size_re;
    char     diffid[65];
    char     blob_sha[65];
    int      n_new_objects;
    int      n_hit_objects;
    uint64_t sum_ref_comp;
    uint64_t new_obj_comp;
    uint64_t recipe_raw_size;
    double   t_total_ms, t_inflate_ms, t_route_ms, t_compress_ms, t_io_ms;
} hd_ingest_result;

void hd_ingest_cfg_default(hd_ingest_cfg *c);

int hd_ingest_layer(hd_store *st, const hd_ingest_cfg *cfg,
                    const uint8_t *blob, size_t bloblen,
                    hd_recipe *rec_out, hd_ingest_result *res,
                    char recipe_sha[65]);

#endif
