CFLAGS=-std=c99 -Wall -Wextra -pedantic -O2 -pg

.PHONY: clean all test check performance docs

TARGET=shrink
FILE=readme.md

all: ${TARGET}

lib${TARGET}.a: ${TARGET}.o
	ar rcs $@ $^

${TARGET}.o: ${TARGET}.c ${TARGET}.h

main.o: main.c lib${TARGET}.a ${TARGET}.h

${TARGET}: lib${TARGET}.a main.o

%.lzss %.big: ${TARGET} %
	./${TARGET} -v -c ${FILE} ${FILE}.lzss
	./${TARGET} -v -d ${FILE}.lzss ${FILE}.big

%.rle %.wle : ${TARGET} %
	./${TARGET} -v -r -c ${FILE} ${FILE}.rle
	./${TARGET} -v -r -d ${FILE}.rle ${FILE}.wle

test: ${TARGET} ${FILE}.big ${FILE}.wle
	cmp ${FILE} ${FILE}.big
	cmp ${FILE} ${FILE}.wle
	./${TARGET} -t

check:
	cppcheck --enable=all *.c

zero.bin:
	dd if=/dev/zero of=zero.bin count=2048

performance: ${TARGET} zero.bin
	time -p ./${TARGET} -v -c zero.bin zero.lzss
	time -p ./${TARGET} -r -v -c zero.bin zero.rle

%.htm: %.md
	markdown $< > $@

docs: readme.htm

clean:
	git clean -dfx

