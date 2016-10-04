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

AR=lib
AOPT=-nologo

_USE_NATIVE_128b=TRUE
!IF ("$(CC)" == "icl")
CFLAGS=-nologo -DUSE_COMPILER_F128_TYPE=1 -DUSE_COMPILER_F80_TYPE=1 -DWINDOWS\
-od /Qlong-double /Qpc80 /Qstd=c99\
-Qoption,cpp,--extended_float_types\
-UBID_BIG_ENDIAN
!ELSE
_USE_NATIVE_128b=FALSE
CFLAGS=-nologo /DUSE_COMPILER_F128_TYPE=0 /DUSE_COMPILER_F80_TYPE=0 /DWINDOWS -UBID_BIG_ENDIAN\
-UBID_BIG_ENDIAN
!ENDIF

!IFDEF DBG
!IF ($(DBG)==1)
DEBUG=/Od /Zi
!ELSE
DEBUG=
!ENDIF
!ELSE
DEBUG=
!ENDIF

CFLAG_F128_CONF=-DUSE_NATIVE_QUAD_TYPE=0 -Dia32 -Dwnt -UBID_BIG_ENDIAN
CFLAG_F53_CONF=-DT_FLOAT -Dia32 -Dwnt -UBID_BIG_ENDIAN

!IFDEF CALL_BY_REF 
!IF ($(CALL_BY_REF)==1)
COPT_REF=-DDECIMAL_CALL_BY_REFERENCE=1
!ELSE
COPT_REF=-DDECIMAL_CALL_BY_REFERENCE=0
!ENDIF
!ENDIF

!IFDEF GLOBAL_RND
!IF ($(GLOBAL_RND)==1)
COPT_RND=-DDECIMAL_GLOBAL_ROUNDING=1
!ELSE
COPT_RND=-DDECIMAL_GLOBAL_ROUNDING=0
!ENDIF
!ENDIF

!IFDEF GLOBAL_FLAGS
!IF ($(GLOBAL_FLAGS)==1)
COPT_GLOBAL=-DDECIMAL_GLOBAL_EXCEPTION_FLAGS=1
!ELSE
COPT_GLOBAL=-DDECIMAL_GLOBAL_EXCEPTION_FLAGS=0
!ENDIF
!ENDIF

!IFDEF UNCHANGED_BINARY_FLAGS
!IF ($(UNCHANGED_BINARY_FLAGS)==1)
COPT_UNCHANGED_BINARY=-DUNCHANGED_BINARY_STATUS_FLAGS
!ENDIF
!ENDIF

O=.
S=.\src
F=.\float128

OBJ=obj

AFLAG=-nologo

BID_LIB=libbid.lib

