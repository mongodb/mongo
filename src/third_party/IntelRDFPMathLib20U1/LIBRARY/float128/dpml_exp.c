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

#define MAKE_COMMON
#if !defined(BUILD_FILE_NAME)
#   define BUILD_FILE_NAME  F_POW_BUILD_FILE_NAME
#endif


/*
** Pick up the latest default DPML definitions. however, don't let
** dpml_private.h default TABLE_NAME.  This has already been done or over-
** ridden when the constant file was generated.
*/

#define	DONT_DEFAULT_TABLE_NAME		1
#define NEW_DPML_MACROS			1
#include "dpml_private.h"

#if defined(UNDEF_TABLE_NAME)
#   undef TABLE_NAME
#endif

/*
** Pick up common build time constants and definitions from the generated
** power constant table file
*/

#define DEFINE_SYMBOLIC_CONSTANTS	1
#include STR( BUILD_FILE_NAME )

/*
** Pick up common compile time constants and definitions shared by
** pow and exp.
*/

#include "dpml_pow.h"		/* Must come after BUILD_FILE_NAME */

/*
** Select data type and algorithm specific constants from the included file
*/

#if EXP2
#   define EXP_SELECT(a,b,c)         a
#   define EXP_EXP10_SELECT(a,b)
#   define EXP2_SELECT(a,b)          a
#   define NOT_EXP2(a)
#elif EXP10
#   define EXP_SELECT(a,b,c)         b
#   define EXP_EXP10_SELECT(a,b)     b
#   define EXP2_SELECT(a,b)          b
#   define NOT_EXP2(a)               a
#else
#   define EXP_SELECT(a,b,c)         c
#   define EXP_EXP10_SELECT(a,b)     a
#   define EXP2_SELECT(a,b)          b
#   define NOT_EXP2(a)               a
#endif


#if (!ONE_TYPE && USE_BACKUP)
#   define EXP_HI_CHECK         EXP_SELECT(EXP2_HI_CHECK_R, \
                                           EXP_HI_CHECK_R,  \
                                           EXP10_HI_CHECK_R)
#   define EXP_LO_CHECK		EXP_LO_CHECK_R
#   define POW2_LO_CHECK	POW2_LO_CHECK_R
#   define POW2_HI_CHECK	POW2_HI_CHECK_R
#else
#   define EXP_HI_CHECK         EXP_SELECT(EXP2_HI_CHECK_F, \
                                           EXP_HI_CHECK_F,  \
                                           EXP10_HI_CHECK_F)
#   define EXP_LO_CHECK		EXP_LO_CHECK_F
#   define POW2_LO_CHECK	POW2_LO_CHECK_F
#   define POW2_HI_CHECK	POW2_HI_CHECK_F
#endif

#define BIG			ACC_BIG

/*
**  Design Overview:
**  ---------------
**
**  Given x and a positive integer POW2_K, for b = 2, e or 10 define:
**
**	t = x * (lnb/ln2)
**	m = nint(t * (2^POW2_K))/2^POW2_K			(1)
**      z = x - m ( ln2/lnb)
**      w = t - m
**
**  Then
**
**	e^x = 2^(x*(lnb/ln2))
**	    = 2^(m + w)
**	    = 2^m * 2^w						(2)
**	    = 2^m * e^z						(3)
**
**  From the definition of m, it follows that m can be re-written as
**
**          m  =  I + j/(2^POW2_K)
**
**  where I and j are integers and 0 <= j <= 2^POW2_K - 1. It follows that (3)
**  can be written as:
**
**      e^x = 2^(I + j/(2^POW2_K)) * e^z
**          = 2^I * 2^(j/(2^POW2_K)) * e^z			(4)
**
**  Now e^z can be approximated by a polynomial of the form:
**
**      e^z = 1 + z*p(z)
**
**  So that (4) can be rewritten as:
**
**      e^x = 2^I  *  2^(j/(2^POW2_K))*(1 + z*p(z))		(5)
**
**  Design Details:
**  --------------
**
**	o The multiplication by 2^I can be accomplished by adding I to the
**        exponent field of a binary floating point number or creating  an
**        appropriate floating point value and multiplying.
**
**	o There are exactly 2^(POW2_K) possible values represented by
**        2^(j/(2^POW2_K)).   These  can  be stored in a table and looked up
**        at run time using the bit sequence in 'j' as an index.  The form of
**	  the table values (hi and lo pieces) is determined by efficiency
**	  considerations for fast pow and exp, although the performance
**	  impact on exp is minimal: one floating point add, if sequential
**	  polynomial evaluation is used; none if parallel evaluation is used.
**
*/

