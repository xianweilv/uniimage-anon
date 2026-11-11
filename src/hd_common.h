/* hd_common.h - Common definitions for the UniImage engine */
#ifndef HD_COMMON_H
#define HD_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    uint32_t v;
    uint32_t d;
} hd_token;

#define HD_MAX_BITS       15
#define HD_LITLEN_SYMS    288
#define HD_NLITLEN        286
#define HD_DIST_SYMS      30
#define HD_CLEN_SYMS      19
#define HD_EOB            256

static const uint8_t HD_CLEN_ORDER[HD_CLEN_SYMS] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static const uint16_t HD_LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t HD_LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const uint16_t HD_DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const uint8_t HD_DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

extern uint16_t HD_LEN2SYM[259];

extern uint8_t HD_FIXED_LITLEN_LEN[HD_LITLEN_SYMS];
extern uint8_t HD_FIXED_DIST_LEN[HD_DIST_SYMS];

void hd_common_init(void);

static inline double hd_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

typedef struct {
    uint8_t *data;
    size_t   len, cap;
} hd_buf;

typedef struct hd_blkinfo {
    uint64_t start_bit;
    uint64_t end_bit;
    uint8_t  btype;       /* 0=stored 1=fixed 2=dynamic */
} hd_blkinfo;

void hd_buf_init(hd_buf *b, size_t cap);
void hd_buf_free(hd_buf *b);
void hd_buf_reserve(hd_buf *b, size_t need);
static inline void hd_buf_push(hd_buf *b, const void *p, size_t n) {
    hd_buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
static inline void hd_buf_push_byte(hd_buf *b, uint8_t c) {
    hd_buf_reserve(b, 1);
    b->data[b->len++] = c;
}

void hd_die(const char *fmt, ...);

#endif
