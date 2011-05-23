WTBUILD=../../build_posix
CFLAGS=-O3 -I$(WTBUILD)
LDFLAGS=-O3 -L$(WTBUILD) -lwiredtiger

wttest:	wttest.o $(WTBUILD)/libwiredtiger.a
	cc -o $@ $< $(LDFLAGS)

clean:
	rm -f wttest wttest.o