/*
** IMPLEMENTATION ISSUES:
** ----------------------
**
** In general, extra precision is required at two key points in the algorithm:
** the argument reduction (computing z or w), and the scaling by 2^(j/2^POW2_K).
** On systems where no backup precision is available, these calculations are
** performed in hi and lo pieces to preserve the overall accuracy of the final
** result.
**
** Let T(j) =  2^(j/2^POW2_K) correctly rounded to F_PRECISION and define
** R(j) = [2^(j/2^POW2_K) - T(j)]/T(j).  We assume that these values are
** precomputed and stored in a table.
**
** 
**    Data Types With No Backup
**    -------------------------
**
**	Compute the reduced argument, z = x - m*(ln2/lnb), carefully, using
**	ln2 in hi and lo pieces:
**
**              z <-- [ x - m*hi_bits(ln2/lnb) ] - m*lo_bits(ln2/lnb)
** 
**	Perform the evaluation as:
**
**		e^x = 2^I * 2^(j/(2^POW2_K)) * [ 1 + z*p(z) ]
**		    = 2^I * [ T(j) + T(j)*R(j) ] * [ 1 + z*p(z) ]
**		    = 2^I * T(j) * [ 1 + R(j) ] * [ 1 + z*p(z) ]
**		    = [ 2^I * T(j) ] * [ 1 + R(j) + z*p(z) + R(j)*z*p(z) ]
**
**	Denote 2^I*T(j) as V(I,j) and note that the last term is insignificant
**	relative to the final result, then we have:
**
**		e^x = [ 2^I * T(j) ] * [ 1 + R(j) + z*p(z) + R(j)*z*p(z) ]
**		    = V(I,j) * { 1 + [ R(j) + z*p(z) ] }
**		    = V(I,j) + V(I,j)*[ R(j) + z*p(z) ]
**
**    Data Types With Backup
**    ----------------------
**
**	Compute all intermediate results in backup precision:
**
**		w <-- (x/ln2) - m
**
**		e^x = 2^I * 2^(j/(2^POW2_K)) * 2^w
**		    = 2^I * T(j) * [1 + w*q(w)]
**		    = V(I,j) * Q(w)
**
** where V(I,j) is as above.
*/


#ifndef BASE_NAME
#    define BASE_NAME       EXP_SELECT( EXP2_BASE_NAME, \
                                        EXP10_BASE_NAME,  \
                                        EXP_BASE_NAME)
#endif

#define ACC_POLY_F          EXP_SELECT(ACC_POW2_POLY_F, ACC_EXP10_POLY_F, ACC_EXP_POLY_F)
#define ERROR_OVERFLOW      EXP2_SELECT(EXP2_OVERFLOW, EXP_OVERFLOW)
#define ERROR_UNDERFLOW     EXP2_SELECT(EXP2_UNDERFLOW, EXP_UNDERFLOW)
#define NO_ERR_POS_INF      EXP2_SELECT(EXP2_OF_INF, EXP_OF_INF)
#define NO_ERR_NEG_INF      EXP2_SELECT(EXP2_OF_NEG_INF, EXP_OF_NEG_INF)


#if !defined(SPECIAL_EXP)

#   if !defined F_ENTRY_NAME
#       define F_ENTRY_NAME	   EXP_SELECT(F_EXP2_NAME, \
                                              F_EXP10_NAME,  \
                                              F_EXP_NAME)
