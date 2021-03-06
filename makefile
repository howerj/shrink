# Shrink makefile
# See <https://github.com/howerj/shrink> for more information
#
VERSION=0x010101
CFLAGS=-std=c99 -Wall -Wextra -pedantic -g -O2 -DSHRINK_VERSION="${VERSION}"
TARGET=shrink
DESTDIR =install

.PHONY: clean all test check install dist

all: ${TARGET}

lib${TARGET}.a: ${TARGET}.o
	ar rcs $@ $^

${TARGET}.o: ${TARGET}.c ${TARGET}.h

main.o: main.c lib${TARGET}.a ${TARGET}.h

${TARGET}: main.o lib${TARGET}.a
	${CC} ${CFLAGS} $^ -o $@
	-strip $@

${TARGET}.1: readme.md
	-pandoc -s -f markdown -t man $< -o $@

.git:
	git clone https://github.com/howerj/shrink shrink-repo
	mv shrink-repo/.git .
	rm -rf shrink-repo

install: ${TARGET} lib${TARGET}.a ${TARGET}.1 .git
	install -p -D ${TARGET} ${DESTDIR}/bin/${TARGET}
	install -p -m 644 -D lib${TARGET}.a ${DESTDIR}/lib/lib${TARGET}.a
	install -p -m 644 -D ${TARGET}.h ${DESTDIR}/include/${TARGET}.h
	-install -p -m 644 -D ${TARGET}.1 ${DESTDIR}/man/${TARGET}.1
	mkdir -p ${DESTDIR}/src
	cp -a .git ${DESTDIR}/src
	cd ${DESTDIR}/src && git reset --hard HEAD

dist: install
	tar zcf ${TARGET}-${VERSION}.tgz ${DESTDIR}

check:
	cppcheck --enable=all *.c

clean:
	git clean -dffx

# Tests
TEST_FILES=readme.md random.bin zero.bin

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

