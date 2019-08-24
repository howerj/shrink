/* Project:    Shrink, an LSZZ and RLE compression library
 * Repository: <https://github.com/howerj/shrink>
 * Maintainer: Richard James Howe
 * License:    Public Domain
 * Email:      howe.r.j.89@gmail.com
 *
 * The LZSS CODEC is originally from Haruhiko Okumura, also placed
 * in the public domain, and has been modified since.
 * See <https://oku.edu.mie-u.ac.jp/~okumura/compression/lzss.c>.
 *
 * Some ideas for improvement:
 *
 * - A hash could be calculated on the input and output, for example
 *   CCITT-16, and stored in the 'shrink_t' structure.
 * - Other non-compression related CODECS could be added, for example
 *   base-64 encoding. The 'shrink_t' structure provides an interface
 *   for creating generic byte filters.
 * - Calculate the maximum compression ratio for each CODEC and provide
 *   functions to retrieve this information.
 * - Allow the LZSS dictionary to be initialized with a custom corpus
 *   for both sides of the CODEC, this should allow higher compression
 *   ratios for certain forms of input. The easiest way of doing this
 *   would be to use an extra bit address bit to refer to the built
 *   in fixed dictionary.
 * - The LZSS compressor could do with speeding up. The two ways of
 *   doing this would be to improve I/O speed (perhaps by grouping
 *   control bits, which would allow for byte wise I/O and perhaps
 *   make entropy left on the table easier to exploit) or by increasing 
 *   the match search speed, which would be the best option.
 * - The library could use more assertions, documentation, testing,
 *   and fuzzing.
 * - As it is not worth encoding matches for small runs, we can
 *   use this to increase the maximum number of matches.
 * - The library could be made to be more (compile time) configurable,
 *   whilst using (compile time) assertions to ensure correctness.  */

#include "shrink.h"
#include <assert.h>
#include <string.h> /* memset, memmove, memcmp, strlen */
#include <stdint.h>

#ifdef NDEBUG
#define DEBUGGING (0)
#else
#define DEBUGGING (1)
#endif

#ifdef USE_STATIC /* large internal buffers are statically allocated */
#define STATIC static
#else  /* large internal buffers are allocated on stack */
#define STATIC auto
#endif

#define BUILD_BUG_ON(condition)   ((void)sizeof(char[1 - 2*!!(condition)]))

                           /* LZSS Parameters */
#define EI (11)            /* typically 10..13 */
#define EJ (4)             /* typically 4..5 */
#define P  (2)             /* If match length <= P then output one character */
#define N  (1 << EI)       /* buffer size */
#define F  ((1 << EJ) + 1) /* lookahead buffer size */
#define CH (' ')           /* initial dictionary contents */
                           /* RLE Parameters */
#define RL (128)           /* run length */

#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))

enum { REFERENCE, LITERAL };

typedef struct {
	uint8_t buffer[N * 2];
	shrink_t *io;
	unsigned bit_buffer, bit_mask;
} lzss_t;

typedef struct {
	unsigned char *b;
	size_t used, length;
} buffer_t;

static int buffer_get(void *in) {
	buffer_t *b = in;
	assert(b);
	assert(b->b);
	if (b->used >= b->length)
		return -1;
	return b->b[b->used++];
}

static int buffer_put(const int ch, void *out) {
	buffer_t *b = out;
	assert(b);
	assert(b->b);
	if (b->used >= b->length)
		return -1;
	return b->b[b->used++] = ch;
}

static int get(shrink_t *io) {
	assert(io);
	const int r = io->get(io->in);
	io->read += r >= 0;
	assert((r >= 0 && r <= 255) || r == -1);
	return r;
}

static int put(const int ch, shrink_t *io) {
	assert(io);
	const int r = io->put(ch, io->out);
	io->wrote += r >= 0;
	assert((r >= 0 && r <= 255) || r == -1);
	return r;
}

static int init(lzss_t *l, const size_t length) {
	assert(l);
	assert(length < sizeof l->buffer);
	memset(l->buffer, CH, length);
	return 0;
}

static int putbit(lzss_t *l, const unsigned one) {
	assert(l);
	assert(l->bit_mask <= 128u);
	assert((l->bit_buffer & ~0xFFu) == 0);
	if (one)
		l->bit_buffer |= l->bit_mask;
	if ((l->bit_mask >>= 1) == 0) {
		if (put(l->bit_buffer, l->io) < 0)
			return -1;
		l->bit_buffer = 0;
		l->bit_mask = 128;
	}
	return 0;
}

