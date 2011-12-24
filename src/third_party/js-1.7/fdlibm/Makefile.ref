# -*- Mode: makefile -*-
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
# Sun Microsystems, Inc.
# Portions created by the Initial Developer are Copyright (C) 1998
# the Initial Developer. All Rights Reserved.
#
# Contributor(s):
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
#  @(#)Makefile 1.4 95/01/18 
# 
#  ====================================================
#  Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
# 
#  Developed at SunSoft, a Sun Microsystems, Inc. business.
#  Permission to use, copy, modify, and distribute this
#  software is freely granted, provided that this notice 
#  is preserved.
#  ====================================================
# 
# 

#
# There are two options in making libm at fdlibm compile time:
#       _IEEE_LIBM      --- IEEE libm; smaller, and somewhat faster
#       _MULTI_LIBM     --- Support multi-standard at runtime by 
#                           imposing wrapper functions defined in 
#                           fdlibm.h:
#                               _IEEE_MODE      -- IEEE
#                               _XOPEN_MODE     -- X/OPEN
#                               _POSIX_MODE     -- POSIX/ANSI
#                               _SVID3_MODE     -- SVID
#
# Here is how to set up CFLAGS to create the desired libm at 
# compile time:
#
#       CFLAGS = -D_IEEE_LIBM           ... IEEE libm (recommended)
#       CFLAGS = -D_SVID3_MODE  ... Multi-standard supported
#                                           libm with SVID as the 
#                                           default standard
#       CFLAGS = -D_XOPEN_MODE  ... Multi-standard supported
#                                           libm with XOPEN as the 
#                                           default standard
#       CFLAGS = -D_POSIX_MODE  ... Multi-standard supported
#                                           libm with POSIX as the 
#                                           default standard
#       CFLAGS =                        ... Multi-standard supported
#                                           libm with IEEE as the 
#                                           default standard
# 
# NOTE: if scalb's second arguement is an int, then one must
# define _SCALB_INT in CFLAGS. The default prototype of scalb
# is double scalb(double, double)
#

DEPTH           = ..

include $(DEPTH)/config.mk

#
# Default IEEE libm
#
CFLAGS          += -DXP_UNIX $(OPTIMIZER) $(OS_CFLAGS) $(DEFINES) $(INCLUDES) \
		  -DJSFILE $(XCFLAGS) -D_IEEE_LIBM

# Need for jstypes.h and friends
INCLUDES += -I..
INCLUDES += -I../$(OBJDIR)

#CC = cc

INCFILES = fdlibm.h
.INIT: $(INCFILES)
.KEEP_STATE:
FDLIBM_CFILES =         \
	k_standard.c k_rem_pio2.c \
	k_cos.c k_sin.c k_tan.c \
	e_acos.c e_acosh.c e_asin.c e_atan2.c \
	e_atanh.c e_cosh.c e_exp.c e_fmod.c \
	e_gamma.c e_gamma_r.c e_hypot.c e_j0.c \
	e_j1.c e_jn.c e_lgamma.c e_lgamma_r.c \
	e_log.c e_log10.c e_pow.c e_rem_pio2.c e_remainder.c \
	e_scalb.c e_sinh.c e_sqrt.c \
	w_acos.c w_acosh.c w_asin.c w_atan2.c \
	w_atanh.c w_cosh.c w_exp.c w_fmod.c \
	w_gamma.c w_gamma_r.c w_hypot.c w_j0.c \
	w_j1.c w_jn.c w_lgamma.c w_lgamma_r.c \
	w_log.c w_log10.c w_pow.c w_remainder.c \
	w_scalb.c w_sinh.c w_sqrt.c \
	s_asinh.c s_atan.c s_cbrt.c s_ceil.c s_copysign.c \
	s_cos.c s_erf.c s_expm1.c s_fabs.c s_finite.c s_floor.c \
	s_frexp.c s_ilogb.c s_isnan.c s_ldexp.c s_lib_version.c \
	s_log1p.c s_logb.c s_matherr.c s_modf.c s_nextafter.c \
	s_rint.c s_scalbn.c s_signgam.c s_significand.c s_sin.c \
	s_tan.c s_tanh.c

ifdef USE_MSVC
FDLIBM_OBJS = $(addprefix $(OBJDIR)/, $(FDLIBM_CFILES:.c=.obj))
else
FDLIBM_OBJS = $(addprefix $(OBJDIR)/, $(FDLIBM_CFILES:.c=.o))
endif

ifdef USE_MSVC
LIBRARY = $(OBJDIR)/fdlibm.lib
else
LIBRARY = $(OBJDIR)/libfdm.a
endif

define MAKE_OBJDIR
if test ! -d $(@D); then rm -rf $(@D); mkdir -p $(@D); fi
endef

all: $(LIBRARY) 

export:

$(OBJDIR)/%: %.c
	@$(MAKE_OBJDIR)
	$(CC) -o $@ $(CFLAGS) $*.c $(LDFLAGS)

$(OBJDIR)/%.o: %.c
	@$(MAKE_OBJDIR)
	$(CC) -o $@ -c $(CFLAGS) $*.c

$(OBJDIR)/%.o: %.s
	@$(MAKE_OBJDIR)
	$(AS) -o $@ $(ASFLAGS) $*.s

# windows only
$(OBJDIR)/%.obj: %.c
	@$(MAKE_OBJDIR)
	$(CC) -Fo$(OBJDIR)/ -c $(CFLAGS) $*.c

ifeq ($(OS_ARCH),OS2)
$(LIBRARY): $(FDLIBM_OBJS)
	$(AR) $@ $? $(AR_OS2_SUFFIX)
	$(RANLIB) $@
else
ifdef USE_MSVC
$(LIBRARY): $(FDLIBM_OBJS)
	lib.exe /out:"$@" $?
else
$(LIBRARY): $(FDLIBM_OBJS)
	$(AR) rv $@ $?
	$(RANLIB) $@
endif
endif

libfdm.a : $(FDLIBM_OBJS) 
	$(AR) cru $(OBJDIR)/libfdm.a $(FDLIBM_OBJS)
	$(RANLIB) $(OBJDIR)/libfdm.a

clean:
	rm -rf $(FDLIBM_OBJS)

clobber:
	rm -rf $(FDLIBM_OBJS) $(LIBRARY) $(DEPENDENCIES)

SUFFIXES: .i
%.i: %.c
	$(CC) -C -E $(CFLAGS) $< > $*.i
