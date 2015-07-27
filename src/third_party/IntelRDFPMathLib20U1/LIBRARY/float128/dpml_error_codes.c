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

#undef  MAKE_INCLUDE
#define MAKE_INCLUDE
#undef  F_FLOAT
#define F_FLOAT
#define __F_SUFFIX  DPML_NULL_MACRO_TOKEN

/*
 * In enumeration mode, this file produces mphoc that prints out symbolic
 * constants for each of the DPML functions and error codes.  The function
 * symbols are of the form M_<function_name> for historic purposes.  The
 * error code names are of the form <function_name>_<error_string>, where
 * function_name is the same as before and error_string is any character
 * string descriptive of the actual error.
 */

#define FUNC_ERR_CODE(a,b)              PASTE_3(a, _, b)

#ifdef MAKE_DPML_ERROR_CODES_ENUM

#   ifndef BUILD_FILE_NAME
#       define BUILD_FILE_NAME          dpml_error_codes_enum.h
#   endif
#   define ENUMERATE(n)                 printf("#define\t"STR(n)"\t%i\n", i++);
#   define ENUMERATE_FUNC(n)            ENUMERATE(PASTE_2(M_, n))
#   define INIT_COUNTER                 i = 0
#   define DEFINE_RESPONSE(a,b,c,d,e,f) ENUMERATE(FUNC_ERR_CODE(a,b))
#   define LAST_ENUM_ENTRY              DEFAULT_RESPONSE(LAST, ERROR_CODE, 0)

#else

/*
 * In table generation mode, this file produce mphoc code that initialize
 * several arrays.  The values in the arrays are printed to create the
 * response table.
 *
 * The default_<ieee/fast>_ret arrays contain an index into the globals
 * offset table.  They define the default return value for each DPML
 * exception in ieee/fast mode.
 *
 * The remaining arrays have one entry for each error code.  They are:
 *
 *              func_code               function that caused the error
 *              <fast/ieee>_ret         index into the globals offset table
 *                                      that defines the return value for this
 *                                      error in fast/ieee mode
 *              <fast/ieee>_error_code  generic DPML error in fast/ieee mode
 */

#   include "dpml_error_codes_enum.h"

#   ifndef BUILD_FILE_NAME
#       define BUILD_FILE_NAME          dpml_error_codes.h
#   endif

#   ifndef RESPONSE_TABLE_NAME
#      define RESPONSE_TABLE_NAME       __TABLE_NAME(dpml_response_table)
#   endif

#    define DEFINE_RESPONSE(a,b,fe,fv,ie,iv) \
                                        i = FUNC_ERR_CODE(a,b); \
                                        func_code[i] = PASTE_2(M_, a); \
                                        fast_error_code[i] = UNSIGNED_ERR(fe); \
                                        fast_ret[i] = fv; \
                                        ieee_error_code[i] = UNSIGNED_ERR(ie); \
                                        ieee_ret[i] = iv;
#    define INIT_COUNTER                DEFAULT_RESPONSE(LAST, ERROR_CODE, 0)
#    define LAST_ENUM_ENTRY
#    define PRINT_RESPONSE_INFO(i)      printf("    /* %i */ " \
                                           "{%i, %i, %i, %i, %i},\n", \
                                           i, func_code[i], \
                                           fast_error_code[i], fast_ret[i], \
                                           ieee_error_code[i], ieee_ret[i])
#endif

#define DEFAULT_RESPONSE(a,b,c) DEFINE_RESPONSE(a,b,c,default_fast_ret[c], \
                                          c,default_ieee_ret[c])
#define DEFAULT_NO_ERROR(a,b,c) DEFINE_RESPONSE(a,b,DPML_NO_ERROR, c ## _INDEX,\
                                          DPML_NO_ERROR, c ## _INDEX);
#define DEFAULT_NAN(a,b) DEFINE_RESPONSE(a,b,DPML_NO_ERROR, POS_ZERO_INDEX,\
                                           DPML_NO_ERROR, NAN_INDEX);

#include "dpml_private.h"


@divert divertText

#ifdef MAKE_DPML_ERROR_CODES_ENUM

    /* Create function List for DPML */
