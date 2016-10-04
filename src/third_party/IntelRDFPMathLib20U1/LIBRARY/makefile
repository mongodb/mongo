# ##############################################################################
# ==============================================================================
#  Copyright (c) 2007-2011, Intel Corp.
#  All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without 
#  modification, are permitted provided that the following conditions are met:
#
#    * Redistributions of source code must retain the above copyright notice, 
#      this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above copyright 
#      notice, this list of conditions and the following disclaimer in the 
#      documentation and/or other materials provided with the distribution.
#    * Neither the name of Intel Corporation nor the names of its contributors 
#      may be used to endorse or promote products derived from this software 
#      without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
#  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
#  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
#  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
#  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
#  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
#  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
#  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
#  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
#  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
#  THE POSSIBILITY OF SUCH DAMAGE.
# ==============================================================================
# ##############################################################################
# ==============================================================================

# Makefile for math functions for the Intel(r)
# Decimal Floating-Point Math Library

HELP_TEXT := \
@\
@=======================================================================\
@\
@This makefile has the following standard (.PHONY) targets:\
@\
@    top           The default target. Can be modified via the symbol\
@                      TOP. The default value is \'lib\'\
@    lib           Builds the bid library in LIB_DIR directory\
@    help          Prints this message\
@    package       Creates a .tar file of the interesting sources\
@    cleanLib      Deletes all BID library object files\
@    realCleanLib  Deletes all BID library object files and the library \
@    cleanBinary   Deletes all binary trancendental support object files\
@    clean         Implies realCleanLib, cleanBinary \
@\
@\
@Useful command line symbols \
@\
@    USE_COMPILER_F128_TYPE	'true' will use the compiler intrinsic\
@				128-bit floating point type. Otherwise\
@				use an internal 128-bit floating point\
@				emulation. Default is 'true'\
@    USE_COMPILER_F128_TYPE	'true' will use the compiler intrinsic\
@				80-bit floating point type. Otherwise\
@				use the selected 128-bit choice. The\
@				default is 'true'\
@    IML_MAKEFILE_PRE		Path to local makefile definitions\
@                               is \$$(TSRC_DIR)/readtest.known_errors\
@    Others to follow\
@=======================================================================\

# ==============================================================================
# Define the default directory structure
# ==============================================================================

BID_SRC_ROOT ?= .
SRC_DIR      ?= $(BID_SRC_ROOT)/src
TSRC_DIR     ?= $(BID_SRC_ROOT)/tests
F128_DIR     ?= $(BID_SRC_ROOT)/float128
OBJ_DIR      ?= $(BID_SRC_ROOT)
GEN_DIR      ?= $(BID_SRC_ROOT)
TOBJ_DIR     ?= $(BID_SRC_ROOT)
EXE_DIR      ?= $(OBJ_DIR)
LIB_DIR      ?= $(OBJ_DIR)
RES_DIR      ?= $(BID_SRC_ROOT)

include makefile.iml_head

BID_SRC_ROOT := $(BID_SRC_ROOT)
SRC_DIR      := $(SRC_DIR)
TSRC_DIR     := $(TSRC_DIR)
F128_DIR     := $(F128_DIR)
OBJ_DIR      := $(OBJ_DIR)
GEN_DIR      := $(GEN_DIR)
TOBJ_DIR     := $(TOBJ_DIR)
EXE_DIR      := $(EXE_DIR)
LIB_DIR      := $(LIB_DIR)
RES_DIR      := $(RES_DIR)

# =============================================================================
# Cancel implict rules
# =============================================================================

%   : %.o
%.o : %.c

# =============================================================================
# Set up the default compilation and preprocessing flags. 
# =============================================================================

_CFLAGS_INC    := -I$(SRC_DIR)
_CFLAGS_CONFIG := 
_CFLAGS_OS     := $(call HostOsTypeSelect, -DLINUX, -DWINDOWS)
_CFLAGS_ARCH   := $(call HostArchTypeSelect,-Dia32,-DITANIUM -Dia64, -Defi2)
_CFLAGS_CC     :=
_CFLAGS_OPT    := 
    
