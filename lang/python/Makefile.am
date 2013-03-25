AM_CPPFLAGS = -I$(abs_top_builddir)

PYSRC = $(top_srcdir)/lang/python
if DEBUG
PY_SETUP_DEBUG = -g
endif
all-local: _wiredtiger.so
$(PYSRC)/wiredtiger_wrap.c: $(top_srcdir)/src/include/wiredtiger.in $(PYSRC)/wiredtiger.i
	@(cd $(PYSRC) && \
	    $(SWIG) -python -threads -O -Wall -nodefaultctor -nodefaultdtor -I$(abs_top_builddir) wiredtiger.i)

_wiredtiger.so: $(top_builddir)/libwiredtiger.la $(PYSRC)/wiredtiger_wrap.c
	$(PYTHON) $(PYSRC)/setup.py build_ext -b . -t . -f $(PY_SETUP_DEBUG)

TESTS = run-ex_access

clean-local:
	rm -rf WT_TEST
