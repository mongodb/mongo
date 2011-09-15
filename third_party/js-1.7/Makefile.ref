# -*- Mode: makefile -*-
# vim: ft=make
#
# ***** BEGIN LICENSE BLOCK *****
# Version: MPL 1.1/GPL 2.0/LGPL 2.1
#
# The contents of this file are subject to the Mozilla Public License Version
# 1.1 (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
# http://www.mozilla.org/MPL/
#
# Software distributed under the License is distributed on an "AS IS" basis,
# WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
# for the specific language governing rights and limitations under the
# License.
#
# The Original Code is Mozilla Communicator client code, released
# March 31, 1998.
#
# The Initial Developer of the Original Code is
# Netscape Communications Corporation.
# Portions created by the Initial Developer are Copyright (C) 1998
# the Initial Developer. All Rights Reserved.
#
# Contributor(s):
#   Michael Ang <mang@subcarrier.org>
#   Kevin Buhr <buhr@stat.wisc.edu>
#
# Alternatively, the contents of this file may be used under the terms of
# either of the GNU General Public License Version 2 or later (the "GPL"),
# or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
# in which case the provisions of the GPL or the LGPL are applicable instead
# of those above. If you wish to allow use of your version of this file only
# under the terms of either the GPL or the LGPL, and not to allow others to
# use your version of this file under the terms of the MPL, indicate your
# decision by deleting the provisions above and replace them with the notice
# and other provisions required by the GPL or the LGPL. If you do not delete
# the provisions above, a recipient may use your version of this file under
# the terms of any one of the MPL, the GPL or the LGPL.
#
# ***** END LICENSE BLOCK *****

#
# JSRef GNUmake makefile.
#
# Note: dependency rules are missing for some files (some
#       .h, all .msg, etc.)  Re-make clean if in doubt.
#


DEPTH = .

include config.mk

#NS_USE_NATIVE = 1

ifdef NARCISSUS
DEFINES += -DNARCISSUS
endif

# Look in OBJDIR to find jsautocfg.h and jsautokw.h
INCLUDES   += -I$(OBJDIR)

ifdef JS_THREADSAFE
DEFINES += -DJS_THREADSAFE
INCLUDES += -I$(DIST)/include/nspr
ifdef USE_MSVC
OTHER_LIBS += $(DIST)/lib/libnspr$(NSPR_LIBSUFFIX).lib
else
OTHER_LIBS += -L$(DIST)/lib -lnspr$(NSPR_LIBSUFFIX)
endif
endif

ifdef JS_NO_THIN_LOCKS
DEFINES += -DJS_USE_ONLY_NSPR_LOCKS
endif

ifdef JS_HAS_FILE_OBJECT
DEFINES += -DJS_HAS_FILE_OBJECT
endif

#
# XCFLAGS may be set in the environment or on the gmake command line
#
CFLAGS          += $(OPTIMIZER) $(OS_CFLAGS) $(DEFINES) $(INCLUDES) $(XCFLAGS)

LDFLAGS		= $(XLDFLAGS)

ifndef NO_LIBM
LDFLAGS += -lm
endif

# Prevent floating point errors caused by VC++ optimizations
ifeq ($(OS_ARCH),WINNT)
_MSC_VER = $(shell $(CC) 2>&1 | sed -n 's/.*Compiler Version \([0-9]*\)\.\([0-9]*\).*/\1\2/p')
ifeq (,$(filter-out 1200 1300 1310,$(_MSC_VER)))
CFLAGS += -Op
else
CFLAGS += -fp:precise
endif
endif # WINNT

#
# Ask perl what flags it was built with, so we can build js with similar flags
# and link properly.  Viva gmake.
#
ifdef JS_PERLCONNECT
DEFINES += -DPERLCONNECT -D_GNU_SOURCE

PERLCFLAGS := $(shell perl -MExtUtils::Embed -e ccopts)
PERLLDFLAGS := $(shell perl -MExtUtils::Embed -e ldopts)

# perl erroneously reports compiler flag -rdynamic (interpreted by ld
# as -r) when it really meant -export-dynamic.
PERLLDFLAGS := $(subst -rdynamic,-export-dynamic,$(PERLLDFLAGS))

