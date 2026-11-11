/* hd_huffman.h - Canonical Huffman code construction, fast decode tables, length-limited codes */
#ifndef HD_HUFFMAN_H
#define HD_HUFFMAN_H

#include "hd_common.h"

typedef struct {
    uint32_t *tab;
    int       maxlen;
    int       nbits;        /* = maxlen */
    uint32_t  mask;
} hd_huffdec;

int hd_huffdec_build2(hd_huffdec *h, const uint8_t *lens, int n, int allow_incomplete);
static inline int hd_huffdec_build(hd_huffdec *h, const uint8_t *lens, int n) {
    return hd_huffdec_build2(h, lens, n, 0);
}

static inline void hd_huffdec_free(hd_huffdec *h) {
    free(h->tab);
    h->tab = NULL;
}

static inline int hd_huff_decode(const hd_huffdec *h, uint32_t bits) {
    return (int)(h->tab[bits & h->mask] >> 16);
}
static inline int hd_huff_decodelen(const hd_huffdec *h, uint32_t bits) {
    return (int)(h->tab[bits & h->mask] & 0xFF);
}

typedef struct {
    uint32_t code[288];
    uint8_t  len[288];
    int      nsyms;
} hd_huffenc;

int hd_huffenc_build(hd_huffenc *e, const uint8_t *lens, int n);

int hd_huff_lengths(const uint32_t *freq, int n, int maxbits, uint8_t *lens);

#endif
