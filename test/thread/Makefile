SRC=	../../src
WT=	../../build_posix
LIB=	${WT}/libwiredtiger.a

CFLAGS=-g -Wall ${__GCC_WARN} -I${WT} -I${SRC}/include

HDRS=	thread.h
SRCS=	t.c load.c run.c stats.c
OBJS=	t.o load.o run.o stats.o

all: t tags

t: ${OBJS} ${LIB}
	cc ${CFLAGS} -o $@ ${OBJS} ${LIB} -lpthread

distclean realclean clean:
	rm -f t ${OBJS} *.core __* tags

tags: ${HDRS} ${SRCS}
	@type ctags > /dev/null 2>&1 && ctags -w ${HDRS} ${SRCS} || exit 0

${SRCS}: ${HDRS}
${OBJS}: ${SRCS}
