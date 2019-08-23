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
 * TODO: Documentation, add references.
 * TODO: Allow LZSS dictionary to be preloaded with a custom corpus,
 * TODO: Add unit tests, add more assertions, fuzz the CODECS,
 * TODO: Speed up the LZSS compressor. Get coverage results.
 * TODO: Allow the RLE CODEC to be more configurable,
 * TODO: Calculate maximum compression ratio for both CODECS. */

#include "shrink.h"
#include <assert.h>
#include <string.h>
#include <stdint.h>

#ifdef NDEBUG
#define DEBUGGING (0)
#else
#define DEBUGGING (1)
#endif

#ifndef STATIC
#define STATIC /* define as 'static' so big structs are not stored on stack */
#endif

#define BUILD_BUG_ON(condition)   ((void)sizeof(char[1 - 2*!!(condition)]))
#define implies(P, Q)             assert(!(P) || (Q)) /* material implication, immaterial if NDEBUG defined */
#define mutual(P, Q)              (implies((P), (Q)), implies((Q), (P)))

                           /* LZSS Parameters */
#define EI (11)            /* typically 10..13 */
#define EJ (4)             /* typically 4..5 */
#define P  (1)             /* If match length <= P then output one character */
#define N  (1 << EI)       /* buffer size */
#define F  ((1 << EJ) + 1) /* lookahead buffer size */
#define CH (' ')           /* initial dictionary contents */
                           /* RLE Parameters */
#define RL   (128)         /* run length */

typedef struct {
	shrink_t *io;
	uint8_t buffer[N * 2];
	unsigned obit_buffer, obit_mask, ibit_buffer, ibit_mask;
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

static int dictionary(lzss_t *l, const size_t length) {
	assert(l);
	assert(length < sizeof l->buffer);
	memset(l->buffer, CH, length);
	return 0;
}

static int putbit(lzss_t *l, const unsigned one) {
	assert(l);
	assert(l->obit_mask <= 128u);
	assert((l->obit_buffer & ~0xFFu) == 0);
	if (one)
		l->obit_buffer |= l->obit_mask;
	if ((l->obit_mask >>= 1) == 0) {
		if (put(l->obit_buffer, l->io) < 0)
			return -1;
		l->obit_buffer = 0;
		l->obit_mask = 128;
	}
	return 0;
}

static int flush_bit_buffer(lzss_t *l) {
	assert(l);
	if (l->obit_mask != 128)
		if (put(l->obit_buffer, l->io) < 0)
			return -1;
	return 0;
}

static int output_literal(lzss_t *l, const unsigned c) {
	assert(l);
	if (putbit(l, 1) < 0)
		return -1;
	for (unsigned mask = 256; mask >>= 1; )
		if (putbit(l, c & mask) < 0)
			return -1;
	return 0;
}

static int output_reference(lzss_t *l, const unsigned x, const unsigned y) {
	assert(l);
	if (putbit(l, 0) < 0)
		return -1;
	for (unsigned mask = N; mask >>= 1;)
		if (putbit(l, x & mask) < 0)
			return -1;
	for (unsigned mask = 1 << EJ; mask >>= 1; )
		if (putbit(l, y & mask) < 0)
			return -1;
	return 0;
}

int shrink_lzss_encode(shrink_t *io) {
	assert(io);
	STATIC lzss_t l = { .obit_mask = 128, };
	l.io = io; /* need because of STATIC */
	long bufferend = 0, c = 0;

	if (dictionary(&l, N - F) < 0)
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
		if (l->ibit_mask == 0u) {
			const int ch = get(l->io);
			if (ch < 0)
				return -1;
			l->ibit_buffer = ch;
			l->ibit_mask   = 128;
		}
		x <<= 1;
		if (l->ibit_buffer & l->ibit_mask)
			x++;
		l->ibit_mask >>= 1;
	}
	return x;
}

