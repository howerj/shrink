CFLAGS=-std=c99 -Wall -Wextra -pedantic -g -O2

.PHONY: clean all test check performance docs

TARGET=shrink
FILE=readme.md

all: ${TARGET}

lib${TARGET}.a: ${TARGET}.o
	ar rcs $@ $^

${TARGET}.o: ${TARGET}.c ${TARGET}.h

main.o: main.c lib${TARGET}.a ${TARGET}.h

${TARGET}: main.o lib${TARGET}.a
	${CC} ${CFLAGS} $^ -o $@

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
	dd if=/dev/zero of=$@ count=2048

random.bin:
	dd if=/dev/urandom of=$@ count=2048

performance: zero.bin ${TARGET}
	./${TARGET} -v -c zero.bin zero.lzss
	./${TARGET} -v -d zero.lzss zero.big
	./${TARGET} -r -v -c zero.bin zero.rle
	./${TARGET} -r -v -d zero.rle zero.wle

%.htm: %.md
	markdown $< > $@

docs: readme.htm

clean:
	git clean -dfx

