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
 * chained together. Also of note, as a generic filter library this could
 * be used for things other than compression, like error correction codes. */

#include "shrink.h"
#include <assert.h>
#include <string.h> /* memset, memmove, memcmp, memchr, strlen */
#include <stdint.h>

#ifndef SHRINK_VERSION
#define SHRINK_VERSION (0x000000ul) /* all zeros indicates and error */
#endif

#ifdef NDEBUG
#define DEBUGGING (0)
#else
#define DEBUGGING (1)
#endif

#ifdef USE_STATIC /* large internal buffers are statically allocated */
#define STATIC_ON (1)
#define STATIC static
#else  /* large internal buffers are allocated on stack */
#define STATIC_ON (0)
#define STATIC auto
#endif

#define implies(P, Q)             assert(!(P) || (Q)) /* material implication, immaterial if NDEBUG defined */
#define never                     assert(0)
#define BUILD_BUG_ON(condition)   ((void)sizeof(char[1 - 2*!!(condition)]))

                                  /* RLE Parameters */
#define RL    (128)               /* run length */
#define ROVER (1)                 /* encoding only run lengths greater than ROVER + 1 */

#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))

enum { REFERENCE, LITERAL, };

typedef struct {
	unsigned buffer, mask;
} bit_buffer_t;

typedef struct {
	unsigned char *b;
	size_t used, length;
} buffer_t;

typedef struct {
	uint8_t buffer[(1ul << 11) * 2u];
	uint8_t *init;
	size_t init_length;
	unsigned EI, EJ, P, N, F, CH, defaults;
	shrink_t *io;
	bit_buffer_t bit;
} lzss_t;

