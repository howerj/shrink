/* Project:    Shrink, an LSZZ and RLE compression library
 * Repository: <https://github.com/howerj/shrink>
 * Maintainer: Richard James Howe
 * License:    The Unlicense
 * Email:      howe.r.j.89@gmail.com 
 *
 * The LZSS CODEC is originally from Haruhiko Okumura, also placed
 * in the public domain, and has been modified since.
 * See <https://oku.edu.mie-u.ac.jp/~okumura/compression/lzss.c>.
 *
 * View the projects 'readme.md' file for more information.
 *
 * The only major feature missing from this library is the ability to yield
 * within each of the CODECS, which would allow this library to both be used
 * in a non-blocking fashion, but also so that the various CODECS can be
 * chained together. Another minor missing feature is runtime configurable
 * LZSS parameters. 
 *
 * Experiments should be done with setting the initial LZSS dictionary
 * contents to a custom dictionary. This could allow for memory savings,
 * especially for small strings of a known distribution (such as many
 * small JSON strings). This does not actually have to be done within
 * the library, but could be done by manipulating the I/O callbacks to
 * take their input from the custom dictionaries. The ngrams package
 * <https://github.com/howerj/ngram> can be used to come up with a list
 * of common phrases given a corpus of representative data. 
 *
 * Another missing feature is control over the location of the lookahead
 * buffer, this could have been passed in via the "shrink_t" structure,
 * it still can be whilst maintaining API compatibility.
 *
 * In place compression and decompression should also be looked at. As
 * well as small string compression. */

/* TODO: Describe header only library */

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
} shrink_t; /**< I/O abstraction, use to redirect to wherever you want... */

enum { CODEC_RLE, CODEC_LZSS, };

#ifndef SHRINK_IMPLEMENTATION

/* TODO: Add buffer to API */
/* negative on error, zero on success */
extern int shrink(shrink_t *io, int codec, int encode);
extern int shrink_buffer(int codec, int encode, const char *in, size_t inlength, char *out, size_t *outlength);
/* TODO: Remove from API */
extern int shrink_version(unsigned long *version); /* version in x.y.z, z = LSB, MSB = options */

#ifdef SHRINK_UNIT_TESTS
extern int shrink_tests(void);
#endif

#else

#include <assert.h>
#include <string.h> /* memset, memmove, memcmp, memchr, strlen */
#include <stdint.h>

/* TODO: Assert = to build version */
#ifndef SHRINK_VERSION
#define SHRINK_VERSION (0x000000ul) /* all zeros indicates and error */
#endif

#ifdef NDEBUG
#define SHRINK_DEBUGGING (0)
#else
#define SHRINK_DEBUGGING (1)
#endif

#ifdef SHRINK_USE_STATIC /* large internal buffers are statically allocated */
#define SHRINK_STATIC_ON (1)
#define SHRINK_STATIC static
#else  /* large internal buffers are allocated on stack */
#define SHRINK_STATIC_ON (0)
#define SHRINK_STATIC
#endif

#define shrink_implies(SHRINK_P, Q)    assert(!(SHRINK_P) || (Q)) /* material implication, immaterial if NDEBUG defined */
#define shrink_never                   assert(0)
#define SHRINK_BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))

/* It would be a good idea for these parameters to be part of the format, a
 * single bit could be used to determine whether to use the defaults or not
 * so as to save space. The maximum buffer size should also be configurable
 * so embedded systems that are tight on RAM can still use this library. 
 * Sensible ranges would have to be checked for. */
                 /* LZSS Parameters */
#ifndef SHRINK_EI
#define SHRINK_EI (11u) /* dictionary size: typically 10..13 */
#endif
#ifndef SHRINK_EJ
#define SHRINK_EJ (4u)  /* match length:    typically 4..5 */
#endif
#ifndef SHRINK_P
#define SHRINK_P  (2u)  /* If match length <= SHRINK_P then output one character */
#endif
#ifndef SHRINK_CH
#define SHRINK_CH (' ') /* initial dictionary contents */
#endif
 
/* Derived LZSS Parameters */
#define SHRINK_N  (1u << SHRINK_EI)             /* buffer size */
#define SHRINK_F  ((1u << SHRINK_EJ) + (SHRINK_P - 1u)) /* lookahead buffer size */

