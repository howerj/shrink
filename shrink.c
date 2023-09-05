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

#include "shrink.h"
#include <assert.h>
#include <string.h> /* memset, memmove, memcmp, memchr, strlen */
#include <stdint.h>

#define ELINE (-__LINE__)

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

/* It would be a good idea for these parameters to be part of the format, a
 * single bit could be used to determine whether to use the defaults or not
 * so as to save space. The maximum buffer size should also be configurable
 * so embedded systems that are tight on RAM can still use this library. 
 * Sensible ranges would have to be checked for. */
                 /* LZSS Parameters */
#ifndef EI
#define EI (11u) /* dictionary size: typically 10..13 */
#endif
#ifndef RJ
#define EJ (4u)  /* match length:    typically 4..5 */
#endif
#ifndef P
#define P  (2u)  /* If match length <= P then output one character */
#endif
#ifndef CH
#define CH (' ') /* initial dictionary contents */
#endif
 
/* Derived LZSS Parameters */
#define N  (1u << EI)             /* buffer size */
#define F  ((1u << EJ) + (P - 1u)) /* lookahead buffer size */


/* RLE Parameters */
#ifndef RL
#define RL    (128)               /* run length */
#endif
#ifndef ROVER
#define ROVER (1)                 /* encoding only run lengths greater than ROVER + 1 */
#endif

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
		return ELINE;
	return b->b[b->used++];
}

static int buffer_put(const int ch, void *out) {
	buffer_t *b = out;
	assert(b);
	assert(b->b);
	if (b->used >= b->length)
		return ELINE;
	return b->b[b->used++] = ch;
}

static int get(shrink_t *io) {
	assert(io);
	const int r = io->get(io->in);
	io->read += r >= 0;
	assert(r <= 255);
	return r;
}

static int put(const int ch, shrink_t *io) {
	assert(io);
	const int r = io->put(ch, io->out);
	io->wrote += r >= 0;
	assert(r <= 255);
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
			return ELINE;
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
				return ELINE;
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
			return ELINE;
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
		return ELINE;
	for (unsigned mask = 256; mask >>= 1; )
		if (bit_buffer_put_bit(l->io, &l->bit, ch & mask) < 0)
			return ELINE;
	return 0;
}

static int output_reference(lzss_t *l, const unsigned position, const unsigned length) {
	assert(l);
	assert(position < (1 << EI));
	assert(length < ((1 << EJ) + P));
	if (bit_buffer_put_bit(l->io, &l->bit, REFERENCE) < 0)
		return ELINE;
	for (unsigned mask = N; mask >>= 1;)
		if (bit_buffer_put_bit(l->io, &l->bit, position & mask) < 0)
			return ELINE;
	for (unsigned mask = 1 << EJ; mask >>= 1; )
		if (bit_buffer_put_bit(l->io, &l->bit, length & mask) < 0)
			return ELINE;
	return 0;
}

