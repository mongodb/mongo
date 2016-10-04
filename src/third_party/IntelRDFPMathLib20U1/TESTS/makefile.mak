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

!IFDEF CALL_BY_REF
!IF ($(CALL_BY_REF)==1)
COPT1 = -DDECIMAL_CALL_BY_REFERENCE=1
!ELSE
COPT1 = -DDECIMAL_CALL_BY_REFERENCE=0
!ENDIF
!ENDIF

!IFDEF GLOBAL_RND 
!IF ($(GLOBAL_RND)==1)
COPT2 = -DDECIMAL_GLOBAL_ROUNDING=1
!ELSE
COPT2 = -DDECIMAL_GLOBAL_ROUNDING=0
!ENDIF
!ENDIF

!IFDEF GLOBAL_FLAGS 
!IF ($(GLOBAL_FLAGS)==1)
COPT3 = -DDECIMAL_GLOBAL_EXCEPTION_FLAGS=1
!ELSE
COPT3 = -DDECIMAL_GLOBAL_EXCEPTION_FLAGS=0
!ENDIF
!ENDIF

!IFDEF BID_BIG_ENDIAN
!IF ($(BID_BIG_ENDIAN)==1)
COPT4 = -DBID_BIG_ENDIAN=1
!ELSE
COPT4 = -UBID_BIG_ENDIAN
!ENDIF
!ENDIF
 
COPT5 =

!IFDEF UNCHANGED_BINARY_FLAGS
!IF ($(UNCHANGED_BINARY_FLAGS)==1)
COPT6 = -DUNCHANGED_BINARY_STATUS_FLAGS 
!ELSE
COPT6 = 
!ENDIF
!ENDIF

!IF ("$(CC)"=="icl")
COPT = -D_CRT_SECURE_NO_DEPRECATE -Qlong_double -Qoption,cpp,--extended_float_types -Qpc80
LMOPT = -oreadtest$(EXE)
!ELSE
COPT = -D_CRT_SECURE_NO_DEPRECATE -DBID_MS_FLAGS 
!IF ("$(HOST_TYPE)"=="WIN_IA64")
LMOPT = /Fereadtest$(EXE) bufferoverflowU.lib
!ELSE
LMOPT = /Fereadtest$(EXE)
!ENDIF 
!ENDIF
CFLAGS = -Od -I./ -D__intptr_t_defined -DWINDOWS /nologo $(COPT) $(COPT1) $(COPT2) $(COPT3) $(COPT4) $(COPT6) 
EXE = .exe
OBJ = .obj
LIBEXT = .lib
RM = del

BID_LIB = ..\LIBRARY\libbid$(LIBEXT)

default : readtest$(EXE)

readtest$(OBJ) : readtest.c readtest.h test_bid_conf.h test_bid_functions.h
	$(CC) -c $(CFLAGS) readtest.c

clean:
	$(RM) *$(OBJ)
	$(RM) readtest$(EXE)

readtest$(EXE): readtest$(OBJ) $(BID_LIB)
	$(CC) $(LMOPT) readtest$(OBJ) $(BID_LIB) 