CFLAGS += $(PERLCFLAGS)
#LDFLAGS += $(PERLLDFLAGS) #PH removed this assgnment
INCLUDES += -I. #needed for perlconnect/jsperl.c
endif

#
#	Server-related changes :
#
ifdef NES40
DEFINES += -DNES40
endif

#
# Line editing support.
# Define JS_READLINE or JS_EDITLINE to enable line editing in the
# js command-line interpreter.
#
ifdef JS_READLINE
# For those platforms with the readline library installed.
DEFINES += -DEDITLINE
PROG_LIBS += -lreadline -ltermcap
else
ifdef JS_EDITLINE
# Use the editline library, built locally.
PREDIRS += editline
DEFINES += -DEDITLINE
PROG_LIBS += editline/$(OBJDIR)/libedit.a
endif
endif

# For purify
PURE_CFLAGS     = -DXP_UNIX $(OPTIMIZER) $(PURE_OS_CFLAGS) $(DEFINES) \
                  $(INCLUDES) $(XCFLAGS)

#
# JS file lists
#
JS_HFILES =		\
	jsarray.h	\
	jsatom.h	\
	jsbool.h	\
	jsconfig.h	\
	jscntxt.h	\
	jsdate.h	\
	jsemit.h	\
	jsexn.h		\
	jsfun.h		\
	jsgc.h		\
	jsinterp.h	\
	jsiter.h	\
	jslibmath.h	\
	jslock.h	\
	jsmath.h	\
	jsnum.h		\
	jsobj.h		\
	jsopcode.h      \
	jsparse.h	\
	jsarena.h	\
	jsclist.h	\
	jsdhash.h	\
	jsdtoa.h	\
	jshash.h	\
	jslong.h	\
	jsosdep.h	\
	jstypes.h	\
	jsprvtd.h	\
	jspubtd.h	\
	jsregexp.h	\
	jsscan.h	\
	jsscope.h	\
	jsscript.h	\
	jsstr.h		\
	jsxdrapi.h	\
	jsxml.h		\
	$(NULL)

API_HFILES =		\
	jsapi.h		\
	jsdbgapi.h	\
	$(NULL)

OTHER_HFILES =		\
	jsbit.h		\
	jscompat.h	\
	jscpucfg.h	\
	jsotypes.h	\
	jsstddef.h	\
	prmjtime.h	\
	resource.h	\
	jsopcode.tbl	\
	jsproto.tbl     \
	js.msg		\
	jsshell.msg	\
	jskeyword.tbl	\
	$(NULL)

ifndef PREBUILT_CPUCFG
OTHER_HFILES += $(OBJDIR)/jsautocfg.h
endif
OTHER_HFILES += $(OBJDIR)/jsautokw.h

HFILES = $(JS_HFILES) $(API_HFILES) $(OTHER_HFILES)

JS_CFILES =		\
	jsapi.c		\
	jsarena.c	\
	jsarray.c	\
	jsatom.c	\
	jsbool.c	\
	jscntxt.c	\
	jsdate.c	\
	jsdbgapi.c	\
	jsdhash.c	\
	jsdtoa.c	\
	jsemit.c	\
	jsexn.c		\
	jsfun.c		\
	jsgc.c		\
	jshash.c	\
	jsinterp.c	\
	jsiter.c	\
	jslock.c	\
	jslog2.c	\
	jslong.c	\
	jsmath.c	\
	jsnum.c		\
	jsobj.c		\
	jsopcode.c      \
	jsparse.c	\
	jsprf.c		\
	jsregexp.c	\
	jsscan.c	\
	jsscope.c	\
	jsscript.c	\
	jsstr.c		\
	jsutil.c        \
	jsxdrapi.c	\
	jsxml.c		\
	prmjtime.c	\
	$(NULL)

ifdef JS_LIVECONNECT
DIRS      += liveconnect
endif

ifdef JS_PERLCONNECT
JS_CFILES += perlconnect/jsperl.c
endif

ifdef JS_HAS_FILE_OBJECT
JS_CFILES += jsfile.c
JS_HFILES += jsfile.h
endif

LIB_CFILES  = $(JS_CFILES)
LIB_ASFILES := $(wildcard *_$(OS_ARCH).s)
PROG_CFILES = js.c

