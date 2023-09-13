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
	char *arg;   /* parsed argument */
	int index,   /* index into argument list */
	    option,  /* parsed option */
	    reset;   /* set to reset */
	char *place; /* internal use: scanner position */
	int  init;   /* internal use: initialized or not */
	FILE *error; /* turn error reporting on/off */
} custom_getopt_t;   /* getopt clone; with a few modifications */

typedef struct {
	int (*get)(void *in);
	int (*put)(int ch, void *out);
	void *in, *out;
	uint16_t hash_in, hash_out;
} hashed_io_t;

/* Adapted from: <https://stackoverflow.com/questions/10404448>, this
 * could be extended to parse out numeric values, and do other things, but
 * that is not needed here. The function and structure should be turned
 * into a header only library. */
static int custom_getopt(custom_getopt_t *opt, const int argc, char *const argv[], const char *fmt) {
	assert(opt);
	assert(fmt);
	assert(argv);
	enum { BADARG_E = ':', BADCH_E = '?', BADIO_E = '!', };

	if (!(opt->init)) {
		opt->place = ""; /* option letter processing */
		opt->init  = 1;
		opt->index = 1;
	}

	if (opt->reset || !*opt->place) { /* update scanning pointer */
		opt->reset = 0;
		if (opt->index >= argc || *(opt->place = argv[opt->index]) != '-') {
			opt->place = "";
			return -1;
		}
		if (opt->place[1] && *++opt->place == '-') { /* found "--" */
			opt->index++;
			opt->place = "";
			return -1;
		}
	}

	const char *oli = NULL; /* option letter list index */
	if ((opt->option = *opt->place++) == ':' || !(oli = strchr(fmt, opt->option))) { /* option letter okay? */
		 /* if the user didn't specify '-' as an option, assume it means -1.  */
		if (opt->option == '-')
			return -1;
		if (!*opt->place)
			opt->index++;
		if (opt->error && *fmt != ':')
			if (fprintf(opt->error, "illegal option -- %c\n", opt->option) < 0)
				return BADIO_E;
		return BADCH_E;
	}

	if (*++oli != ':') { /* don't need argument */
		opt->arg = NULL;
		if (!*opt->place)
			opt->index++;
	} else {  /* need an argument */
		if (*opt->place) { /* no white space */
			opt->arg = opt->place;
		} else if (argc <= ++opt->index) { /* no arg */
			opt->place = "";
			if (*fmt == ':')
				return BADARG_E;
			if (opt->error)
				if (fprintf(opt->error, "option requires an argument -- %c\n", opt->option) < 0)
					return BADIO_E;
			return BADCH_E;
		} else	{ /* white space */
			opt->arg = argv[opt->index];
		}
		opt->place = "";
		opt->index++;
	}
	return opt->option; /* dump back option letter */
}

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

static const char *codec_name(const int codec) {
	if (codec < CODEC_RLE || codec > CODEC_LZP)
		return "unknown";
	const char *names[] = {
		[CODEC_RLE]   = "rle",
		[CODEC_LZSS]  = "lzss",
		[CODEC_ELIAS] = "elias",
		[CODEC_MTF]   = "mtf",
		[CODEC_LZP]   = "lzp",
	};
	return names[codec];
}

