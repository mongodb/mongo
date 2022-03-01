# Copyright (C) 2016 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html
#******************************************************************************
#
#   Copyright (C) 2010-2011, International Business Machines Corporation and others.  All Rights Reserved.
#
#******************************************************************************
# This is to be called from ../Makefile.in
#
# This will only work if subversion is installed.
# You must checkout ICU4C at the `/icu`  or `/icu/icu4c` level - not just `…/source`
# also note that `make dist` does NOT reflect any local modifications, but pulls from HEAD.

top_builddir = .

include $(top_builddir)/icudefs.mk

DISTY_DIR=dist
DISTY_TMP=dist/tmp
DISTY_ICU=$(DISTY_TMP)/icu
DISTY_DATA=$(DISTY_ICU)/source/data
# The following line controls what is removed in the data/ subdirectory for the source tarball.
DISTY_RMV=brkitr coll curr lang locales mappings rbnf region translit xml zone misc/*.txt misc/*.mk unit
DISTY_RMDIR=$(DISTY_RMV:%=$(DISTY_DATA)/%)
DISTY_IN=$(DISTY_DATA)/in
DOCZIP=icu-docs.zip

ICU4CTOP=$(top_srcdir)/..
GITVER=$(shell (cd $(ICU4CTOP) && (git describe --tags --exact-match 2>/dev/null || git rev-parse --short HEAD)) || echo 'unknown')
DISTY_VER=$(shell echo $(VERSION) | tr '.' '_' )
DISTY_PREFIX=icu4c
DISTY_FILE_DIR=$(shell pwd)/$(DISTY_DIR)
DISTY_FILE_TGZ=$(DISTY_FILE_DIR)/$(DISTY_PREFIX)-$(DISTY_VER)-$(GITVER)-src.tgz
DISTY_FILE_ZIP=$(DISTY_FILE_DIR)/$(DISTY_PREFIX)-$(DISTY_VER)-$(GITVER)-src.zip
DISTY_DOC_ZIP=$(DISTY_FILE_DIR)/$(DISTY_PREFIX)-$(DISTY_VER)-$(GITVER)-docs.zip
DISTY_DATA_ZIP=$(DISTY_FILE_DIR)/$(DISTY_PREFIX)-$(DISTY_VER)-$(GITVER)-data.zip
DISTY_DAT:=$(firstword $(wildcard data/out/tmp/icudt$(SO_TARGET_VERSION_MAJOR)*.dat))

DISTY_FILES_SRC=$(DISTY_FILE_TGZ) $(DISTY_FILE_ZIP)
DISTY_FILES=$(DISTY_FILES_SRC) $(DISTY_DOC_ZIP)
# colon-equals because we watn to run this once!
EXCLUDES_FILE:=$(shell mktemp)

$(DISTY_FILE_DIR):
	$(MKINSTALLDIRS) $(DISTY_FILE_DIR)

$(DISTY_TMP):
	$(MKINSTALLDIRS) $(DISTY_TMP)

$(DISTY_DOC_ZIP):  $(DOCZIP) $(DISTY_FILE_DIR)
	cp $(DOCZIP) $(DISTY_DOC_ZIP)
	ln -sf $(shell basename $(DISTY_DOC_ZIP)) $(DISTY_FILE_DIR)/icu4c-docs.zip
	ln -f $(DISTY_DOC_ZIP) $(DISTY_FILE_DIR)/icu4c-$(DISTY_VER)-docs.zip

$(DISTY_DAT):
	echo Missing $@
	/bin/false

# make sure we get the non-lgpl docs
$(DOCZIP):
	-$(RMV) "$(top_builddir)"/doc
	"$(MAKE)" -C . srcdir="$(srcdir)" top_srcdir="$(top_srcdir)" builddir=. $@

$(DISTY_FILE_TGZ) $(DISTY_FILE_ZIP) $(DISTY_DATA_ZIP):  $(DISTY_DAT) $(DISTY_TMP)
	@echo Export icu4c@$(GITVER) to "$(DISTY_TMP)/icu"
	-$(RMV) $(DISTY_FILE) $(DISTY_TMP)
	$(MKINSTALLDIRS) $(DISTY_TMP)
	( cd $(ICU4CTOP)/.. && git archive --format=tar --prefix=icu/ HEAD:icu4c/ ) | ( cd "$(DISTY_TMP)" && tar xf - )
	( cd $(DISTY_TMP)/icu/source ; zip -rlq $(DISTY_DATA_ZIP) data )
	$(MKINSTALLDIRS) $(DISTY_IN)
	echo DISTY_DAT=$(DISTY_DAT)
	cp $(DISTY_DAT) $(DISTY_IN)
	$(RMV) $(DISTY_RMDIR)
	( cd $(DISTY_TMP)/icu ; python as_is/bomlist.py > as_is/bomlist.txt || rm -f as_is/bomlist.txt )
	( cd $(DISTY_TMP) ; tar cfpz $(DISTY_FILE_TGZ) icu )
	( cd $(DISTY_TMP) ; zip -rlq $(DISTY_FILE_ZIP) icu )
	$(RMV) $(DISTY_TMP)
	ln -sf $(shell basename $(DISTY_FILE_ZIP)) $(DISTY_FILE_DIR)/icu4c-src.zip
	ln -sf $(shell basename $(DISTY_FILE_TGZ)) $(DISTY_FILE_DIR)/icu4c-src.tgz
	ln -sf $(shell basename $(DISTY_DATA_ZIP)) $(DISTY_FILE_DIR)/icu4c-data.zip
	ln -f $(DISTY_FILE_ZIP) $(DISTY_FILE_DIR)/icu4c-$(DISTY_VER)-src.zip
	ln -f $(DISTY_FILE_TGZ) $(DISTY_FILE_DIR)/icu4c-$(DISTY_VER)-src.tgz
	ln -f $(DISTY_DATA_ZIP) $(DISTY_FILE_DIR)/icu4c-$(DISTY_VER)-data.zip
	ls -l $(DISTY_FILE_TGZ) $(DISTY_FILE_ZIP) $(DISTY_DATA_ZIP)

dist-local: $(DISTY_FILES)
	VERSION=$(VERSION) $(SHELL) $(top_srcdir)/config/dist-data.sh

distcheck: distcheck-tgz

DISTY_CHECK=$(DISTY_TMP)/check

distcheck-tgz: $(DISTY_FILE_TGZ)
	@echo Checking $(DISTY_FILE_TGZ)
	@-$(RMV) $(DISTY_CHECK)
	@$(MKINSTALLDIRS) $(DISTY_CHECK)
	@(cd $(DISTY_CHECK) && tar xfpz $(DISTY_FILE_TGZ) && cd icu/source && $(SHELL) ./configure $(DISTCHECK_CONFIG_OPTIONS) && $(MAKE) check $(DISTCHECK_MAKE_OPTIONS) ) && (echo "!!! PASS: $(DISTY_FILE_TGZ)" )
