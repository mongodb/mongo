#******************************************************************************
# Copyright (c) 2007-2011, Intel Corp.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#   * Redistributions of source code must retain the above copyright notice,
#     this list of conditions and the following disclaimer.
#   * Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the distribution.
#   * Neither the name of Intel Corporation nor the names of its contributors
#     may be used to endorse or promote products derived from this software
#     without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
# THE POSSIBILITY OF SUCH DAMAGE.
#***************************************************************************** 

# Makefile for the readtest program - tests for the Intel(r)
# Decimal Floating-Point Math Library

ifeq ($(CALL_BY_REF),1)
COPT1 = -DDECIMAL_CALL_BY_REFERENCE=1
else
COPT1 = -DDECIMAL_CALL_BY_REFERENCE=0
endif
 
ifeq ($(GLOBAL_RND),1)
COPT2 = -DDECIMAL_GLOBAL_ROUNDING=1
else
COPT2 = -DDECIMAL_GLOBAL_ROUNDING=0
endif
 
ifeq ($(GLOBAL_FLAGS),1)
COPT3 = -DDECIMAL_GLOBAL_EXCEPTION_FLAGS=1
else
COPT3 = -DDECIMAL_GLOBAL_EXCEPTION_FLAGS=0
endif

ifeq ($(BID_BIG_ENDIAN),1)
COPT4 = -DBID_BIG_ENDIAN=1
else
COPT4 = -UBID_BIG_ENDIAN
endif
 
ifeq ($(HPUX_OS),1)
ifeq ($(DD64),1)
COPT5 = -DHPUX_OS +DD64
else
COPT5 = -DHPUX_OS +DD32
endif
else
COPT5 = -UHPUX_OS
endif

ifeq ($(UNCHANGED_BINARY_FLAGS),1)
COPT6 = -DUNCHANGED_BINARY_STATUS_FLAGS 
else
COPT6 = 
endif

ifeq ($(OS_TYPE),LINUX)
CC =
ifeq ($(HPUX_OS),1)
COPT = +Ofenvaccess
else
ifeq ($(CC),icc)
COPT = -Werror -Qoption,cpp,--extended_float_types
else
COPT = -Werror
endif
endif

CFLAGS = -O0 -D__intptr_t_defined -DLINUX $(COPT) $(COPT1) $(COPT2) $(COPT3) $(COPT4) $(COPT5) $(COPT6) $(COPT_ADD) $(DEFINES)
EXE = 
OBJ = .o
LIBEXT = .a
RM = rm -f
LMOPT = -lm 

else
CC = icl
ifeq ($(CC),icl)
COPT = -D_CRT_SECURE_NO_DEPRECATE -Qlong_double -Qoption,cpp,--extended_float_types -Qpc80
LMOPT = 
else
COPT = -D_CRT_SECURE_NO_DEPRECATE -DBID_MS_FLAGS
ifeq ($(OS_TYPE),WIN_IA64)
LMOPT = bufferoverflowU.lib
else
LMOPT = 
endif
endif
CFLAGS = -Od -I./ -D__intptr_t_defined -DWINDOWS /nologo $(COPT) $(COPT1) $(COPT2) $(COPT3) $(COPT4) $(COPT6) $(COPT_ADD) $(DEFINES)
EXE = .exe
OBJ = .obj
LIBEXT = .lib
RM = del

endif

BID_LIB = ../LIBRARY/libbid$(LIBEXT)

default : readtest$(EXE)

%$(OBJ) : %.c
	$(CC) -c $(CFLAGS) $<

clean:
	$(RM) *$(OBJ)
	$(RM) readtest$(EXE)

readtest$(EXE): readtest.c readtest.h test_bid_conf.h test_bid_functions.h $(BID_LIB)
	$(CC) $(CFLAGS) -o readtest$(EXE) readtest.c $(BID_LIB) $(LMOPT)