ifeq ($(BID_BIG_ENDIAN),true)
    _CFLAGS_CONFIG += -DBID_BIG_ENDIAN=1
endif

ifeq ($(HPUX),1)
    _CFLAGS_OPT += -DHPUX_OS=1 +Ofenvaccess
ifeq ($(DD64),1)
    _CFLAGS_OPT += +DD64 -DHPUX_OS_64
else
ifeq ($(DD32),1)
    _CFLAGS_OPT += +DD32
endif
endif
endif

ifeq ($(IS_INTEL_CC),true)
    ifeq ($(CC_NAME),icl)
        _CFLAGS_CC += /Qlong-double /Qpc80 /Qstd=c99
    endif
endif

ifeq ($(IS_INTEL_CC),true)
    _USE_COMPILER_F128_TYPE := true
    _USE_COMPILER_F80_TYPE  := true
else
    _USE_COMPILER_F128_TYPE := false
    _USE_COMPILER_F80_TYPE  := false
endif

USE_COMPILER_F128_TYPE ?= $(_USE_COMPILER_F128_TYPE)
USE_COMPILER_F80_TYPE  ?= $(_USE_COMPILER_F80_TYPE)

ifneq ($(USE_COMPILER_F128_TYPE),true)
    _CFLAGS_CONFIG += -DUSE_COMPILER_F128_TYPE=0
else
    _CFLAGS_CONFIG += -DUSE_COMPILER_F128_TYPE=1
    ifeq ($(IS_INTEL_CC),true)
        _CFLAGS_CC += -Qoption,cpp,--extended_float_types
    endif
endif

ifneq ($(strip $(USE_COMPILER_F80_TYPE)),true)
    _CFLAGS_CONFIG += -DUSE_COMPILER_F80_TYPE=0
else
    _CFLAGS_CONFIG += -DUSE_COMPILER_F80_TYPE=1
endif

# =============================================================================
# Assemble all of the CFLAG parts and override values
# =============================================================================

CFLAGS_AUX    ?= $(_CFLAGS_AUX)
CFLAGS_OPT    ?= $(_CFLAGS_OPT)
CFLAGS_CC     ?= $(_CFLAGS_CC)
CFLAGS_ARCH   ?= $(_CFLAGS_ARCH)
CFLAGS_OS     ?= $(_CFLAGS_OS)
CFLAGS_INC    ?= $(_CFLAGS_INC)
CFLAGS_CONFIG ?= $(_CFLAGS_CONFIG)

CFLAGS        ?= $(foreach n,INC CONFIG OS ARCH CC OPT AUX,$(CFLAGS_$n))
# get rid of extra blank characters
CFLAGS        := $(foreach n,$(CFLAGS),$n)

#========================================================================
#   Added BID build options (used for open source release)
#========================================================================

ifneq ($(DFP_WRAP),1)
    override DFP_WRAP := 0
endif

ifneq ($(CALL_BY_REF),1)
    override CALL_BY_REF := 0
else
    override DFP_WRAP := 0
endif

ifneq ($(GLOBAL_RND),1)
    override GLOBAL_RND := 0
    override DFP_WRAP := 0
endif

ifneq ($(GLOBAL_FLAGS),1)
    override GLOBAL_FLAGS := 0
    override DFP_WRAP := 0
endif

BID_BLD_FLAGS := -DDECIMAL_CALL_BY_REFERENCE=$(CALL_BY_REF)	\
                 -DDECIMAL_GLOBAL_ROUNDING=$(GLOBAL_RND)	\
                 -DDECIMAL_GLOBAL_EXCEPTION_FLAGS=$(GLOBAL_FLAGS)

ifeq ($(UNCHANGED_BINARY_FLAGS),1)
    BID_BLD_FLAGS += -DUNCHANGED_BINARY_STATUS_FLAGS
endif

ifeq ($(IS_INTEL_CC),true)
ifeq ($(DFP_WRAP),1)
    BID_BLD_FLAGS += -D__DFP_WRAPPERS_ON=1              