static int stats(shrink_t *l, const int codec, const int encode, const int hash, const double time, FILE *out) {
	assert(l);
	const char *name = codec_name(codec);
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

static int file_op(unsigned char *buffer, size_t buffer_length, 
	int codec, int encode, int hash, int verbose, FILE *in, FILE *out) {
	assert(buffer);
	assert(in);
	assert(out);
	hashed_io_t hobj = {
		.get     = file_get, .put      = file_put,
		.in      = in,       .out      = out,
		.hash_in = CRC_INIT, .hash_out = CRC_INIT,
	};
	shrink_t unhashed = { .get = file_get, .put = file_put, .in  = in,   .out = out,   .buffer = buffer, .buffer_length = buffer_length, };
	shrink_t hashed   = { .get = hash_get, .put = hash_put, .in = &hobj, .out = &hobj, .buffer = buffer, .buffer_length = buffer_length, };
	shrink_t *io = hash ? &hashed : &unhashed;
	const clock_t begin = clock();
	const int r = shrink(io, codec, encode);
	const clock_t end = clock();
	const double time = (double)(end - begin) / CLOCKS_PER_SEC;
	if (!r && verbose)
		if (stats(io, codec, encode, hash, time, stderr) < 0)
			return -1;
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

static int string_op(unsigned char *buffer, size_t buffer_length, int codec, int encode, int verbose, const char *in, const size_t length, FILE *dump) {
	assert(buffer);
	assert(in);
	const size_t inlength = length;
	size_t outlength = inlength * 16ull; /* This is a hack! We should realloc more if shrink_buffer fails */
	char *out = calloc(outlength, 1);
	if (!out)
		return -1;
	const int r1 = shrink_buffer(buffer, buffer_length, codec, encode, in, inlength, out, &outlength);
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

static int hex_char_to_nibble(int c) {
	c = tolower(c);
	if ('a' <= c && c <= 'f')
		return 0xa + c - 'a';
	return c - '0';
}

/* converts up to two characters and returns number of characters converted */
static int hex_string_to_int(const char *str, int *const val) {
	assert(str);
	assert(val);
	*val = 0;
	if (!isxdigit(*str))
		return 0;
	*val = hex_char_to_nibble(*str++);
	if (!isxdigit(*str))
		return 1;
	*val = (*val << 4) + hex_char_to_nibble(*str);
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
				const int pos = hex_string_to_int(&r[j + 1], &val);
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
	const int o = (version >> 24) & 255;
	const int x = (version >> 16) & 255;
	const int y = (version >>  8) & 255;
	const int z = (version >>  0) & 255;
	static const char *fmt = "\
usage: %s -[-htdclrezsH] infile? outfile?\n\n\
Repository: "SHRINK_REPOSITORY"\n\
Author:     "SHRINK_AUTHOR"\n\
License:    "SHRINK_LICENSE"\n\
Version:    %d.%d.%d\n\
Options:    %x\n\
Email:      "SHRINK_AUTHOR"\n\n\
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
\t-e\tuse Elias Gamma Encoding\n\
\t-m\tuse Move-To-Front Encoding\n\
\t-z\tuse LZP\n\
\t-H\tadd hash to output, implies -v\n\
\t-p file.bin\tpreload compression working buffer with file\n\
\t-P file.bin\tsave compression working buffer\n\
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

static int unload(const char *name, unsigned char *buffer, size_t buffer_length) {
	if (!name) return 0;
	FILE *f = fopen_or_die(name, "wb");
	if (fwrite(buffer, 1, buffer_length, f) != buffer_length)
		return -1;
	if (fclose(f) < 0) 
		return -1;
	return 0;
}

int main(int argc, char **argv) {
	binary(stdin);
	binary(stdout);
	FILE *in = stdin, *out = stdout;
	int encode = 1, codec = CODEC_LZSS, verbose = 0, hash = 0;
	custom_getopt_t opt = { .error = stderr, };
	unsigned char buffer[1<<16] = { 0, };
	char *save = NULL;

	for (int ch = 0; (ch = custom_getopt(&opt, argc, argv, "hHtvcs:edrlmzp:P:")) != -1; ) {
		switch (ch) {
		case 'h': (void)usage(stderr, argv[0]); return 0;
		case 'H': hash = 1; verbose++; break;
		case 'v': verbose++; break;
		case 'c': encode = 1; break;
		case 'd': encode = 0; break;
		case 'r': codec = CODEC_RLE; break;
		case 'e': codec = CODEC_ELIAS; break;
		case 'l': codec = CODEC_LZSS; break;
		case 'm': codec = CODEC_MTF; break;
		case 'z': codec = CODEC_LZP; break;
		case 't': { 
			const int r = shrink_tests(buffer, sizeof buffer); 
			if (verbose && r < 0)
				(void)fprintf(stderr, "Tests failed %d\n", r); 
			return r < 0 ? 1 : 0; 
		}
		case 's': {
			char *s = duplicate(opt.arg);
			if (!s)
				return 1;
			size_t length = strlen(s) + 1ul;
			if (unescape(s, &length) < 0) {
				(void)fprintf(stderr, "Invalid escape sequence\n");
				free(s);
				return 1;
			}
			const int r = string_op(buffer, sizeof buffer, codec, encode, /*hash, <- should do this as well! */ verbose, s, length, stdout);
			free(s);
			if (unload(save, buffer, sizeof buffer) < 0)
				return 1;
			return r < 0 ? 1 : 0;
		}
		case 'p': {
			FILE *f = fopen_or_die(opt.arg, "rb");
			size_t read = fread(buffer, 1, sizeof buffer, f);
			if (verbose)
				if (fprintf(stderr, "Preloaded %ld bytes\n", (long)read) < 0)
					return 1;
			if (fclose(f) < 0) 
				return 1;
			break; 
		}
		case 'P': 
			if (save) {
				(void)fprintf(stderr, "-P already set (with '%s') cannot set to '%s'\n", save, opt.arg);
				return 1;
			}
			save = opt.arg; 
			break;
		default: return 1;
		}
	}

	int i = opt.index;
	if (i < argc) {  in = fopen_or_die(argv[i++], "rb"); }
	if (i < argc) { out = fopen_or_die(argv[i++], "wb"); }

	static char inb[BUFSIZ], outb[BUFSIZ];
	if (setvbuf(in, inb,  _IOFBF, sizeof inb) < 0)
		return 1;
	if (setvbuf(in, outb, _IOFBF, sizeof outb) < 0)
		return 1;

	const int r = file_op(buffer, sizeof buffer, codec, encode, hash, verbose, in, out);
	if (fclose(in) < 0)
		return 1;
	if (fclose(out) < 0)
		return 1;
	if (unload(save, buffer, sizeof buffer) < 0)
		return 1;
	return !!r;
}

