DEBUGFLAG = -g
CFLAGS = $(DEBUGFLAG) -I../../../build_posix
LIBRARIES = -lbz2
LDFLAGS = $(DEBUGFLAG) $(LIBRARIES)

all: bzip2_compress.so

bzip2_compress.so: bzip2_compress.o
	gcc $(LDFLAGS) -shared -o bzip2_compress.so bzip2_compress.o