endif
endif


# =============================================================================
# Set up default target
# =============================================================================

TOP ?= lib

.PHONY : top
top : $(TOP)

.PHONY :  default

# =============================================================================
# %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
# =============================================================================
# Targets for building the BID library
# =============================================================================
# %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
# =============================================================================

# =============================================================================
# The BID transcendental functions assume the existence of a corresponding
# binary transcendental function. If such a function is not available on the 
# target system, then we need to provide it as part of the the BID library.
# If the binary functions are missing, it is usually the 128-bit quad precision
# routines. Below, we create the list of quad functions that are required by
# the BID package
# =============================================================================

F128_NAMES := $(addprefix dpml_ux_, bid bessel cbrt erf exp int inv_hyper  \
                                    inv_trig lgamma log mod powi pow sqrt \
                                    trig ops ops_64 )                      \
               dpml_four_over_pi dpml_exception sqrt_tab_t

F128_OBJS  := $(call PrefixSuffix, $(OBJ_DIR)/, $(F128_NAMES), .$O )

# =============================================================================
# For some systems, some of the double precision binary transcendentals are
# missing. Again, here we create the list of missing functions
# =============================================================================

ifeq ($(IML_HOST_OS)_$(CC_NAME),WINNT_cl) 

    F53_NAMES  := $(call PrefixSuffix,dpml_, asinh acosh cbrt erf erfc expm1 \
                    exp10 exp2 lgamma log1p tgamma rt_lgamma,_t) \
                         dpml_pow_t_table dpml_cbrt_t_table \
                         dpml_special_exp_t
    ifeq ($(IML_HOST_ARCH),IA32)
        F53_NAMES += dpml_log2_t
    endif
    F53_OBJS   := $(call PrefixSuffix,$(OBJ_DIR)/, $(F53_NAMES), .$O)

endif

# =============================================================================
# Define the contents of the library
#
#	BID_COMMON_LIBM	Transcendental function routines that are supported in
#                          32, 64 and 128 bit forms and include bid_trans.h
#                          rather than bid_internal.h
#	BID_COMMON_OPS	Decimal operation routines that are supported in
#                          32, 64 and 128 bit forms and include bid_internal.h
#	COMMON          Decimal operation routines that are supported in
#                          32, 64 and 128 bit forms but don't have a 'bid'
#                          prefix
#	BID_<n>		Routines that are supported in only <n> bit forms
#	BID		Generic bid routines not specifically related to 
#                          data type length
#       BID_MISC	Required files that don't begin with the 'bid'
#                       prefix
#
# =============================================================================

BID_COMMON_LIBM := \
	acos acosh asin asinh atan atan2 atanh cbrt cos cosh erf erfc exp    \
	exp10 exp2 expm1 hypot lgamma log log10 log1p log2 pow sin sinh tan  \
	tanh tgamma

BID_COMMON_OPS := \
        add compare div fdimd fma fmod frexp ldexp llrintd logb logbd lrintd \
	lround minmax modf mul nearbyintd next nexttowardd noncomp quantexpd \
        quantize rem round_integral scalb scalbl sqrt string to_int16        \
        to_int32 to_int64 to_int8 to_uint16 to_uint32 to_uint64 to_uint8

COMMON  := strtod wcstod

BID_32  := sub to_bid128 to_bid64
 
BID_64  := to_bid128
 
BID_128 := 2_str_tables

BID     := \
        binarydecimal convert_data decimal_data decimal_globals dpd \
        feclearexcept fegetexceptflag feraiseexcept fesetexceptflag \
        fetestexcept flag_operations from_int round

BID_MISC := bid128

BID_TRANS_OBJS := \
	$(call CrossCat5, $(OBJ_DIR)/bid,64 128,_,$(BID_COMMON_LIBM),.$O)

