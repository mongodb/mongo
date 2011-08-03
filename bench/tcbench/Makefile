WTBUILD=../../build_posix
CFLAGS=-O3 -I$(WTBUILD)
LDFLAGS=-O3
LIBS=-lpthread

DEPS=wttest.o $(WTBUILD)/libwiredtiger.a
wttest: $(DEPS)
	cc $(LDFLAGS) -o $@ $(DEPS) $(LIBS)

clean:
	rm -f wttest wttest.o
