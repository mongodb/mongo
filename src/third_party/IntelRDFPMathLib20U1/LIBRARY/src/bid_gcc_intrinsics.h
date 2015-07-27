/******************************************************************************
  Copyright (c) 2007-2011, Intel Corp.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without 
  modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, 
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright 
      notice, this list of conditions and the following disclaimer in the 
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors 
      may be used to endorse or promote products derived from this software 
      without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
  THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#ifndef _BID_GCC_INTRINSICS_H
#define _BID_GCC_INTRINSICS_H

#ifdef IN_LIBGCC2

#include "tconfig.h"
#include "coretypes.h"
#include "tm.h"

#ifndef LIBGCC2_WORDS_BIG_ENDIAN
#define LIBGCC2_WORDS_BIG_ENDIAN WORDS_BIG_ENDIAN
#endif

#ifndef LIBGCC2_FLOAT_WORDS_BIG_ENDIAN
#define LIBGCC2_FLOAT_WORDS_BIG_ENDIAN LIBGCC2_WORDS_BIG_ENDIAN
#endif

#ifndef LIBGCC2_LONG_DOUBLE_TYPE_SIZE
#define LIBGCC2_LONG_DOUBLE_TYPE_SIZE LONG_DOUBLE_TYPE_SIZE
#endif

#ifndef LIBGCC2_HAS_XF_MODE
#define LIBGCC2_HAS_XF_MODE \
  (BITS_PER_UNIT == 8 && LIBGCC2_LONG_DOUBLE_TYPE_SIZE == 80)
#endif

#ifndef LIBGCC2_HAS_TF_MODE
#define LIBGCC2_HAS_TF_MODE \
  (BITS_PER_UNIT == 8 && LIBGCC2_LONG_DOUBLE_TYPE_SIZE == 128)
#endif

#ifndef BID_HAS_XF_MODE
#define BID_HAS_XF_MODE LIBGCC2_HAS_XF_MODE
#endif

#ifndef BID_HAS_TF_MODE
#define BID_HAS_TF_MODE LIBGCC2_HAS_TF_MODE
#endif

/* Some handy typedefs.  */

typedef float SFtype __attribute__ ((mode (SF)));
typedef float DFtype __attribute__ ((mode (DF)));
#if LIBGCC2_HAS_XF_MODE
typedef float XFtype __attribute__ ((mode (XF)));
#endif /* LIBGCC2_HAS_XF_MODE */
#if LIBGCC2_HAS_TF_MODE
typedef float TFtype __attribute__ ((mode (TF)));
#endif /* LIBGCC2_HAS_XF_MODE */

typedef int SItype __attribute__ ((mode (SI)));
typedef int DItype __attribute__ ((mode (DI)));
typedef unsigned int USItype __attribute__ ((mode (SI)));
typedef unsigned int UDItype __attribute__ ((mode (DI)));

/* The type of the result of a decimal float comparison.  This must
   match `word_mode' in GCC for the target.  */

typedef int CMPtype __attribute__ ((mode (word)));

typedef int BID_SINT8 __attribute__ ((mode (QI)));
typedef unsigned int BID_UINT8 __attribute__ ((mode (QI)));
typedef USItype BID_UINT32;
typedef SItype BID_SINT32;
typedef UDItype BID_UINT64;
typedef DItype BID_SINT64;

/* It has to be identical to the one defined in bid_functions.h.  */
typedef __attribute__ ((aligned(16))) struct
{
  BID_UINT64 w[2];
} BID_UINT128;
#else	/* if not IN_LIBGCC2 */

#ifndef BID_HAS_XF_MODE
#define BID_HAS_XF_MODE 1
#endif

#ifndef BID_HAS_TF_MODE
#if defined __i386__
#define BID_HAS_TF_MODE 0
#else
#define BID_HAS_TF_MODE 1
#endif
#endif

#ifndef SFtype
#define SFtype float
#endif

#ifndef DFtype
#define DFtype double
#endif

#if BID_HAS_XF_MODE
#ifndef XFtype
#define XFtype long double
#endif

#endif   /* IN_LIBGCC2 */

#if BID_HAS_TF_MODE
#ifndef TFtype
#define TFtype __float128
#endif
#endif

#ifndef SItype
#define SItype BID_SINT32
#endif

#ifndef DItype
#define DItype BID_SINT64
#endif

#ifndef USItype
#define USItype BID_UINT32
#endif

#ifndef UDItype
#define UDItype BID_UINT64
#endif

#ifndef CMPtype
#define CMPtype long
#endif
#endif	/* IN_LIBGCC2 */

#if BID_HAS_GCC_DECIMAL_INTRINSICS
/* Prototypes for gcc instrinsics  */

