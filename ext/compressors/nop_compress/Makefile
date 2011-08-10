CFLAGS = -I../../../build_posix

all: nop_compress.so

nop_compress.so: nop_compress.o
	gcc -shared -o nop_compress.so nop_compress.o
