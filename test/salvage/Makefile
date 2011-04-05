SRC=	../../src
WT=	../../build_posix
LIB=	${WT}/libwiredtiger.a

CFLAGS=-g ${__GCC_WARN} -I${WT} -I${SRC}/include

SRCS=	salvage.c
OBJS=	salvage.o

all: salvage

salvage: salvage.o ${LIB}
	cc ${CFLAGS} -o $@ salvage.o \
	    ${WT}/simple_setup.o ${WT}/libwiredtiger.a -lpthread

clean distclean realclean:
	rm -f salvage ${OBJS} __* *.core

${OBJS}: ${SRCS}