static int shrink_lzss_encode(shrink_t *io) {
	assert(io);
	STATIC lzss_t l = { .bit = { .mask = 128, }, };
	l.io = io; /* need because of STATIC */
	unsigned bufferend = 0;

	if (init(&l, N - F) < 0)
		return ELINE;

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
			if ((y + P - 1) > F) /* maximum length reach, stop search */
				break;
		}
		if (y <= P) { /* is match worth it? */
			y = 1;
			if (output_literal(&l, ch) < 0) /* Not worth it */
				return ELINE;
		} else { /* L'Oreal: Because you're worth it. */
			if (output_reference(&l, x & (N - 1u), y - P) < 0)
				return ELINE;
		}
		assert((r + y) > r);
		assert((s + y) > s);
		r += y;
		s += y;
		if (r >= ((N * 2u) - F)) { /* move and refill buffer */
			BUILD_BUG_ON(sizeof l.buffer < N);
			memmove(l.buffer, l.buffer + N, N);
			assert(bufferend - N < bufferend);
			assert((r - N) < r);
			assert((s - N) < s);
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
	STATIC lzss_t l = { .bit = { .mask = 0, }, };
	l.io = io; /* need because of STATIC */

	if (init(&l, N - F) < 0)
		return ELINE;

	int c = 0;
	for (unsigned r = N - F; (c = bit_buffer_get_n_bits(l.io, &l.bit, 1)) >= 0; ) {
		if (c == LITERAL) { /* control bit: literal, emit a byte */
			if ((c = bit_buffer_get_n_bits(l.io, &l.bit, 8)) < 0)
				break;
			if (put(c, l.io) != c)
				return ELINE;
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
		for (unsigned k = 0; k < j + P; k++) { /* copy (pos,len) to output and dictionary */
			c = l.buffer[(i + k) & (N - 1)];
			if (put(c, l.io) != c)
				return ELINE;
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
		return ELINE;
	for (int i = 0; i < idx; i++)
		if (put(buf[i], io) < 0)
			return ELINE;
	return 0;
}

static int rle_write_run(shrink_t *io, const int count, const int ch) {
	assert(io);
	assert(ch >= 0 && ch < 256);
	assert(count >= 0);
	if (put(count, io) < 0)
		return ELINE;
	if (put(ch, io) < 0)
		return ELINE;
	return 0;
}

static int shrink_rle_encode(shrink_t *io) { /* this could do with simplifying... */
	assert(io);
	uint8_t buf[RL] = { 0, }; /* buffer to store data with no runs */
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
						return ELINE;
					idx = 0;
				}
				if (rle_write_run(io, j - ROVER, prev) < 0)
					return ELINE;
			} else { /* run length is not worth encoding */
				while (j-- >= 0) { /* encode too small run as literal */
					buf[idx++] = prev;
					if (idx == (RL - 1)) {
						if (rle_write_buf(io, buf, idx) < 0)
							return ELINE;
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
				return ELINE;
			idx = 0;
		}
		assert(idx < (RL - 1));
	}
end: /* no more input */
	if (rle_write_buf(io, buf, idx) < 0) /* we might still have something in the buffer though */
		return ELINE;
	return 0;
}

static int shrink_rle_decode(shrink_t *io) {
	assert(io);
	for (int c = 0, count = 0; (c = get(io)) >= 0;) {
		if (c > RL) { /* process run of literal data */
			count = c - RL;
			for (int i = 0; i < count; i++) {
				if ((c = get(io)) < 0)
					return ELINE;
				if (put(c, io) != c)
					return ELINE;
			}
			continue;
		}
		/* process repeated byte */
		count = c + 1 + ROVER;
		if ((c = get(io)) < 0)
			return ELINE;
		for (int i = 0; i < count; i++)
			if (put(c, io) != c)
				return ELINE;
	}
	return 0;
}

static int gamma_size(unsigned v) {
	int sz = 1;
	
	while (v) {
		v--;
		sz += 2;
		v >>= 1;
	}

	return sz;
}

/* Elias Gamma Encoding, with 256 as a terminal value.

	   0: 0
	 1-2: 10x     - 0=1, 1=2
	 3-6: 110xx   - 00=3, 01=4, 10=5, 11=6
	7-14: 1110xxx - 000=7, 001=8, 010=9, 011=10, 100=11, 101=12, 110=13, 111=14

The Elias Gamma CODEC should be combined with LZSS to produce
a better format. 

The Elias-Gamma encoder should also be turned into a set
of functions that can be reused throughout this code. */

#define ELIAS_BITS (4)
#define ELIAS_TERMINAL (1 + (1 << ELIAS_BITS))

static int shrink_elias_encode(shrink_t *io) {
	assert(io);
	bit_buffer_t buf = { .mask = 128, }, ibuf = { .mask = 0, };
	for (int end = 0;!end;) {
		int c = bit_buffer_get_n_bits(io, &ibuf, ELIAS_BITS);
		if (c < 0) {
			c = ELIAS_TERMINAL;
			end = 1;
		}
		const int bit_sz = (gamma_size(c) - 1) / 2;
		int bit_msk = 1;

		for (int x = 0; x < bit_sz; x++) {
			if (bit_buffer_put_bit(io, &buf, 1) < 0)
				return ELINE;
			bit_msk <<= 1;
		}
		if (bit_buffer_put_bit(io, &buf, 0) < 0)
			return ELINE;
		c++;
		bit_msk >>= 1;

		for (int x = 0; x < bit_sz; x++) {
			if (bit_buffer_put_bit(io, &buf, c & bit_msk) < 0)
				return ELINE;
			bit_msk >>= 1;
		}
	}
	if (bit_buffer_flush(io, &buf) < 0)
		return ELINE;
	return 0;
}

static int shrink_elias_decode(shrink_t *io) {
	assert(io);
	bit_buffer_t buf = { .mask = 0, }, obuf = { .mask = 128, };
	for (;;) {
		int v = 1;
		int bit_count = 0;
		for (;;) {
			const int r = bit_buffer_get_n_bits(io, &buf, 1);
			if (r < 0) {
				if (bit_count > 0)
					return ELINE;
				break;
			}
			if (r == 0)
				break;
			/*assert(bit_count < INT_MAX);*/
			bit_count++;
		}
		while (bit_count--) {
			v <<= 1;
			const int r = bit_buffer_get_n_bits(io, &buf, 1);
			if (r < 0)
				return ELINE;
			if (r)
				v++;
			if (v > ELIAS_TERMINAL)
				return 0;
		}
		v--;
		assert(v >= 0);
		assert(v <= ELIAS_TERMINAL);
		for (int x = 0; x < ELIAS_BITS; x++) {
			if (bit_buffer_put_bit(io, &obuf, v & (1 << (ELIAS_BITS - 1))) < 0)
				return ELINE;
			v <<= 1;
		}
		/*if (put(v, io) < 0)
			return ELINE;*/
	}
	if (bit_buffer_flush(io, &obuf) < 0)
		return ELINE;
	return 0;
}

#if 0
/* Move To Front CODEC 
 *
 * Author: Richard James Howe
 * License: The Unlicense
 * Email: howe.r.j.89@gmail.com
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#define UNUSED(X) ((void)(X))
#ifdef _WIN32 /* Used to unfuck file mode for "Win"dows. Text mode is for losers. */
#include <windows.h>
#include <io.h>
#include <fcntl.h>
static void binary(FILE *f) { _setmode(_fileno(f), _O_BINARY); }
#else
static inline void binary(FILE *f) { UNUSED(f); }
#endif

#define ELEM (256)

static int imtf(unsigned char *model, FILE *in, FILE *out) {
	assert(model);
	assert(in);
	assert(out);
	if (init(model) < 0)
		return -1;
	for (int ch = 0; (ch = fgetc(in)) != EOF;) {
		assert(ch >= 0 && ch <= ELEM);
		const int e = model[ch];
		memmove(model + 1, model, ch);
		model[0] = e;
		if (fputc(e, out) < 0)
			return -1;
	}
	return 0;
}

int main(int argc, char **argv) {
	static unsigned char model[ELEM];
	binary(stdin);
	binary(stdout);
	if (argc != 2) {
		(void)fprintf(stderr, "usage: %s -d or -e\n", argv[0]);
		return 1;
	}
	if (!strcmp("-e", argv[1]))
		return mtf(model, stdin, stdout) < 0;
	if (!strcmp("-d", argv[1]))
		return imtf(model, stdin, stdout) < 0;
	return 1;
}
#endif

#define ELEM (256)

static int mtf_init(unsigned char *model) {
	assert(model);
	for (size_t i = 0; i < ELEM; i++)
		model[i] = i;
	return 0;
}

static int mtf_find(const unsigned char *model, int ch) {
	assert(model);
	for (size_t i = 0; i < ELEM; i++)
		if (ch == model[i])
			return i;
	return -1;
}

static int mtf_update(unsigned char *model, const int index) {
	assert(model);
	assert(index >= 0 && index < ELEM);
	const int m = model[index];
	memmove(model + 1, model, index);
	model[0] = m;
	return index;
}

static int shrink_mtf_encode(shrink_t *io) {
	assert(io);
	unsigned char model[ELEM];
	if (mtf_init(model) < 0)
		return -1;
	for (int ch = 0; (ch = get(io)) >= 0;)
		if (put(mtf_update(model, mtf_find(model, ch)), io) < 0)
			return -1;
	return 0;
}

static int shrink_mtf_decode(shrink_t *io) {
	assert(io);
	unsigned char model[ELEM];
	if (mtf_init(model) < 0)
		return -1;
	for (int ch = 0; (ch = get(io)) >= 0;) {
		assert(ch >= 0 && ch <= ELEM);
		const int e = model[ch];
		memmove(model + 1, model, ch);
		model[0] = e;
		if (put(e, io) < 0)
			return -1;
	}

	return 0;
}

int shrink(shrink_t *io, const int codec, const int encode) {
	assert(io);
	switch (codec) {
	/* N.B. Each CODEC should be optional, as in it should be possible
	 * to compile out each CODEC to save space. */
	case CODEC_RLE:  return encode ? shrink_rle_encode(io)  : shrink_rle_decode(io);
	case CODEC_LZSS: return encode ? shrink_lzss_encode(io) : shrink_lzss_decode(io);
	case CODEC_ELIAS: return encode ? shrink_elias_encode(io) : shrink_elias_decode(io);
	case CODEC_MTF: return encode ? shrink_mtf_encode(io) : shrink_mtf_decode(io);
	}
	never;
	return ELINE;
}

int shrink_buffer(const int codec, const int encode, const char *in, const size_t inlength, char *out, size_t *outlength) {
	assert(in);
	assert(out);
	assert(outlength);
	buffer_t ib = { .b = (unsigned char*)in,  .used = 0, .length = inlength, };
	buffer_t ob = { .b = (unsigned char*)out, .used = 0, .length = *outlength, };
	shrink_t io = { .get = buffer_get, .put = buffer_put, .in  = &ib, .out = &ob, };
	const int r = shrink(&io, codec, encode);
	*outlength = r == 0 ? io.wrote : 0;
	return r;
}

#define TBUFL (512u)

static inline int test(const int codec, const char *msg, const size_t msglen) {
	assert(msg);
	char compressed[TBUFL] = { 0, }, decompressed[TBUFL] = { 0, };
	size_t complen = sizeof compressed, decomplen = sizeof decompressed;
	if (msglen > TBUFL)
		return ELINE;
	const int r1 = shrink_buffer(codec, 1, msg,        msglen,  compressed,   &complen);
	if (r1 < 0)
		return r1;
	const int r2 = shrink_buffer(codec, 0, compressed, complen, decompressed, &decomplen);
	if (r2 < 0)
		return r2;
	if (msglen != decomplen)
		return ELINE;
	if (memcmp(msg, decompressed, msglen))
		return ELINE;
	return 0;
}

int shrink_tests(void) {
	BUILD_BUG_ON(EI > 15); /* 1 << EI would be larger than smallest possible INT_MAX */
	BUILD_BUG_ON(EI < 6);  /* no point in encoding */
	BUILD_BUG_ON(EJ > EI); /* match length needs to be smaller than thing we are matching in */
	BUILD_BUG_ON(RL > 128);
	BUILD_BUG_ON(P  <= 1);
	BUILD_BUG_ON((EI + EJ) > 16); /* unsigned and int used, minimum size is 16 bits */
	BUILD_BUG_ON((N - 1) & N); /* N must be power of 2 */

	if (!DEBUGGING)
		return 0;

	char *ts[] = {
		"If not to heaven, then hand in hand to hell",
		"",
		"aaaaaaaaaabbbbbbbbccddddddeeeeeeeefffffffhh",
		"I am Sam\nSam I am\nThat Sam-I-am!\n\
		That Sam-I-am!\nI do not like\nthat Sam-I-am!\n\
		Do you like green eggs and ham?\n\
		I do not like them, Sam-I-am.\n\
		I do not like green eggs and ham.\n"
	};

	for (size_t i = 0; i < (sizeof ts / sizeof (ts[0])); i++)
		for (int j = CODEC_RLE; j <= CODEC_MTF; j++) {
			const int r = test(j, ts[i], strlen(ts[i]) + 1);
			if (r < 0)
				return r;
		}
	return 0;
}

