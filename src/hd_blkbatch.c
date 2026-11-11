 *
 */
#ifndef HD_BLKBATCH_H
#define HD_BLKBATCH_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>

#define BLK_IDX_BITS 23
#define BLK_IDX_SIZE (1u << BLK_IDX_BITS)
#define BLK_IDX_MASK (BLK_IDX_SIZE - 1)

typedef struct {
    uint8_t  blk_sha[32];
    char     grp_sha[65];
    uint64_t grp_raw_len;
    uint32_t grp_crc;
    uint32_t offset;
    uint32_t len;
} blk_idx_entry_t;

typedef struct {
    blk_idx_entry_t *slots;
    size_t count;
    size_t collisions;
} blk_index_t;

static void blk_index_init(blk_index_t *bi) {
    bi->slots = calloc(BLK_IDX_SIZE, sizeof(blk_idx_entry_t));
    bi->count = 0;
    bi->collisions = 0;
    if (!bi->slots) {
        fprintf(stderr, "FATAL: blk_index calloc %zu bytes\n",
                (size_t)BLK_IDX_SIZE * sizeof(blk_idx_entry_t));
        exit(1);
    }
}

static void blk_index_free(blk_index_t *bi) {
    free(bi->slots);
    bi->slots = NULL;
}

static uint32_t blk_index_hash(const uint8_t *sha) {
    uint32_t h;
    memcpy(&h, sha, 4);
    return (h * 2654435761u) & BLK_IDX_MASK;
}

static blk_idx_entry_t *blk_index_find(blk_index_t *bi,
                                       const uint8_t *sha) {
    uint32_t h = blk_index_hash(sha);
    while (bi->slots[h].len != 0) {
        if (memcmp(bi->slots[h].blk_sha, sha, 32) == 0)
            return &bi->slots[h];
        h = (h + 1) & BLK_IDX_MASK;
        if (h == blk_index_hash(sha)) return NULL;
    }
    return NULL;
}

static void blk_index_add(blk_index_t *bi, const uint8_t *blk_sha,
                          const char *grp_sha, uint64_t grp_raw_len,
                          uint32_t grp_crc, uint32_t offset, uint32_t len) {
    uint32_t h = blk_index_hash(blk_sha);
    while (bi->slots[h].len != 0) {
        if (memcmp(bi->slots[h].blk_sha, blk_sha, 32) == 0)
            return;
        bi->collisions++;
        h = (h + 1) & BLK_IDX_MASK;
    }
    memcpy(bi->slots[h].blk_sha, blk_sha, 32);
    strncpy(bi->slots[h].grp_sha, grp_sha, 64);
    bi->slots[h].grp_sha[64] = 0;
    bi->slots[h].grp_raw_len = grp_raw_len;
    bi->slots[h].grp_crc = grp_crc;
    bi->slots[h].offset = offset;
    bi->slots[h].len = len;
    bi->count++;
}

#define BLK_BATCH_MAX_BYTES (512 * 1024)
#define BLK_BATCH_MAX_BLOCKS 4096

typedef struct {
    uint8_t  buf[BLK_BATCH_MAX_BYTES];
    uint32_t len;
    struct {
        uint8_t  sha[32];
        uint32_t offset;
        uint32_t blen;
        int      seg_idx;
    } blocks[BLK_BATCH_MAX_BLOCKS];
    int n_blocks;
} blk_batch_t;

static void blk_batch_reset(blk_batch_t *bb) {
    bb->len = 0;
    bb->n_blocks = 0;
}

static int blk_batch_can_fit(blk_batch_t *bb, uint32_t blen,
                             uint32_t theta) {
    return bb->n_blocks < BLK_BATCH_MAX_BLOCKS &&
           (bb->len == 0 || bb->len + blen <= theta);
}

static int blk_batch_append(blk_batch_t *bb, const uint8_t *blk,
                            uint32_t blen, const uint8_t *sha) {
    memcpy(bb->buf + bb->len, blk, blen);
    memcpy(bb->blocks[bb->n_blocks].sha, sha, 32);
    bb->blocks[bb->n_blocks].offset = bb->len;
    bb->blocks[bb->n_blocks].blen = blen;
    bb->blocks[bb->n_blocks].seg_idx = -1;
    bb->n_blocks++;
    bb->len += blen;
    return bb->n_blocks - 1;
}

#endif /* HD_BLKBATCH_H */
