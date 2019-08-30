CFLAGS=-std=c99 -Wall -Wextra -pedantic -g -O2 -ftest-coverage -fprofile-arcs
CFLAGS=-std=c99 -Wall -Wextra -pedantic -g -O2
TARGET=shrink
TEST_FILES=readme.md random.bin zero.bin

.PHONY: clean all test check docs

all: ${TARGET}

lib${TARGET}.a: ${TARGET}.o
	ar rcs $@ $^

${TARGET}.o: ${TARGET}.c ${TARGET}.h

main.o: main.c lib${TARGET}.a ${TARGET}.h

${TARGET}: main.o lib${TARGET}.a
	${CC} ${CFLAGS} $^ -o $@

zero.bin:
	dd if=/dev/zero of=$@ count=2048

random.bin:
	dd if=/dev/urandom of=$@ count=2048

%.lzss %.big: % ${TARGET}
	./${TARGET} -v -c $< $<.lzss
	./${TARGET} -v -d $<.lzss $<.big
	cmp $< $<.big

%.rle %.wle: % ${TARGET}
	./${TARGET} -v -r -c $< $<.rle
	./${TARGET} -v -r -d $<.rle $<.wle
	cmp $< $<.wle

WLE:=${TEST_FILES:=.wle}
BIG:=${TEST_FILES:=.big}

test: ${TARGET} ${WLE} ${BIG}
	./${TARGET} -t

check:
	cppcheck --enable=all *.c

%.htm: %.md
	markdown $< > $@

docs: readme.htm

clean:
	git clean -dfx

