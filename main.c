/* Shrink library test driver program, see usage() */
#include "shrink.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#define UNUSED(X) ((void)(X))
#define CRC_INIT (0xFFFFu)

#ifdef _WIN32 /* Used to unfuck file mode for "Win"dows. Text mode is for losers. */
#include <windows.h>
#include <io.h>
#include <fcntl.h>
static void binary(FILE *f) { _setmode(_fileno(f), _O_BINARY); }
#else
static inline void binary(FILE *f) { UNUSED(f); }
#endif

typedef struct {
	int (*get)(void *in);
	int (*put)(int ch, void *out);
	void *in, *out;
	uint16_t hash_in, hash_out;
} hashed_io_t;

static int file_get(void *in) {
	assert(in);
	return fgetc((FILE*)in);
}

static int file_put(int ch, void *out) {
	assert(out);
	return fputc(ch, (FILE*)out);
}

static uint16_t crc_update(const uint16_t crc, const uint8_t nb) {
	uint16_t x = (crc >> 8) ^ nb;
        x ^= x >> 4;
	x ^= x << 12;
	x ^= x << 5;
        return x ^ (crc << 8);
}

static int hash_get(void *out) {
	assert(out);
	hashed_io_t *h = out;
	const int r = h->get(h->in);
	if (r >= 0)
		h->hash_in = crc_update(h->hash_in, r);
	return r;
}

static int hash_put(const int ch, void *out) {
	assert(out);
	hashed_io_t *h = out;
	const int r = h->put(ch, h->out);
	if (r >= 0)
		h->hash_out = crc_update(h->hash_out, ch);
	return r;
}

static int stats(shrink_t *l, const int codec, const int encode, const int hash, const double time, FILE *out) {
	assert(l);
	const char *name = codec ? "lzss" : "rle";
	const char *op = encode ? "shrink" : "expand";
	if (hash) {
		hashed_io_t *h = l->in;
		if (fprintf(out, "hash:  in(0x%04x) / out(0x%04x)\n", h->hash_in, h->hash_out) < 0)
			return -1;
	}
	if (fprintf(out, "time:  %g\n", time) < 0)
		return -1;
	if (fprintf(out, "codec: %s/%s\n",  name, op) < 0)
		return -1;
	if (fprintf(out, "text:  %u bytes\n", (unsigned)l->read) < 0)
		return -1;
	if (l->read) {
		const double wrote = l->wrote, read = l->read;
		const double percent = (wrote * 100.0) / read;
		if (fprintf(out, "code:  %.0f bytes (%.2f%%)\n", wrote, percent) < 0)
			return -1;
	}
	return 0;
}

static int file_op(int codec, int encode, int hash, int verbose, FILE *in, FILE *out) {
	assert(in);
	assert(out);
	hashed_io_t hobj = {
		.get     = file_get, .put      = file_put,
		.in      = in,       .out      = out,
		.hash_in = CRC_INIT, .hash_out = CRC_INIT,
	};
	unsigned char buffer[4096];
	shrink_t unhashed = { .get = file_get, .put = file_put, .in  = in,   .out = out,  .buffer = buffer, .buffer_length = sizeof (buffer), };
	shrink_t hashed   = { .get = hash_get, .put = hash_put, .in = &hobj, .out = &hobj, };
	shrink_t *io = hash ? &hashed : &unhashed;
	const clock_t begin = clock();
	const int r = shrink(io, codec, encode);
	const clock_t end = clock();
	const double time = (double)(end - begin) / CLOCKS_PER_SEC;
	if (!r && verbose)
		stats(io, codec, encode, hash, time, stderr);
	return r;
}

static int dump_hex(FILE *d, const char *o, const unsigned long long l) {
	assert(d);
	assert(o);
	typedef unsigned long long ull_t;
	const unsigned char *p = (unsigned char *)o;
	static const ull_t inc = 16;
	for (ull_t i = 0; i < l; i += inc) {
		fprintf(d, "%04lX:\t", (unsigned long)i);
		for (ull_t j = i; j < (i + inc); j++)
			 if ((j < l ? fprintf(d, "%02X ", p[j]) : fputs("   ", d)) < 0)
				 return -1;
		if (fputs("| ", d) < 0)
			return -1;
		for (ull_t j = i; j < (i + inc); j++)
			if ((j < l ? fputc(isgraph(p[j]) ? p[j] : '.', d) : fputc(' ', d)) < 0)
				return -1;
		if (fputs(" |\n", d) < 0)
			return -1;
	}
	return -(fputc('\n', d) < 0);
}

static int string_op(int codec, int encode, int verbose, const char *in, const size_t length, FILE *dump) {
	assert(in);
	const size_t inlength = length;
	size_t outlength = inlength * 16ull;
	char *out = calloc(outlength, 1);
	if (!out)
		return -1;
	const int r1 = shrink_buffer(codec, encode, in, inlength, out, &outlength);
	const int r2 = r1 ? -1 : dump_hex(dump, out, outlength);
	free(out);
	if (!r1 && verbose) {
		if (fprintf(stderr, "uncompressed: %u\n", (unsigned)inlength) < 0)
			return -1;
		if (fprintf(stderr, "compressed:   %u\n", (unsigned)outlength) < 0)
			return -1;
	}
	return -(r1 || r2);
}

static int hexCharToNibble(int c) {
	c = tolower(c);
	if ('a' <= c && c <= 'f')
		return 0xa + c - 'a';
	return c - '0';
}