/* RLE Parameters */
#ifndef SHRINK_RL
#define SHRINK_RL (128) /* run length */
#endif
#ifndef SHRINK_ROVER
#define SHRINK_ROVER (1) /* encoding only run lengths greater than SHRINK_ROVER + 1 */
#endif

enum { SHRINK_REFERENCE, SHRINK_LITERAL, };

typedef struct {
	unsigned buffer, mask;
} shrink_bit_shrink_buffer_t;

typedef struct {
	unsigned char *b;
	size_t used, length;
} shrink_buffer_t;

typedef struct {
	uint8_t buffer[SHRINK_N * 2];
	shrink_t *io;
	shrink_bit_shrink_buffer_t bit;
} shrink_lzss_t;

SHRINK_API int shrink_version(unsigned long *version) {
	assert(version);
	unsigned long options = 0;
	options |= SHRINK_DEBUGGING << 0;
	options |= SHRINK_STATIC_ON << 1;
	*version = (options << 24) | SHRINK_VERSION;
	return SHRINK_VERSION == 0 ? -1 : 0;
}

static int shrink_buffer_get(void *in) {
	shrink_buffer_t *b = (shrink_buffer_t*)in;
	assert(b);
	assert(b->b);
	if (b->used >= b->length)
		return -1;
	return b->b[b->used++];
}

static int shrink_buffer_put(const int ch, void *out) {
	shrink_buffer_t *b = (shrink_buffer_t*)out;
	assert(b);
	assert(b->b);
	if (b->used >= b->length)
		return -1;
	return b->b[b->used++] = ch;
}

static int shrink_get(shrink_t *io) {
	assert(io);
	const int r = io->get(io->in);
	io->read += r >= 0;
	assert((r >= 0 && r <= 255) || r == -1);
	return r;
}

static int shrink_put(const int ch, shrink_t *io) {
	assert(io);
	const int r = io->put(ch, io->out);
	io->wrote += r >= 0;
	assert((r >= 0 && r <= 255) || r == -1);
	return r;
}

static int shrink_bit_buffer_put_bit(shrink_t *io, shrink_bit_shrink_buffer_t *bit, const unsigned one) {
	assert(io);
	assert(bit);
	assert(bit->mask <= 128u);
	assert((bit->buffer & ~0xFFu) == 0);
	if (one)
		bit->buffer |= bit->mask;
	if ((bit->mask >>= 1) == 0) {
		if (shrink_put(bit->buffer, io) < 0)
			return -1;
		bit->buffer = 0;
		bit->mask   = 128;
	}
	return 0;
}

static int shrink_bit_buffer_get_n_bits(shrink_t *io, shrink_bit_shrink_buffer_t *bit, unsigned n) {
	assert(io);
	assert(bit);
	unsigned x = 0;
	assert(n < 16u);
	for (unsigned i = 0; i < n; i++) {
		if (bit->mask == 0u) {
			const int ch = shrink_get(io);
			if (ch < 0)
				return -1;
			bit->buffer = ch;
			bit->mask   = 128;
		}
		x <<= 1;
		if (bit->buffer & bit->mask)
			x++;
		bit->mask >>= 1;
	}
	return x;
}

static int shrink_bit_buffer_flush(shrink_t *io, shrink_bit_shrink_buffer_t *bit) {
	assert(io);
	assert(bit);
	if (bit->mask != 128)
		if (shrink_put(bit->buffer, io) < 0)
			return -1;
	bit->mask = 0;
	return 0;
}

static int shrink_init(shrink_lzss_t *l, const size_t length) {
	assert(l);
	assert(length < sizeof l->buffer);
	memset(l->buffer, SHRINK_CH, length);
	return 0;
}

static int shrink_output_literal(shrink_lzss_t *l, const unsigned ch) {
	assert(l);
	if (shrink_bit_buffer_put_bit(l->io, &l->bit, SHRINK_LITERAL) < 0)
		return -1;
	for (unsigned mask = 256; mask >>= 1; )
		if (shrink_bit_buffer_put_bit(l->io, &l->bit, ch & mask) < 0)
			return -1;
	return 0;
}