BID_OBJS = \
$O\bid128.$(OBJ) $O\bid128_2_str_tables.$(OBJ) $O\bid128_acos.$(OBJ) $O\bid128_acosh.$(OBJ) $O\bid128_add.$(OBJ) $O\bid128_asin.$(OBJ)\
$O\bid128_asinh.$(OBJ) $O\bid128_atan.$(OBJ) $O\bid128_atan2.$(OBJ) $O\bid128_atanh.$(OBJ) $O\bid128_cbrt.$(OBJ) $O\bid128_compare.$(OBJ)\
$O\bid128_cos.$(OBJ) $O\bid128_cosh.$(OBJ) $O\bid128_div.$(OBJ) $O\bid128_erf.$(OBJ) $O\bid128_erfc.$(OBJ) $O\bid128_exp.$(OBJ)\
$O\bid128_exp10.$(OBJ) $O\bid128_exp2.$(OBJ) $O\bid128_expm1.$(OBJ) $O\bid128_fdimd.$(OBJ) $O\bid128_fma.$(OBJ) $O\bid128_fmod.$(OBJ)\
$O\bid128_frexp.$(OBJ) $O\bid128_hypot.$(OBJ) $O\bid128_ldexp.$(OBJ) $O\bid128_lgamma.$(OBJ) $O\bid128_llrintd.$(OBJ) $O\bid128_log.$(OBJ)\
$O\bid128_log10.$(OBJ) $O\bid128_log1p.$(OBJ) $O\bid128_log2.$(OBJ) $O\bid128_logb.$(OBJ) $O\bid128_logbd.$(OBJ) $O\bid128_lrintd.$(OBJ)\
$O\bid128_lround.$(OBJ) $O\bid128_minmax.$(OBJ) $O\bid128_modf.$(OBJ) $O\bid128_mul.$(OBJ) $O\bid128_nearbyintd.$(OBJ) $O\bid128_next.$(OBJ)\
$O\bid128_nexttowardd.$(OBJ) $O\bid128_noncomp.$(OBJ) $O\bid128_pow.$(OBJ) $O\bid128_quantexpd.$(OBJ) $O\bid128_quantize.$(OBJ) $O\bid128_rem.$(OBJ)\
$O\bid128_round_integral.$(OBJ) $O\bid128_scalb.$(OBJ) $O\bid128_scalbl.$(OBJ) $O\bid128_sin.$(OBJ) $O\bid128_sinh.$(OBJ) $O\bid128_sqrt.$(OBJ)\
$O\bid128_string.$(OBJ) $O\bid128_tan.$(OBJ) $O\bid128_tanh.$(OBJ) $O\bid128_tgamma.$(OBJ) $O\bid128_to_int16.$(OBJ) $O\bid128_to_int32.$(OBJ)\
$O\bid128_to_int64.$(OBJ) $O\bid128_to_int8.$(OBJ) $O\bid128_to_uint16.$(OBJ) $O\bid128_to_uint32.$(OBJ) $O\bid128_to_uint64.$(OBJ) $O\bid128_to_uint8.$(OBJ)\
$O\bid32_acos.$(OBJ) $O\bid32_acosh.$(OBJ) $O\bid32_add.$(OBJ) $O\bid32_asin.$(OBJ) $O\bid32_asinh.$(OBJ) $O\bid32_atan.$(OBJ)\
$O\bid32_atan2.$(OBJ) $O\bid32_atanh.$(OBJ) $O\bid32_cbrt.$(OBJ) $O\bid32_compare.$(OBJ) $O\bid32_cos.$(OBJ) $O\bid32_cosh.$(OBJ)\
$O\bid32_div.$(OBJ) $O\bid32_erf.$(OBJ) $O\bid32_erfc.$(OBJ) $O\bid32_exp.$(OBJ) $O\bid32_exp10.$(OBJ) $O\bid32_exp2.$(OBJ)\
$O\bid32_expm1.$(OBJ) $O\bid32_fdimd.$(OBJ) $O\bid32_fma.$(OBJ) $O\bid32_fmod.$(OBJ) $O\bid32_frexp.$(OBJ) $O\bid32_hypot.$(OBJ)\
$O\bid32_ldexp.$(OBJ) $O\bid32_lgamma.$(OBJ) $O\bid32_llrintd.$(OBJ) $O\bid32_log.$(OBJ) $O\bid32_log10.$(OBJ) $O\bid32_log1p.$(OBJ)\
$O\bid32_log2.$(OBJ) $O\bid32_logb.$(OBJ) $O\bid32_logbd.$(OBJ) $O\bid32_lrintd.$(OBJ) $O\bid32_lround.$(OBJ) $O\bid32_minmax.$(OBJ)\
$O\bid32_modf.$(OBJ) $O\bid32_mul.$(OBJ) $O\bid32_nearbyintd.$(OBJ) $O\bid32_next.$(OBJ) $O\bid32_nexttowardd.$(OBJ) $O\bid32_noncomp.$(OBJ)\
$O\bid32_pow.$(OBJ) $O\bid32_quantexpd.$(OBJ) $O\bid32_quantize.$(OBJ) $O\bid32_rem.$(OBJ) $O\bid32_round_integral.$(OBJ) $O\bid32_scalb.$(OBJ)\
$O\bid32_scalbl.$(OBJ) $O\bid32_sin.$(OBJ) $O\bid32_sinh.$(OBJ) $O\bid32_sqrt.$(OBJ) $O\bid32_string.$(OBJ) $O\bid32_sub.$(OBJ)\
$O\bid32_tan.$(OBJ) $O\bid32_tanh.$(OBJ) $O\bid32_tgamma.$(OBJ) $O\bid32_to_bid128.$(OBJ) $O\bid32_to_bid64.$(OBJ) $O\bid32_to_int16.$(OBJ)\
$O\bid32_to_int32.$(OBJ) $O\bid32_to_int64.$(OBJ) $O\bid32_to_int8.$(OBJ) $O\bid32_to_uint16.$(OBJ) $O\bid32_to_uint32.$(OBJ) $O\bid32_to_uint64.$(OBJ)\
$O\bid32_to_uint8.$(OBJ) $O\bid64_acos.$(OBJ) $O\bid64_acosh.$(OBJ) $O\bid64_add.$(OBJ) $O\bid64_asin.$(OBJ) $O\bid64_asinh.$(OBJ)\
$O\bid64_atan.$(OBJ) $O\bid64_atan2.$(OBJ) $O\bid64_atanh.$(OBJ) $O\bid64_cbrt.$(OBJ) $O\bid64_compare.$(OBJ) $O\bid64_cos.$(OBJ)\
$O\bid64_cosh.$(OBJ) $O\bid64_div.$(OBJ) $O\bid64_erf.$(OBJ) $O\bid64_erfc.$(OBJ) $O\bid64_exp.$(OBJ) $O\bid64_exp10.$(OBJ)\
$O\bid64_exp2.$(OBJ) $O\bid64_expm1.$(OBJ) $O\bid64_fdimd.$(OBJ) $O\bid64_fma.$(OBJ) $O\bid64_fmod.$(OBJ) $O\bid64_frexp.$(OBJ)\
$O\bid64_hypot.$(OBJ) $O\bid64_ldexp.$(OBJ) $O\bid64_lgamma.$(OBJ) $O\bid64_llrintd.$(OBJ) $O\bid64_log.$(OBJ) $O\bid64_log10.$(OBJ)\
$O\bid64_log1p.$(OBJ) $O\bid64_log2.$(OBJ) $O\bid64_logb.$(OBJ) $O\bid64_logbd.$(OBJ) $O\bid64_lrintd.$(OBJ) $O\bid64_lround.$(OBJ)\
$O\bid64_minmax.$(OBJ) $O\bid64_modf.$(OBJ) $O\bid64_mul.$(OBJ) $O\bid64_nearbyintd.$(OBJ) $O\bid64_next.$(OBJ) $O\bid64_nexttowardd.$(OBJ)\
$O\bid64_noncomp.$(OBJ) $O\bid64_pow.$(OBJ) $O\bid64_quantexpd.$(OBJ) $O\bid64_quantize.$(OBJ) $O\bid64_rem.$(OBJ) $O\bid64_round_integral.$(OBJ)\
$O\bid64_scalb.$(OBJ) $O\bid64_scalbl.$(OBJ) $O\bid64_sin.$(OBJ) $O\bid64_sinh.$(OBJ) $O\bid64_sqrt.$(OBJ) $O\bid64_string.$(OBJ)\
$O\bid64_tan.$(OBJ) $O\bid64_tanh.$(OBJ) $O\bid64_tgamma.$(OBJ) $O\bid64_to_bid128.$(OBJ) $O\bid64_to_int16.$(OBJ) $O\bid64_to_int32.$(OBJ)\
$O\bid64_to_int64.$(OBJ) $O\bid64_to_int8.$(OBJ) $O\bid64_to_uint16.$(OBJ) $O\bid64_to_uint32.$(OBJ) $O\bid64_to_uint64.$(OBJ) $O\bid64_to_uint8.$(OBJ)\
$O\bid_binarydecimal.$(OBJ) $O\bid_convert_data.$(OBJ) $O\bid_decimal_data.$(OBJ) $O\bid_decimal_globals.$(OBJ) $O\bid_dpd.$(OBJ) $O\bid_feclearexcept.$(OBJ)\
$O\bid_fegetexceptflag.$(OBJ) $O\bid_feraiseexcept.$(OBJ) $O\bid_fesetexceptflag.$(OBJ) $O\bid_fetestexcept.$(OBJ) $O\bid_flag_operations.$(OBJ) $O\bid_from_int.$(OBJ)\
$O\bid_round.$(OBJ) $O\strtod128.$(OBJ) $O\strtod32.$(OBJ) $O\strtod64.$(OBJ) $O\wcstod128.$(OBJ) $O\wcstod32.$(OBJ) $O\wcstod64.$(OBJ)

