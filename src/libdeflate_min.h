/* libdeflate_min.h - Minimal public API declarations for libdeflate (subset used by UniImage) */
#ifndef LIBDEFLATE_MIN_H
#define LIBDEFLATE_MIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBDEFLATE_VERSION_MAJOR 1
#define LIBDEFLATE_VERSION_MINOR 19
#define LIBDEFLATE_VERSION_STRING "1.19"

struct libdeflate_compressor;
struct libdeflate_decompressor;

/* compression */
struct libdeflate_compressor *
libdeflate_alloc_compressor(int compression_level);

void
libdeflate_free_compressor(struct libdeflate_compressor *compressor);

size_t
libdeflate_deflate_compress(struct libdeflate_compressor *compressor,
                            const void *in, size_t in_nbytes,
                            void *out, size_t out_nbytes_avail);

size_t
libdeflate_gzip_compress(struct libdeflate_compressor *compressor,
                         const void *in, size_t in_nbytes,
                         void *out, size_t out_nbytes_avail);

/* decompression */
struct libdeflate_decompressor *
libdeflate_alloc_decompressor(void);

void
libdeflate_free_decompressor(struct libdeflate_decompressor *decompressor);

enum libdeflate_result {
    LIBDEFLATE_SUCCESS = 0,
    LIBDEFLATE_BAD_DATA = 1,
    LIBDEFLATE_SHORT_OUTPUT = 2,
    LIBDEFLATE_INSUFFICIENT_SPACE = 3,
};

enum libdeflate_result
libdeflate_deflate_decompress(struct libdeflate_decompressor *decompressor,
                              const void *in, size_t in_nbytes,
                              void *out, size_t out_nbytes_avail,
                              size_t *actual_out_nbytes_ret);

enum libdeflate_result
libdeflate_gzip_decompress(struct libdeflate_decompressor *decompressor,
                           const void *in, size_t in_nbytes,
                           void *out, size_t out_nbytes_avail,
                           size_t *actual_out_nbytes_ret);

/* CRC/Adler */
uint32_t
libdeflate_crc32(uint32_t crc, const void *buffer, size_t len);

uint32_t
libdeflate_adler32(uint32_t adler, const void *buffer, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LIBDEFLATE_MIN_H */
