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
 * LZSS parameters, which can only be changed at compile time.
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
 * well as small string compression. 
 * 
 * TODO:
 * - [ ] Turn in to header-only library
 * - [ ] Add LZ_XOR CODEC, see:
 *   * <https://news.ycombinator.com/item?id=32401548>,
 *   * <https://richg42.blogspot.com/2022/01/lzxor.html>
 *   * <https://richg42.blogspot.com/2023/05/lzxorlzadd-progress.html>
 *   * <http://bitmagic.io/bm-xor.html>
 * - [ ] More generic Elias coding functions / Golomb codes
 *   - [ ] Elias-Gamma encode LZSS length
 * - [ ] Remove version function, define a MACRO instead.
 * - [ ] Remove usage of even small buffers
 * - [ ] Change options / version info
 * - [ ] Skip disabled tests
 */

#include "shrink.h"
#include <assert.h>
#include <string.h> /* memset, memmove, memcmp, memchr, strlen */
#include <stdint.h>

#define SHRINK_ELINE (-__LINE__)

#ifndef SHRINK_RLE_ENABLE
#define SHRINK_RLE_ENABLE (1)
#endif

#ifndef SHRINK_LZSS_ENABLE
#define SHRINK_LZSS_ENABLE (1)
#endif

#ifndef SHRINK_ELIAS_ENABLE
#define SHRINK_ELIAS_ENABLE (1)
#endif

#ifndef SHRINK_MTF_ENABLE
#define SHRINK_MTF_ENABLE (1)
#endif

#ifndef SHRINK_LZP_ENABLE
#define SHRINK_LZP_ENABLE (1)
#endif


#ifndef SHRINK_VERSION
#define SHRINK_VERSION (0x000000ul) /* all zeros indicates and error */
#endif

#ifdef NDEBUG
#define SHRINK_DEBUGGING (0)
#else
#define SHRINK_DEBUGGING (1)
#endif

#define shrink_implies(P, Q)      assert(!(P) || (Q)) /* material implication, immaterial if NDEBUG defined */
#define shrink_never              assert(0)
#define BUILD_BUG_ON(condition)   ((void)sizeof(char[1 - 2*!!(condition)]))

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
 
/* Derived LZSS Parameters */
#define SHRINK_N  (1u << SHRINK_EI)             /* buffer size */
#define SHRINK_F  ((1u << SHRINK_EJ) + (SHRINK_P - 1u)) /* lookahead buffer size */

/* RLE Parameters */
#ifndef SHRINK_RL
#define SHRINK_RL    (128)               /* run length */
#endif
#ifndef SHRINK_ROVER
#define SHRINK_ROVER (1)                 /* encoding only run lengths greater than ROVER + 1 */
#endif

#ifndef MIN
#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#endif

enum { SHRINK_REFERENCE, SHRINK_LITERAL, };

typedef struct {
	unsigned buffer, mask;
} shrink_bit_buffer_t;

typedef struct {
	unsigned char *b;
	size_t used, length;
} shrink_buffer_t;

typedef struct {
	shrink_t *io;
	shrink_bit_buffer_t bit;
} shrink_lzss_t;

int shrink_version(unsigned long *version) {
	assert(version);
	unsigned long options = 0;
	options |= SHRINK_DEBUGGING << 0;
	*version = (options << 24) | SHRINK_VERSION;
	return SHRINK_VERSION == 0 ? -1 : 0;
}

static int shrink_buffer_get(void *in) {
	shrink_buffer_t *b = in;
	assert(b);
	assert(b->b);
	if (b->used >= b->length)
		return SHRINK_ELINE;
	return b->b[b->used++];
}

static int shrink_buffer_put(const int ch, void *out) {
	shrink_buffer_t *b = out;
	assert(b);
	assert(b->b);
	if (b->used >= b->length)
		return SHRINK_ELINE;
	return b->b[b->used++] = ch;
}

static int shrink_get(shrink_t *io) {
	assert(io);
	const int r = io->get(io->in);
	io->read += r >= 0;
	assert(r <= 255);
	return r;
}

