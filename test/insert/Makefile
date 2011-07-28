SRC=	../../src
WT=	../../build_posix
LIB=	${WT}/libwiredtiger.a

CFLAGS=-g ${__GCC_WARN} -I${WT} -I${SRC}/include

SRCS=	insert.c
OBJS=	insert.o

all: t

t: insert.o ${LIB}
	cc ${CFLAGS} -o $@ insert.o \
	    ${WT}/libwiredtiger.a -lpthread

clean distclean realclean:
	rm -f t ${OBJS} __* *.core vgout.* tags xx*

tags: ${SRCS}
	ctags -w $>

${OBJS}: ${SRCS}
