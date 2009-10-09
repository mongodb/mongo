WT=	../build_posix
LIB=	${WT}/libwiredtiger.a -lpthread
CFLAGS=-g -I${WT}

all: get

get: get.o
	cc -g -o $@ get.o ${WT}/simple_setup.o ${LIB}

get.o: ${LIB}

t: t.o
	cc -g -o $@ t.o ${WT}/simple_setup.o ${LIB}

t.o: ${LIB}


clean:
	rm -f get t
	rm -f a.* *.o *.core
	rm -f DUMP flexlint.out memory.out
