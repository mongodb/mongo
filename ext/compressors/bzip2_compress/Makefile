DEBUGFLAG = -g
CFLAGS = $(DEBUGFLAG) -fPIC -I../../../src/include -I../../../build_posix
LIBRARIES = -lbz2
LDFLAGS = $(DEBUGFLAG) $(LIBRARIES)

all: bzip2_compress.so

bzip2_compress.so: bzip2_compress.o
	gcc $(LDFLAGS) -shared -o bzip2_compress.so bzip2_compress.o

clean:
	rm -f bzip2_compress.o bzip2_compress.so
