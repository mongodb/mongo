include mkinc.mak
CFLAGS=-Iinclude
all: libstemmer.o stemwords
libstemmer.o: $(snowball_sources:.c=.o)
	$(AR) -cru $@ $^
stemwords: examples/stemwords.o libstemmer.o
	$(CC) -o $@ $^
clean:
	rm -f stemwords *.o src_c/*.o runtime/*.o libstemmer/*.o
