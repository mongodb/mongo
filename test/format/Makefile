BDB=	db
SRC=	../../src
WT=	../../build_posix
LIB=	${WT}/libwiredtiger.a ${BDB}/build_unix/libdb.a
CPPFLAGS=

CFLAGS=-g -Wall ${__GCC_WARN} -I${BDB} -I${WT} -I${SRC}/include ${CPPFLAGS}

HDRS=	config.h format.h
SRCS=	bdb.c config.c t.c util.c wts.c
OBJS=	bdb.o config.o t.o util.o wts.o 

all: t tags

t: ${OBJS} ${LIB}
	cc ${CFLAGS} -o $@ ${OBJS} ${LIB} -lpthread

clean distclean realclean:
	rm -f t ${OBJS} __* *.core vgcore* vgout.* tags

tags: ${HDRS} ${SRCS}
	@type ctags > /dev/null 2>&1 && ctags -w ${HDRS} ${SRCS} || exit 0

${SRCS}: ${HDRS}
${OBJS}: ${SRCS}