#   endif

    F_TYPE
    F_ENTRY_NAME( F_TYPE x )
        {
        EXCEPTION_RECORD_DECLARATION
        B_TYPE fm, z, w, t;
        B_UNION stack_tmp_u;
        F_UNION stack_tmp_v;
    
        U_WORD status_word;
        WORD   m, i, j;
# if !COMPATIBILITY_MODE
        WORD   func_error_word ;
# endif

        /*
        ** Weed out: near overflow and underflow cases; NaN's, Inf's and
        ** denorms; arguments that would underflow during polynomial
        ** evaluation.
        **
        ** The product x*(1/ln2) is on the critical path of this routine.
        ** Because the code is structured with a branch prior to the
        ** multiplication, some compilers fail to schedule the load of the
        ** constant 1/ln2 early enough to avoid delaying the reduced argument
        ** computation.  To avoid this delay, we preload (1/ln2).
        */
    
        stack_tmp_v.f = x;
        i = stack_tmp_v.F_SIGNED_HI_WORD;
        m = i & MAKE_MASK(F_SIGN_BIT_POS, 0);
        NOT_EXP2( t = EXP_EXP10_SELECT(RECIP_LN2, LN10_OV_LN2); )
        if (((U_WORD) m - EXP_LO_CHECK) >= EXP_HI_CHECK)
            goto possible_problems;
    
        /*
        ** compute the reduced argument, w.  Note that we obtain
        ** nint((x/ln2) * (2^POW2_K)) by adding x/ln2 to a suitably large
        ** constant (BIG).  This requires that the rounding mode in effect
        ** at the time the add takes place must be round-to-nearest.
        */

        z = EXP2_SELECT( (B_TYPE) x , ((B_TYPE) x) * t ) ;

        INIT_FPU_STATE_AND_ROUND_TO_NEAREST(status_word);
    
        t = BIG + z;		/* Save for getting m later on */
        fm = D_GROUP(t - BIG);

        #define CONS_HI	EXP_EXP10_SELECT( LN2_HI, LN2_OV_LN10_HI )
        #define CONS_LO	EXP_EXP10_SELECT( LN2_LO, LN2_OV_LN10_LO )
        w = EXP2_SELECT(
             z - fm , BACKUP_SELECT( z - fm, D_GROUP(x - fm*CONS_HI) - fm*CONS_LO ) );

        /*
        ** Now get the bits of m as a integer and break it up into I and j
        */
    
        stack_tmp_u.f = t;
        GET_LOW_32_BITS(m, stack_tmp_u);
        j = (m & POW2_INDEX_MASK) << POW2_INDEX_POS;
        i = m & (~POW2_INDEX_MASK);

        z = BACKUP_SELECT(
                ACC_POW2_POLY_R(w),
                ACC_POLY_F( POW2_LO_OV_POW2_HI(j), w )
                );
    
        /*
        ** Scale 2^(j/2^POW2_K) by 2^I, so that only one multiply is done.
        ** Note that the macros IPOW2(j) and IPOW2_LO(u,j) are used to access
        ** the table value T(j) as an integer rather than as a floating point
        ** value.
        */

        IF_SMALL_WORD(IPOW2_LO(stack_tmp_u,j);)
        m = IPOW2(j);
        i = ALIGN_SCALE_WITH_EXP(i);
        m = W_ADD_TO_EXP_FIELD(m, i);
        stack_tmp_u.B_HI_WORD = m;
        t = stack_tmp_u.f;
    
        /* Check for possible problems and do the final multiply */

        if (((U_WORD) IEEE_SELECT(m, (m & LO_MASK)) - POW2_LO_CHECK)
          >= POW2_HI_CHECK)
            goto boundary_check;

        z = BACKUP_SELECT(z*t, t + t*z);
    
        RESTORE_FPU_STATE(status_word);
        return (F_TYPE) z;
    
    boundary_check:
    
        /*
        ** Need to check for overflow, underflow and denormals.  Don't need
        ** round to nearest state any more so restore original state
        */
    
        RESTORE_FPU_STATE(status_word);
    
    
        /* 
        ** Do the "final" multiple.  However, when no backup is available
        ** the final multiply might involve a denormal or NaN, so we need to
        ** do this scaling carefully.  Note that for the no-backup case,
        ** we are essentially re-doing the calculation as
        ** 2^I*(T(j) + T(j)*p(z))
        */

        BACKUP_SELECT(z = t*z;, t = POW2_HI(j);
                                z = t + t*z;     )

        stack_tmp_u.f = z;
        m = stack_tmp_u.B_HI_WORD;

        IF_NO_BACKUP(
            /* "Multiply" by 2^i */
            m = W_ADD_TO_EXP_FIELD(m, i);
            stack_tmp_u.B_HI_WORD = m;
            z = stack_tmp_u.f;
            )
    
        /*
        ** Isolate exponent field and check for overflow and underflow.  Note
        ** that subtracting 1 from the biased exponent field causes 0 exponents
        ** (which indicate underflows or denormal results) to be mapped to the 
        ** high integer range.  This allows testing for OK results, overflows
        ** and underflow by checking the size of (i-1)
        */
    
        i = ((U_WORD) m) >> B_EXP_POS;
        IF_VAX( i &= MAKE_MASK(B_EXP_WIDTH + 1, 0); )
        i -=  BACKUP_SELECT( (F_MIN_BIN_EXP + B_EXP_BIAS), 1);
        if ((U_WORD) i <= 
          F_MAX_BIN_EXP - BACKUP_SELECT( F_MIN_BIN_EXP, (1 - B_EXP_BIAS)))
            /* No exceptions, return final result */
            return (F_TYPE) z;

# if COMPATIBILITY_MODE
        j = ERROR_OVERFLOW;
# else
	func_error_word = ERROR_WORD( STATUS_OVERFLOW,
				      POS_HUGE_INDEX,
				      POS_INFINITY_INDEX,
				      F_TYPE_ENUM,
				      DPML_ERANGE,
				      0 ) ;
# endif

        if (((U_WORD) i) < (2*F_MAX_BIN_EXP + B_EXP_BIAS)) 
           goto do_exception;
    
        /*
        ** OK, If we get here, the result is (most likely) an underflow or
        ** a denormal value.
        */
    
# if COMPATIBILITY_MODE
        j = ERROR_UNDERFLOW;
# else
	func_error_word = ERROR_WORD( STATUS_UNDERFLOW,
				      POS_ZERO_INDEX,
				      POS_ZERO_INDEX,
				      F_TYPE_ENUM,
				      DPML_ERANGE,
				      0 ) ;
# endif
    
#       if IEEE_FLOATING
            
            i += (F_PRECISION + 1);
            if ((WORD) i < 0)
		goto do_exception;
    
            /*
            ** The result is probably denormal so we have to (carefully)
            ** generate the result.  Begin by setting the exponent field to
            ** the exponent field of 1, minus the number of bits of
            ** "denormalization" and  convert to F_TYPE
            */
    
            {
            F_TYPE u, v;
    
            stack_tmp_u.B_HI_WORD = m - ALIGN_WITH_B_TYPE_EXP(F_MIN_BIN_EXP);
            v = (F_TYPE) stack_tmp_u.f;
    
            /*
            ** Now add 1 to scaled result to force an alignment shift.  The
            ** result of the addition will have correct fraction field for
            ** the denormalized result, but not the correct exponent
            */
    
            u = (F_TYPE) ONE;
            v += u;
            stack_tmp_v.f = v;
    
            /*
            ** If the result of the sum is equal to one, then the final result
            ** will underflow.  Otherwise remove the scale factor (i.e. get
            ** the correct exponent field) by subtracting the exponent of 1
            ** from v.
            */

            if (v == u) 
               goto do_exception;
            m = stack_tmp_v.F_HI_WORD - ALIGN_W_EXP_FIELD(F_EXP_BIAS - F_NORM);
            stack_tmp_v.F_HI_WORD = m;
            v = stack_tmp_v.f;

            /*
            ** check to see if final result really was a denorm
            */
    
# if COMPATIBILITY_MODE
            if (((m & F_EXP_MASK) == 0) && !PROCESS_DENORMS) 
               goto do_exception;
# else

	    if ( ( m & F_EXP_MASK ) == 0 ) {
		P_EXCPTN_VALUE_F( tmp_rec.value_under_test, v ) ;
		func_error_word = ERROR_WORD( STATUS_DENORM_PROCESSING | STATUS_UNDERFLOW,
					      POS_ZERO_INDEX,
					      POS_ZERO_INDEX,
					      F_TYPE_ENUM,
					      DPML_ERANGE,
					      0 ) ;
		goto do_exception ;
		}
# endif
	    return v ;

            }
    
        NaN_or_Inf:
    
           /*
           ** If we get here, i and m are the high words of x and |x|
           ** respectively.  If m doesn't contain all of the bits of x,
           ** (i.e. BITS_PER_WORD < BITS_PER_F_TYPE) check for non-zero bits
           ** in the low word
           */

           m <<= (BITS_PER_WORD - F_EXP_POS);
           IF_SMALL_WORD( m = m OR_LOW_BITS_SET(stack_tmp_v); )
           if (m != 0)
               /* x was NaN, just return it */
               return x;

            /* x was +/- infinity */
# if COMPATIBILITY_MODE
            j = ((WORD) i < 0) ? NO_ERR_NEG_INF : NO_ERR_POS_INF;
# else
	    func_error_word = ( ( WORD )i < 0 ) ?
                 ERROR_WORD( EXP2_SELECT(STATUS_NO_ERROR,STATUS_UNDERFLOW),
				      POS_ZERO_INDEX,
				      POS_ZERO_INDEX,
				      F_TYPE_ENUM,
				      DPML_ERANGE,
			              0 ) :
                 ERROR_WORD( EXP2_SELECT(STATUS_NO_ERROR,STATUS_OVERFLOW),
				      POS_HUGE_INDEX,
				      POS_INFINITY_INDEX,
				      F_TYPE_ENUM,
				      DPML_ERANGE,
				      0 ) ;

# endif
#       endif


        goto do_exception;



    
    possible_problems:
    
        /*
        ** If we get here, x was either very small (e^x = 1), very big (e^x is
        ** overflow or underflow) or and IEEE special case (NaN or Inf)
        */

    
        IF_IEEE(
            /* Screen out NaN's and Inf's */    
            if (m >= F_EXP_MASK) goto NaN_or_Inf;
            )
    
        /* If argument is tiny (including denorms and zero) just return 1. */
    
        if (m <= EXP_LO_CHECK)
            return (F_TYPE) ONE;
    
        /*
        ** If we get here, the final result is guaranteed to overflow or 
        ** underflow, depending on the sign of x.  Since i contains the high
        ** bits of x, just branch on the sign bit of x.
        */
    
# if COMPATIBILITY_MODE
        j = (i & F_SIGN_BIT_MASK) ? ERROR_UNDERFLOW : ERROR_OVERFLOW;
            /* Argument was positive */

# else
	func_error_word = ( i & F_SIGN_BIT_MASK ) ?
	                  ERROR_WORD( STATUS_UNDERFLOW,
				      POS_ZERO_INDEX,
				      POS_ZERO_INDEX,
				      F_TYPE_ENUM,
				      DPML_ERANGE,
				      0 ) :
                          ERROR_WORD( STATUS_OVERFLOW,
				      POS_HUGE_INDEX,
				      POS_INFINITY_INDEX,
				      F_TYPE_ENUM,
				      DPML_ERANGE,
				      0 ) ;



# endif
        goto do_exception;


    do_exception:

# if COMPATIBILITY_MODE
        GET_EXCEPTION_RESULT_1(j, x, x);
        return x;
# else
	RETURN_EXCEPTION_RESULT_1( func_error_word, x, F_F, _FpCodeExp ) ;
# endif    

        }
    
