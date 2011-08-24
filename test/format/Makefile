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
	${CC} ${CFLAGS} -o $@ ${OBJS} ${LIB} -lpthread

tags: ${HDRS} ${SRCS}
	@type ctags > /dev/null 2>&1 && ctags -w ${HDRS} ${SRCS} || exit 0

backup:
	@rm -rf BACKUP && mkdir BACKUP && cp -p -r __* BACKUP
	@echo 'make: current state files saved to BACKUP directory'

distclean realclean: clean
	rm -rf BACKUP

clean:
	rm -f t ${OBJS} __* *.core vgcore* vgout.* tags

${SRCS}: ${HDRS}
${OBJS}: ${SRCS}
