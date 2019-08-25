/* Shrink library test driver program, see usage() */
#include "shrink.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define UNUSED(X) ((void)(X))

#ifdef _WIN32 /* Used to unfuck file mode for "Win"dows. Text mode is for losers. */
#include <windows.h>
#include <io.h>
#include <fcntl.h>
extern int _fileno(FILE *stream);
static void binary(FILE *f) { _setmode(_fileno(f), _O_BINARY); }
#else
static inline void binary(FILE *f) { UNUSED(f); }
#endif

static int file_get(void *in) {
	assert(in);
	return fgetc((FILE*)in);
}

static int file_put(int ch, void *out) {
	assert(out);
	return fputc(ch, (FILE*)out);
}

static int stats(shrink_t *l, const int codec, const int encode, FILE *out) {
	assert(l);
	const char *name = codec ? "lzss" : "rle";
	const char *shrink = encode ? "shrink" : "expand";
	if (fprintf(out, "codec: %s/%s\n",  name, shrink) < 0)
		return -1;
	if (fprintf(out, "text:  %u bytes\n", (unsigned)l->read) < 0)
		return -1;
	if (l->read) {
		const double wrote = l->wrote, read = l->read;
		const double percent = (wrote * 100.0) / read;
		if (fprintf(out, "code:  %g bytes (%g%%)\n", wrote, percent) < 0)
			return -1;
	}
	return 0;
}

static int file_op(int codec, int encode, int verbose, FILE *in, FILE *out) {
	assert(in);
	assert(out);
	shrink_t io = { .get = file_get, .put = file_put, .in  = in, .out = out, };
	const int r = shrink(&io, codec, encode);
	if (!r && verbose)
		stats(&io, codec, encode, stderr);
	return r;
}

static int dump_hex(FILE *d, const char *o, const unsigned long long l) {
	assert(d);
	assert(o);
	typedef unsigned long long ull_t;
	const unsigned char *p = (unsigned char *)o;
	static const ull_t inc = 16;
	for (ull_t i = 0; i < l; i += inc) {
		fprintf(d, "%04llX:\t", i);
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

static int string_op(int codec, int encode, int verbose, const char *in, FILE *dump) {
	assert(in);
	const size_t inlength = strlen(in);
	size_t outlength = inlength * 16ull;
	char *out = calloc(outlength, 1);
	if (!out)
		return -1;
	const int r1 = shrink_buffer(codec, encode, in, inlength, out, &outlength);
	const int r2 = r1 ? -1 : dump_hex(dump, out, outlength);
	free(out);
	if (!r1 && verbose)
		if (fprintf(stderr, "compressed: %u\n", (unsigned)outlength) < 0)
			return -1;
	return -(r1 || r2);
}

static int usage(FILE *out, const char *arg0) {
	assert(arg0);
	static const char *fmt = "\
usage: %s -[-htdclrs] infile? outfile?\n\n\
Repository: <https://github.com/howerj/shrink>\n\
Maintainer: Richard James Howe\n\
License:    Public Domain\n\
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
\t-s #\thex dump encoded string instead of file I/O\n\n";

	return fprintf(out, fmt, arg0);
}

static FILE *fopen_or_die(const char *name, const char *mode) {
	errno = 0;
	FILE *f = fopen(name, mode);
	if (!f) {
		fprintf(stderr, "unable to open file '%s' (mode = %s): %s",
				name, mode, strerror(errno));
		exit(EXIT_FAILURE);
	}
	return f;
}

int main(int argc, char **argv) {
	binary(stdin);
	binary(stdout);
	FILE *in = stdin, *out = stdout;
	int encode = 1, codec = CODEC_LZSS, i = 1, verbose = 0, string = 0;
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
			default: goto done;
			}
	}
done:
	if (string) {
		if (i < argc)
			return string_op(codec, encode, verbose, argv[i], stdout);
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

	const int r = file_op(codec, encode, verbose, in, out);
	if (fclose(in) < 0)
		return 1;
	if (fclose(out) < 0)
		return 1;
	return !!r;
}