static int flush_bit_buffer(lzss_t *l) {
	assert(l);
	if (l->bit_mask != 128)
		if (put(l->bit_buffer, l->io) < 0)
			return -1;
	return 0;
}

static int output_literal(lzss_t *l, const unsigned c) {
	assert(l);
	if (putbit(l, LITERAL) < 0)
		return -1;
	for (unsigned mask = 256; mask >>= 1; )
		if (putbit(l, c & mask) < 0)
			return -1;
	return 0;
}

static int output_reference(lzss_t *l, const unsigned x, const unsigned y) {
	assert(l);
	if (putbit(l, REFERENCE) < 0)
		return -1;
	for (unsigned mask = N; mask >>= 1;)
		if (putbit(l, x & mask) < 0)
			return -1;
	for (unsigned mask = 1 << EJ; mask >>= 1; )
		if (putbit(l, y & mask) < 0)
			return -1;
	return 0;
}

static int shrink_lzss_encode(shrink_t *io) {
	assert(io);
	STATIC lzss_t l = { .bit_mask = 128, };
	l.io = io; /* need because of STATIC */
	long bufferend = 0, c = 0;

	if (init(&l, N - F) < 0)
		return -1;

	for (bufferend = N - F; bufferend < N * 2; bufferend++) {
		if ((c = get(l.io)) < 0)
			break;
		l.buffer[bufferend] = c;
	}

	for (long r = N - F, s = 0; r < bufferend; ) {
		const long f1 = (F <= bufferend - r) ? F : bufferend - r;
		long x = 0, y = 1;
		c = l.buffer[r];
		for (long i = r - 1; i >= s; i--) /* search for match */
			if (l.buffer[i] == c) { /* single char */
				long j = 1;
				for (j = 1; j < f1; j++) /* run of matches */
					if (l.buffer[i + j] != l.buffer[r + j])
						break;
				if (j > y) {
					x = i; /* match position */
					y = j; /* match length */
				}
			}
		if (y <= P) { /* is match worth it? */
			y = 1;
			if (output_literal(&l, c) < 0) /* Not worth it */
				return -1;
		} else { /* L'Oreal: Because you're worth it. */
			if (output_reference(&l, x & (N - 1), y - 2) < 0)
				return -1;
		}
		r += y;
		s += y;
		if (r >= ((N * 2) - F)) { /* move and refill buffer */
			memmove(l.buffer, l.buffer + N, N);
			bufferend -= N;
			r -= N;
			s -= N;
			while (bufferend < (N * 2)) {
				if ((c = get(l.io)) < 0)
					break;
				l.buffer[bufferend++] = c;
			}
		}
	}
	return flush_bit_buffer(&l);
}

static int getnbit(lzss_t *l, unsigned n) {
	assert(l);
	unsigned x = 0;
	assert(n < 16u);
	for (unsigned i = 0; i < n; i++) {
		if (l->bit_mask == 0u) {
			const int ch = get(l->io);
			if (ch < 0)
				return -1;
			l->bit_buffer = ch;
			l->bit_mask   = 128;
		}
		x <<= 1;
		if (l->bit_buffer & l->bit_mask)
			x++;
		l->bit_mask >>= 1;
	}
	return x;
}

static int shrink_lzss_decode(shrink_t *io) {
	assert(io);
	STATIC lzss_t l = { .bit_mask = 0 };
	l.io = io; /* need because of STATIC */

	if (init(&l, N - F) < 0)
		return -1;

	for (long r = N - F, c = 0; (c = getnbit(&l, 1)) >= 0; ) {
		if (c == LITERAL) { /* control bit */
			if ((c = getnbit(&l, 8)) < 0)
				break;
			if (put(c, l.io) != c)
				return -1;
			l.buffer[r++] = c;
			r &= (N - 1);
			continue;
		}
		const int i = getnbit(&l, EI);
		if (i < 0)
			break;
		const int j = getnbit(&l, EJ);
		if (j < 0)
			break;
		for (long k = 0; k <= j + 1; k++) {
			c = l.buffer[(i + k) & (N - 1)];
			if (put(c, l.io) != c)
				return -1;
			l.buffer[r++] = c;
			r &= (N - 1);
		}
	}
	return 0;
}

