CFLAGS=-g -W -Wall -Wpointer-arith -Wmissing-prototypes

WT=	../../build_posix
BDB=	../../../db-4.8.26
LIB=	${WT}/libwiredtiger.a ${BDB}/build_unix/libdb.a ${EXTLIB}

SRCS=	bdb.c config.c t.c util.c wts.c wts.h config.h
OBJS=	bdb.o config.o t.o util.o wts.o 

all: t tags

t: ${OBJS} ${WT}/libwiredtiger.a ${BDB}/build_unix/libdb.a
	cc -g -o $@ ${OBJS} ${WT}/simple_setup.o ${LIB}

clean:
	rm -f t ${OBJS}
	rm -f DUMP LOG flexlint.out memory.out

distclean realclean: clean
	rm -f __* *.core

tags: ${SRCS}
	ctags -w *.h *.c ../*/*.c ../*/*.h

bdb.o:
	${CC} ${CFLAGS} -I${BDB} -c $< -o $@

config.o t.o util.o wts.o:
	${CC} ${CFLAGS} -I../../build_posix -I../include  -c $< -o $@

${OBJS}: ${SRCS} wts.h
config.o: config.h
