BDB=	../../../db-4.8.26
SRC=	../../src
WT=	../../build_posix
LIB=	${WT}/libwiredtiger.a ${BDB}/build_unix/libdb.a

CFLAGS=-g -Wall -Wextra -Waddress -Waggregate-return -Wbad-function-cast -Wcast-align -Wdeclaration-after-statement -Wformat-security -Wformat-nonliteral -Wformat=2-Winline -Wmissing-declarations -Wmissing-field-initializers -Wmissing-prototypes -Wnested-externs -Wold-style-definition -Wpointer-arith -Wredundant-decls -Wshadow -Wstrict-prototypes -Wundef -Wunreachable-code -Wunsafe-loop-optimizations -Wunused -Wwrite-strings

HDRS=	config.h wts.h
SRCS=	bdb.c config.c t.c util.c wts.c
OBJS=	bdb.o config.o t.o util.o wts.o 

all: t tags

t: ${OBJS} ${WT}/libwiredtiger.a ${BDB}/build_unix/libdb.a
	cc -g -o $@ ${OBJS} ${WT}/simple_setup.o ${LIB} -lpthread

clean:
	rm -f t tags ${OBJS}

distclean realclean: clean
	rm -f __* *.core

bdb.o:
	${CC} ${CFLAGS} -I${BDB} -c $< -o $@

config.o t.o util.o wts.o:
	${CC} ${CFLAGS} -I${WT} -I${SRC}/include  -c $< -o $@

${SRCS}: ${HDRS}
${OBJS}: ${SRCS}

tags: ${HDRS} ${SRCS}
	ctags -w $>