#   define GEN_FUNC_INFO(a,b,c)	ENUMERATE_FUNC(a)

    i = 0;
#   include "dpml_function_info.h"
    ENUMERATE_FUNC( LAST )
    printf("\n\n");

#endif

#ifndef MAKE_DPML_ERROR_CODES_ENUM

    /*
     * Set up default response values for fast and IEEE mode)
     */

        default_fast_ret[INVALID_ARGUMENT]     = POS_ZERO_INDEX;
        default_fast_ret[DPML_NO_ERROR]        = POS_ZERO_INDEX;
        default_fast_ret[POS_UNDERFLOW_ERR]    = POS_ZERO_INDEX;
        default_fast_ret[NEG_UNDERFLOW_ERR]    = NEG_ZERO_INDEX;
        default_fast_ret[POS_OVERFLOW_ERR]     = POS_HUGE_INDEX;
        default_fast_ret[NEG_OVERFLOW_ERR]     = NEG_HUGE_INDEX;
        default_fast_ret[POS_SINGULARITY]      = POS_HUGE_INDEX;
        default_fast_ret[NEG_SINGULARITY]      = NEG_HUGE_INDEX;
        default_fast_ret[LOSS_OF_SIGNIFICANCE] = POS_ZERO_INDEX;
    
        default_ieee_ret[INVALID_ARGUMENT]     = NAN_INDEX;
        default_ieee_ret[DPML_NO_ERROR]        = POS_ZERO_INDEX;
        default_ieee_ret[POS_UNDERFLOW_ERR]    = POS_ZERO_INDEX;
        default_ieee_ret[NEG_UNDERFLOW_ERR]    = NEG_ZERO_INDEX;
        default_ieee_ret[POS_OVERFLOW_ERR]     = POS_INFINITY_INDEX;
        default_ieee_ret[NEG_OVERFLOW_ERR]     = NEG_INFINITY_INDEX;
        default_ieee_ret[POS_SINGULARITY]      = POS_INFINITY_INDEX;
        default_ieee_ret[NEG_SINGULARITY]      = NEG_INFINITY_INDEX;
        default_ieee_ret[LOSS_OF_SIGNIFICANCE] = POS_ZERO_INDEX;

       /*
        * if platform specific initialization need to over-ride the above
        * they should be put here.  For example to change the default fast
        * return value for invalid argument to -HUGE for osf:
        *
        *       #if OP_SYSTEM == osf
        *
        *           default_fast_ret[INVALID_ARGUMENT]     = NEG_HUGE_INDEX;
        *
        *       #endif 
        */

