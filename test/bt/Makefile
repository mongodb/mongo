WT=	../build_posix
LIB=	${WT}/libwiredtiger.a -lpthread
CFLAGS=-g -I${WT}

all: t

t: t.o ${LIB}
	cc -g -o $@ t.o ${WT}/simple_setup.o ${LIB}

clean:
	rm -f t
	rm -f a.* *.o *.core
	rm -f DUMP LOG flexlint.out memory.out
