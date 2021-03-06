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
 * View the projects 'readme.md' file for more information. Making this
 * optionally non-blocking would be good. */

#include "shrink.h"
#include <assert.h>
#include <string.h> /* memset, memmove, memcmp, memchr, strlen */
#include <stdint.h>

#define implies(P, Q)             assert(!(P) || (Q)) /* material implication, immaterial if NDEBUG defined */
#define never                     assert(0)
#define BUILD_BUG_ON(condition)   ((void)sizeof(char[1 - 2*!!(condition)]))

#ifndef SHRINK_VERSION
#define SHRINK_VERSION (0x000000ul) /* all zeros indicates and error */
#endif
#ifdef NDEBUG
#define DEBUGGING (0)
#else
#define DEBUGGING (1)
#endif
#ifdef USE_STATIC
#define ALLOCATE static
#else
#define ALLOCATE auto
#endif
#ifndef LZSS_DICTIONARY_SIZE /* binary logarithm of dictionary size */
#define LZSS_DICTIONARY_SIZE (11u)
#endif
#ifndef LZSS_MATCH_LENGTH /* maximum length of LZSS match */
#define LZSS_MATCH_LENGTH (4u)
#endif
#ifndef LZSS_MINIMUM_MATCH /* minimum length of LZSS match */
#define LZSS_MINIMUM_MATCH (2u)
#endif
                                  /* LZSS Parameters */
#define EI LZSS_DICTIONARY_SIZE   /* dictionary size: typically 10..13 */
#define EJ LZSS_MATCH_LENGTH      /* match length:    typically 4..5 */
#define P  LZSS_MINIMUM_MATCH     /* If match length <= P then output one character */
#define N  (1u << EI)             /* buffer size */
#define F  ((1u << EJ) + (P - 1)) /* lookahead buffer size */
#define CH (' ')                  /* initial dictionary contents if custom flag not set */
                                  /* RLE Parameters */
#define RL    (128)               /* run length */
#define ROVER (1)                 /* encoding only run lengths greater than ROVER + 1 */

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
	shrink_t *io;
	bit_buffer_t bit;
} lzss_t;

int shrink_version(unsigned long *version) {
	assert(version);
	unsigned long options = 0;
	options |= DEBUGGING << 0;
	*version = (options << 24) | SHRINK_VERSION;
	return SHRINK_VERSION == 0 ? -1 : 0;
}

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

static int bit_buffer_get_n_bits(shrink_t *io, bit_buffer_t *bit, const unsigned n) {
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
	assert(length < l->io->buffer_length);
	if (l->io->custom == 0) /* The user must clear the register in this case */
		memset(l->io->buffer, CH, length);
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
	assert(position < (1 << EI));
	assert(length < ((1 << EJ) + P));
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
	lzss_t l = { .bit = { .mask = 128, }, .io = io, };
	if ((N * 2ul) < io->buffer_length)
		return -1;
	unsigned bufferend = 0;

	if (init(&l, N - F) < 0)
		return -1;

	for (bufferend = N - F; bufferend < N * 2u; bufferend++) {
		const int c = get(l.io);
		if (c < 0)
			break;
		io->buffer[bufferend] = c;
	}

	for (unsigned r = N - F, s = 0; r < bufferend; ) {
		unsigned x = 0, y = 1;
		const int ch = io->buffer[r];
		for (unsigned i = s; i < r; i++) { /* search for longest match */
			assert(r >= r - i);
			uint8_t *m = memchr(&io->buffer[i], ch, r - i); /* match first char */
			if (!m)
				break;
			assert(i < io->buffer_length);
			i += m - &io->buffer[i];
			const unsigned f1 = (F <= bufferend - r) ? F : bufferend - r;
			unsigned j = 1;
			for (j = 1; j < f1; j++) { /* run of matches */
				assert((i + j) < io->buffer_length);
				assert((r + j) < io->buffer_length);
				if (io->buffer[i + j] != io->buffer[r + j])
					break;
			}
			if (j > y) {
				x = i; /* match position */
				y = j; /* match length */
			}
			if ((y + P - 1) > F) /* maximum length reach, stop search */
				break;
		}
		if (y <= P) { /* is match worth it? */
			y = 1;
			if (output_literal(&l, ch) < 0) /* Not worth it */
				return -1;
		} else { /* L'Oreal: Because you're worth it. */
			if (output_reference(&l, x & (N - 1u), y - P) < 0)
				return -1;
		}
		assert(r + y > r);
		assert(s + y > s);
		r += y;
		s += y;
		if (r >= ((N * 2u) - F)) { /* move and refill buffer */
			memmove(io->buffer, io->buffer + N, N);
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
				assert(bufferend < io->buffer_length);
				io->buffer[bufferend++] = c;
			}
		}
	}
	return bit_buffer_flush(l.io, &l.bit);
}

