BDB=	db
SRC=	../../src
WT=	../../build_posix
LIB=	${WT}/libwiredtiger.a ${BDB}/build_unix/libdb.a

CFLAGS=-g -Wall ${__GCC_WARN} -I${BDB} -I${WT} -I${SRC}/include

HDRS=	config.h format.h
SRCS=	bdb.c config.c t.c util.c wts.c
OBJS=	bdb.o config.o t.o util.o wts.o 

all: t tags

t: ${OBJS} ${LIB}
	cc ${CFLAGS} -o $@ ${OBJS} ${LIB} -lpthread

clean:
	rm -f t ${OBJS} *.core dbm.db dbm.dump vgcore* vgout.* xx.log

distclean realclean: clean
	rm -f __* tags

tags: ${HDRS} ${SRCS}
	@type ctags > /dev/null 2>&1 && ctags -w ${HDRS} ${SRCS} || exit 0

${SRCS}: ${HDRS}
${OBJS}: ${SRCS}
