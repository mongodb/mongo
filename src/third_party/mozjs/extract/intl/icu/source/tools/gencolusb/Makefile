## Makefile for rebuilding 'unsafe backward' data
## Copyright (C) 2016 and later: Unicode, Inc. and others.
## License & terms of use: http://www.unicode.org/copyright.html
## Copyright (c) 2015, International Business Machines Corporation and
## others. All Rights Reserved.

##
## CONFIGURATION:
## 1. create Makefile.local containing overrides if necessary:
##    BUILD_ROOT=/home/user/icu-build    (location of 'config.status' etc.)
##    PATH_VAR=DYLD_LIBRARY_PATH         (if on OSX etc)
##
## UPDATING
## 1.  make 'reset-icu' will reset ICU to 'bootstrap' state, zeroing out source/i18n/collunsafe.h
## 2.  make 'gen-file'  will generate and test source/i18n/collunsafe.h

subdir=tools/gencolusb
srcdir=$(shell pwd)
SOURCE_ROOT=$(shell cd ../.. ; pwd)
BUILD_ROOT=$(SOURCE_ROOT)
BUILD_HERE=$(BUILD_ROOT)/$(subdir)
TOOL=extract_unsafe_backwards
TEST=verify_uset
PATH_VAR=LD_LIBRARY_PATH

-include Makefile.local

GEN_FILE=$(SOURCE_ROOT)/i18n/collunsafe.h
BUILD_OPTS=-I$(SOURCE_ROOT)/common -I$(SOURCE_ROOT)/i18n -L$(BUILD_ROOT)/lib -licuuc -licui18n -licudata
RUN_OPTS=env $(PATH_VAR)=$(BUILD_ROOT)/lib

reset-icu:
	>$(GEN_FILE)
	$(MAKE) -C $(BUILD_ROOT)/i18n

gen-file: reset-icu
	mkdir -p $(BUILD_HERE)
	$(CXX) -o $(BUILD_HERE)/$(TOOL) $(srcdir)/$(TOOL).cpp  $(BUILD_OPTS)
	$(RUN_OPTS) $(BUILD_HERE)/$(TOOL) > $(GEN_FILE) || exit 1
	$(CXX) -o $(BUILD_HERE)/$(TEST) $(srcdir)/$(TEST).cpp $(BUILD_OPTS)
	$(RUN_OPTS) $(BUILD_HERE)/$(TEST) || exit 1
	$(MAKE) -C $(BUILD_ROOT)/i18n
	$(RUN_OPTS) $(BUILD_HERE)/$(TEST) || exit 1

.PHONY: reset-icu gen-file
