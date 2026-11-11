#include "hd_common.h"
#include <stdarg.h>

uint16_t HD_LEN2SYM[259];
uint8_t HD_FIXED_LITLEN_LEN[HD_LITLEN_SYMS];
uint8_t HD_FIXED_DIST_LEN[HD_DIST_SYMS];

void hd_common_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;

    for (int sym = 0; sym < 29; sym++) {
        uint16_t lo = HD_LEN_BASE[sym];
        uint16_t hi = (sym < 28) ? HD_LEN_BASE[sym + 1] - 1 : 258;
        for (int l = lo; l <= hi; l++)
            HD_LEN2SYM[l] = (uint16_t)(257 + sym);
    }

    for (int i = 0; i < 144; i++) HD_FIXED_LITLEN_LEN[i] = 8;
    for (int i = 144; i < 256; i++) HD_FIXED_LITLEN_LEN[i] = 9;
    for (int i = 256; i < 280; i++) HD_FIXED_LITLEN_LEN[i] = 7;
    for (int i = 280; i < HD_LITLEN_SYMS; i++) HD_FIXED_LITLEN_LEN[i] = 8;
    for (int i = 0; i < HD_DIST_SYMS; i++) HD_FIXED_DIST_LEN[i] = 5;
}

void hd_buf_init(hd_buf *b, size_t cap) {
    if (cap < 256) cap = 256;
    b->data = malloc(cap);
    if (!b->data) hd_die("OOM in hd_buf_init(%zu)", cap);
    b->len = 0;
    b->cap = cap;
}

void hd_buf_free(hd_buf *b) {
    free(b->data);
    b->data = NULL; b->len = b->cap = 0;
}

void hd_buf_reserve(hd_buf *b, size_t need) {
    if (b->len + need <= b->cap) return;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + need) ncap *= 2;
    uint8_t *nd = realloc(b->data, ncap);
    if (!nd) hd_die("OOM in hd_buf_reserve(%zu)", ncap);
    b->data = nd;
    b->cap = ncap;
}

void hd_die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}
