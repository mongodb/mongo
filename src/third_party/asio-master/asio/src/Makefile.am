if STANDALONE
SUBDIRS = examples/cpp11 tests
else
SUBDIRS = examples/cpp03 tests
endif

DIST_SUBDIRS = examples/cpp03 examples/cpp11 examples/cpp14 tests

EXTRA_DIST = \
	Makefile.mgw \
	Makefile.msc \
	tools/handlerviz.pl

MAINTAINERCLEANFILES = \
	$(srcdir)/Makefile.in
