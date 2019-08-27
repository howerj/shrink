# Shrink 115 104 114 105 110 107

	| Project    | Shrink: Small Compression Library  |
	| ---------- | ---------------------------------- |
	| Maintainer | Richard James Howe                 |
	| License    | Public Domain                      |
	| Email      | howe.r.j.89@gmail.com              |
	| Website    | <https://github.com/howerj/shrink> |

	         __         _       __
	   _____/ /_  _____(_)___  / /__
	  / ___/ __ \/ ___/ / __ \/ //_/
	 (__  ) / / / /  / / / / / ,<
	/____/_/ /_/_/  /_/_/ /_/_/|_|


This project provides [LZSS][] and [RLE][] compression routines, neither of which
do any allocation internally. The project provides the routines as a library
and as a utility program. The library is suitable for inclusion in an
[embedded][] project or product.

# License

The code is placed in the public domain, do what thou wilt.

# Building

You will need [GNU Make][] and a [C][] compiler that is capable of compiling
[C99][]. Type:

	make

This will build a library ('libshrink.a') and an executable ('shrink', or
'shrink.exe' on Windows).

To execute a series of tests (this will require '[cmp][]' to be installed and on
the [PATH][]):

	make test

Which will also build the library and execute if not, and compress and
decompress some files.

# Running

For a full list of commands, after building, consult the build in help by
giving the '-h' option:

	./shrink -h

Example, compressing then decompressing a file:

	./shrink -c file.txt file.smol
	./shrink -d file.smol file.big

The command can be run as a filter:

	./shrink < file.txt > file.smol

There is not too much to it.

# C API and library integration

The [C][] [API][] is minimal, it provides just three function, a single data structure
and an enumeration to select which [CODEC][] is used. All functions return
negative on failure and zero on success. All function assert their inputs so
long as [NDEBUG][] was not defined when the library was compiled.

The function *shrink* can be used with *shrink\_t* data structure to redirect
the compressors input and output arbitrarily. It requires that the user
provides their own [fgetc][] and [fputc][] equivalent functions as the
functions pointers *get* and *put* respectively. *get* returns the same as
[fgetc][] and *put* returns the same as [fputc][]. *get* is passed _\*in_ and
*put* is passed _\*out_, as one would expect.

	typedef struct {
		int (*get)(void *in);
		int (*put)(int ch, void *out);
		void *in, *out;
		size_t read, wrote;
	} shrink_t;

	int shrink(shrink_t *io, int codec, int encode);

The [CODEC][] will continue to consume input with *get* until and [EOF][] is
returned or there is an error (an error could include being unable to output a
byte or invalid input).

The *read* and *wrote* fields contain the number of bytes read in by *get* and
written by *put*, they do not need to be updated by the [API][] user.

A common use of any compression library is encoding blocks bytes in memory, as
such the common example is provided for with the function *shrink\_buffer*.
Internally it uses *shrink* with some internally defined callbacks for *get*
and *put*. Before execution _\*outlength_ should contain the length of _\*out\_
and after it contains the number of bytes written to the output (and zero if
*shrink\_buffer* return an error).

	int shrink_buffer(int codec, int encode, const char *in,
		size_t inlength, char *out, size_t *outlength);

*codec* and *encode* are both used in the same way as *shrink*.

The function *shrink\_tests* executes a series of built in self tests that
checks that the basic functionality of the library is correct. If the
[NDEBUG][] macro is defined at library compile time then this function
always returns success and the test functionality (and the associated code) is
removed. If the tests are compiled in then a return value of zero indicates the
tests completed successfully, and a negative value indicates failure.

	int shrink_tests(void);

The library has minimal dependencies, just some memory related functions
(specifically [memset][], [memmove][], [memchr][], and if tests are compiled
in then [memcmp][] and [strlen][] are also used). [assert][] is also used. If
you have to hand roll your own memory functions in an [embedded][] system, do
not implement these functions naively - look into how optimize these functions,
they will improve the performance of this library (and the rest of your
system).

The [CODEC][] is especially sensitive to the speed at which [memchr][] matches
characters, the [musl][] C library shows that optimizing these very simple
functions is non-trivial.