int shrink_version(unsigned long *version) {
	assert(version);
	unsigned long options = 0;
	options |= DEBUGGING << 0;
	options |= STATIC_ON << 1;
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

static int check(lzss_t *l) {
	assert(l);
	if (l->EI > 15) return -1; /* 1 << EI would be larger than smaller possible INT_MAX */
	if (l->EI < 6) return -1; /* No point in encoding */
	if ((l->N * 2ul) > sizeof(l->buffer)) return -1; /* buffer too small */
	if (l->P <= 1 /*|| l->P > PMAX*/) return -1;
	if ((l->N - 1u) & l->N) return -1;  /* N must be power of 2 */
	if (l->EJ > l->EI) return -1; /* match length needs to be smaller than thing we are matching in */
	if ((l->EI + l->EJ) > 16) return -1; /* unsigned and int used, minimum size is 16 bits */
	return 0;
}

static int init(lzss_t *l) {
	assert(l);
	if (l->defaults) {
		l->EI = 11;                    /* dictionary size: typically 10..13 */
		l->EJ = 4;                     /* match length: typically 4..5 */
		l->P  = 2;                     /* if match length <= P then output one character */
		l->CH = ' ';                   /* initial dictionary contents */
	}
	l->N  = 1u << l->EI;                   /* buffer size */
	l->F  = ((1u << l->EJ) + (l->P - 1u)); /* lookahead buffer size */
	if (check(l) < 0)
		return -1;
	const size_t length = l->N - l->F;
	assert(length < sizeof l->buffer);
	memset(l->buffer, l->CH, length);
	if (l->init)
		memcpy(l->buffer, l->init, MIN(length, l->init_length));
	return 0;
}

static int output(lzss_t *l, unsigned ch) {
	assert(l);
	for (unsigned mask = 256; mask >>= 1; )
		if (bit_buffer_put_bit(l->io, &l->bit, ch & mask) < 0)
			return -1;
	return 0;
}

static int output_literal(lzss_t *l, const unsigned ch) {
	assert(l);
	if (bit_buffer_put_bit(l->io, &l->bit, LITERAL) < 0)
		return -1;
	return output(l, ch);
}

static int output_reference(lzss_t *l, const unsigned position, const unsigned length) {
	assert(l);
	assert(position < (1u << l->EI));
	assert(length < ((1u << l->EJ) + l->P));
	if (bit_buffer_put_bit(l->io, &l->bit, REFERENCE) < 0)
		return -1;
	for (unsigned mask = l->N; mask >>= 1;)
		if (bit_buffer_put_bit(l->io, &l->bit, position & mask) < 0)
			return -1;
	for (unsigned mask = 1 << l->EJ; mask >>= 1; )
		if (bit_buffer_put_bit(l->io, &l->bit, length & mask) < 0)
			return -1;
	return 0;
}

static int shrink_lzss_encode(shrink_t *io, lzss_t *l) {
	assert(io);
	assert(l);
	l->io = io; /* need because of STATIC */
	l->bit.mask = 128;
	unsigned bufferend = 0;

	if (bit_buffer_put_bit(l->io, &l->bit, l->defaults) < 0)
		return -1;
	if (l->defaults == 0) {
		l->EI = 11;
		l->EJ = 4;
		l->P  = 2;
		l->CH = ' ';
		assert(l->EI < 16);
		assert(l->EJ < 16);
		assert(l->P  >= 2);
		const uint8_t prr = l->P  | (!!(l->init) << 4);
		const uint8_t eij = l->EI | (l->EJ << 4);
		if (output(l, prr) < 0)
			return -1;
		if (output(l, eij) < 0)
			return -1;
	}

	if (init(l) < 0)
		return -1;

	for (bufferend = l->N - l->F; bufferend < l->N * 2u; bufferend++) {
		const int c = get(l->io);
		if (c < 0)
			break;
		l->buffer[bufferend] = c;
	}

	/* NB. More efficient matches might be found by delaying encoding,
	 * as a longer match might happen if we take more input.  */
	for (unsigned r = l->N - l->F, s = 0; r < bufferend; ) {
		unsigned x = 0, y = 1;
		const int ch = l->buffer[r];
		for (unsigned i = s; i < r; i++) { /* search for longest match */
			assert(r >= r - i);
			uint8_t *m = memchr(&l->buffer[i], ch, r - i); /* match first char */
			if (!m)
				break;
			assert(i < sizeof l->buffer);
			i += m - &l->buffer[i];
			const unsigned f1 = (l->F <= bufferend - r) ? l->F : bufferend - r;
			unsigned j = 1;
			for (j = 1; j < f1; j++) { /* run of matches */
				assert((i + j) < sizeof l->buffer);
				assert((r + j) < sizeof l->buffer);
				if (l->buffer[i + j] != l->buffer[r + j])
					break;
			}
			if (j > y) {
				x = i; /* match position */
				y = j; /* match length */
			}
			if ((y + l->P - 1u) > l->F) /* maximum length reach, stop search */
				break;
		}
		if (y <= l->P) { /* is match worth it? */
			y = 1;
			if (output_literal(l, ch) < 0) /* Not worth it */
				return -1;
		} else { /* L'Oreal: Because you're worth it. */
			if (output_reference(l, x & (l->N - 1u), y - l->P) < 0)
				return -1;
		}
		assert(r + y > r);
		assert(s + y > s);
		r += y;
		s += y;
		if (r >= ((l->N * 2u) - l->F)) { /* move and refill buffer */
			assert(sizeof (l->buffer) >= (l->N * 2u));
			memmove(l->buffer, l->buffer + l->N, l->N);
			assert(bufferend - l->N < bufferend);
			assert(r - l->N < r);
			assert(s - l->N < s);
			bufferend -= l->N;
			r -= l->N;
			s -= l->N;
			while (bufferend < (l->N * 2u)) {
				int c = get(l->io);
				if (c < 0)
					break;
				assert(bufferend < sizeof(l->buffer));
				l->buffer[bufferend++] = c;
			}
		}
	}
	return bit_buffer_flush(l->io, &l->bit);
}

static int shrink_lzss_decode(shrink_t *io, lzss_t *l) { /* NB. A tiny standalone decoder would be useful */
	assert(io);
	assert(l);
	l->io = io;
	int c = bit_buffer_get_n_bits(l->io, &l->bit, 1);
	if (c < 0)
		return -1;
	l->defaults = c;
	if (l->defaults == 0) { /* TODO: Allow user to set custom settings */
		const int prr = bit_buffer_get_n_bits(l->io, &l->bit, 8);
		const int eij = bit_buffer_get_n_bits(l->io, &l->bit, 8);
		if (eij < 0 || prr < 0)
			return -1;
		l->CH = ' ';
		l->EI = (eij >> 0) & 0xFu;
		l->EJ = (eij >> 4) & 0xFu;
		l->P  = (prr >> 0) & 0xFu;
		if ((prr >> 4) & 0xE) /* reserved bits, should be zero */
			return -1;
		const int custom = (prr >> 4) & 1u;
		if (custom && !(l->init)) /* TODO: Allow user to set custom initial dictionary contents */
			return -1;
	}

	if (init(l) < 0)
		return -1;

	for (unsigned r = l->N - l->F; (c = bit_buffer_get_n_bits(l->io, &l->bit, 1)) >= 0; ) {
		if (c == LITERAL) { /* control bit: literal, emit a byte */
			if ((c = bit_buffer_get_n_bits(l->io, &l->bit, 8)) < 0)
				break;
			if (put(c, l->io) != c)
				return -1;
			l->buffer[r++] = c;
			r &= (l->N - 1u); /* wrap around */
			continue;
		}
		const int i = bit_buffer_get_n_bits(l->io, &l->bit, l->EI); /* position */
		if (i < 0)
			break;
		const int j = bit_buffer_get_n_bits(l->io, &l->bit, l->EJ); /* length */
		if (j < 0)
			break;
		for (unsigned k = 0; k < j + l->P; k++) { /* copy (pos,len) to output and dictionary */
			c = l->buffer[(i + k) & (l->N - 1)];
			if (put(c, l->io) != c)
				return -1;
			l->buffer[r++] = c;
			r &= (l->N - 1u); /* wrap around */
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

static int shrink_lzss(shrink_t *io, const int encode) {
	STATIC lzss_t l = { .bit = { .mask = 0 }, .defaults = 1, };
	return encode ? shrink_lzss_encode(io, &l) : shrink_lzss_decode(io, &l);
}

/* TODO: Allow custom LZSS initial dictionary contents to be entered? */
int shrink(shrink_t *io, const int codec, const int encode) {
	assert(io);
	switch (codec) {
	case CODEC_RLE:  return encode ? shrink_rle_encode(io)  : shrink_rle_decode(io);
	case CODEC_LZSS: return shrink_lzss(io, encode);
	}
	never;
	return -1;
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
			const int r = test(j, ts[i], strlen(ts[i]) + 1);
			if (r < 0)
				return r;
		}
	return 0;
}

