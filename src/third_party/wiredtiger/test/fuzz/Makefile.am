AM_CPPFLAGS = -fsanitize=fuzzer-no-link
AM_CPPFLAGS += -I$(top_builddir)
AM_CPPFLAGS += -I$(top_srcdir)/src/include
AM_CPPFLAGS += -I$(top_srcdir)/test/fuzz
AM_CPPFLAGS += -I$(top_srcdir)/test/utility

AM_LDFLAGS = -fsanitize=fuzzer -static

noinst_LTLIBRARIES = libfuzz_util.la
noinst_PROGRAMS = fuzz_config fuzz_modify

libfuzz_util_la_SOURCES = fuzz_util.c
libfuzz_util_la_LIBADD = $(top_builddir)/libwiredtiger.la
libfuzz_util_la_LIBADD += $(top_builddir)/test/utility/libtest_util.la

fuzz_config_SOURCES = config/fuzz_config.c
fuzz_config_LDADD = libfuzz_util.la

fuzz_modify_SOURCES = modify/fuzz_modify.c
fuzz_modify_LDADD = libfuzz_util.la