#endif

    INIT_COUNTER;

    /*
     * BEGIN_ENUMERATION_LIST:
     *
     * Every error code used by the DPML must appear between this comment
     * and the comment 'END_ENUMERATION_LIST'.  This will cause a #define
     * and table entry to be generated.
     */

    DEFAULT_RESPONSE(ACOS, ARG_GT_ONE, INVALID_ARGUMENT)

    DEFAULT_RESPONSE(ACOSD, ARG_GT_ONE, INVALID_ARGUMENT)

    DEFAULT_RESPONSE(ACOSH, ARG_LT_ONE, INVALID_ARGUMENT)

    DEFAULT_RESPONSE(ASIN, ARG_GT_ONE, INVALID_ARGUMENT) 

    DEFAULT_RESPONSE(ASIND, ARG_GT_ONE, INVALID_ARGUMENT) 

    /* PLACEHOLDER for ATAN Function */

    /* PLACEHOLDER for ATAND Function */

    DEFAULT_RESPONSE(ATANH, ABS_ARG_GT_ONE, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(ATANH, OF_ONE,         POS_SINGULARITY)
    DEFAULT_RESPONSE(ATANH, OF_NEG_ONE,     NEG_SINGULARITY)

    DEFAULT_RESPONSE(ATAN2, BOTH_ZERO, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(ATAN2, BOTH_INF,  INVALID_ARGUMENT)
    DEFAULT_RESPONSE(ATAN2, UNDERFLOW, POS_UNDERFLOW_ERR)

    DEFAULT_RESPONSE(ATAND2, BOTH_ZERO, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(ATAND2, BOTH_INF,  INVALID_ARGUMENT)
    DEFAULT_RESPONSE(ATAND2, UNDERFLOW, POS_UNDERFLOW_ERR)

    DEFAULT_RESPONSE(CABS, OVERFLOW, POS_OVERFLOW_ERR)

    DEFAULT_RESPONSE(CDIV, DIV_BY_ZERO, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(CDIV, OVERFLOW,    POS_OVERFLOW_ERR)

    DEFAULT_RESPONSE(COS, OF_INFINITY, INVALID_ARGUMENT)

    DEFAULT_RESPONSE(COSD, OF_INFINITY, INVALID_ARGUMENT)

    DEFAULT_RESPONSE(COSH, OVERFLOW, POS_OVERFLOW_ERR)

    DEFAULT_RESPONSE(COT, UNDERFLOW,    POS_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(COT, POS_OVERFLOW, POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(COT, NEG_OVERFLOW, NEG_OVERFLOW_ERR)
    DEFAULT_RESPONSE(COT, OF_INFINITY,  INVALID_ARGUMENT)
    DEFAULT_RESPONSE(COT, OF_ZERO,      POS_SINGULARITY)
    DEFAULT_RESPONSE(COT, OF_NEG_ZERO,  NEG_SINGULARITY)

    DEFAULT_RESPONSE(COTD, UNDERFLOW,       POS_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(COTD, POS_OVERFLOW,    POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(COTD, NEG_OVERFLOW,    NEG_OVERFLOW_ERR)
    DEFAULT_RESPONSE(COTD, OF_INFINITY,     INVALID_ARGUMENT)
    DEFAULT_RESPONSE(COTD, OF_ZERO,         POS_SINGULARITY)
    DEFAULT_RESPONSE(COTD, OF_NEG_ZERO,     NEG_SINGULARITY)
    DEFAULT_RESPONSE(COTD, MULTIPLE_OF_180, POS_SINGULARITY)

    /* PLACEHOLDER for CSQRT Function */

    DEFAULT_RESPONSE(EXP, OVERFLOW,   POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(EXP, UNDERFLOW,  POS_UNDERFLOW_ERR)
    DEFAULT_NO_ERROR(EXP, OF_INF,     POS_INFINITY)
    DEFAULT_NO_ERROR(EXP, OF_NEG_INF, POS_ZERO)

    DEFAULT_RESPONSE(EXPM1, OVERFLOW,   POS_OVERFLOW_ERR)
    DEFAULT_NO_ERROR(EXPM1, OF_INF,     POS_INFINITY)
    DEFAULT_NO_ERROR(EXPM1, OF_NEG_INF, NEG_ONE)

    DEFAULT_RESPONSE(LDEXP, OVERFLOW,     POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(LDEXP, NEG_OVERFLOW, NEG_OVERFLOW_ERR)
    DEFAULT_RESPONSE(LDEXP, UNDERFLOW,    POS_UNDERFLOW_ERR)

    DEFAULT_RESPONSE(SCALB, OVERFLOW,             POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(SCALB, NEG_OVERFLOW,         NEG_OVERFLOW_ERR)
    DEFAULT_RESPONSE(SCALB, UNDERFLOW,            POS_UNDERFLOW_ERR)
    DEFAULT_NO_ERROR(SCALB, OF_POS_TO_POS_INF,    POS_INFINITY)
    DEFAULT_NO_ERROR(SCALB, OF_NEG_TO_POS_INF,    NEG_INFINITY)
    DEFAULT_NO_ERROR(SCALB, OF_FINITE_TO_NEG_INF, POS_ZERO)
    DEFAULT_RESPONSE(SCALB, OF_INF_TO_NEG_INF,    INVALID_ARGUMENT)
    DEFAULT_RESPONSE(SCALB, INVALID,              INVALID_ARGUMENT)

    DEFAULT_RESPONSE(LOGB, OF_ZERO, NEG_SINGULARITY)

    DEFAULT_RESPONSE(LOG, OF_NEGATIVE, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(LOG, OF_ZERO,     NEG_SINGULARITY)

    DEFAULT_RESPONSE(LOG2, OF_NEGATIVE, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(LOG2, OF_ZERO,     NEG_SINGULARITY)

    DEFAULT_RESPONSE(LOG10, OF_NEGATIVE, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(LOG10, OF_ZERO,     NEG_SINGULARITY)

    DEFAULT_RESPONSE(LOG1P, LESS_M1,    INVALID_ARGUMENT)
    DEFAULT_RESPONSE(LOG1P, M1,         NEG_SINGULARITY)

    DEFAULT_RESPONSE(MOD, UNDERFLOW, POS_UNDERFLOW_ERR)
    DEFAULT_NO_ERROR(MOD, BY_ZERO,   POS_ZERO)
    DEFAULT_RESPONSE(MOD, OF_INF,    INVALID_ARGUMENT)

    DEFAULT_RESPONSE(NEXTAFTER, POS_OVERFLOW,  POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(NEXTAFTER, NEG_OVERFLOW,  NEG_OVERFLOW_ERR)
    DEFAULT_RESPONSE(NEXTAFTER, POS_UNDERFLOW, POS_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(NEXTAFTER, NEG_UNDERFLOW, NEG_UNDERFLOW_ERR)

    DEFAULT_RESPONSE(POWER, POS_OVERFLOW,       POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(POWER, NEG_OVERFLOW,       NEG_OVERFLOW_ERR)
    DEFAULT_RESPONSE(POWER, UNDERFLOW,          POS_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(POWER, NEG_BASE,           INVALID_ARGUMENT)
    DEFAULT_RESPONSE(POWER, ZERO_TO_NEG,        NEG_SINGULARITY)
    DEFAULT_RESPONSE(POWER, INF_TO_ZERO,        INVALID_ARGUMENT)
    DEFAULT_RESPONSE(POWER, ONE_TO_INF,         INVALID_ARGUMENT)
    DEFAULT_RESPONSE(POWER, NEG_ZERO_TO_NEG,    NEG_OVERFLOW_ERR)
    DEFAULT_RESPONSE(POWER, ZERO_TO_ZERO,       INVALID_ARGUMENT)
    DEFAULT_NO_ERROR(POWER, POS_INF_TO_POS,     POS_INFINITY)
    DEFAULT_NO_ERROR(POWER, NEG_INF_TO_POS,     POS_INFINITY)
    DEFAULT_NO_ERROR(POWER, NEG_INF_TO_POS_ODD, NEG_INFINITY)
    DEFAULT_NO_ERROR(POWER, FINITE_TO_INF,      POS_INFINITY)
    DEFAULT_NO_ERROR(POWER, INF_TO_NEG,         POS_ZERO)
    DEFAULT_NO_ERROR(POWER, SMALL_TO_INF,       POS_ZERO)

    DEFAULT_RESPONSE(INTPOWER, POS_OVERFLOW,     POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(INTPOWER, NEG_OVERFLOW,     NEG_OVERFLOW_ERR)
    DEFAULT_RESPONSE(INTPOWER, POS_UNDERFLOW,    POS_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(INTPOWER, NEG_UNDERFLOW,    NEG_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(INTPOWER, ZERO_TO_ZERO,     INVALID_ARGUMENT)
    DEFAULT_RESPONSE(INTPOWER, POS_DIV_BY_ZERO,  POS_SINGULARITY)
    DEFAULT_RESPONSE(INTPOWER, NEG_DIV_BY_ZERO,  NEG_SINGULARITY)

    DEFAULT_RESPONSE(INTINTPOWER, OVERFLOW, POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(INTINTPOWER, ZERODIV,  INVALID_ARGUMENT)

    DEFAULT_RESPONSE(REM, UNDERFLOW, POS_UNDERFLOW_ERR)
    DEFAULT_NO_ERROR(REM, BY_ZERO,   POS_ZERO)
    DEFAULT_RESPONSE(REM, OF_INF,    INVALID_ARGUMENT)

    DEFAULT_RESPONSE(SIN, OF_INFINITY, INVALID_ARGUMENT)

    DEFAULT_RESPONSE(SINCOS, OF_INFINITY, INVALID_ARGUMENT)

    DEFAULT_RESPONSE(SINCOSD, OF_INFINITY, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(SINCOSD, UNDERFLOW,   POS_UNDERFLOW_ERR)

    DEFAULT_RESPONSE(SIND, OF_INFINITY, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(SIND, UNDERFLOW,   POS_UNDERFLOW_ERR)

    DEFAULT_RESPONSE(SINH, OVERFLOW,     POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(SINH, NEG_OVERFLOW, NEG_OVERFLOW_ERR)
    DEFAULT_RESPONSE(SINH, UNDERFLOW,    POS_UNDERFLOW_ERR)

    DEFAULT_RESPONSE(SQRT, OF_NEGATIVE, INVALID_ARGUMENT)

    DEFAULT_RESPONSE(RSQRT, OF_POS_ZERO, POS_SINGULARITY)
    DEFAULT_RESPONSE(RSQRT, OF_NEG_ZERO, NEG_SINGULARITY)

    DEFAULT_RESPONSE(TAN, OF_INFINITY, INVALID_ARGUMENT)

    DEFAULT_RESPONSE(TAND, UNDERFLOW,          POS_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(TAND, OVERFLOW,           POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(TAND, OF_INFINITY,        INVALID_ARGUMENT)
    DEFAULT_RESPONSE(TAND, ODD_MULTIPLE_OF_90, POS_SINGULARITY)

    DEFAULT_RESPONSE(TANH, OVERFLOW,  POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(TANH, UNDERFLOW, POS_UNDERFLOW_ERR)

    DEFAULT_RESPONSE(TANCOT, OF_INFINITY, INVALID_ARGUMENT)

    DEFAULT_RESPONSE(TANCOTD, OF_INFINITY, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(TANCOTD, UNDERFLOW,   POS_UNDERFLOW_ERR)

    DEFAULT_NO_ERROR(BES_J0, OF_INFINITY, POS_ZERO)
    DEFAULT_NO_ERROR(BES_J1, OF_INFINITY, POS_ZERO)
    DEFAULT_NO_ERROR(BES_JN, OF_INFINITY, POS_ZERO)

    DEFAULT_RESPONSE(BES_J1, UNDERFLOW,     POS_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(BES_J1, NEG_UNDERFLOW, NEG_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(BES_JN, UNDERFLOW,     POS_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(BES_JN, NEG_UNDERFLOW, NEG_UNDERFLOW_ERR)

    DEFAULT_NO_ERROR(BES_Y0, OF_INFINITY, POS_ZERO)
    DEFAULT_NO_ERROR(BES_Y1, OF_INFINITY, POS_ZERO)
    DEFAULT_NO_ERROR(BES_YN, OF_INFINITY, POS_ZERO)

    DEFAULT_RESPONSE(BES_Y0, OF_NEGATIVE, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(BES_Y0, OF_ZERO,     NEG_SINGULARITY)

    DEFAULT_RESPONSE(BES_Y1, OF_NEGATIVE, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(BES_Y1, OF_ZERO,     NEG_SINGULARITY)
    DEFAULT_RESPONSE(BES_Y1, OVERFLOW,    NEG_OVERFLOW_ERR)

    DEFAULT_RESPONSE(BES_YN, OF_NEGATIVE,  INVALID_ARGUMENT)
    DEFAULT_RESPONSE(BES_YN, OF_ZERO,      NEG_SINGULARITY)
    DEFAULT_RESPONSE(BES_YN, NEG_OVERFLOW, NEG_OVERFLOW_ERR)
    DEFAULT_RESPONSE(BES_YN, POS_OVERFLOW, POS_OVERFLOW_ERR)

    DEFAULT_RESPONSE(LGAMMA, OVERFLOW,      POS_OVERFLOW_ERR)
    DEFAULT_NO_ERROR(LGAMMA, POS_INF,       POS_INFINITY)
    DEFAULT_RESPONSE(LGAMMA, NEG_INF,       INVALID_ARGUMENT)
    DEFAULT_RESPONSE(LGAMMA, NON_POS_INT,   POS_SINGULARITY)
    DEFAULT_RESPONSE(LGAMMA, OF_ZERO,       POS_SINGULARITY)

    /* PLACEHOLDER for ERF Function */

    DEFAULT_RESPONSE(ERFC, UNDERFLOW,  POS_UNDERFLOW_ERR)
    DEFAULT_NAN(NANFUNC, CANONICAL_NAN)
    DEFAULT_RESPONSE(EXP2, OVERFLOW,   POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(EXP2, UNDERFLOW,  POS_UNDERFLOW_ERR)
    DEFAULT_NO_ERROR(EXP2, OF_INF,     POS_INFINITY)
    DEFAULT_NO_ERROR(EXP2, OF_NEG_INF, POS_ZERO)
    DEFAULT_RESPONSE(SCALBN, OVERFLOW,     POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(SCALBN, NEG_OVERFLOW, NEG_OVERFLOW_ERR)
    DEFAULT_RESPONSE(SCALBN, UNDERFLOW,    POS_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(SCALBLN, OVERFLOW,     POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(SCALBLN, NEG_OVERFLOW, NEG_OVERFLOW_ERR)
    DEFAULT_RESPONSE(SCALBLN, UNDERFLOW,    POS_UNDERFLOW_ERR) 
    DEFAULT_RESPONSE(TGAMMA, OVERFLOW,      POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(TGAMMA, NEG_OVERFLOW,  NEG_OVERFLOW_ERR)
    DEFAULT_NO_ERROR(TGAMMA, POS_INF,       POS_INFINITY)
    DEFAULT_RESPONSE(TGAMMA, NEG_INF,       INVALID_ARGUMENT)
    DEFAULT_RESPONSE(TGAMMA, EVEN_NEG_INT,  POS_SINGULARITY)
    DEFAULT_RESPONSE(TGAMMA, ODD_NEG_INT,   NEG_SINGULARITY)
    DEFAULT_RESPONSE(TGAMMA, OF_ZERO,       POS_SINGULARITY)
    DEFAULT_RESPONSE(LRINT,   OVERFLOW, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(LROUND,  OVERFLOW, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(LLRINT,  OVERFLOW, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(LLROUND, OVERFLOW, INVALID_ARGUMENT)
    DEFAULT_RESPONSE(REMQUO, UNDERFLOW, POS_UNDERFLOW_ERR)
    DEFAULT_NO_ERROR(REMQUO, BY_ZERO,   POS_ZERO)
    DEFAULT_RESPONSE(REMQUO, OF_INF,    INVALID_ARGUMENT)
    DEFAULT_RESPONSE(NEXTTOWARD, POS_OVERFLOW,  POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(NEXTTOWARD, NEG_OVERFLOW,  NEG_OVERFLOW_ERR)
    DEFAULT_RESPONSE(NEXTTOWARD, POS_UNDERFLOW, POS_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(NEXTTOWARD, NEG_UNDERFLOW, NEG_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(FDIM, POS_OVERFLOW,  POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(FDIM, POS_UNDERFLOW, POS_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(FMA, POS_UNDERFLOW, POS_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(FMA, NEG_UNDERFLOW, NEG_UNDERFLOW_ERR)
    DEFAULT_RESPONSE(FMA, POS_OVERFLOW,  POS_OVERFLOW_ERR)
    DEFAULT_RESPONSE(FMA, NEG_OVERFLOW,  NEG_OVERFLOW_ERR)
    DEFAULT_RESPONSE(FMA, INF_AND_ZERO,  INVALID_ARGUMENT)
    DEFAULT_RESPONSE(FMA, INF_AND_INF,   INVALID_ARGUMENT) 
    /*
     * END_ENUMERATION_LIST:
     */

    LAST_ENUM_ENTRY

#ifndef MAKE_DPML_ERROR_CODES_ENUM

    /*
     * At this point, any platform specific modifications to the default
     * reponses can be made in the manner described above by using any of
     * the DEFINE_REPONSE, DEFAULT_RESPONSE and DEFAULT_NO_ERROR macros.  
     */

#   if (OP_SYSTEM == osf) || (OP_SYSTEM == vms) || (OP_SYSTEM == linux)

        DEFAULT_RESPONSE(ATANH, OF_NEG_ONE,           NEG_OVERFLOW_ERR)
        DEFAULT_RESPONSE(ATANH, OF_ONE,               POS_OVERFLOW_ERR)
        DEFAULT_RESPONSE(EXP,   OF_INF,               POS_OVERFLOW_ERR)
        DEFAULT_RESPONSE(EXP,   OF_NEG_INF,           POS_UNDERFLOW_ERR)
        DEFAULT_RESPONSE(LOG,   OF_ZERO,              NEG_OVERFLOW_ERR)
        DEFAULT_RESPONSE(LOG10, OF_ZERO,              NEG_OVERFLOW_ERR)
        DEFAULT_RESPONSE(LOG1P, M1,                   NEG_OVERFLOW_ERR)
        DEFAULT_RESPONSE(LOG2,  OF_ZERO,              NEG_OVERFLOW_ERR)
        DEFAULT_RESPONSE(POWER, FINITE_TO_INF,        POS_OVERFLOW_ERR)
        DEFAULT_RESPONSE(POWER, SMALL_TO_INF,         POS_UNDERFLOW_ERR)
        DEFAULT_RESPONSE(SCALB, OF_FINITE_TO_NEG_INF, POS_UNDERFLOW_ERR)
        DEFAULT_RESPONSE(SCALB, OF_NEG_TO_POS_INF,    NEG_OVERFLOW_ERR)
        DEFAULT_RESPONSE(SCALB, OF_POS_TO_POS_INF,    POS_OVERFLOW_ERR)

        /*
         * IEEE mode response to invalid arguments for these functions should
         * be 0 rather than NaN.  Restore default response for any subsequent
         * processing
         */

        save_index = default_ieee_ret[INVALID_ARGUMENT];
        default_ieee_ret[INVALID_ARGUMENT] = POS_ZERO_INDEX;

        DEFAULT_RESPONSE(MOD, BY_ZERO,   INVALID_ARGUMENT)
        DEFAULT_RESPONSE(REM, BY_ZERO,   INVALID_ARGUMENT)

        default_ieee_ret[INVALID_ARGUMENT] = save_index;


        /*
         * Fast mode response to invalid arguments for these functions should
         * be -HUGE rather than 0.  Restore default response for any subsequent
         * processing
         */

        save_index = default_fast_ret[INVALID_ARGUMENT];
        default_fast_ret[INVALID_ARGUMENT] = NEG_HUGE_INDEX;

        DEFAULT_RESPONSE(BES_Y0, OF_NEGATIVE, INVALID_ARGUMENT)
        DEFAULT_RESPONSE(BES_Y1, OF_NEGATIVE, INVALID_ARGUMENT)
        DEFAULT_RESPONSE(BES_YN, OF_NEGATIVE, INVALID_ARGUMENT)
        DEFAULT_RESPONSE(LOG,    OF_NEGATIVE, INVALID_ARGUMENT)
        DEFAULT_RESPONSE(LOG2,   OF_NEGATIVE, INVALID_ARGUMENT)
        DEFAULT_RESPONSE(LOG10,  OF_NEGATIVE, INVALID_ARGUMENT)
        DEFAULT_RESPONSE(LOG1P,  LESS_M1,     INVALID_ARGUMENT)

        default_fast_ret[INVALID_ARGUMENT] = save_index;

#   endif

    /*
     * END_PLATFORM_SPECIFIC_RESPONSES
     *
     * Now loop through the array an generate the response table
     */

    printf("#define RESPONSE_TABLE      " STR(RESPONSE_TABLE_NAME) "\n");
    printf("static const DPML_EXCEPTION_RESPONSE RESPONSE_TABLE[] = {\n");
    for (i = 0; i < LAST_ERROR_CODE; i++)
        PRINT_RESPONSE_INFO(i);
    printf("    };\n");

#endif

    @end_divert
    @eval my $outText    = MphocEval GetStream( "divertText" );	\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),	\
                             "Dpml error code enumerations",	\
                                __FILE__ );			\
             print "$headerText\n\n$outText";


