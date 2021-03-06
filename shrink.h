/* Project:    Shrink, an LSZZ and RLE compression library
 * Repository: <https://github.com/howerj/shrink>
 * Maintainer: Richard James Howe
 * License:    The Unlicense
 * Email:      howe.r.j.89@gmail.com */
#ifndef SHRINK_H
#define SHRINK_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#ifndef SHRINK_API
#define SHRINK_API /* Used to apply attributes to exported functions */
#endif

typedef struct {
	int (*get)(void *in);          /* return negative on error, a byte (0-255) otherwise */
	int (*put)(int ch, void *out); /* return ch on no error */
	void *in, *out;                /* passed to 'get' and 'put' respectively */
	size_t read, wrote;            /* read only, bytes 'get' and 'put' respectively */
	unsigned char *buffer;         /* buffer for storage */
	size_t buffer_length;          /* length of buffer */
	int custom;                    /* initialize buffer contents on each call? */
} shrink_t; /**< I/O abstraction, use to redirect to wherever you want... */

enum { CODEC_RLE, CODEC_LZSS };

/* negative on error, zero on success */
SHRINK_API int shrink(shrink_t *io, int codec, int encode);
SHRINK_API int shrink_buffer(int codec, int encode, const char *in, size_t inlength, char *out, size_t *outlength);
SHRINK_API int shrink_tests(void);
SHRINK_API int shrink_version(unsigned long *version); /* version in x.y.z, z = LSB, MSB = options */

#ifdef __cplusplus
}
#endif
#endif
