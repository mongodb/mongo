CFLAGS=-g -W -Wall -Wpointer-arith -Wmissing-prototypes

WT=	../build_posix
BDB=	../../bdb.releases/db-4.8.26
LIB=	${WT}/libwiredtiger.a ${BDB}/build_unix/libdb.a ${EXTLIB}

SRCS=	bdb.c config.c t.c util.c wts.c wts.h 
OBJS=	bdb.o config.o t.o util.o wts.o 

all: t tags

t: ${OBJS} ${LIB}
	cc -g -o $@ ${OBJS} ${WT}/simple_setup.o ${LIB}

clean:
	rm -f t ${OBJS}
	rm -f __* *.core
	rm -f DUMP LOG flexlint.out memory.out

tags: ${SRCS}
	ctags -w *.h *.c ../*/*.c ../*/*.h

${OBJS}: wts.h

bdb.o:
	${CC} -DBDB ${CFLAGS} -I${BDB} -c $< -o $@

config.o: config.h
config.o t.o util.o wts.o:
	${CC} ${CFLAGS} -I${WT} -c $< -o $@
