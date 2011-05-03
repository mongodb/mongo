BDB=	db
SRC=	../../src
WT=	../../build_posix
LIB=	${WT}/libwiredtiger.a ${BDB}/build_unix/libdb.a

CFLAGS=-g -Wall ${__GCC_WARN} -I${BDB} -I${WT} -I${SRC}/include

HDRS=	config.h wts.h
SRCS=	bdb.c config.c t.c util.c wts.c
OBJS=	bdb.o config.o t.o util.o wts.o 

t: ${OBJS} ${LIB}
	cc ${CFLAGS} -o $@ ${OBJS} ${LIB} -lpthread

clean:
	rm -f t ${OBJS} *.core dbm.db dbm.dump vgcore* vgout.* xx.log

distclean realclean: clean
	rm -f __* tags

${SRCS}: ${HDRS}
${OBJS}: ${SRCS}

tags: ${HDRS} ${SRCS}
	ctags -w $>
