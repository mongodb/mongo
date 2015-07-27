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

#ifndef BUILD_H
#define BUILD_H

// =============================================================================
// The intel compiler defines __GNUC__ which causes incorrect type definitions.
// to be chosen. Detect the situation here and correct so we get a consistent
// set of type definitions
// =============================================================================

#if defined(__GNUC__) && (defined(__ICC) || defined(__ECC))
#   undef __GNUC__
#endif

// =============================================================================
// Since Intel quad names have a leading '__', the default single precision
// names are incorrect, so make sure the log2 function is correctly defined
// for dpml_ux_bessel.c
// =============================================================================

#define S_LOG2_NAME     log2f

// =============================================================================
// Select calling interface to the DPML routines
// =============================================================================

#if !defined USE_NATIVE_QUAD_TYPE
#   define  USE_NATIVE_QUAD_TYPE 0
#endif

#ifdef T_FLOAT
    #define _Quad   long double
#else
    #if USE_NATIVE_QUAD_TYPE

        // =====================================================================
        // On intel platforms, return quad values and arguments are "by-value"
        // =====================================================================
        #   define EMT64_LINUX_QUAD_INTERFACE

        #define ADD_BASE_NAME      _add_
        #define CMP_BASE_NAME      _cmp_
        #define DIV_BASE_NAME      _div_
        #define MUL_BASE_NAME      _mul_
        #define NEG_BASE_NAME      _neg_
        #define SUB_BASE_NAME      _sub_
        #define ITOF_BASE_NAME     _itof_

        // =====================================================================
        // Intel quad precision names end in q rather than l and have a '__'
        // prefix
        // =====================================================================

        #ifndef F_NAME_SUFFIX
        #   define F_NAME_SUFFIX    q
        #endif

        #ifndef F_NAME_PREFIX
        #   define F_NAME_PREFIX   __
        #endif

    #else

        // =====================================================================
        // Use the defualt "pass-by-reference" interface. But this case we need
        // to provide some type defintion for the _Quad type.
        // =====================================================================

        #define _Quad   long double
        #define __int64 long long

        #define HYPOT_BASE_NAME     hypot 
        #define NEXTAFTER_BASE_NAME nextafter

        #ifndef F_NAME_SUFFIX
        #   define F_NAME_SUFFIX    DPML_NULL_MACRO_TOKEN
        #endif

        #ifndef F_NAME_PREFIX
        #   define F_NAME_PREFIX   bid_f128_
        #endif

        #ifndef INTERNAL_PREFIX
        #   define INTERNAL_PREFIX   __dpml_bid_
        #endif
    #endif
#endif

#ifndef INTERNAL_PREFIX
#   define INTERNAL_PREFIX   __dpml_bid_
#endif

#ifndef TABLE_PREFIX
#   define TABLE_PREFIX      __dpml_bid_
#endif

// =============================================================================
// Set up the appropriate exception handling macros
// =============================================================================

#define IEEE_FLOATING				1
#define COMPATIBILITY_MODE			1
#define EXCEPTION_INTERFACE_RECEIVE		receive_exception_record
#define EXCEPTION_INTERFACE_SEND		send_exception_record
#define __DPML_EXCPT_ENVIRONMENT		ENABLE_NO_ERROR
#define DPML_GET_FPCSR(fpcsr)			fpcsr = 0
#define DPML_SET_FPCSR(fpcsr)
#define FPCSR_STICKY_BITS(x)			0

// =============================================================================
// Force long double to be 128 bits
// =============================================================================

#define LONG_DOUBLE_128

// =============================================================================
// Now generic system stuff
// =============================================================================

#if defined(__linux__) && defined(__ICC) /* Linux, Proton compiler(i.e. IA32) */
#pragma const_seg(".rodata")
#endif

#if (defined(__ICC) || defined(__ICL) || defined(__ECC) || defined(__ECL)) && 0
# pragma warning( disable : 68 )	/* #68: integer conversion resulted in a change of sign */
# pragma warning( disable : 186 )	/* #186: pointless comparison of unsigned integer with zero */
# pragma warning( disable : 1572 )	/* #1572: floating-point equality and inequality comparisons are unreliable */
#endif

#endif  /* BUILD_H */

