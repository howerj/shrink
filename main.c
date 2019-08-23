/* Shrink library test driver program, see usage() */
#include "shrink.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int file_get(void *in) { 
	assert(in); 
	return fgetc((FILE*)in); 
}

static int file_put(int ch, void *out) { 
	assert(out); 
	return fputc(ch, (FILE*)out); 
}

static int stats(shrink_t *l, const int encode, const int lzss, FILE *out) {
	assert(l);
	const char *codec = lzss ? "lzss" : "rle";
	const char *shrink = encode ? "shrink" : "expand";
	fprintf(out, "codec: %s/%s\n",  codec, shrink);
	fprintf(out, "text:  %u bytes\n", (unsigned)l->read);
	if (l->read) {
		const double wrote = l->wrote, read = l->read;
		const double percent = (wrote * 100.0) / read;
		fprintf(out, "code:  %g bytes (%g%%)\n", wrote, percent);
	}
	return 0;
}

static int file_op(int encode, int lzss, int verbose, FILE *in, FILE *out) {
	assert(in);
	assert(out);
	shrink_t l = { .get = file_get, .put = file_put, .in  = in, .out = out, };
	const int r = (encode ? (lzss ? shrink_lzss_encode : shrink_rle_encode) : (lzss ? shrink_lzss_decode : shrink_rle_decode))(&l);
	if (!r && verbose)
		stats(&l, encode, lzss, stderr);
	return r;
}

static int usage(FILE *out, const char *arg0) {
	assert(arg0);
	static const char *fmt = "\
usage: %s -[-htdclr] infile? outfile?\n\n\
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
\t-r\tuse Run Length Encoding\n\n";

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
	FILE *in = stdin, *out = stdout;
	int enc = 1, lzss = 1, i = 1, verbose = 0;
	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-')
			break;
		for (int j = 1, ch = 0; (ch = argv[i][j]); j++)
			switch (ch) {
			case '-': i++; goto done;
			case 't': return -shrink_tests();
			case 'h': usage(stderr, argv[0]); return 0;
			case 'v': verbose++; break;
			case 'd': enc = 0; break;
			case 'c': enc = 1; break;
			case 'l': lzss = 1; break;
			case 'r': lzss = 0; break;
			default: goto done;
			}
	}
done:
	if (i < argc) {  in = fopen_or_die(argv[i++], "rb"); }
	if (i < argc) { out = fopen_or_die(argv[i++], "wb"); }

	const int r = file_op(enc, lzss, verbose, in, out);
	fclose(in);
	fclose(out);
	return !!r;
}

