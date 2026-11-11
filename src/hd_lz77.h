/* hd_lz77.h - Standalone LZ77 matcher (hash-chain + lazy matching, zlib L6 parameters) */
#ifndef HD_LZ77_H
#define HD_LZ77_H

#include "hd_common.h"
#include "hd_tokdec.h"

 * return 0 success。 */
int hd_lz77_encode(const uint8_t *in, size_t inlen, int level, hd_tokstream *ts);

#endif
