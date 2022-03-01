# Copyright (C) 2016 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html
#******************************************************************************
#
#   Copyright (C) 2002-2011, International Business Machines
#   Corporation and others.  All Rights Reserved.
#
#******************************************************************************

TOP=../..

srcdir = .
top_srcdir = $(TOP)
top_builddir = $(TOP)

# override if you have an out-of-source build (not yet working.)
BUILDDIR = $(top_builddir)

ICUDIR=ICUunrenamed

# Extra flags to prevent internal API from being hidden.
# This is important because ELF (Linux) based platforms that don't hide internal
# API will allow a duplicate internal name to resolve to an external library.
# See the gcc manual on the "visibility" attribute for details.
FLAG_OVERRIDE= LIBCFLAGS= LIBCXXFLAGS= $(EXTRA_MAKE_OPTIONS)

## any local overrides
-include Makefile.local

#  load definition of .SO, etc (but not if we are doing 'make clean')
ifneq ($(patsubst %clean,,$(MAKECMDGOALS)),)
include $(BUILDDIR)/icudefs.mk
endif

# For MinGW, do we want the DLL to go in the bin location?
ifeq ($(MINGW_MOVEDLLSTOBINDIR),YES)
installdir = bin
else
installdir = lib
endif

COM=$(ICUDIR)/$(installdir)/libicuuc.$(SO)
I18=$(ICUDIR)/$(installdir)/libicui18n.$(SO)
LEX=$(ICUDIR)/$(installdir)/libiculx.$(SO)
DAT=$(ICUDIR)/stubdata/libicudata.$(SO)
UIO=$(ICUDIR)/$(installdir)/libicuio.$(SO)

LIBS=$(COM) $(I18) $(LEX) $(UIO)

## Targets.


all:
	@cat README

clean:
	-rm -rf $(ICUDIR) urename.* *~

# We use config.status to mean we have a valid out of source tree.

$(ICUDIR)/config.status:
	-mv $(ICUDIR) $(ICUDIR)old
	-(rm -rf $(ICUDIR)old &)
	mkdir $(ICUDIR)
	( cd $(ICUDIR) ; CPPFLAGS="-DU_DISABLE_RENAMING=1 -DUCONFIG_ENABLE_PLUGINS" $(GENREN_CONFIGURE_ENV) $(top_srcdir)/../configure --with-data-packaging=archive --enable-tests=no --prefix=`pwd` $(GENREN_CONFIGURE_OPTS) )
    # cause lib and bin to be created, and any other general sanity
	$(MAKE) $(FLAG_OVERRIDE) -C $(ICUDIR) clean
	$(MAKE) $(FLAG_OVERRIDE) -C $(ICUDIR) all-local

# build the libraries
$(DAT): $(ICUDIR)/config.status Makefile
	$(MAKE) $(FLAG_OVERRIDE) -C $(ICUDIR)/stubdata  all-local

$(COM): $(DAT) $(ICUDIR)/config.status Makefile
	$(MAKE) $(FLAG_OVERRIDE) -C $(ICUDIR)/common all-local

$(I18): $(DAT) $(COM) $(ICUDIR)/config.status Makefile
	$(MAKE) $(FLAG_OVERRIDE) -C $(ICUDIR)/i18n all-local

$(LEX): $(DAT) $(I18) $(COM) $(ICUDIR)/config.status Makefile
	$(MAKE) $(FLAG_OVERRIDE) -C $(ICUDIR)/layoutex all-local

$(UIO):	$(DAT) $(I18) $(COM) $(ICUDIR)/config.status Makefile
	$(MAKE) $(FLAG_OVERRIDE) -C $(ICUDIR)/io all-local

# the header itself
urename.h: $(LIBS) genren.pl
	-cp urename.h urename.h.old
	perl ./genren.pl $(GENREN_PL_OPTS) $(LIBS)

# This is still here, but less useful with the "new" macro-based rename. Just use 'svn diff'.
sorts:	urename.sort urename.old.sort
	@echo "*** Please check urename.h manually before committing it."
	@echo "Try 'diff --side-by-side urename.old.sort urename.sort'"

urename.sort: urename.h
	sort urename.h > $@

urename.old.sort: $(top_srcdir)/common/unicode/urename.h
	sort  $(top_srcdir)/common/unicode/urename.h > $@

install-header: urename.h
	cp urename.h $(top_srcdir)/common/unicode/
	@echo "*** Please check urename.h manually before committing it."