When the [LZSS][] [CODEC][] is used a fairly large buffer (~4KiB depending on
options) is allocated on the stack. The [RLE][] [CODEC][] uses comparatively
negligible resources. This large stack allocation may cause problems in
embedded environments. There are two solutions to this problem, either
decreasing the sliding window dictionary size (and hence decreasing the size
of the stack allocation and decreasing compression efficiency) or by defining
the macro *USE\_STATIC* at compile time. This will change the allocation to
be of a static storage duration, which means the [LZSS][] [CODEC][] will
not be [reentrant][] nor [thread safe][].

Further customization of the library can be done by editing to [shrink.c][].
This is not usually ideal, however the library is tiny and the configurable
parameters are macros at the beginning of [shrink.c][]. Parameters that can be
configured include the lookahead buffer size, the dictionary windows size, and
the minimal match length of the [LZSS][] [CODEC][], the [RLE][] [CODEC][] is
also configurable, the only parameter is the run length however.

The test driver program [main.c][] contains an example of stream redirection
that reads from a [FILE][] handle. This is not part of the library itself as
the [FILE][] set of functionality is not usually available in embedded systems.

Whilst every effort was made to ensure program correctness the library is
written in an unsafe language, do not return the [CODEC][] on untrusted input.

# CODECS

There are two CODECS available, [RLE][] (Run Length Encoding) and [LZSS][] (a type of
dictionary encoding). The former is best for data that has lots of repetitions
(such as sparse files contain many zeros), whereas [LZSS][] can be used with
arbitrary (compressible) data. The performance of [LZSS][] is pretty poor in terms
of its compression ratio, and the performance of the compressor code itself
could be improved in terms of its speed. Both CODECS are small.

## RLE

The Run Length Encoder works on binary data, it encodes successive runs
of bytes as a length and the repeated byte up to runs of 127, or until
a differing byte is found. This is then emitted as two bytes; the length
and the byte to repeat. Input data with no runs in it is encoded as the
length of the data that differs plus that data, up to 128 bytes can be
encoded this way.  If the first byte is larger than 128 then a run of
literal data is encoded, if it is less, then it encodes a run of data.

The encoded data looks like this, for a run of literals:

	 Command Byte       Data Byte(s)
	.------------.--------------------------.
	| 128-255    | Literal Data ....        |
	.------------.--------------------------.

And this for a repetition of characters:

	 Command Byte Data Byte
	.------------.---------.
	| 0-127      | Byte    |
	.------------.---------.

An error occurs if a command byte is not follow by the requisite number of data
bytes. How the encoding and decoding works should be trivial - examine the
input for sequences of characters greater than the break-even point and output
the repetition command, else output a literal run. [LZSS][] requires more
explanation.

## LZSS

[LZSS][] belongs to the [dictionary compression][] family of lossless
compression algorithms. They work by building up a dictionary and replacing
strings with smaller references to those strings in the dictionary. The
difference between all of the [dictionary compression][] algorithms is in the
details of how matches are encoded and how things are evicted from the
dictionary (mostly).

[LZSS][] encodes the data into references into the dynamically generated
dictionary and into literal bytes, references look like this:

	16-bit Reference Encoding:
	.---------------------------.---------------.
	| 0 bit   | 11 bit position | 4 bit length  |
	.---------------------------.---------------.
	11 and 4 are typical values.

And a literal is encoded as:

	9-bit Literal Encoding:
	.----------------------.
	| 1 bit   | 8 bit byte |
	.----------------------.

The encoder works like so:

1. The dictionary is initialized, it must be initialized to the same value as
   the decoder is.
2. The input buffer is filled up to the maximal string match
3. Search for the longest match in the dictionary and if it equal to or larger
   than the minimal match length output a reference (a position and length) to
   that match, else write a literal value to the output. The output is
   prefixed with a single bit indicating whether the output is a reference or
   a literal.
4. Move the emit output from the input buffer into the dictionary.
5. Attempt to refill the input buffer with a string of equal length to the
   string just emitted.
6. If there is no more input finish, else go to step 2 and repeat the process.

The decoder:

1. Perform the same initialization as the encoder.
2. Read in a single bit indicating the command. If it is a dictionary reference
   then output the string it refers to into the dictionary, if it is a literal,
   then output that literal character.
3. Shift the output character(s) into the dictionary, evicting an equal number
   of the oldest characters that were in there.
4. Repeat from step 2 until all input is consumed.

From the 'comp.compression' FAQ, [part 2][], the compression/encoder algorithm
is described by the following succinct pseudocode:

	while (lookAheadBuffer not empty) {
		get a pointer {position, match} to the longest match
		in the window for the lookahead buffer;

		if (length > MINIMUM_MATCH_LENGTH) {
			output a ( position, length ) pair;
			shift the window length characters along;
		} else {
			output the first character in the lookahead buffer;
			shift the window 1 character along;
		}
	}

