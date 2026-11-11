/* hd_synth.h - Huffman-domain layer synthesis engine */
#ifndef HD_SYNTH_H
#define HD_SYNTH_H

#include "hd_common.h"
#include "hd_store.h"
#include "hd_recipe.h"

typedef struct {
    uint32_t beta_tokens;    /* outputblocksize（tokens/block），default 16384 */
    int threads;
} hd_synth_cfg;

typedef struct {
    uint64_t out_comp_size;
    uint64_t tar_size;
    uint32_t crc;
    double   t_decode_ms;
    double   t_encode_ms;
    double   t_total_ms;
    double   t_misc_ms;
    double   t_load_ms;
    double   t_loop_ms;
    double   t_reserve_ms;
    double   t_pass1_ms;
    uint64_t n_tokens;
    uint64_t tokens_from_obj;
    uint64_t tokens_literalized;
} hd_synth_result;

void hd_synth_cfg_default(hd_synth_cfg *c);
extern int hd_synth_refsonly_flag;

int hd_synth_layer(hd_store *st, const hd_synth_cfg *cfg,
                   const hd_recipe *rec,
                   hd_buf *out_buf, hd_synth_result *res);

 * return NULL failure。 */
char *hd_synth_verify(hd_store *st, const hd_recipe *rec,
                      const uint8_t *gz, size_t gzlen);

#endif