BID_INTERNAL_OBJS := \
	$(call CrossCat5, $(OBJ_DIR)/bid,32 64 128,_,$(BID_COMMON_OPS),.$O) \
	$(call CrossCat4, $(OBJ_DIR)/,$(COMMON),32 64 128,.$O)              \
	$(call CrossCat3, $(OBJ_DIR)/bid32_, $(BID_COMMON_LIBM),.$O)        \
	$(call CrossCat5, $(OBJ_DIR)/bid32_, $(BID_32),.$O)                 \
	$(call CrossCat3, $(OBJ_DIR)/bid64_, $(BID_64),.$O)                 \
	$(call CrossCat3, $(OBJ_DIR)/bid128_,$(BID_128),.$O)                \
	$(call CrossCat3, $(OBJ_DIR)/bid_,   $(BID),.$O)                    \
	$(call CrossCat3, $(OBJ_DIR)/,       $(BID_MISC),.$O)
       
ALL_BID_OBJS := $(BID_TRANS_OBJS) $(BID_INTERNAL_OBJS)

ifneq ($(strip $(USE_COMPILER_F128_TYPE)),true)

   # ==========================================================================
   # Include the necessary binary transcendental routines that are not
   # available on the target system
   # ==========================================================================

   ALL_BID_OBJS := $(ALL_BID_OBJS) $(F128_OBJS) $(F53_OBJS)

endif

$(ALL_BID_OBJS)           :: $(OBJ_DIR)/.directory_exists

$(OBJ_DIR)/bid_b2d.$O      :: $(SRC_DIR)/bid_b2d.h
$(OBJ_DIR)/strtod32.$O     :: $(SRC_DIR)/bid_strtod.h
$(OBJ_DIR)/bid64_fma.$O    :: $(SRC_DIR)/bid_inline_add.h
$(OBJ_DIR)/bid32_string.$O :: $(SRC_DIR)/bid128_2_str_macros.h
$(OBJ_DIR)/bid32_string.$O :: $(SRC_DIR)/bid128_2_str.h
$(OBJ_DIR)/bid32_sqrt.$O   :: $(SRC_DIR)/bid_sqrt_macros.h
$(OBJ_DIR)/bid32_div.$O    :: $(SRC_DIR)/bid_div_macros.h

$(BID_TRANS_OBJS)    :: $(OBJ_DIR)/%.$O : $(SRC_DIR)/%.c $(SRC_DIR)/bid_trans.h
	$(CC) -c $(FO)$@ $(CFLAGS) $(BID_BLD_FLAGS) $<
	

$(BID_INTERNAL_OBJS) :: $(OBJ_DIR)/%.$O : $(SRC_DIR)/%.c \
                        $(SRC_DIR)/bid_internal.h
	$(CC) -c $(FO)$@ $(CFLAGS) $(BID_BLD_FLAGS) $<

$(SRC_DIR)/bid_trans.h : $(SRC_DIR)/bid_internal.h
	touch $@

$(SRC_DIR)/bid_internal.h : $(SRC_DIR)/bid_conf.h $(SRC_DIR)/bid_functions.h
	touch $@

ifeq ($(CC),gcc)
$(SRC_DIR)/bid_functions.h : $(SRC_DIR)/bid_gcc_intrinsics.h
	touch $@
endif

BID_LIB = $(LIB_DIR)/libbid.$A

lib : $(BID_LIB)

$(BID_LIB) :: $(LIB_DIR)/.directory_exists

$(BID_LIB) :: $(ALL_BID_OBJS)
	$(AR_CMD) $(AR_OUT)$@ $^

.PHONY : cleanLib realCleanLib

cleanLib :
	$(RM) $(ALL_BID_OBJS)

realCleanLib : cleanLib
	$(RM) $(BID_LIB)
	
# =============================================================================
# Targets for the non-native binary transcendental functions.
#
# Most of the 128-bit functions have a simple build rule:
#
#	dpml_<foo>_x.o : dpml_ux_<foo>.c dpml_<foo>_x.h
#		$(CC) ... $<
#
# Files that fit this rule are included in the F128_HDR_xxx lists. Other files
# are handled individually
# =============================================================================

F128_PLATFORM_FLAGS := $(foreach n, IML_HOST_ARCH IML_HOST_OS CC_NAME \
                        ,-D$(call ToLower,$($n)))

