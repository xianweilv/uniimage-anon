/* hd_splice.h - B6: bit-level block splicing ablation baseline */
#ifndef HD_SPLICE_H
#define HD_SPLICE_H

#include "hd_common.h"
#include "hd_store.h"
#include "hd_recipe.h"

typedef struct {
    uint64_t out_comp_size;
    uint64_t tar_size;
    uint32_t crc;
    double   t_total_ms;
    double   t_bitcopy_ms;
    double   t_misc_ms;
} hd_splice_result;

int hd_splice_layer(hd_store *st, const hd_recipe *rec,
                    hd_buf *out_buf, hd_splice_result *res);

#endif
