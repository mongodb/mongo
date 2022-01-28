# Makefile for example of using zstd wrapper for zlib
#
# make - compiles examples
# make MOREFLAGS=-DZWRAP_USE_ZSTD=1 - compiles examples with zstd compression turned on
# make test - runs examples


# Paths to static and dynamic zlib and zstd libraries
# Use "make ZLIB_PATH=path/to/zlib ZLIB_LIBRARY=path/to/libz.so" to select a path to library
ZLIB_LIBRARY ?= -lz
ZLIB_PATH ?= .

ZSTDLIBDIR = ../lib
ZSTDLIBRARY = $(ZSTDLIBDIR)/libzstd.a
ZLIBWRAPPER_PATH = .
GZFILES = gzclose.o gzlib.o gzread.o gzwrite.o
EXAMPLE_PATH = examples
PROGRAMS_PATH = ../programs
TEST_FILE = ../doc/zstd_compression_format.md

vpath %.c $(PROGRAMS_PATH) $(EXAMPLE_PATH) $(ZLIBWRAPPER_PATH)


CPPFLAGS += -DXXH_NAMESPACE=ZSTD_ -I$(ZLIB_PATH) -I$(PROGRAMS_PATH)       \
            -I$(ZSTDLIBDIR) -I$(ZSTDLIBDIR)/common -I$(ZLIBWRAPPER_PATH)
STDFLAGS  = -std=c89 -pedantic -Wno-long-long -Wno-variadic-macros -Wc++-compat \
            -DNO_snprintf -DNO_vsnprintf  # strict ANSI C89 is missing these prototypes
DEBUGFLAGS= -Wall -Wextra -Wcast-qual -Wcast-align -Wshadow -Wswitch-enum \
            -Wdeclaration-after-statement -Wstrict-prototypes -Wundef     \
            -Wstrict-aliasing=1
CFLAGS   ?= -O3
CFLAGS   += $(STDFLAGS) $(DEBUGFLAGS)
CPPFLAGS += $(MOREFLAGS)
LDLIBS   += $(ZLIB_LIBRARY)

# Define *.exe as extension for Windows systems
ifneq (,$(filter Windows%,$(OS)))
EXT =.exe
else
EXT =
endif

default : release

release : STDFLAGS =
release : DEBUGFLAGS =
release : all

all: fitblk example zwrapbench minigzip

test: example fitblk example_zstd fitblk_zstd zwrapbench minigzip minigzip_zstd
	./example
	./example_zstd
	./fitblk 10240 <$(TEST_FILE)
	./fitblk 40960 <$(TEST_FILE)
	./fitblk_zstd 10240 <$(TEST_FILE)
	./fitblk_zstd 40960 <$(TEST_FILE)
	@echo ---- minigzip start ----
	./minigzip_zstd example$(EXT)
	#cp example$(EXT).gz example$(EXT)_zstd.gz
	./minigzip_zstd -d example$(EXT).gz
	./minigzip example$(EXT)
	#cp example$(EXT).gz example$(EXT)_gz.gz
	./minigzip_zstd -d example$(EXT).gz
	@echo ---- minigzip end ----
	./zwrapbench -qi1b3B1K $(TEST_FILE)
	./zwrapbench -rqi1b1e3 ../lib

#valgrindTest: ZSTDLIBRARY = $(ZSTDLIBDIR)/libzstd.so
valgrindTest: VALGRIND = LD_LIBRARY_PATH=$(ZSTDLIBDIR) valgrind --track-origins=yes --leak-check=full --error-exitcode=1
valgrindTest: clean example fitblk example_zstd fitblk_zstd zwrapbench
	@echo "\n ---- valgrind tests ----"
	$(VALGRIND) ./example
	$(VALGRIND) ./example_zstd
	$(VALGRIND) ./fitblk 10240 <$(TEST_FILE)
	$(VALGRIND) ./fitblk 40960 <$(TEST_FILE)
	$(VALGRIND) ./fitblk_zstd 10240 <$(TEST_FILE)
	$(VALGRIND) ./fitblk_zstd 40960 <$(TEST_FILE)
	$(VALGRIND) ./zwrapbench -qi1b3B1K $(TEST_FILE)
	$(VALGRIND) ./zwrapbench -rqi1b1e5 ../lib ../programs ../tests

#.c.o:
#	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

minigzip: minigzip.o zstd_zlibwrapper.o $(GZFILES) $(ZSTDLIBRARY)

minigzip_zstd: minigzip.o zstdTurnedOn_zlibwrapper.o $(GZFILES) $(ZSTDLIBRARY)
	$(LINK.o) $^ $(LDLIBS) $(OUTPUT_OPTION)

example: example.o zstd_zlibwrapper.o $(GZFILES) $(ZSTDLIBRARY)

example_zstd: example.o zstdTurnedOn_zlibwrapper.o $(GZFILES) $(ZSTDLIBRARY)
	$(LINK.o) $^ $(LDLIBS) $(OUTPUT_OPTION)

fitblk: fitblk.o zstd_zlibwrapper.o $(ZSTDLIBRARY)

fitblk_zstd: fitblk.o zstdTurnedOn_zlibwrapper.o $(ZSTDLIBRARY)
	$(LINK.o) $^ $(LDLIBS) $(OUTPUT_OPTION)

zwrapbench: zwrapbench.o zstd_zlibwrapper.o util.o timefn.o datagen.o $(ZSTDLIBRARY)


zstd_zlibwrapper.o: zstd_zlibwrapper.h

zstdTurnedOn_zlibwrapper.o: CPPFLAGS += -DZWRAP_USE_ZSTD=1
zstdTurnedOn_zlibwrapper.o: zstd_zlibwrapper.c zstd_zlibwrapper.h
	$(COMPILE.c) $< $(OUTPUT_OPTION)


$(ZSTDLIBRARY):
	$(MAKE) -C $(ZSTDLIBDIR) libzstd.a

$(ZSTDLIBDIR)/libzstd.so:
	$(MAKE) -C $(ZSTDLIBDIR) libzstd


clean:
	-$(RM) $(ZLIBWRAPPER_PATH)/*.o $(EXAMPLE_PATH)/*.o *.o foo.gz example$(EXT) example_zstd$(EXT) fitblk$(EXT) fitblk_zstd$(EXT) zwrapbench$(EXT) minigzip$(EXT) minigzip_zstd$(EXT)
	@echo Cleaning completed
