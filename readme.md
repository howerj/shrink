# Shrink

| Project    | Shrink: Small Compression Library  |
| ---------- | ---------------------------------- |
| Maintainer | Richard James Howe                 |
| License    | Public Domain                      |
| Email      | howe.r.j.89@gmail.com              |
| Website    | <https://github.com/howerj/shrink> |

This project provides [LZSS][] and [RLE][] compression routines, neither of which
do any allocation internally. The project provides the routines as a library
and as a utility program. The library is suitable for inclusion in an embedded
project.

# License

The code is placed in the public domain, do what thou whilt.

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

There is not too much to it.

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

## LZSS

# References

[LZSS]: https://en.wikipedia.org/wiki/Lempel%E2%80%93Ziv%E2%80%93Storer%E2%80%93Szymanski
[RLE]: https://en.wikipedia.org/wiki/Run-length_encoding
[GNU Make]: https://www.gnu.org/software/make/
[C]: https://en.wikipedia.org/wiki/C_(programming_language)
[C99]: https://en.wikipedia.org/wiki/C99
[PATH]: https://en.wikipedia.org/wiki/PATH_(variable)
[cmp]: https://en.wikipedia.org/wiki/Cmp_(Unix)

<style type="text/css">body{margin:40px auto;max-width:850px;line-height:1.6;font-size:16px;color:#444;padding:0 10px}h1,h2,h3{line-height:1.2}table {width: 100%; border-collapse: collapse;}table, th, td{border: 1px solid black;}code { color: #091992; } </style>
