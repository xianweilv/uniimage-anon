/* hd_tokenc.h - LZ77 token stream -> Deflate dynamic Huffman blocks (Huffman encoding only) */
#ifndef HD_TOKENC_H
#define HD_TOKENC_H

#include "hd_common.h"
#include "hd_bitio.h"
#include "hd_huffman.h"

int64_t hd_deflate_from_tokens(const hd_token *tok, size_t ntok,
                               uint32_t beta_tokens,
                               uint8_t *out, size_t outcap);

int hd_encode_block(hd_bw *w, const hd_token *tok, size_t n, int final);
void hd_tokenc_init(void);

int64_t hd_gzip_from_tokens(const hd_token *tok, size_t ntok,
                            uint32_t beta_tokens,
                            uint32_t crc32_val, uint32_t isize,
                            uint8_t *out, size_t outcap);

int hd_dyn_header_bits(const uint8_t *ll_lens, int hlit,
                       const uint8_t *dist_lens, int hdist);

#endif
