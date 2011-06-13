SRC=	../../src
WT=	../../build_posix
LIB=	${WT}/libwiredtiger.a

CFLAGS=-g ${__GCC_WARN} -I${WT} -I${SRC}/include

SRCS=	salvage.c
OBJS=	salvage.o

all: t

t: salvage.o ${LIB}
	cc ${CFLAGS} -o $@ salvage.o \
	    ${WT}/libwiredtiger.a -lpthread

clean distclean realclean:
	rm -f t ${OBJS} __* *.core

${OBJS}: ${SRCS}