ifdef USE_MSVC
LIBRARY = $(OBJDIR)/js32.lib
SHARED_LIBRARY = $(OBJDIR)/js32.dll
PROGRAM = $(OBJDIR)/js.exe
else
LIBRARY = $(OBJDIR)/libjs.a
SHARED_LIBRARY = $(OBJDIR)/libjs.$(SO_SUFFIX)
PROGRAM = $(OBJDIR)/js
ifdef JS_PERLCONNECT
PROG_LIBS += $(PERLLDFLAGS)
endif
endif

include rules.mk

MOZ_DEPTH = ../..
include jsconfig.mk

nsinstall-target:
	cd ../../config; $(MAKE) OBJDIR=$(OBJDIR) OBJDIR_NAME=$(OBJDIR)

#
# Rules for keyword switch generation
#

GARBAGE += $(OBJDIR)/jsautokw.h $(OBJDIR)/jskwgen$(HOST_BIN_SUFFIX)
GARBAGE += $(OBJDIR)/jskwgen.$(OBJ_SUFFIX)

$(OBJDIR)/jsscan.$(OBJ_SUFFIX): $(OBJDIR)/jsautokw.h jskeyword.tbl

$(OBJDIR)/jskwgen.$(OBJ_SUFFIX): jskwgen.c jskeyword.tbl

$(OBJDIR)/jsautokw.h: $(OBJDIR)/jskwgen$(HOST_BIN_SUFFIX) jskeyword.tbl
	$(OBJDIR)/jskwgen$(HOST_BIN_SUFFIX) $@

ifdef USE_MSVC

$(OBJDIR)/jskwgen.obj: jskwgen.c jskeyword.tbl
	@$(MAKE_OBJDIR)
	$(CC) -Fo$(OBJDIR)/ -c $(CFLAGS) $<

$(OBJDIR)/jskwgen$(HOST_BIN_SUFFIX): $(OBJDIR)/jskwgen.$(OBJ_SUFFIX)
	link.exe -out:"$@" $(EXE_LINK_FLAGS) $^

else

$(OBJDIR)/jskwgen.o: jskwgen.c jskeyword.tbl
	@$(MAKE_OBJDIR)
	$(CC) -o $@ -c $(CFLAGS) $<

$(OBJDIR)/jskwgen$(HOST_BIN_SUFFIX): $(OBJDIR)/jskwgen.$(OBJ_SUFFIX)
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $^

endif

#
# JS shell executable
#

ifdef USE_MSVC
$(PROGRAM): $(PROG_OBJS) $(LIBRARY)
	link.exe -out:"$@" $(EXE_LINK_FLAGS) $^
else
$(PROGRAM): $(PROG_OBJS) $(LIBRARY)
	$(CC) -o $@ $(CFLAGS) $(PROG_OBJS) $(LIBRARY) $(LDFLAGS) $(OTHER_LIBS) \
	    $(PROG_LIBS)
endif

$(PROGRAM).pure: $(PROG_OBJS) $(LIBRARY)
	purify $(PUREFLAGS) \
	    $(CC) -o $@ $(PURE_OS_CFLAGS) $(PROG_OBJS) $(LIBRARY) $(LDFLAGS) \
		$(OTHER_LIBS) $(PROG_LIBS)

ifndef PREBUILT_CPUCFG
$(HFILES) $(CFILES): $(OBJDIR)/jsautocfg.h

$(OBJDIR)/jsautocfg.h: $(OBJDIR)/jscpucfg
	rm -f $@
	$(OBJDIR)/jscpucfg > $@

$(OBJDIR)/jscpucfg: $(OBJDIR)/jscpucfg.o
	$(CC) -o $@ $(OBJDIR)/jscpucfg.o

# Add to TARGETS for clobber rule
TARGETS += $(OBJDIR)/jsautocfg.h $(OBJDIR)/jscpucfg \
	$(OBJDIR)/jscpucfg.o
endif

#
# Hardwire dependencies on jsopcode.tbl
#
jsopcode.h jsopcode.c: jsopcode.tbl

-include $(DEPENDENCIES)

TARNAME = jsref.tar
TARFILES = files `cat files`

SUFFIXES: .i
%.i: %.c
	$(CC) -C -E $(CFLAGS) $< > $*.i