int shrink_lzss_decode(shrink_t *io) {
	assert(io);
	STATIC lzss_t l = { .obit_mask = 128, };
	l.io = io; /* need because of STATIC */
	if (dictionary(&l, N - F) < 0)
		return -1;

	for (long r = N - F, c = 0; (c = getnbit(&l, 1)) >= 0; ) {
		if (c) {
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

int shrink_rle_encode(shrink_t *io) {
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

int shrink_rle_decode(shrink_t *io) { 
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

static int buffer_op(const int encode, const int lzss, const char *in, const size_t inlength, char *out, size_t *outlength) {
	assert(in);
	assert(out);
	assert(outlength);
	buffer_t ib = { .b = (unsigned char*)in,  .used = 0, .length = inlength };
	buffer_t ob = { .b = (unsigned char*)out, .used = 0, .length = *outlength };
	shrink_t l = { .get = buffer_get, .put = buffer_put, .in  = &ib, .out = &ob, };
	const int r = (encode ? (lzss ? shrink_lzss_encode : shrink_rle_encode) : (lzss ? shrink_lzss_decode : shrink_rle_decode))(&l);
	*outlength = r == 0 ? l.wrote : 0;
	return r;
}

int shrink_lzss_decode_buffer(const char *in, const size_t inlength, char *out, size_t *outlength) {
	return buffer_op(0, 1, in, inlength, out, outlength);
}

int shrink_lzss_encode_buffer(const char *in, const size_t inlength, char *out, size_t *outlength) {
	return buffer_op(1, 1, in, inlength, out, outlength);
}

int shrink_rle_decode_buffer(const char *in, const size_t inlength, char *out, size_t *outlength) {
	return buffer_op(0, 0, in, inlength, out, outlength);
}

int shrink_rle_encode_buffer(const char *in, const size_t inlength, char *out, size_t *outlength) {
	return buffer_op(1, 0, in, inlength, out, outlength);
}

#if 0
#include <stdio.h>
#include <ctype.h>

int dhex(FILE *out, char *b, size_t l) {
	assert(out);
	assert(b);
	unsigned char *m = (unsigned char*)b;
	for (size_t i = 0; i < l; i += 16) {
		fprintf(out, "%04X:\t", (unsigned)i);
		for (size_t j = i; j < l && j < i + 16; j++)
			fprintf(out, "%02X ", m[j]);
		fputs("\t|", out);
		for (size_t j = i; j < l && j < i + 16; j++)
			fprintf(out, "%c", isgraph(m[j]) ? m[j] : '.');
		fputs("|\n", out);
	}
	fprintf(out, "\n");
	return 0;
}
#endif

#define TBUFL (256u)

static inline int test(const int lzss, const char *msg, const size_t msglen) {
	assert(msg);
	char compressed[TBUFL] = { 0 }, decompressed[TBUFL] = { 0 };
	size_t complen = sizeof compressed, decomplen = sizeof decompressed;
	if (msglen > TBUFL)
		return -1;
	const int r1 = (lzss ? shrink_lzss_encode_buffer : shrink_rle_encode_buffer)(msg, msglen, compressed, &complen);
	if (r1 < 0) 
		return -2;
	const int r2 = (lzss ? shrink_lzss_decode_buffer : shrink_rle_decode_buffer)(compressed, complen, decompressed, &decomplen);
	if (r2 < 0) 
		return -3;
	if (msglen != decomplen) 
		return -4;
	if (memcmp(msg, decompressed, msglen)) 
		return -5;
	return 0;
}

int shrink_tests(void) {
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

	for (size_t i = 0; i < (sizeof ts / sizeof (ts[0])); i++) {
		int r = 0;
		if ((r = test(0, ts[i], strlen(ts[i]) + 1)) < 0)
			return r;
		if ((r = test(1, ts[i], strlen(ts[i]) + 1)) < 0)
			return r;
	}

	return 0;
}