static int shrink_output_reference(shrink_lzss_t *l, const unsigned position, const unsigned length) {
	assert(l);
	assert(position < (1 << SHRINK_EI));
	assert(length < ((1 << SHRINK_EJ) + SHRINK_P));
	if (shrink_bit_buffer_put_bit(l->io, &l->bit, SHRINK_REFERENCE) < 0)
		return -1;
	for (unsigned mask = SHRINK_N; mask >>= 1;)
		if (shrink_bit_buffer_put_bit(l->io, &l->bit, position & mask) < 0)
			return -1;
	for (unsigned mask = 1 << SHRINK_EJ; mask >>= 1; )
		if (shrink_bit_buffer_put_bit(l->io, &l->bit, length & mask) < 0)
			return -1;
	return 0;
}

static int shrink_lzss_encode(shrink_t *io) {
	assert(io);
	SHRINK_STATIC shrink_lzss_t l = { .bit = { .mask = 128, }, };
	l.io = io; /* need because of SHRINK_STATIC */
	unsigned bufferend = 0;

	if (shrink_init(&l, SHRINK_N - SHRINK_F) < 0)
		return -1;

	for (bufferend = SHRINK_N - SHRINK_F; bufferend < SHRINK_N * 2u; bufferend++) {
		const int c = shrink_get(l.io);
		if (c < 0)
			break;
		l.buffer[bufferend] = c;
	}

	for (unsigned r = SHRINK_N - SHRINK_F, s = 0; r < bufferend; ) {
		unsigned x = 0, y = 1;
		const int ch = l.buffer[r];
		for (unsigned i = s; i < r; i++) { /* search for longest match */
			assert(r >= r - i);
			uint8_t *m = (uint8_t*)memchr(&l.buffer[i], ch, r - i); /* match first char */
			if (!m)
				break;
			assert(i < sizeof l.buffer);
			i += m - &l.buffer[i];
			const unsigned f1 = (SHRINK_F <= bufferend - r) ? SHRINK_F : bufferend - r;
			unsigned j = 1;
			for (j = 1; j < f1; j++) { /* run of matches */
				assert((i + j) < sizeof l.buffer);
				assert((r + j) < sizeof l.buffer);
				if (l.buffer[i + j] != l.buffer[r + j])
					break;
			}
			if (j > y) {
				x = i; /* match position */
				y = j; /* match length */
			}
			if ((y + SHRINK_P - 1) > SHRINK_F) /* maximum length reach, stop search */
				break;
		}
		if (y <= SHRINK_P) { /* is match worth it? */
			y = 1;
			if (shrink_output_literal(&l, ch) < 0) /* Not worth it */
				return -1;
		} else { /* L'Oreal: Because you're worth it. */
			if (shrink_output_reference(&l, x & (SHRINK_N - 1u), y - SHRINK_P) < 0)
				return -1;
		}
		assert((r + y) > r);
		assert((s + y) > s);
		r += y;
		s += y;
		if (r >= ((SHRINK_N * 2u) - SHRINK_F)) { /* move and refill buffer */
			SHRINK_BUILD_BUG_ON(sizeof l.buffer < SHRINK_N);
			memmove(l.buffer, l.buffer + SHRINK_N, SHRINK_N);
			assert(bufferend - SHRINK_N < bufferend);
			assert((r - SHRINK_N) < r);
			assert((s - SHRINK_N) < s);
			bufferend -= SHRINK_N;
			r -= SHRINK_N;
			s -= SHRINK_N;
			while (bufferend < (SHRINK_N * 2u)) {
				const int c = shrink_get(l.io);
				if (c < 0)
					break;
				assert(bufferend < sizeof(l.buffer));
				l.buffer[bufferend++] = c;
			}
		}
	}
	return shrink_bit_buffer_flush(l.io, &l.bit);
}

