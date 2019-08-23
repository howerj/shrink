/* Project:    Shrink, an LSZZ and RLE compression library
 * Repository: <https://github.com/howerj/shrink>
 * Maintainer: Richard James Howe
 * License:    Public Domain
 * Email:      howe.r.j.89@gmail.com */
#ifndef SHRINK_H
#define SHRINK_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#ifndef COMPRESS_API 
#define COMPRESS_API /* Used to apply attributes to exported functions */
#endif

typedef struct {
	int (*get)(void *in);          /* return negative on error, a byte otherwise */
	int (*put)(int ch, void *out); /* return ch on no error */
	void *in, *out;                /* passed to 'get' and 'put' respectively */
	size_t read, wrote;            /* read only, bytes 'get' and 'put' respectively */
} shrink_t;

COMPRESS_API int shrink_lzss_decode(shrink_t *io);
COMPRESS_API int shrink_lzss_encode(shrink_t *io);
COMPRESS_API int shrink_lzss_decode_buffer(const char *in, size_t inlength, char *out, size_t *outlength);
COMPRESS_API int shrink_lzss_encode_buffer(const char *in, size_t inlength, char *out, size_t *outlength);

COMPRESS_API int shrink_rle_decode(shrink_t *io);
COMPRESS_API int shrink_rle_encode(shrink_t *io);
COMPRESS_API int shrink_rle_decode_buffer(const char *in, size_t inlength, char *out, size_t *outlength);
COMPRESS_API int shrink_rle_encode_buffer(const char *in, size_t inlength, char *out, size_t *outlength);

COMPRESS_API int shrink_tests(void);

#ifdef __cplusplus
}
#endif
#endif
