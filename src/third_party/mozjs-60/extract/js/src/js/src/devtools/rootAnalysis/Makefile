# THIS FILE WAS AUTOMATICALLY GENERATED. DO NOT MODIFY BY HAND.
DEPTH := ../../../..
topobjdir := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src
topsrcdir := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release
srcdir := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/devtools/rootAnalysis
VPATH := /home/ec2-user/src/mongo/src/third_party/mozjs-60/mozilla-release/js/src/devtools/rootAnalysis
relativesrcdir := js/src/devtools/rootAnalysis
include $(DEPTH)/config/autoconf.mk
# -*- Mode: makefile -*-
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# This Makefile is used to kick off a static rooting analysis. This Makefile is
# NOT intended for use as part of the standard Mozilla build. Instead, this
# Makefile will use $PATH to subvert compiler invocations to add in the sixgill
# plugin, and then do a regular build of whatever portion of the tree you are
# analyzing. The plugins will dump out several xdb database files. Various
# analysis scripts, written in JS, will run over those database files to
# produce the final analysis output.

include $(topsrcdir)/config/config.mk

# Tree to build and analyze, defaulting to the current tree
TARGET_JSOBJDIR ?= $(MOZ_BUILD_ROOT)

# Path to a JS binary to use to run the analysis. You really want this to be an
# optimized build.
JS ?= $(MOZ_BUILD_ROOT)/shell/js

# Path to an xgill checkout containing the GCC plugin, xdb-processing binaries,
# and compiler wrapper scripts used to automatically integrate into an existing
# build system.
SIXGILL ?= 

# Path to the JS scripts that will perform the analysis, defaulting to the same
# place as this Makefile.in, which is probably what you want.
ANALYSIS_SCRIPT_DIR ?= $(srcdir)

# Number of simultanous analyzeRoots.js scripts to run.
JOBS ?= 6

all : rootingHazards.txt allFunctions.txt

CALL_JS := time env PATH=$$PATH:$(SIXGILL)/bin XDB=$(SIXGILL)/bin/xdb.so $(JS) 

src_body.xdb src_comp.xdb: run_complete
	@echo Started compilation at $$(date)
	$(ANALYSIS_SCRIPT_DIR)/run_complete --foreground --build-root=$(TARGET_JSOBJDIR) --work-dir=work -b $(SIXGILL)/bin $(CURDIR)
	@echo Finished compilation at $$(date)

callgraph.txt: src_body.xdb src_comp.xdb computeCallgraph.js
	@echo Started computation of $@ at $$(date)
	$(CALL_JS) $(ANALYSIS_SCRIPT_DIR)/computeCallgraph.js > $@.tmp
	mv $@.tmp $@
	@echo Finished computation of $@ at $$(date)

gcFunctions.txt: callgraph.txt computeGCFunctions.js annotations.js
	@echo Started computation of $@ at $$(date)
	$(CALL_JS) $(ANALYSIS_SCRIPT_DIR)/computeGCFunctions.js ./callgraph.txt > $@.tmp
	mv $@.tmp $@
	@echo Finished computation of $@ at $$(date)

gcFunctions.lst: gcFunctions.txt
	perl -lne 'print $$1 if /^GC Function: (.*)/' gcFunctions.txt > $@

suppressedFunctions.lst: gcFunctions.txt
	perl -lne 'print $$1 if /^Suppressed Function: (.*)/' gcFunctions.txt > $@

gcTypes.txt: src_comp.xdb computeGCTypes.js annotations.js
	@echo Started computation of $@ at $$(date)
	$(CALL_JS) $(ANALYSIS_SCRIPT_DIR)/computeGCTypes.js > $@.tmp
	mv $@.tmp $@
	@echo Finished computation of $@ at $$(date)

allFunctions.txt: src_body.xdb
	@echo Started computation of $@ at $$(date)
	time $(SIXGILL)/bin/xdbkeys $^ > $@.tmp
	mv $@.tmp $@
	@echo Finished computation of $@ at $$(date)

rootingHazards.txt: gcFunctions.lst suppressedFunctions.lst gcTypes.txt analyzeRoots.js annotations.js gen-hazards.sh
	@echo Started computation of $@ at $$(date)
	time env JS=$(JS) ANALYZE='$(ANALYSIS_SCRIPT_DIR)/analyzeRoots.js' SIXGILL='$(SIXGILL)' '$(ANALYSIS_SCRIPT_DIR)/gen-hazards.sh' $(JOBS) > $@.tmp
	mv $@.tmp $@
	@echo Finished computation of $@ at $$(date)

include $(topsrcdir)/config/recurse.mk