#else /* defined(SPECIAL_EXP) */
    
#   if USE_BACKUP
#      define LO_PART_DECL
#   else
#      define LO_PART      	pow2_low
#      define LO_PART_DECL	, F_TYPE *LO_PART
#   endif

    B_TYPE
    F_EXP_SPECIAL_ENTRY_NAME (F_TYPE x, EXP_WORD_TYPE *pow_of_two LO_PART_DECL)
        {
        B_TYPE fm, z, w, t;
        B_UNION stack_tmp_u;
    
        WORD   m, j;
    
        /*
        ** compute the reduced argument, w.
        */
    
        z = (B_TYPE) x * RECIP_LN2;  
    
        t = BIG + z;		/* Save for getting m later on */
        fm = t - BIG;
        w = BACKUP_SELECT( (z - fm), (x - fm*LN2_HI) - fm*LN2_LO );
    
        /*
        ** Now get the bits of m as a integer and break it up into I and j.
        ** Return the value of I in *pow_of_two.
        */
    
        stack_tmp_u.f = t;
        GET_LOW_32_BITS(m, stack_tmp_u);
        j = (m & POW2_INDEX_MASK) << POW2_INDEX_POS;
    
#       if USE_BACKUP
    
            z = POW2_HI(j) * ACC_POW2_POLY_R(w);
    
#       else
    
            z = POW2_HI(j);
            *LO_PART = z*ACC_EXP_POLY_F( POW2_LO_OV_POW2_HI(j), w );

#       endif
    
        *pow_of_two = m & (~POW2_INDEX_MASK);
        return z;
    }

#endif