static int shrink_lzss_decode(shrink_t *io) {
	assert(io);
	/* TODO: Add to header, instead of here. */
	SHRINK_STATIC shrink_lzss_t l = { .bit = { .mask = 0, }, };
	l.io = io; /* need because of SHRINK_STATIC */

	if (shrink_init(&l, SHRINK_N - SHRINK_F) < 0)
		return -1;

	int c = 0;
	for (unsigned r = SHRINK_N - SHRINK_F; (c = shrink_bit_buffer_get_n_bits(l.io, &l.bit, 1)) >= 0; ) {
		if (c == SHRINK_LITERAL) { /* control bit: literal, emit a byte */
			if ((c = shrink_bit_buffer_get_n_bits(l.io, &l.bit, 8)) < 0)
				break;
			if (shrink_put(c, l.io) != c)
				return -1;
			assert(r < sizeof(l.buffer));
			l.buffer[r++] = c;
			r &= (SHRINK_N - 1u); /* wrap around */
			continue;
		}
		const int i = shrink_bit_buffer_get_n_bits(l.io, &l.bit, SHRINK_EI); /* position */
		if (i < 0)
			break;
		const int j = shrink_bit_buffer_get_n_bits(l.io, &l.bit, SHRINK_EJ); /* length */
		if (j < 0)
			break;
		for (unsigned k = 0; k < j + SHRINK_P; k++) { /* copy (pos,len) to output and dictionary */
			c = l.buffer[(i + k) & (SHRINK_N - 1)];
			if (shrink_put(c, l.io) != c)
				return -1;
			assert(r < sizeof(l.buffer));
			l.buffer[r++] = c;
			r &= (SHRINK_N - 1u); /* wrap around */
		}
	}
	return 0;
}

static int shrink_rle_write_buf(shrink_t *io, uint8_t *buf, const int idx) {
	assert(io);
	assert(buf);
	assert(idx >= 0);
	if (idx == 0)
		return 0;
	if (shrink_put(idx + SHRINK_RL, io) < 0)
		return -1;
	for (int i = 0; i < idx; i++)
		if (shrink_put(buf[i], io) < 0)
			return -1;
	return 0;
}

static int shrink_rle_write_run(shrink_t *io, const int count, const int ch) {
	assert(io);
	assert(ch >= 0 && ch < 256);
	assert(count >= 0);
	if (shrink_put(count, io) < 0)
		return -1;
	if (shrink_put(ch, io) < 0)
		return -1;
	return 0;
}

static int shrink_rle_encode(shrink_t *io) { /* this could do with simplifying... */
	assert(io);
	uint8_t buf[SHRINK_RL] = { 0, }; /* buffer to store data with no runs */
	int idx = 0, prev = -1;
	for (int c = 0; (c = shrink_get(io)) >= 0; prev = c) {
		if (c == prev) { /* encode runs of data */
			int j = 0, k = 0;  /* count of runs */
			if (idx == 1 && buf[0] == c) {
				k++;
				idx = 0;
			}
again:
			for (j = k; (c = shrink_get(io)) == prev && j < SHRINK_RL + SHRINK_ROVER; j++)
				/*loop does everything*/;
			k = 0;
			if (j > SHRINK_ROVER) { /* run length is worth encoding */
				if (idx >= 1) { /* output any existing data */
					if (shrink_rle_write_buf(io, buf, idx) < 0)
						return -1;
					idx = 0;
				}
				if (shrink_rle_write_run(io, j - SHRINK_ROVER, prev) < 0)
					return -1;
			} else { /* run length is not worth encoding */
				while (j-- >= 0) { /* encode too small run as literal */
					assert(idx < (int)sizeof (buf));
					buf[idx++] = prev;
					if (idx == (SHRINK_RL - 1)) {
						if (shrink_rle_write_buf(io, buf, idx) < 0)
							return -1;
						idx = 0;
					}
					assert(idx < (SHRINK_RL - 1));
				}
			}
			if (c < 0)
				goto end;
			if (c == prev && j == SHRINK_RL) /* more in current run */
				goto again;
			/* fall-through */
		}
		assert(idx < (int)sizeof (buf));
		buf[idx++] = c;
		if (idx == (SHRINK_RL - 1)) {
			if (shrink_rle_write_buf(io, buf, idx) < 0)
				return -1;
			idx = 0;
		}
		assert(idx < (SHRINK_RL - 1));
	}
end: /* no more input */
	if (shrink_rle_write_buf(io, buf, idx) < 0) /* we might still have something in the buffer though */
		return -1;
	return 0;
}