FLOAT128_OBJS = \
$O\dpml_ux_bid.$(OBJ) $O\dpml_ux_bessel.$(OBJ) $O\dpml_ux_cbrt.$(OBJ) $O\dpml_ux_erf.$(OBJ) $O\dpml_ux_exp.$(OBJ) $O\dpml_ux_int.$(OBJ)\
$O\dpml_ux_inv_hyper.$(OBJ) $O\dpml_ux_inv_trig.$(OBJ) $O\dpml_ux_lgamma.$(OBJ) $O\dpml_ux_log.$(OBJ) $O\dpml_ux_mod.$(OBJ)\
$O\dpml_ux_powi.$(OBJ) $O\dpml_ux_pow.$(OBJ) $O\dpml_ux_sqrt.$(OBJ) $O\dpml_ux_trig.$(OBJ) $O\dpml_ux_ops.$(OBJ) $O\dpml_ux_ops_64.$(OBJ)\
$O\dpml_four_over_pi.$(OBJ) $O\dpml_exception.$(OBJ) $O\sqrt_tab_t.$(OBJ) 

FLOAT53_OBJS = \
$O\dpml_asinh_t.$(OBJ) $O\dpml_acosh_t.$(OBJ) $O\dpml_cbrt_t.$(OBJ) $O\dpml_erf_t.$(OBJ) $O\dpml_erfc_t.$(OBJ) $O\dpml_expm1_t.$(OBJ)\
$O\dpml_exp10_t.$(OBJ) $O\dpml_exp2_t.$(OBJ) $O\dpml_lgamma_t.$(OBJ) $O\dpml_log1p_t.$(OBJ) $O\dpml_log2_t.$(OBJ) $O\dpml_tgamma_t.$(OBJ)\
$O\dpml_rt_lgamma_t.$(OBJ) $O\dpml_pow_t_table.$(OBJ) $O\dpml_cbrt_t_table.$(OBJ) $O\dpml_special_exp_t.$(OBJ) 

