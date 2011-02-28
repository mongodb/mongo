BDB=	db
SRC=	../../src
WT=	../../build_posix
LIB=	${WT}/libwiredtiger.a ${BDB}/build_unix/libdb.a

CFLAGS=-g -Wall -Wextra -Waddress -Waggregate-return -Wbad-function-cast -Wcast-align -Wdeclaration-after-statement -Wformat-security -Wformat-nonliteral -Wformat=2-Winline -Wmissing-declarations -Wmissing-field-initializers -Wmissing-prototypes -Wnested-externs -Wold-style-definition -Wpointer-arith -Wredundant-decls -Wshadow -Wstrict-prototypes -Wundef -Wunreachable-code -Wunsafe-loop-optimizations -Wunused -Wwrite-strings -I${BDB} -I${WT} -I${SRC}/include

HDRS=	config.h wts.h
SRCS=	bdb.c config.c t.c util.c wts.c
OBJS=	bdb.o config.o t.o util.o wts.o 

all: t

t: ${OBJS} ${WT}/libwiredtiger.a ${BDB}/build_unix/libdb.a
	cc ${CFLAGS} -o $@ ${OBJS} ${WT}/simple_setup.o ${LIB} -lpthread

clean:
	rm -f t tags ${OBJS} *.core vgout.* vgcore*

distclean realclean: clean
	rm -f __*

${SRCS}: ${HDRS}
${OBJS}: ${SRCS}

tags: ${HDRS} ${SRCS}
	ctags -w $>
