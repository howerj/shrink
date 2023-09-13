# Shrink makefile
# See <https://github.com/howerj/shrink> for more information
#
VERSION=0x020000
CFLAGS=-std=c99 -Wall -Wextra -pedantic -g -O2 -DSHRINK_VERSION="${VERSION}"
TARGET=shrink
DBG=
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
	${DBG} ./${TARGET} -v -c $< $<.lzss
	${DBG} ./${TARGET} -v -d $<.lzss $<.big
	cmp $< $<.big

%.rle %.wle: % ${TARGET}
	${DBG} ./${TARGET} -v -r -c $< $<.rle
	${DBG} ./${TARGET} -v -r -d $<.rle $<.wle
	cmp $< $<.wle

%.elias %.saile : % ${TARGET}
	${DBG} ./${TARGET} -v -e -c $< $<.elias
	${DBG} ./${TARGET} -v -e -d $<.elias $<.saile
	cmp $< $<.saile

%.mtf %.ftm: % ${TARGET}
	${DBG} ./${TARGET} -v -m -c $< $<.mtf
	#od -vtu1 -An -w1 my.file | sort -n | uniq -c
	${DBG} ./${TARGET} -v -m -d $<.mtf $<.ftm
	cmp $< $<.ftm

%.lzp %.plz : % ${TARGET}
	${DBG} ./${TARGET} -v -z -c $< $<.lzp
	${DBG} ./${TARGET} -v -z -d $<.lzp $<.plz
	cmp $< $<.plz

WLE:=${TEST_FILES:=.wle}
BIG:=${TEST_FILES:=.big}
FTM:=${TEST_FILES:=.ftm}
SAL:=${TEST_FILES:=.saile}
LZP:=${TEST_FILES:=.plz}

test: ${TARGET} ${WLE} ${BIG} ${FTM} ${SAL} ${LZP}
	./${TARGET} -t