static int shrink_lzss_decode(shrink_t *io) {
	assert(io);
	lzss_t l = { .bit = { .mask = 0 }, .io = io, };
	if ((N * 2ul) < io->buffer_length)
		return -1;

	if (init(&l, N - F) < 0)
		return -1;

	int c = 0;
	for (unsigned r = N - F; (c = bit_buffer_get_n_bits(l.io, &l.bit, 1)) >= 0; ) {
		if (c == LITERAL) { /* control bit: literal, emit a byte */
			if ((c = bit_buffer_get_n_bits(l.io, &l.bit, 8)) < 0)
				break;
			if (put(c, l.io) != c)
				return -1;
			io->buffer[r++] = c;
			r &= (N - 1u); /* wrap around */
			continue;
		}
		const int i = bit_buffer_get_n_bits(l.io, &l.bit, EI); /* position */
		if (i < 0)
			break;
		const int j = bit_buffer_get_n_bits(l.io, &l.bit, EJ); /* length */
		if (j < 0)
			break;
		for (unsigned k = 0; k < j + P; k++) { /* copy (pos,len) to output and dictionary */
			c = io->buffer[(i + k) & (N - 1)];
			if (put(c, l.io) != c)
				return -1;
			io->buffer[r++] = c;
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
	if (io->buffer_length < RL)
		return -1;
	memset(io->buffer, 0, io->buffer_length);
	int idx = 0, prev = -1;
	for (int c = 0; (c = get(io)) >= 0; prev = c) {
		if (c == prev) { /* encode runs of data */
			int j = 0, k = 0;  /* count of runs */
			if (idx == 1 && io->buffer[0] == c) {
				k++;
				idx = 0;
			}
again:
			for (j = k; (c = get(io)) == prev && j < RL + ROVER; j++)
				/*loop does everything*/;
			k = 0;
			if (j > ROVER) { /* run length is worth encoding */
				if (idx >= 1) { /* output any existing data */
					if (rle_write_buf(io, io->buffer, idx) < 0)
						return -1;
					idx = 0;
				}
				if (rle_write_run(io, j - ROVER, prev) < 0)
					return -1;
			} else { /* run length is not worth encoding */
				while (j-- >= 0) { /* encode too small run as literal */
					io->buffer[idx++] = prev;
					if (idx == (RL - 1)) {
						if (rle_write_buf(io, io->buffer, idx) < 0)
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
		io->buffer[idx++] = c;
		if (idx == (RL - 1)) {
			if (rle_write_buf(io, io->buffer, idx) < 0)
				return -1;
			idx = 0;
		}
		assert(idx < (RL - 1));
	}
end: /* no more input */
	if (rle_write_buf(io, io->buffer, idx) < 0) /* we might still have something in the buffer though */
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
	assert(io->buffer);
	switch (codec) {
	case CODEC_RLE:  return encode ? shrink_rle_encode(io)  : shrink_rle_decode(io);
	case CODEC_LZSS: return encode ? shrink_lzss_encode(io) : shrink_lzss_decode(io);
	}
	never;
	return -1;
}

int shrink_buffer(const int codec, const int encode, const char *in, const size_t inlength, char *out, size_t *outlength) {
	assert(in);
	assert(out);
	assert(outlength);
	ALLOCATE unsigned char buffer[N * 2u];
	buffer_t ib = { .b = (unsigned char*)in,  .used = 0, .length = inlength };
	buffer_t ob = { .b = (unsigned char*)out, .used = 0, .length = *outlength };
	shrink_t io = { .get = buffer_get, .put = buffer_put, .in  = &ib, .out = &ob, .buffer = buffer, .buffer_length = sizeof buffer, };
	const int r = shrink(&io, codec, encode);
	*outlength = r == 0 ? io.wrote : 0;
	return r;
}

/* TODO: Do test with custom buffer */
static inline int test(const int codec, const char *msg, const size_t msglen) {
	assert(msg);
	char compressed[256] = { 0 }, decompressed[256] = { 0 };
	size_t complen = sizeof compressed, decomplen = sizeof decompressed;
	if (msglen > 256)
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
	BUILD_BUG_ON(P  <= 1);
	BUILD_BUG_ON((EI + EJ) > 16); /* unsigned and int used, minimum size is 16 bits */
	BUILD_BUG_ON((N - 1u) & N); /* N must be power of 2 */

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
			const int r = test(j, ts[i], strlen(ts[i]) + 1);
			if (r < 0)
				return r;
		}
	return 0;
}