ALL: $(BID_LIB) 

.SUFFIXES:

.SUFFIXES: .obj .c 

BID_OBJECTS: $(BID_OBJS)

{$S}.c{$O}.$(OBJ) :
   $(CC) -I\$S -c -Fd$O\ $(CFLAGS) $(COPT_REF) $(COPT_RND) $(COPT_GLOBAL) $(COPT_UNCHANGED_BINARY) $(DEBUG) $<

$(BID_OBJS) :

FLOAT128: $(FLOAT128_OBJS)

{$F}.c{$O}.$(OBJ) ::
   $(CC) -I\$F -c -Fd$O\ $(CFLAG_F128_CONF) $(DEBUG) $<

$(FLOAT128_OBJS) :

FLOAT53: $(FLOAT53_OBJS)

$O\dpml_asinh_t.$(OBJ) : $F\dpml_asinh.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) -DASINH $(DEBUG) $**

$O\dpml_acosh_t.$(OBJ) : $F\dpml_asinh.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) -DACOSH $(DEBUG) $**

$O\dpml_cbrt_t.$(OBJ) : $F\dpml_cbrt.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) -DCBRT -DBUILD_FILE_NAME=$(**B)_t_table.c $(DEBUG) $**

$O\dpml_erf_t.$(OBJ) : $F\dpml_erf.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) -DERF -DBUILD_FILE_NAME=$(**B)_t.h $(DEBUG) $**

$O\dpml_erfc_t.$(OBJ) : $F\dpml_erf.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) -DERFC -DBUILD_FILE_NAME=$(**B)_t.h $(DEBUG) $**

$O\dpml_expm1_t.$(OBJ) : $F\dpml_expm1.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) -DEXPM1 -DUSE_CONTROL87 $(DEBUG) $**

$O\dpml_exp10_t.$(OBJ) : $F\dpml_exp.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) -DEXP10 -DUSE_CONTROL87 $(DEBUG) $**

$O\dpml_exp2_t.$(OBJ) : $F\dpml_exp.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) -DEXP2 -DUSE_CONTROL87 $(DEBUG) $**

$O\dpml_lgamma_t.$(OBJ) : $F\dpml_lgamma.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) -DDO_LGAMMA -DHACK_GAMMA_INLINE=0 -DBUILD_FILE_NAME=$(**B)_t.h $(DEBUG) $**

$O\dpml_log2_t.$(OBJ) : $F\dpml_log.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) -DLOG2 -DBASE_OF_LOG=1 $(DEBUG) $**

$O\dpml_log1p_t.$(OBJ) : $F\dpml_log.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) -DLOG1P $(DEBUG) $**

$O\dpml_tgamma_t.$(OBJ) : $F\dpml_tgamma.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) -DTGAMMA $(DEBUG) $**

$O\dpml_rt_lgamma_t.$(OBJ) : $F\dpml_lgamma.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) -DBUILD_FILE_NAME=$(**B)_t.h $(DEBUG) $**

$O\dpml_pow_t_table.$(OBJ) : $F\dpml_pow_t_table.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) $(DEBUG) $**

$O\dpml_cbrt_t_table.$(OBJ) : $F\dpml_cbrt_t_table.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) $(DEBUG) $**

$O\dpml_special_exp_t.$(OBJ) : $F\dpml_exp.c
   $(CC) -c /Fo$@ $(CFLAG_F53_CONF) -DSPECIAL_EXP $(DEBUG) $**

!IF ("$(_USE_NATIVE_128b)"=="FALSE")
$(BID_LIB): $(BID_OBJS) $(FLOAT128_OBJS) $(FLOAT53_OBJS)
   $(AR) $(AOPT) /out:$(BID_LIB) $(BID_OBJS) $(FLOAT128_OBJS) $(FLOAT53_OBJS)

!ELSE
#Use native 128b data types
$(BID_LIB): $(BID_OBJS)
   $(AR) $(AOPT) /out:$(BID_LIB) $(BID_OBJS) 

!ENDIF

clean :
   del *.$(OBJ) libbid.lib


