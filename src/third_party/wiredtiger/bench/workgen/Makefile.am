# FIX-ME-WT-8263: Remove the next line to keep the flags defined in configure.ac.in
AM_CXXFLAGS =
AM_CPPFLAGS = -I$(top_builddir)
AM_CPPFLAGS +=-I$(top_srcdir)/src/include
AM_CPPFLAGS +=-I$(top_srcdir)/test/utility

PYSRC = $(top_srcdir)/bench/workgen
PYDIRS = -t $(abs_builddir) -I $(abs_top_srcdir):$(abs_top_builddir) -L $(abs_top_builddir)/.libs:$(abs_top_builddir)/bench/workgen/.libs
all-local: _workgen.so libworkgen.la
libworkgen_la_SOURCES = workgen.cxx workgen_func.c
noinst_LTLIBRARIES = libworkgen.la

# We keep generated Python sources under bench/workgen.
$(PYSRC)/workgen_wrap.cxx: $(PYSRC)/workgen.h $(PYSRC)/workgen.i
	(cd $(PYSRC) && \
	    $(SWIG) -c++ -python -threads -O -Wall -I$(abs_top_builddir) -outdir ./workgen workgen.i)

_workgen.so: $(top_builddir)/libwiredtiger.la $(PYSRC)/workgen_wrap.cxx libworkgen.la $(PYSRC)/workgen.h $(PYSRC)/workgen_time.h
	(cd $(PYSRC) && \
	    $(PYTHON) setup.py build_ext -f -b $(abs_builddir) $(PYDIRS))

install-exec-local:
	(cd $(PYSRC) && \
	    $(PYTHON) setup.py build_py -d $(abs_builddir)/build && \
	    $(PYTHON) setup.py build_ext -f -b $(abs_builddir)/build $(PYDIRS) && \
	    $(PYTHON) setup.py install_lib -b $(abs_builddir)/build --skip-build $(PYTHON_INSTALL_ARG))

# We build in different places for an install vs running from the tree:
# clean up both.  Don't rely on "setup.py clean" -- everything that should
# be removed is created under the build directory.
clean-local:
	rm -rf build _workgen.so workgen_wrap.o WT_TEST
