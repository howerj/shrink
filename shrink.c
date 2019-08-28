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
 * - A hash could be calculated over the input and the output, the
 *   best way of doing this would _not_ to do this as part of this 
 *   library but pass in callbacks and data that would wrap any I/O
 *   and calculate the hash as well.
 * - Other non-compression related CODECS could be added, for example
 *   base-64 encoding. The 'shrink_t' structure provides an interface
 *   for creating generic byte filters. A compression related filter
 *   would be Adaptive Huffman Coding, which compliment LZSS well.
 * - Being able to chain together these filters together arbitrarily
 *   would greatly increase the utility of this library, the best
 *   way would be to use coroutines to do so; each filter yielding
 *   when it needs to perform I/O to the other, however coroutines in
 *   C either are not portable or make the code ugly depending on
 *   the implementation of them, see:
 *   <https://www.chiark.greenend.org.uk/~sgtatham/coroutines.html>
 * - Calculate the maximum compression ratio for each CODEC and provide
 *   functions to retrieve this information.
 * - A custom, fixed, dictionary could be switched into the LZSS
 *   CODEC. This could be supplied via the API. This could be
 *   encoded in many ways.
 * - The LZSS compressor could do with speeding up. The two ways of
 *   doing this would be to improve I/O speed (perhaps by grouping
 *   control bits, which would allow for byte wise I/O and perhaps
 *   make entropy left on the table easier to exploit) or by increasing
 *   the match search speed, which would be the best option. One way
 *   of potentially speeding up the search would be to store a hash
 *   pointer to the first character in a potential match.
 * - The library could use more assertions, documentation, testing,
 *   and fuzzing.
 * - As it is not worth encoding matches for small runs, we can
 *   use this to increase the maximum number of matches. (RLE done,
 *   LZSS needs looking into).
 * - The library could be made to be more (compile time) configurable,
 *   whilst using (compile time) assertions to ensure correctness. */

#include "shrink.h"
#include <assert.h>
#include <string.h> /* memset, memmove, memcmp, memchr, strlen */
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
#define EI (11u)             /* typically 10..13 */
#define EJ (4u)              /* typically 4..5 */
#define P  (2u)              /* If match length <= P then output one character */
#define N  (1u << EI)        /* buffer size */
#define F  ((1u << EJ) + 1u) /* lookahead buffer size */
#define CH (' ')             /* initial dictionary contents */
                             /* RLE Parameters */
#define RL    (128)          /* run length */
#define ROVER (1)            /* encoding only run lengths greater than ROVER + 1 */

#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))

enum { REFERENCE, LITERAL };

typedef struct {
	unsigned buffer, mask;
} bit_buffer_t;

typedef struct {
	unsigned char *b;
	size_t used, length;
} buffer_t;

typedef struct {
	uint8_t buffer[N * 2];
	shrink_t *io;
	bit_buffer_t bit;
} lzss_t;

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

static int bit_buffer_put_bit(shrink_t *io, bit_buffer_t *bit, const unsigned one) {
	assert(io);
	assert(bit);
	assert(bit->mask <= 128u);
	assert((bit->buffer & ~0xFFu) == 0);
	if (one)
		bit->buffer |= bit->mask;
	if ((bit->mask >>= 1) == 0) {
		if (put(bit->buffer, io) < 0)
			return -1;
		bit->buffer = 0;
		bit->mask   = 128;
	}
	return 0;
}