/* converts up to two characters and returns number of characters converted */
static int hexStr2ToInt(const char *str, int *const val) {
	assert(str);
	assert(val);
	*val = 0;
	if (!isxdigit(*str))
		return 0;
	*val = hexCharToNibble(*str++);
	if (!isxdigit(*str))
		return 1;
	*val = (*val << 4) + hexCharToNibble(*str);
	return 2;
}

static char *duplicate(const char *s) {
	assert(s);
	const size_t sl = strlen(s) + 1ul;
	char *r = malloc(sl);
	if (!r)
		return NULL;
	return memcpy(r, s, sl);
}

static int unescape(char *r, size_t *rlength) {
	assert(r);
	assert(rlength);
	const size_t length = *rlength;
	*rlength = 0;
	if (!length)
		return -1;
	size_t k = 0;
	for (size_t j = 0, ch = 0; (ch = r[j]) && k < length; j++, k++) {
		if (ch == '\\') {
			j++;
			switch (r[j]) {
			case '\0': return -1;
			case '\n': k--;         break; /* multi-line hack (Unix line-endings only) */
			case '\\': r[k] = '\\'; break;
			case  'a': r[k] = '\a'; break;
			case  'b': r[k] = '\b'; break;
			case  'e': r[k] = 27;   break;
			case  'f': r[k] = '\f'; break;
			case  'n': r[k] = '\n'; break;
			case  'r': r[k] = '\r'; break;
			case  't': r[k] = '\t'; break;
			case  'v': r[k] = '\v'; break;
			case  'x': {
				int val = 0;
				const int pos = hexStr2ToInt(&r[j + 1], &val);
				if (pos < 1)
					return -2;
				j += pos;
				r[k] = val;
				break;
			}
			default:
				r[k] = r[j]; break;
			}
		} else {
			r[k] = ch;
		}
	}
	*rlength = k;
	r[k] = '\0';
	return k;
}

static int usage(FILE *out, const char *arg0) {
	assert(arg0);
	unsigned long version = 0;
	(void)shrink_version(&version);
	const int o = (version >> 24) & 0xff;
	const int x = (version >> 16) & 0xff;
	const int y = (version >>  8) & 0xff;
	const int z = (version >>  0) & 0xff;
	static const char *fmt = "\
usage: %s -[-htdclrsH] infile? outfile?\n\n\
Repository: <https://github.com/howerj/shrink>\n\
Maintainer: Richard James Howe\n\
License:    The Unlicense\n\
Version:    %d.%d.%d\n\
Options:    %x\n\
Email:      howe.r.j.89@gmail.com\n\n\
File de/compression utility, default is compress with LZSS, can use RLE. If\n\
outfile is not given output is written to standard out, if infile and\n\
outfile are not given input is taken from standard in and output to standard\n\
out. Have fun.\n\n\
\t--\tstop processing arguments\n\
\t-t\trun built in self tests, zero is pass\n\
\t-h\tprint help and exit\n\
\t-v\tverbose\n\
\t-c\tcompress\n\
\t-d\tdecompress\n\
\t-l\tuse LZSS\n\
\t-r\tuse Run Length Encoding\n\
\t-H\tadd hash to output, implies -v\n\
\t-s #\thex dump encoded string instead of file I/O\n\n";

	return fprintf(out, fmt, arg0, x, y, z, o);
}

static FILE *fopen_or_die(const char *name, const char *mode) {
	errno = 0;
	FILE *f = fopen(name, mode);
	if (!f) {
		fprintf(stderr, "unable to open file '%s' (mode = %s): %s\n",
				name, mode, strerror(errno));
		exit(EXIT_FAILURE);
	}
	return f;
}

int main(int argc, char **argv) {
	binary(stdin);
	binary(stdout);
	FILE *in = stdin, *out = stdout;
	int encode = 1, codec = CODEC_LZSS, i = 1, verbose = 0, string = 0, hash = 0;
	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-')
			break;
		for (int j = 1, ch = 0; (ch = argv[i][j]); j++)
			switch (ch) {
			case '-': i++; goto done;
			case 't': return -shrink_tests();
			case 'h': usage(stderr, argv[0]); return 0;
			case 'v': verbose++; break;
			case 'd': encode = 0; break;
			case 'c': encode = 1; break;
			case 'l': codec = CODEC_LZSS; break;
			case 'r': codec = CODEC_RLE; break;
			case 's': string = 1; break;
			case 'H': hash = 1; verbose++; break;
			default: goto done;
			}
	}
done:
	if (string) {
		if (i < argc) {
			char *s = duplicate(argv[i]);
			if (!s)
				return 1;
			size_t length = strlen(s) + 1ul;
			if (unescape(s, &length) < 0) {
				(void)fprintf(stderr, "Invalid escape sequence\n");
				free(s);
				return 1;
			}
			const int r = string_op(codec, encode, verbose, s, length, stdout);
			free(s);
			return r;
		}
		usage(stderr, argv[0]);
		return 1;
	}

	if (i < argc) {  in = fopen_or_die(argv[i++], "rb"); }
	if (i < argc) { out = fopen_or_die(argv[i++], "wb"); }

	static char inb[BUFSIZ], outb[BUFSIZ];
	if (setvbuf(in, inb,  _IOFBF, sizeof inb) < 0)
		return 1;
	if (setvbuf(in, outb, _IOFBF, sizeof outb) < 0)
		return 1;

	const int r = file_op(codec, encode, hash, verbose, in, out);
	if (fclose(in) < 0)
		return 1;
	if (fclose(out) < 0)
		return 1;
	return !!r;
}