static int rle_write_buf(shrink_t *io, uint8_t *buf, int idx) {
	assert(io);
	assert(buf);
	assert(idx >= 0);
	if (put(idx + RL, io) < 0)
		return -1;
	for (int i = 0; i < idx; i++)
		if (put(buf[i], io) < 0)
			return -1;
	return 0;
}

static int shrink_rle_encode(shrink_t *io) {
	assert(io);
	uint8_t buf[RL] = { 0 }; /* buffer to store data with no runs */
	int idx = 0, prev = -1; /* no previously read in char can be EOF */
	for (int c = 0; (c = get(io)) >= 0; prev = c) {
		if (c == prev) { /* encode runs of data */
			int j = 0;  /* count of runs */
			if (idx > 1) { /* output any existing data */
				if (rle_write_buf(io, buf, idx) < 0)
					return -1;
				idx = 0;
			}
again:
			for (j = 0; (c = get(io)) == prev && j < RL; j++)
				/*loop does everything*/;
			if (put(j, io) < 0)
				return -1;
			if (put(prev, io) < 0)
				return -1;
			if (c < 0)
				goto end;
			if (c == prev && j == RL)
				goto again;
		}
		buf[idx++] = c;
		if (idx == (RL - 1)) {
			if (rle_write_buf(io, buf, idx) < 0)
				return -1;
			idx = 0;
		}
		assert(idx < (RL - 1));
	}
end: /* no more input */
	if (idx) /* we might still have something in the buffer though */
		if (rle_write_buf(io, buf, idx) < 0)
			return -1;
	return 0;
}

static int shrink_rle_decode(shrink_t *io) {
	assert(io);
	for (int c = 0, count = 0; (c = get(io)) >= 0;) {
		if (c > RL) { /* process run of literal data */
			count = c - RL;
			for (int i = 0; i < count; i++) {
				if ((c = get(io)) < 0)
					return -1;
				if (put(c, io) != c)
					return -1;
			}
			continue;
		}
		/* process repeated byte */
		count = c + 1;
		if ((c = get(io)) < 0)
			return -1;
		for (int i = 0; i < count; i++)
			if (put(c, io) != c)
				return -1;
	}
	return 0;
}

int shrink_buffer(const int codec, const int encode, const char *in, const size_t inlength, char *out, size_t *outlength) {
	assert(codec == CODEC_RLE || codec == CODEC_LZSS);
	assert(encode == 0 || encode == 1);
	assert(in);
	assert(out);
	assert(outlength);
	buffer_t ib = { .b = (unsigned char*)in,  .used = 0, .length = inlength };
	buffer_t ob = { .b = (unsigned char*)out, .used = 0, .length = *outlength };
	shrink_t io = { .get = buffer_get, .put = buffer_put, .in  = &ib, .out = &ob, };
	const int r = (encode ?
			(codec == CODEC_LZSS ? shrink_lzss_encode : shrink_rle_encode) :
			(codec == CODEC_LZSS ? shrink_lzss_decode : shrink_rle_decode))(&io);
	*outlength = r == 0 ? io.wrote : 0;
	return r;
}

int shrink(shrink_t *io, const int codec, const int encode) {
	assert(io);
	assert(encode == 0 || encode == 1);
	assert(codec == CODEC_RLE || codec == CODEC_LZSS);
	return (encode ?
		(codec == CODEC_LZSS ? shrink_lzss_encode : shrink_rle_encode) :
		(codec == CODEC_LZSS ? shrink_lzss_decode : shrink_rle_decode))(io);
}

#define TBUFL (256u)

static inline int test(const int codec, const char *msg, const size_t msglen) {
	assert(msg);
	char compressed[TBUFL] = { 0 }, decompressed[TBUFL] = { 0 };
	size_t complen = sizeof compressed, decomplen = sizeof decompressed;
	if (msglen > TBUFL)
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

int shrink_tests(void) {
	BUILD_BUG_ON(EI > 15); /* 1 << EI would be larger than smallest possible INT_MAX */
	BUILD_BUG_ON(EI < 6);  /* no point in encoding */
	BUILD_BUG_ON(EJ >= EI);
	BUILD_BUG_ON(RL > 128);

	if (!DEBUGGING)
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
			const int r = test(CODEC_RLE, ts[i], strlen(ts[i]) + 1);
			if (r < 0)
				return r;
		}
	return 0;
}