BID_EXTERN_C _Decimal64 __bid_adddd3 (_Decimal64, _Decimal64);
BID_EXTERN_C _Decimal64 __bid_subdd3 (_Decimal64, _Decimal64);
BID_EXTERN_C _Decimal32 __bid_addsd3 (_Decimal32, _Decimal32);
BID_EXTERN_C _Decimal32 __bid_subsd3 (_Decimal32, _Decimal32);
BID_EXTERN_C _Decimal128 __bid_addtd3 (_Decimal128, _Decimal128);
BID_EXTERN_C _Decimal128 __bid_subtd3 (_Decimal128, _Decimal128);
BID_EXTERN_C DFtype __bid_truncdddf (_Decimal64);
BID_EXTERN_C DItype __bid_fixdddi (_Decimal64);
BID_EXTERN_C _Decimal32 __bid_truncddsd2 (_Decimal64);
BID_EXTERN_C SFtype __bid_truncddsf (_Decimal64);
BID_EXTERN_C SItype __bid_fixddsi (_Decimal64);
BID_EXTERN_C _Decimal128 __bid_extendddtd2 (_Decimal64);
#if BID_HAS_TF_MODE
BID_EXTERN_C TFtype __bid_extendddtf (_Decimal64);
#endif
BID_EXTERN_C UDItype __bid_fixunsdddi (_Decimal64);
BID_EXTERN_C USItype __bid_fixunsddsi (_Decimal64);
#if BID_HAS_XF_MODE
BID_EXTERN_C XFtype __bid_extendddxf (_Decimal64);
#endif
BID_EXTERN_C _Decimal64 __bid_extenddfdd (DFtype);
BID_EXTERN_C _Decimal32 __bid_truncdfsd (DFtype);
BID_EXTERN_C _Decimal128 __bid_extenddftd (DFtype);
BID_EXTERN_C _Decimal64 __bid_floatdidd (DItype);
BID_EXTERN_C _Decimal32 __bid_floatdisd (DItype);
BID_EXTERN_C _Decimal128 __bid_floatditd (DItype);
BID_EXTERN_C _Decimal64 __bid_divdd3 (_Decimal64, _Decimal64);
BID_EXTERN_C _Decimal32 __bid_divsd3 (_Decimal32, _Decimal32);
BID_EXTERN_C _Decimal128 __bid_divtd3 (_Decimal128, _Decimal128);
BID_EXTERN_C CMPtype __bid_eqdd2 (_Decimal64, _Decimal64);
BID_EXTERN_C CMPtype __bid_eqsd2 (_Decimal32, _Decimal32);
BID_EXTERN_C CMPtype __bid_eqtd2 (_Decimal128, _Decimal128);
BID_EXTERN_C CMPtype __bid_gedd2 (_Decimal64, _Decimal64);
BID_EXTERN_C CMPtype __bid_gesd2 (_Decimal32, _Decimal32);
BID_EXTERN_C CMPtype __bid_getd2 (_Decimal128, _Decimal128);
BID_EXTERN_C CMPtype __bid_gtdd2 (_Decimal64, _Decimal64);
BID_EXTERN_C CMPtype __bid_gtsd2 (_Decimal32, _Decimal32);
BID_EXTERN_C CMPtype __bid_gttd2 (_Decimal128, _Decimal128);
BID_EXTERN_C CMPtype __bid_ledd2 (_Decimal64, _Decimal64);
BID_EXTERN_C CMPtype __bid_lesd2 (_Decimal32, _Decimal32);
BID_EXTERN_C CMPtype __bid_letd2 (_Decimal128, _Decimal128);
BID_EXTERN_C CMPtype __bid_ltdd2 (_Decimal64, _Decimal64);
BID_EXTERN_C CMPtype __bid_ltsd2 (_Decimal32, _Decimal32);
BID_EXTERN_C CMPtype __bid_lttd2 (_Decimal128, _Decimal128);
BID_EXTERN_C CMPtype __bid_nedd2 (_Decimal64, _Decimal64);
BID_EXTERN_C CMPtype __bid_nesd2 (_Decimal32, _Decimal32);
BID_EXTERN_C CMPtype __bid_netd2 (_Decimal128, _Decimal128);
BID_EXTERN_C CMPtype __bid_unorddd2 (_Decimal64, _Decimal64);
BID_EXTERN_C CMPtype __bid_unordsd2 (_Decimal32, _Decimal32);
BID_EXTERN_C CMPtype __bid_unordtd2 (_Decimal128, _Decimal128);
BID_EXTERN_C _Decimal64 __bid_muldd3 (_Decimal64, _Decimal64);
BID_EXTERN_C _Decimal32 __bid_mulsd3 (_Decimal32, _Decimal32);
BID_EXTERN_C _Decimal128 __bid_multd3 (_Decimal128, _Decimal128);
BID_EXTERN_C _Decimal64 __bid_extendsddd2 (_Decimal32);
BID_EXTERN_C DFtype __bid_extendsddf (_Decimal32);
BID_EXTERN_C DItype __bid_fixsddi (_Decimal32);
BID_EXTERN_C SFtype __bid_truncsdsf (_Decimal32);
BID_EXTERN_C SItype __bid_fixsdsi (_Decimal32);
BID_EXTERN_C _Decimal128 __bid_extendsdtd2 (_Decimal32);
#if BID_HAS_TF_MODE
BID_EXTERN_C TFtype __bid_extendsdtf (_Decimal32);
#endif
BID_EXTERN_C UDItype __bid_fixunssddi (_Decimal32);
BID_EXTERN_C USItype __bid_fixunssdsi (_Decimal32);
#if BID_HAS_XF_MODE
BID_EXTERN_C XFtype __bid_extendsdxf (_Decimal32);
#endif
BID_EXTERN_C _Decimal64 __bid_extendsfdd (SFtype);
BID_EXTERN_C _Decimal32 __bid_extendsfsd (SFtype);
BID_EXTERN_C _Decimal128 __bid_extendsftd (SFtype);
BID_EXTERN_C _Decimal64 __bid_floatsidd (SItype);
BID_EXTERN_C _Decimal32 __bid_floatsisd (SItype);
BID_EXTERN_C _Decimal128 __bid_floatsitd (SItype);
BID_EXTERN_C _Decimal64 __bid_trunctddd2 (_Decimal128);
BID_EXTERN_C DFtype __bid_trunctddf (_Decimal128);
BID_EXTERN_C DItype __bid_fixtddi (_Decimal128);
BID_EXTERN_C _Decimal32 __bid_trunctdsd2 (_Decimal128);
BID_EXTERN_C SFtype __bid_trunctdsf (_Decimal128);
BID_EXTERN_C SItype __bid_fixtdsi (_Decimal128);
#if BID_HAS_TF_MODE
BID_EXTERN_C TFtype __bid_trunctdtf (_Decimal128);
#endif
BID_EXTERN_C UDItype __bid_fixunstddi (_Decimal128);
BID_EXTERN_C USItype __bid_fixunstdsi (_Decimal128);
#if BID_HAS_XF_MODE
BID_EXTERN_C XFtype __bid_trunctdxf (_Decimal128);
#endif
#if BID_HAS_TF_MODE
BID_EXTERN_C _Decimal64 __bid_trunctfdd (TFtype);
BID_EXTERN_C _Decimal32 __bid_trunctfsd (TFtype);
BID_EXTERN_C _Decimal128 __bid_extendtftd (TFtype);
#endif
BID_EXTERN_C _Decimal64 __bid_floatunsdidd (UDItype);
BID_EXTERN_C _Decimal32 __bid_floatunsdisd (UDItype);
BID_EXTERN_C _Decimal128 __bid_floatunsditd (UDItype);
BID_EXTERN_C _Decimal64 __bid_floatunssidd (USItype);
BID_EXTERN_C _Decimal32 __bid_floatunssisd (USItype);
BID_EXTERN_C _Decimal128 __bid_floatunssitd (USItype);
#if BID_HAS_XF_MODE
BID_EXTERN_C _Decimal64 __bid_truncxfdd (XFtype);
BID_EXTERN_C _Decimal32 __bid_truncxfsd (XFtype);
BID_EXTERN_C _Decimal128 __bid_extendxftd (XFtype);
#endif
BID_EXTERN_C int isinfd32 (_Decimal32);
BID_EXTERN_C int isinfd64 (_Decimal64);
BID_EXTERN_C int isinfd128 (_Decimal128);
#endif  /* BID_HAS_GCC_DECIMAL_INTRINSICS */

BID_EXTERN_C void __dfp_set_round (int);
BID_EXTERN_C int __dfp_get_round (void);
BID_EXTERN_C void __dfp_clear_except (void);
BID_EXTERN_C int __dfp_test_except (int);
BID_EXTERN_C void __dfp_raise_except (int);

#if BID_HAS_GCC_DECIMAL_INTRINSICS
/* Used by gcc intrinsics.  We have to define them after BID_UINT128
   is defined.  */
union decimal32 {
  _Decimal32 d;
  BID_UINT32 i;
};
 
union decimal64 {
  _Decimal64 d;
  BID_UINT64 i;
};
 
union decimal128 {
  _Decimal128 d;
  BID_UINT128 i;
};
 
#if BID_HAS_TF_MODE
union float128 {
  TFtype f;
  BID_UINT128 i;
};
#endif
#endif  /* BID_HAS_GCC_DECIMAL_INTRINSICS */

#endif /* _BID_GCC_INTRINSICS_H */