F128_CFLAGS := $(CFLAGS_OPT) -DUSE_NATIVE_QUAD_TYPE=0 $(F128_PLATFORM_FLAGS)

F128_HDR_NAMES := bessel cons int  lgamma powi sqrt     bid  erf inv_hyper \
                  log    pow  trig cbrt   exp  inv_trig mod

F128_HDR_OBJS := $(call PrefixSuffix, $(OBJ_DIR)/dpml_ux_,$(F128_HDR_NAMES),.$O)

$(F128_DIR)/dpml_ux.h : $(F128_DIR)/dpml_private.h  \
                        $(F128_DIR)/dpml_ux_32_64.h \
                        $(F128_DIR)/dpml_cons_x.h
	touch $@

$(F128_DIR)/dpml_private.h : $(call PrefixSuffix, $(F128_DIR)/,               \
                               build op_system compiler architecture i_format \
                               f_format mtc_macros mphoc_macros poly_macros   \
                               assert dpml_names dpml_exception ix86_macros, .h)
	touch $@

$(OBJ_DIR)/dpml_globals.$O :: $(call PrefixSuffix, $(F128_DIR)/,             \
                              build op_system compiler architecture f_format \
                              dpml_names mphoc_macros, .h)

$(OBJ_DIR)/dpml_error_codes.$O :: $(call PrefixSuffix, $(F128_DIR)/, \
                                  dpml_error_codes_enum dpml_function_info, .h)

$(OBJ_DIR)/dpml_exception.$O :: $(call PrefixSuffix, $(F128_DIR)/, \
                                  dpml_error_codes, .h)

$(F128_HDR_OBJS) :: $(OBJ_DIR)/dpml_ux_%.$O : $(F128_DIR)/dpml_%_x.h

$(F128_OBJS) :: $(OBJ_DIR)/%.$O : $(F128_DIR)/%.c $(F128_DIR)/dpml_ux.h
	$(CC) -c $(FO)$@ $(F128_CFLAGS) $<

	
# =============================================================================
# The targets for the double precision binary functions is much less regualar.
# =============================================================================

F53_CFLAGS  := $(subst -DWINDOWS,-DWNT,$(CFLAGS))

$(F53_OBJS) :: $(F128_DIR)/dpml_private.h $(F128_DIR)/dpml_globals.h \
                    $(F128_DIR)/dpml_error_codes_enum.h

BUILD_FILE_NAME = $(basename $(notdir $@)).h
D_F_NAME          = -D$(call ToUpper,$(W2))
D_F_TYPE          = -D$(call ToUpper,$(W3))_FLOAT
 
$(OBJ_DIR)/dpml_asinh_t.$O  :: $(F128_DIR)/sqrt_tab_t.c

$(OBJ_DIR)/dpml_erf_t.$O \
$(OBJ_DIR)/dpml_asinh_t.$O  :: $(OBJ_DIR)/dpml_%_t.$O : $(F128_DIR)/dpml_%.c \
                               $(F128_DIR)/dpml_%_t.h
	$(CC) -c $(FO)$@ $(F53_CFLAGS) $(D_F_NAME) $(D_F_TYPE) $(D_F_NAME) \
             -DBUILD_FILE_NAME=$(BUILD_FILE_NAME) $<
 
$(OBJ_DIR)/dpml_rt_lgamma_t.$O :: $(F128_DIR)/dpml_lgamma.c \
                                  $(F128_DIR)/dpml_lgamma_t.h
	$(CC) -c $(FO)$@ $(F53_CFLAGS) -DT_FLOAT -DBUILD_FILE_NAME=dpml_lgamma_t.h $<
 
$(OBJ_DIR)/dpml_lgamma_t.$O :: $(F128_DIR)/dpml_lgamma.c \
                                  $(F128_DIR)/dpml_lgamma_t.h
	$(CC) -c $(FO)$@ $(F53_CFLAGS) $(D_F_TYPE) -DDO_LGAMMA -DHACK_GAMMA_INLINE=0 \
             -DBUILD_FILE_NAME=dpml_lgamma_t.h $<
 