static int shrink_rle_decode(shrink_t *io) {
	assert(io);
	for (int c = 0, count = 0; (c = shrink_get(io)) >= 0;) {
		if (c > SHRINK_RL) { /* process run of literal data */
			count = c - SHRINK_RL;
			for (int i = 0; i < count; i++) {
				if ((c = shrink_get(io)) < 0)
					return -1;
				if (shrink_put(c, io) != c)
					return -1;
			}
			continue;
		}
		/* process repeated byte */
		count = c + 1 + SHRINK_ROVER;
		if ((c = shrink_get(io)) < 0)
			return -1;
		for (int i = 0; i < count; i++)
			if (shrink_put(c, io) != c)
				return -1;
	}
	return 0;
}

SHRINK_API int shrink(shrink_t *io, const int codec, const int encode) {
	assert(io);
	switch (codec) {
	case CODEC_RLE:  return encode ? shrink_rle_encode(io) : shrink_rle_decode(io);
	case CODEC_LZSS: return encode ? shrink_lzss_encode(io) : shrink_lzss_decode(io);
	}
	shrink_never;
	return -1;
}

SHRINK_API int shrink_buffer(const int codec, const int encode, const char *in, const size_t inlength, char *out, size_t *outlength) {
	assert(in);
	assert(out);
	assert(outlength);
	shrink_buffer_t ib = { .b = (unsigned char*)in,  .used = 0, .length = inlength, };
	shrink_buffer_t ob = { .b = (unsigned char*)out, .used = 0, .length = *outlength, };
	shrink_t io = { .get = shrink_buffer_get, .put = shrink_buffer_put, .in  = &ib, .out = &ob, };
	const int r = shrink(&io, codec, encode);
	*outlength = r == 0 ? io.wrote : 0;
	return r;
}

#ifdef SHRINK_UNIT_TESTS

#define SHRINK_TBUFL (256u)

static inline int shrink_test(const int codec, const char *msg, const size_t msglen) {
	assert(msg);
	char compressed[SHRINK_TBUFL] = { 0, }, decompressed[SHRINK_TBUFL] = { 0, };
	size_t complen = sizeof compressed, decomplen = sizeof decompressed;
	if (msglen > SHRINK_TBUFL)
		return -1;
	const int r1 = shrink_buffer(codec, 1, msg,        msglen,  compressed,   &complen);
	if (r1 < 0)
		return -2;
	const int r2 = shrink_buffer(codec, 0, compressed, complen, decompressed, &decomplen);
	if (r2 < 0)
		return -3;
	if (msglen != decomplen)
		return -4;
	if (memcmp(msg, decompressed, msglen))
		return -5;
	return 0;
}

SHRINK_API int shrink_tests(void) {
	SHRINK_BUILD_BUG_ON(SHRINK_EI > 15); /* 1 << SHRINK_EI would be larger than smallest possible INT_MAX */
	SHRINK_BUILD_BUG_ON(SHRINK_EI < 6);  /* no point in encoding */
	SHRINK_BUILD_BUG_ON(SHRINK_EJ > SHRINK_EI); /* match length needs to be smaller than thing we are matching in */
	SHRINK_BUILD_BUG_ON(SHRINK_RL > 128);
	SHRINK_BUILD_BUG_ON(SHRINK_P  <= 1);
	SHRINK_BUILD_BUG_ON((SHRINK_EI + SHRINK_EJ) > 16); /* unsigned and int used, minimum size is 16 bits */
	SHRINK_BUILD_BUG_ON((SHRINK_N - 1) & SHRINK_N); /* SHRINK_N must be power of 2 */

	if (!SHRINK_DEBUGGING)
		return 0;

	char *ts[] = {
		"If not to heaven, then hand in hand to hell",
		"aaaaaaaaaabbbbbbbbccddddddeeeeeeeefffffffhh",
		"I am Sam\nSam I am\nThat Sam-I-am!\n\
		That Sam-I-am!\nI do not like\nthat Sam-I-am!\n\
		Do you like green eggs and ham?\n\
		I do not like them, Sam-I-am.\n\
		I do not like green eggs and ham.\n"
	};

	for (size_t i = 0; i < (sizeof ts / sizeof (ts[0])); i++)
		for (int j = CODEC_RLE; j <= CODEC_LZSS; j++) {
			const int r = shrink_test(j, ts[i], strlen(ts[i]) + 1);
			if (r < 0)
				return r;
		}
	return 0;
}
#endif /* SHRINK_UNIT_TESTS */
#endif /* SHRINK_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif
#endif