static int shrink_put(const int ch, shrink_t *io) {
	assert(io);
	const int r = io->put(ch, io->out);
	io->wrote += r >= 0;
	assert(r <= 255);
	return r;
}

/* TODO: Auto-initialization if init flag not present, with mask=128|mask=0 */
static int shrink_bit_buffer_put_bit(shrink_t *io, shrink_bit_buffer_t *bit, const unsigned one) {
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

static int shrink_bit_buffer_get_n_bits(shrink_t *io, shrink_bit_buffer_t *bit, unsigned n) {
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

static int shrink_bit_buffer_flush(shrink_t *io, shrink_bit_buffer_t *bit) {
	assert(io);
	assert(bit);
	if (bit->mask != 128)
		if (shrink_put(bit->buffer, io) < 0)
			return -1;
	bit->mask = 0;
	return 0;
}

static int shrink_output_literal(shrink_lzss_t *l, const unsigned ch) {
	assert(l);
	if (shrink_bit_buffer_put_bit(l->io, &l->bit, SHRINK_LITERAL) < 0)
		return SHRINK_ELINE;
	for (unsigned mask = 256; mask >>= 1; )
		if (shrink_bit_buffer_put_bit(l->io, &l->bit, ch & mask) < 0)
			return SHRINK_ELINE;
	return 0;
}

static int shrink_output_reference(shrink_lzss_t *l, const unsigned position, const unsigned length) {
	assert(l);
	assert(position < (1 << SHRINK_EI));
	assert(length < ((1 << SHRINK_EJ) + SHRINK_P));
	if (shrink_bit_buffer_put_bit(l->io, &l->bit, SHRINK_REFERENCE) < 0)
		return SHRINK_ELINE;
	for (unsigned mask = SHRINK_N; mask >>= 1;)
		if (shrink_bit_buffer_put_bit(l->io, &l->bit, position & mask) < 0)
			return SHRINK_ELINE;
	for (unsigned mask = 1 << SHRINK_EJ; mask >>= 1; )
		if (shrink_bit_buffer_put_bit(l->io, &l->bit, length & mask) < 0)
			return SHRINK_ELINE;
	return 0;
}

static int shrink_lzss_encode(shrink_t *io) {
	assert(io);
	shrink_lzss_t l = { .bit = { .mask = 128, }, .io = io, };
	unsigned bufferend = 0;

	if ((SHRINK_N * 2ul) > l.io->buffer_length)
		return SHRINK_ELINE;

	for (bufferend = SHRINK_N - SHRINK_F; bufferend < SHRINK_N * 2u; bufferend++) {
		const int c = shrink_get(l.io);
		if (c < 0)
			break;
		l.io->buffer[bufferend] = c;
	}

	for (unsigned r = SHRINK_N - SHRINK_F, s = 0; r < bufferend; ) {
		unsigned x = 0, y = 1;
		const int ch = l.io->buffer[r];
		for (unsigned i = s; i < r; i++) { /* search for longest match */
			assert(r >= r - i);
			uint8_t *m = memchr(&l.io->buffer[i], ch, r - i); /* match first char */
			if (!m)
				break;
			assert(i < l.io->buffer_length);
			i += m - &l.io->buffer[i];
			const unsigned f1 = (SHRINK_F <= bufferend - r) ? SHRINK_F : bufferend - r;
			unsigned j = 1;
			for (j = 1; j < f1; j++) { /* run of matches */
				assert((i + j) < l.io->buffer_length);
				assert((r + j) < l.io->buffer_length);
				if (l.io->buffer[i + j] != l.io->buffer[r + j])
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
				return SHRINK_ELINE;
		} else { /* L'Oreal: Because you're worth it. */
			if (shrink_output_reference(&l, x & (SHRINK_N - 1u), y - SHRINK_P) < 0)
				return SHRINK_ELINE;
		}
		assert((r + y) > r);
		assert((s + y) > s);
		r += y;
		s += y;
		if (r >= ((SHRINK_N * 2u) - SHRINK_F)) { /* move and refill buffer */
			assert(SHRINK_N <= l.io->buffer_length);
			memmove(l.io->buffer, l.io->buffer + SHRINK_N, SHRINK_N);
			assert(bufferend - SHRINK_N < bufferend);
			assert((r - SHRINK_N) < r);
			assert((s - SHRINK_N) < s);
			bufferend -= SHRINK_N;
			r -= SHRINK_N;
			s -= SHRINK_N;
			while (bufferend < (SHRINK_N * 2u)) {
				int c = shrink_get(l.io);
				if (c < 0)
					break;
				assert(bufferend < l.io->buffer_length);
				l.io->buffer[bufferend++] = c;
			}
		}
	}
	return shrink_bit_buffer_flush(l.io, &l.bit);
}

static int shrink_lzss_decode(shrink_t *io) {
	assert(io);
	shrink_lzss_t l = { .bit = { .mask = 0, }, .io = io, };

	if (SHRINK_N * 2ul > l.io->buffer_length)
		return SHRINK_ELINE;

	int c = 0;
	for (unsigned r = SHRINK_N - SHRINK_F; (c = shrink_bit_buffer_get_n_bits(l.io, &l.bit, 1)) >= 0; ) {
		if (c == SHRINK_LITERAL) { /* control bit: literal, emit a byte */
			if ((c = shrink_bit_buffer_get_n_bits(l.io, &l.bit, 8)) < 0)
				break;
			if (shrink_put(c, l.io) != c)
				return SHRINK_ELINE;
			l.io->buffer[r++] = c;
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
			c = l.io->buffer[(i + k) & (SHRINK_N - 1)];
			if (shrink_put(c, l.io) != c)
				return SHRINK_ELINE;
			l.io->buffer[r++] = c;
			r &= (SHRINK_N - 1u); /* wrap around */
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
	if (shrink_put(idx + SHRINK_RL, io) < 0)
		return SHRINK_ELINE;
	for (int i = 0; i < idx; i++)
		if (shrink_put(buf[i], io) < 0)
			return SHRINK_ELINE;
	return 0;
}

static int rle_write_run(shrink_t *io, const int count, const int ch) {
	assert(io);
	assert(ch >= 0 && ch < 256);
	assert(count >= 0);
	if (shrink_put(count, io) < 0)
		return SHRINK_ELINE;
	if (shrink_put(ch, io) < 0)
		return SHRINK_ELINE;
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
					if (rle_write_buf(io, buf, idx) < 0)
						return SHRINK_ELINE;
					idx = 0;
				}
				if (rle_write_run(io, j - SHRINK_ROVER, prev) < 0)
					return SHRINK_ELINE;
			} else { /* run length is not worth encoding */
				while (j-- >= 0) { /* encode too small run as literal */
					buf[idx++] = prev;
					if (idx == (SHRINK_RL - 1)) {
						if (rle_write_buf(io, buf, idx) < 0)
							return SHRINK_ELINE;
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
		buf[idx++] = c;
		if (idx == (SHRINK_RL - 1)) {
			if (rle_write_buf(io, buf, idx) < 0)
				return SHRINK_ELINE;
			idx = 0;
		}
		assert(idx < (SHRINK_RL - 1));
	}
end: /* no more input */
	if (rle_write_buf(io, buf, idx) < 0) /* we might still have something in the buffer though */
		return SHRINK_ELINE;
	return 0;
}

static int shrink_rle_decode(shrink_t *io) {
	assert(io);
	for (int c = 0, count = 0; (c = shrink_get(io)) >= 0;) {
		if (c > SHRINK_RL) { /* process run of literal data */
			count = c - SHRINK_RL;
			for (int i = 0; i < count; i++) {
				if ((c = shrink_get(io)) < 0)
					return SHRINK_ELINE;
				if (shrink_put(c, io) != c)
					return SHRINK_ELINE;
			}
			continue;
		}
		/* process repeated byte */
		count = c + 1 + SHRINK_ROVER;
		if ((c = shrink_get(io)) < 0)
			return SHRINK_ELINE;
		for (int i = 0; i < count; i++)
			if (shrink_put(c, io) != c)
				return SHRINK_ELINE;
	}
	return 0;
}

static int shrink_gamma_size(unsigned v) {
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

#define SHRINK_ELIAS_BITS (4)
#define SHRINK_ELIAS_TERMINAL (1 + (1 << SHRINK_ELIAS_BITS))

static int shrink_elias_encode(shrink_t *io) {
	assert(io);
	shrink_bit_buffer_t buf = { .mask = 128, }, ibuf = { .mask = 0, };
	for (int end = 0;!end;) {
		int c = shrink_bit_buffer_get_n_bits(io, &ibuf, SHRINK_ELIAS_BITS);
		if (c < 0) {
			c = SHRINK_ELIAS_TERMINAL;
			end = 1;
		}
		const int bit_sz = (shrink_gamma_size(c) - 1) / 2;
		int bit_msk = 1;

		for (int x = 0; x < bit_sz; x++) {
			if (shrink_bit_buffer_put_bit(io, &buf, 1) < 0)
				return SHRINK_ELINE;
			bit_msk <<= 1;
		}
		if (shrink_bit_buffer_put_bit(io, &buf, 0) < 0)
			return SHRINK_ELINE;
		c++;
		bit_msk >>= 1;

		for (int x = 0; x < bit_sz; x++) {
			if (shrink_bit_buffer_put_bit(io, &buf, c & bit_msk) < 0)
				return SHRINK_ELINE;
			bit_msk >>= 1;
		}
	}
	if (shrink_bit_buffer_flush(io, &buf) < 0)
		return SHRINK_ELINE;
	return 0;
}

static int shrink_elias_decode(shrink_t *io) {
	assert(io);
	shrink_bit_buffer_t buf = { .mask = 0, }, obuf = { .mask = 128, };
	for (;;) {
		int v = 1;
		int bit_count = 0;
		for (;;) {
			const int r = shrink_bit_buffer_get_n_bits(io, &buf, 1);
			if (r < 0) {
				if (bit_count > 0)
					return SHRINK_ELINE;
				break;
			}
			if (r == 0)
				break;
			/*assert(bit_count < INT_MAX);*/
			bit_count++;
		}
		while (bit_count--) {
			v <<= 1;
			const int r = shrink_bit_buffer_get_n_bits(io, &buf, 1);
			if (r < 0)
				return SHRINK_ELINE;
			if (r)
				v++;
			if (v > SHRINK_ELIAS_TERMINAL)
				return 0;
		}
		v--;
		assert(v >= 0);
		assert(v <= SHRINK_ELIAS_TERMINAL);
		for (int x = 0; x < SHRINK_ELIAS_BITS; x++) {
			if (shrink_bit_buffer_put_bit(io, &obuf, v & (1 << (SHRINK_ELIAS_BITS - 1))) < 0)
				return SHRINK_ELINE;
			v <<= 1;
		}
	}
	if (shrink_bit_buffer_flush(io, &obuf) < 0)
		return SHRINK_ELINE;
	return 0;
}

#define SHRINK_MTF_ELEMENTS (256)

static int shrink_mtf_init(unsigned char *model) {
	assert(model);
	for (size_t i = 0; i < SHRINK_MTF_ELEMENTS; i++)
		model[i] = i;
	return 0;
}

static int shrink_mtf_find(const unsigned char *model, int ch) {
	assert(model);
	for (size_t i = 0; i < SHRINK_MTF_ELEMENTS; i++)
		if (ch == model[i])
			return i;
	return SHRINK_ELINE;
}

static int shrink_mtf_update(unsigned char *model, const int index) {
	assert(model);
	assert(index >= 0 && index < SHRINK_MTF_ELEMENTS);
	const int m = model[index];
	memmove(model + 1, model, index);
	model[0] = m;
	return index;
}

static int shrink_mtf_encode(shrink_t *io) {
	assert(io);
	unsigned char model[SHRINK_MTF_ELEMENTS];
	if (shrink_mtf_init(model) < 0)
		return SHRINK_ELINE;
	for (int ch = 0; (ch = shrink_get(io)) >= 0;)
		if (shrink_put(shrink_mtf_update(model, shrink_mtf_find(model, ch)), io) < 0)
			return SHRINK_ELINE;
	return 0;
}

static int shrink_mtf_decode(shrink_t *io) {
	assert(io);
	unsigned char model[SHRINK_MTF_ELEMENTS];
	if (shrink_mtf_init(model) < 0)
		return SHRINK_ELINE;
	for (int ch = 0; (ch = shrink_get(io)) >= 0;) {
		assert(ch >= 0 && ch <= SHRINK_MTF_ELEMENTS);
		const int e = model[ch];
		memmove(model + 1, model, ch);
		model[0] = e;
		if (shrink_put(e, io) < 0)
			return SHRINK_ELINE;
	}

	return 0;
}

#ifndef SHRINK_LZP_HASH_ORDER
#define SHRINK_LZP_HASH_ORDER (16l)
#endif

#ifndef SHRINK_LZP_BYTE_LENGTH
#define SHRINK_LZP_BYTE_LENGTH (8)
#endif

#define SHRINK_LZP_HASH_SIZE  (1ul << SHRINK_LZP_HASH_ORDER)

static inline uint16_t shrink_lzp_hash(const uint16_t h, const uint16_t x) {
	return (h << 4) ^ x;
}

int shrink_lzp_encode(shrink_t *io) {
	assert(io);
	assert(io->buffer);

	if (io->buffer_length < SHRINK_LZP_HASH_SIZE)
		return SHRINK_ELINE;

	uint8_t *table = (uint8_t*)io->buffer, buf[SHRINK_LZP_BYTE_LENGTH + 1] = { 0, };
	uint16_t hash = 0;
	for (;;) {
		long i = 0, j = 1, ch = 0, mask = 0;
		for (i = 0; i < SHRINK_LZP_BYTE_LENGTH; i++) {
			ch = shrink_get(io);
			if (ch < 0)
				break;
			assert(hash < io->buffer_length);
			if (ch == table[hash]) {
				mask |= 1 << i;
			} else {
				assert(hash < io->buffer_length);
				table[hash] = ch;
				assert(j < (int)sizeof (buf));
				buf[j++] = ch;
			}
			hash = shrink_lzp_hash(hash, ch);
		}
		if (i > 0) {
			buf[0] = mask;
			for (int k = 0; k < j; k++) {
				assert(k < (int)sizeof (buf));
				if (shrink_put(buf[k], io) < 0)
					return SHRINK_ELINE;
			}
		}
		if (ch < 0)
			break;
	}

	return 0;
}

int shrink_lzp_decode(shrink_t *io) {
	assert(io);
	assert(io->buffer);
	if (io->buffer_length < SHRINK_LZP_HASH_SIZE)
		return SHRINK_ELINE;

	uint8_t *table = (uint8_t*)io->buffer, buf[SHRINK_LZP_BYTE_LENGTH] = { 0, };
	uint16_t hash = 0; 
	for (;;) {
		int i = 0, j = 0, ch = 0;
		int mask = shrink_get(io);
		if (mask < 0)
			break;
		for (i = 0; i < SHRINK_LZP_BYTE_LENGTH; i++) {
			if ((mask & (1 << i)) != 0) {
				assert(hash < io->buffer_length);
				ch = table[hash];
			} else {
				ch = shrink_get(io);
				if (ch < 0)
					break;
				assert(hash < io->buffer_length);
				table[hash] = ch;
			}
			assert(j < (int)sizeof(buf));
			buf[j++] = ch;
			hash = shrink_lzp_hash(hash, ch);
		}
		if (j > 0) {
			for (int k = 0; k < j; k++) {
				assert(k < (int)sizeof(buf));
				if (shrink_put(buf[k], io) < 0)
					return SHRINK_ELINE;
			}
		}
	}
	return 0;
}

int shrink(shrink_t *io, const int codec, const int encode) {
	assert(io);
	/* SHRINK_N.B. Dead code elimination should remove unused
	 * CODECs, even with no optimizations on. */
	switch (codec) {
	case CODEC_RLE:   if (!SHRINK_RLE_ENABLE)   return SHRINK_ELINE; return encode ? shrink_rle_encode(io)   : shrink_rle_decode(io);
	case CODEC_LZSS:  if (!SHRINK_LZSS_ENABLE)  return SHRINK_ELINE; return encode ? shrink_lzss_encode(io)  : shrink_lzss_decode(io);
	case CODEC_ELIAS: if (!SHRINK_ELIAS_ENABLE) return SHRINK_ELINE; return encode ? shrink_elias_encode(io) : shrink_elias_decode(io);
	case CODEC_MTF:   if (!SHRINK_MTF_ENABLE)   return SHRINK_ELINE; return encode ? shrink_mtf_encode(io)   : shrink_mtf_decode(io);
	case CODEC_LZP:   if (!SHRINK_LZP_ENABLE)   return SHRINK_ELINE; return encode ? shrink_lzp_encode(io)   : shrink_lzp_decode(io);
	}
	shrink_never;
	return SHRINK_ELINE;
}

int shrink_buffer(unsigned char *buffer, size_t buffer_length, const int codec, const int encode, const char *in, const size_t inlength, char *out, size_t *outlength) {
	assert(buffer);
	assert(in);
	assert(out);
	assert(outlength);
	shrink_buffer_t ib = { .b = (unsigned char*)in,  .used = 0, .length = inlength, };
	shrink_buffer_t ob = { .b = (unsigned char*)out, .used = 0, .length = *outlength, };
	shrink_t io = { 
		.get = shrink_buffer_get, 
		.put = shrink_buffer_put, 
		.in  = &ib, 
		.out = &ob, 
		.buffer = buffer, 
		.buffer_length = buffer_length, 
	};
	const int r = shrink(&io, codec, encode);
	*outlength = r == 0 ? io.wrote : 0;
	return r;
}

#define SHRINK_TEST_BUFFER_LENGTH (512u)

static inline int shrink_test(unsigned char *buffer, const size_t buffer_length, const int codec, const char *msg, const size_t msglen) {
	assert(buffer);
	assert(msg);
	char compressed[SHRINK_TEST_BUFFER_LENGTH] = { 0, }, decompressed[SHRINK_TEST_BUFFER_LENGTH] = { 0, };
	size_t complen = sizeof compressed, decomplen = sizeof decompressed;
	memset(buffer, 0, buffer_length);
	if (msglen > SHRINK_TEST_BUFFER_LENGTH)
		return SHRINK_ELINE;
	const int r1 = shrink_buffer(buffer, buffer_length, codec, 1, msg,        msglen,  compressed,   &complen);
	if (r1 < 0)
		return r1;
	memset(buffer, 0, buffer_length);
	const int r2 = shrink_buffer(buffer, buffer_length, codec, 0, compressed, complen, decompressed, &decomplen);
	if (r2 < 0)
		return r2;
	if (msglen != decomplen)
		return SHRINK_ELINE;
	if (memcmp(msg, decompressed, msglen))
		return SHRINK_ELINE;
	return 0;
}

int shrink_tests(unsigned char *buffer, const size_t buffer_length) {
	BUILD_BUG_ON(SHRINK_EI > 15); /* 1 << EI would be larger than smallest possible INT_MAX */
	BUILD_BUG_ON(SHRINK_EI < 6);  /* no point in encoding */
	BUILD_BUG_ON(SHRINK_EJ > SHRINK_EI); /* match length needs to be smaller than thing we are matching in */
	BUILD_BUG_ON(SHRINK_RL > 128);
	BUILD_BUG_ON(SHRINK_P  <= 1);
	BUILD_BUG_ON((SHRINK_EI + SHRINK_EJ) > 16); /* unsigned and int used, minimum size is 16 bits */
	BUILD_BUG_ON((SHRINK_N - 1) & SHRINK_N); /* N must be power of 2 */

	if (!SHRINK_DEBUGGING)
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
		for (int j = CODEC_RLE; j <= CODEC_LZP; j++) {
			const int r = shrink_test(buffer, buffer_length, j, ts[i], strlen(ts[i]) + 1);
			if (r < 0)
				return r;
		}
	return 0;
}

