#ifndef SHRINK_H
#define SHRINK_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define SHRINK_PROJECT   "Shrink, a small compression library"
#define SHRINK_RPOSITORY "https://github.com/howerj/shrink"
#define SHRINK_AUTHOR    "Richard James Howe"
#define SHRINK_LICENSE   "The Unlicense / Public Domain"
#define SHRINK_EMAIL     "howe.r.j.89@gmail.com"

#ifndef SHRINK_API
#define SHRINK_API /* Used to apply attributes to exported functions */
#endif

typedef struct {
	int (*get)(void *in);          /* return negative on error, a byte (0-255) otherwise */
	int (*put)(int ch, void *out); /* return ch on no error */
	void *in, *out;                /* passed to 'get' and 'put' respectively */
	size_t read, wrote;            /* read only, bytes 'get' and 'put' respectively */
} shrink_t; /**< I/O abstraction, use to redirect to wherever you want... */

enum { CODEC_RLE, CODEC_LZSS, CODEC_ELIAS, CODEC_MTF, CODEC_LZP, };

/* negative on error, zero on success */
SHRINK_API int shrink(shrink_t *io, int codec, int encode);
SHRINK_API int shrink_buffer(int codec, int encode, const char *in, size_t inlength, char *out, size_t *outlength);
SHRINK_API int shrink_tests(void);
SHRINK_API int shrink_version(unsigned long *version); /* version in x.y.z, z = LSB, MSB = options */

#ifdef __cplusplus
}
#endif
#endif