$(OBJ_DIR)/dpml_acosh_t.$O :: $(F128_DIR)/dpml_asinh.c $(F128_DIR)/dpml_acosh_t.h \
                              $(F128_DIR)/sqrt_tab_t.c
	$(CC) -c $(FO)$@ $(F53_CFLAGS) $(D_F_NAME) $(D_F_TYPE) $<
 
$(OBJ_DIR)/dpml_erfc_t.$O  :: $(F128_DIR)/dpml_erf.c   $(F128_DIR)/dpml_erf_t.h
	$(CC) -c $(FO)$@ $(F53_CFLAGS) $(D_F_NAME) $(D_F_TYPE) \
        -DBUILD_FILE_NAME=dpml_erf_t.h $<
 
$(OBJ_DIR)/dpml_log1p_t.$O :: $(F128_DIR)/dpml_log.c $(F128_DIR)/dpml_log_t.h
	$(CC) -c $(FO)$@ $(F53_CFLAGS) $(D_F_NAME) $(D_F_TYPE) $<
 
$(OBJ_DIR)/dpml_log2_t.$O :: $(F128_DIR)/dpml_log.c $(F128_DIR)/dpml_log_t.h
	$(CC) -c $(FO)$@ $(F53_CFLAGS) $(D_F_NAME) $(D_F_TYPE) $<
 
$(OBJ_DIR)/dpml_cbrt_t.$O :: $(F128_DIR)/dpml_cbrt.c $(F128_DIR)/dpml_cbrt_t_table.c
	$(CC) -c $(FO)$@ $(F53_CFLAGS) $(D_F_NAME) $(D_F_TYPE) \
        -DBUILD_FILE_NAME=dpml_cbrt_t_table.c $<
 
$(OBJ_DIR)/dpml_exp10_t.$O \
$(OBJ_DIR)/dpml_exp2_t.$O  :: $(F128_DIR)/dpml_exp.c $(F128_DIR)/dpml_pow_t_table.c
	$(CC) -c $(FO)$@ $(F53_CFLAGS) $(D_F_NAME) $(D_F_TYPE) -DUSE_CONTROL87 $<

$(OBJ_DIR)/dpml_expm1_t.$O :: $(F128_DIR)/dpml_expm1.c $(F128_DIR)/dpml_pow_t_table.c
	$(CC) -c $(FO)$@ $(F53_CFLAGS) $(D_F_NAME) $(D_F_TYPE) -DUSE_CONTROL87 $<

$(OBJ_DIR)/dpml_tgamma_t.$O :: $(F128_DIR)/dpml_tgamma.c $(F128_DIR)/dpml_special_exp.h
	$(CC) -c $(FO)$@ $(F53_CFLAGS) $(D_F_NAME) $(D_F_TYPE) $<
 
$(OBJ_DIR)/dpml_special_exp_t.$O :: $(F128_DIR)/dpml_exp.c \
                                  $(F128_DIR)/dpml_pow_t_table.c
	$(CC) -c $(FO)$@ $(F53_CFLAGS) -DSPECIAL_EXP -DT_FLOAT $<
 
 
$(OBJ_DIR)/dpml_pow_t_table.$O \
$(OBJ_DIR)/dpml_cbrt_t_table.$O :: $(OBJ_DIR)/%.$O : $(F128_DIR)/%.c
	$(CC) -c $(FO)$@ $(F53_CFLAGS) $(D_F_TYPE) $<

                                                           
cleanBinary :
	$(RM) $(F128_OBJS) $(F53_OBJS)


# =============================================================================
# %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
# =============================================================================



.PHONY : clean



# =============================================================================
# Clean targets
# =============================================================================

clean : 
	$(RM) $(BID_LIB) *.$O


#realClean : realCleanLib cleanBinary


.directory_exists:
	touch $@


# =============================================================================
# %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
# =============================================================================
# End of makefile
# =============================================================================
# %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
# =============================================================================