static int bit_buffer_get_n_bits(shrink_t *io, bit_buffer_t *bit, unsigned n) {
	assert(io);
	assert(bit);
	unsigned x = 0;
	assert(n < 16u);
	for (unsigned i = 0; i < n; i++) {
		if (bit->mask == 0u) {
			const int ch = get(io);
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

static int bit_buffer_flush(shrink_t *io, bit_buffer_t *bit) {
	assert(io);
	assert(bit);
	if (bit->mask != 128)
		if (put(bit->buffer, io) < 0)
			return -1;
	bit->mask = 0;
	return 0;
}

static int init(lzss_t *l, const size_t length) {
	assert(l);
	assert(length < sizeof l->buffer);
	memset(l->buffer, CH, length);
	return 0;
}

static int output_literal(lzss_t *l, const unsigned ch) {
	assert(l);
	if (bit_buffer_put_bit(l->io, &l->bit, LITERAL) < 0)
		return -1;
	for (unsigned mask = 256; mask >>= 1; )
		if (bit_buffer_put_bit(l->io, &l->bit, ch & mask) < 0)
			return -1;
	return 0;
}

static int output_reference(lzss_t *l, const unsigned position, const unsigned length) {
	assert(l);
	if (bit_buffer_put_bit(l->io, &l->bit, REFERENCE) < 0)
		return -1;
	for (unsigned mask = N; mask >>= 1;)
		if (bit_buffer_put_bit(l->io, &l->bit, position & mask) < 0)
			return -1;
	for (unsigned mask = 1 << EJ; mask >>= 1; )
		if (bit_buffer_put_bit(l->io, &l->bit, length & mask) < 0)
			return -1;
	return 0;
}

static int shrink_lzss_encode(shrink_t *io) {
	assert(io);
	STATIC lzss_t l = { .bit = { .mask = 128 }, };
	l.io = io; /* need because of STATIC */
	unsigned bufferend = 0;

	if (init(&l, N - F) < 0)
		return -1;

	for (bufferend = N - F; bufferend < N * 2u; bufferend++) {
		const int c = get(l.io);
		if (c < 0)
			break;
		l.buffer[bufferend] = c;
	}

	for (unsigned r = N - F, s = 0; r < bufferend; ) {
		unsigned x = 0, y = 1;
		const int ch = l.buffer[r];
		for (unsigned i = s; i < r; i++) { /* search for longest match */
			assert(r >= r - i);
			uint8_t *m = memchr(&l.buffer[i], ch, r - i); /* match first char */
			if (!m)
				break;
			assert(i < sizeof l.buffer);
			i += m - &l.buffer[i];
			const unsigned f1 = (F <= bufferend - r) ? F : bufferend - r;
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
			if (y > (F - 1u)) /* maximum length reach, stop search */
				break;
		}
		if (y <= P) { /* is match worth it? */
			y = 1;
			if (output_literal(&l, ch) < 0) /* Not worth it */
				return -1;
		} else { /* L'Oreal: Because you're worth it. */
			if (output_reference(&l, x & (N - 1u), y - 2u) < 0)
				return -1;
		}
		assert(r + y > r);
		assert(s + y > s);
		r += y;
		s += y;
		if (r >= ((N * 2u) - F)) { /* move and refill buffer */
			BUILD_BUG_ON(sizeof l.buffer < N);
			memmove(l.buffer, l.buffer + N, N);
			assert(bufferend - N < bufferend);
			assert(r - N < r);
			assert(s - N < s);
			bufferend -= N;
			r -= N;
			s -= N;
			while (bufferend < (N * 2u)) {
				int c = get(l.io);
				if (c < 0)
					break;
				assert(bufferend < sizeof(l.buffer));
				l.buffer[bufferend++] = c;
			}
		}
	}
	return bit_buffer_flush(l.io, &l.bit);
}

static int shrink_lzss_decode(shrink_t *io) {
	assert(io);
	STATIC lzss_t l = { .bit = { .mask = 0 } };
	l.io = io; /* need because of STATIC */

	if (init(&l, N - F) < 0)
		return -1;

	for (long r = N - F, c = 0; (c = bit_buffer_get_n_bits(l.io, &l.bit, 1)) >= 0; ) {
		if (c == LITERAL) { /* control bit: literal, emit a byte */
			if ((c = bit_buffer_get_n_bits(l.io, &l.bit, 8)) < 0)
				break;
			if (put(c, l.io) != c)
				return -1;
			l.buffer[r++] = c;
			r &= (N - 1u); /* wrap around */
			continue;
		}
		const int i = bit_buffer_get_n_bits(l.io, &l.bit, EI); /* position */
		if (i < 0)
			break;
		const int j = bit_buffer_get_n_bits(l.io, &l.bit, EJ); /* length */
		if (j < 0)
			break;
		for (long k = 0; k <= j + 1; k++) { /* copy (pos,len) to output and dictionary */
			c = l.buffer[(i + k) & (N - 1)];
			if (put(c, l.io) != c)
				return -1;
			l.buffer[r++] = c;
			r &= (N - 1u); /* wrap around */
		}
	}
	return 0;
}

static int rle_write_buf(shrink_t *io, uint8_t *buf, const int idx) {
	assert(io);
	assert(buf);
	assert(idx >= 0);
	if (idx == 0)
		return 0;
	if (put(idx + RL, io) < 0)
		return -1;
	for (int i = 0; i < idx; i++)
		if (put(buf[i], io) < 0)
			return -1;
	return 0;
}

static int rle_write_run(shrink_t *io, const int count, const int ch) {
	assert(io);
	assert(ch >= 0 && ch < 256);
	assert(count >= 0);
	if (put(count, io) < 0)
		return -1;
	if (put(ch, io) < 0)
		return -1;
	return 0;
}

static int shrink_rle_encode(shrink_t *io) { /* this could do with simplifying... */
	assert(io);
	uint8_t buf[RL] = { 0 }; /* buffer to store data with no runs */
	int idx = 0, prev = -1;
	for (int c = 0; (c = get(io)) >= 0; prev = c) {
		if (c == prev) { /* encode runs of data */
			int j = 0, k = 0;  /* count of runs */
			if (idx == 1 && buf[0] == c) {
				k++;
				idx = 0;
			}
again:
			for (j = k; (c = get(io)) == prev && j < RL + ROVER; j++)
				/*loop does everything*/;
			k = 0;
			if (j > ROVER) { /* run length is worth encoding */
				if (idx >= 1) { /* output any existing data */
					if (rle_write_buf(io, buf, idx) < 0)
						return -1;
					idx = 0;
				}
				if (rle_write_run(io, j - ROVER, prev) < 0)
					return -1;
			} else { /* run length is not worth encoding */
				while (j-- >= 0) { /* encode too small run as literal */
					buf[idx++] = prev;
					if (idx == (RL - 1)) {
						if (rle_write_buf(io, buf, idx) < 0)
							return -1;
						idx = 0;
					}
					assert(idx < (RL - 1));
				}
			}
			if (c < 0)
				goto end;
			if (c == prev && j == RL) /* more in current run */
				goto again;
			/* fall-through */
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
	if (rle_write_buf(io, buf, idx) < 0) /* we might still have something in the buffer though */
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
		count = c + 1 + ROVER;
		if ((c = get(io)) < 0)
			return -1;
		for (int i = 0; i < count; i++)
			if (put(c, io) != c)
				return -1;
	}
	return 0;
}

int shrink(shrink_t *io, const int codec, const int encode) {
	assert(io);
	assert(codec == CODEC_RLE || codec == CODEC_LZSS);
	return (encode ?
		(codec == CODEC_LZSS ? shrink_lzss_encode : shrink_rle_encode) :
		(codec == CODEC_LZSS ? shrink_lzss_decode : shrink_rle_decode))(io);
}

int shrink_buffer(const int codec, const int encode, const char *in, const size_t inlength, char *out, size_t *outlength) {
	assert(in);
	assert(out);
	assert(outlength);
	buffer_t ib = { .b = (unsigned char*)in,  .used = 0, .length = inlength };
	buffer_t ob = { .b = (unsigned char*)out, .used = 0, .length = *outlength };
	shrink_t io = { .get = buffer_get, .put = buffer_put, .in  = &ib, .out = &ob, };
	const int r = shrink(&io, codec, encode);
	*outlength = r == 0 ? io.wrote : 0;
	return r;
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
	BUILD_BUG_ON(EJ > EI); /* match length needs to be smaller than thing we are matching in */
	BUILD_BUG_ON(RL > 128);
	BUILD_BUG_ON((EI + EJ) > 16); /* unsigned and int used, minimum size is 16 bits */

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