Decompression is much fast than compression, compression is limited by the
speed of the search for the longest match. Speeding up the match greatly
increases the speed of compression.

## Other Libraries

* [LZSS][] source this library was based by from Haruhiko Okumura
  <https://oku.edu.mie-u.ac.jp/~okumura/compression/lzss.c>, which says it
  also in the public domain.
* This stackoverflow post asks for similar, unencumbered compression
  libraries, for embedded use: <https://stackoverflow.com/questions/3767640>
* A library for compressing mainly (English) text
  <https://github.com/antirez/smaz>

From <https://en.wikibooks.org/wiki/Data_Compression/Dictionary_compression>
and <https://stackoverflow.com/questions/33331552>

	Smaz is a simple compression library suitable for compressing very
	short strings, such as URLs and single English sentences and short
	HTML files.[2]

	Much like Pike's algorithm, Smaz has a hard-wired constant built-in
	codebook of 254 common English words, word fragments, bigrams, and the
	lowercase letters (except j, k, q). The inner loop of the Smaz decoder
	is very simple:

		* Fetch the next byte X from the compressed file.
		 1. Is X == 254? Single byte literal: fetch the next byte L,
		    and pass it straight through to the decoded text.
		 2. Is X == 255? Literal string: fetch the next byte L, then
		    pass the following L+1 bytes straight through to the
		    decoded text.
		 3. Any other value of X: lookup the X'th "word" in the
		    codebook (that "word" can be from 1 to 5 letters), and
		    copy that word to the decoded text.
		* Repeat until there are no more compressed bytes left in
		  the compressed file.

	Because the codebook is constant, the Smaz decoder is unable to "learn"
	new words and compress them, no matter how often they appear in the
	original text.

It might be worth implementing a fixed dictionary [CODEC][] system as part of
this library.

[LZSS]: https://en.wikipedia.org/wiki/Lempel%E2%80%93Ziv%E2%80%93Storer%E2%80%93Szymanski
[RLE]: https://en.wikipedia.org/wiki/Run-length_encoding
[GNU Make]: https://www.gnu.org/software/make/
[C]: https://en.wikipedia.org/wiki/C_(programming_language)
[C99]: https://en.wikipedia.org/wiki/C99
[PATH]: https://en.wikipedia.org/wiki/PATH_(variable)
[cmp]: https://en.wikipedia.org/wiki/Cmp_(Unix)
[dictionary compression]: https://en.wikipedia.org/wiki/Dictionary_coder
[CODEC]: https://en.wikipedia.org/wiki/Codec
[API]: https://en.wikipedia.org/wiki/Application_programming_interface
[fputc]: http://www.cplusplus.com/reference/cstdio/fputc/
[fgetc]: http://www.cplusplus.com/reference/cstdio/fgetc/
[NDEBUG]: https://en.cppreference.com/w/c/error/assert
[EOF]: http://www.cplusplus.com/reference/cstdio/EOF/
[main.c]: main.c
[shrink.c]: shrink.c
[shrink.h]: shrink.h
[memset]:  http://www.cplusplus.com/reference/cstring/memset/
[memmove]: http://www.cplusplus.com/reference/cstring/memmove/
[memcmp]: http://www.cplusplus.com/reference/cstring/memcmp/
[memchr]: http://www.cplusplus.com/reference/cstring/memchr/
[strlen]: http://www.cplusplus.com/reference/cstring/strlen/
[assert]: http://www.cplusplus.com/reference/cassert/assert/
[reentrant]: https://en.wikipedia.org/wiki/Reentrancy_(computing)
[thread safe]: https://en.wikipedia.org/wiki/Thread_safety
[FILE]: https://en.cppreference.com/w/c/io
[part 2]: http://www.faqs.org/faqs/compression-faq/part2/
[embedded]: https://en.wikipedia.org/wiki/Embedded_system
[musl]: https://www.musl-libc.org/download.html

<style type="text/css">body{margin:40px auto;max-width:850px;line-height:1.6;font-size:16px;color:#444;padding:0 10px}h1,h2,h3{line-height:1.2}table {width: 100%; border-collapse: collapse;}table, th, td{border: 1px solid black;}code { color: #091992; } </style>
