CFLAGS=-std=c99 -Wall -Wextra -pedantic -O2 -g

.PHONY: clean all test check

TARGET=shrink
FILE=${TARGET}.c

all: ${TARGET}

lib${TARGET}.a: ${TARGET}.o
	ar rcs $@ $^

${TARGET}: lib${TARGET}.a main.o

%.smol %.big: ${TARGET} %
	./${TARGET} -v -c ${FILE} ${FILE}.smol
	./${TARGET} -v -d ${FILE}.smol ${FILE}.big

%.rle %.wle : ${TARGET} %
	./${TARGET} -v -r -c ${FILE} ${FILE}.rle
	./${TARGET} -v -r -d ${FILE}.rle ${FILE}.wle

test: ${TARGET} ${FILE}.big ${FILE}.wle
	cmp ${FILE} ${FILE}.big
	cmp ${FILE} ${FILE}.wle
	./${TARGET} -t

check:
	cppcheck --enable=all *.c

clean:
	rm -vf *.o *.a *.exe ${TARGET} *.smol *.big *.rle *.wle

