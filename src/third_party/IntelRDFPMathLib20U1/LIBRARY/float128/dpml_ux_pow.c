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

#define	BASE_NAME	pow
#include "dpml_ux.h"

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif


/* 
** The DPML supports two different varieties of the power function: one for
** which 0^0 and (-x)^integer are exceptions (fortran semantics) and one for
** which 0^0 is 1 and (-x)^integer is computed as if by successive
** multiplications (C semantics).  Both of these routines call a common
** interface routine which unpacks the unpacks the argument and performs any
** necessary argument screening.  The common interface routine then calls an
** evaluation routine to actually compute the function result.
** 
** 
** BASIC ALGORITHM
** ---------------
** 
** The power function x^y is computed as 2^[y*log2(x)].  The key point in the
** algorithm is to insure a sufficient number of accurate bits in the product
** y*log2(x) to guarantee the accuracy of the final result.  In particular,
** if we write:
** 
** 		y*log2(x) = I + h	|h| < 1/2		(1)
** 
** Then
** 
** 		x^y = 2^[y*log2(x)]
** 		    = 2^(I + h)
** 		    = (2^I)*(2^h)
** 
** Since the multiplication by 2^I can be done by incrementing the exponent
** field of 2^h, the basic design criteria is to insure a sufficient number
** of bits in h.  For this design, we would like h to have 7 guard bits.  That
** means we want the total number of good bits in h to be about 120.  Since I
** can contain as many as 14 bits, this means that y*log(x) must contain at
** least 134 good bits.  This of course is a problem, because the unpacked
** format only has 128 fraction bits.  The approach we take is to represent
** log2(x) in high and low pieces.
*/ 



/*
** UX_POW is the common evaluation routine for the both the fortran and C
** power functions.  It assumes that all exceptional arguments, except those
** that might cause overflow or underflow, have been screened out.
*/

#if !defined(UX_POW)
#   define UX_POW	__INTERNAL_NAME(ux_pow__)
#endif

void
UX_POW( UX_FLOAT * x, UX_FLOAT * y, UX_FLOAT * result)
    {
    WORD cnt;
    UX_SIGN_TYPE sign;
    UX_EXPONENT_TYPE exponent;
    UX_FRACTION_DIGIT_TYPE _Z, LOG2_HI, tmp_digit, I, inc;
    UX_FLOAT tmp[3], log2_hi, log2_lo, z, z_lo, u, r, w, h;

    /* 
    ** COMPUTING LOG IN HI AND LOW PIECES
    ** -----------------------------------
    ** 
    ** Given x = 2^n*g, where 1/sqrt(2) <= g < sqrt(2), we can compute log2(x)
    ** as:
    ** 		     2(g - 1)		     2
    ** 		z = ----------		w = z
    ** 		    (g + 1)ln2
    ** 		                                      2k
    ** 		                    __Inf [z*ln2/2)^2]
    ** 		log2(x) = n +   z   >     -------------
    ** 		                   /__k=0    (2k+1)
    ** 
    ** 		                    
    ** 		log2(x) ~ n +  z + z^3*p(z^2) 
    ** 
    ** 
    ** Letting z = z_hi + z_lo, then we can write
    ** 
    ** 		log2(x) ~ n + (z_hi + z_lo) + z*w*p(w) 
    ** 		        = (n + z_hi) + (z_lo + z*w*p(w))
    ** 		        = L_hi + L_lo.				(3)
    ** 
    ** The trick here is to define z_hi, so that n + z_hi is exactly
    ** representable in one digit.  This involves computing z to extended
    ** precision which is a fairly complicated process.  We begin by 
    ** putting x in th form x = 2^n*g where 1/sqrt(2) < g <= sqrt(2)
    */

    exponent = G_UX_EXPONENT(x);
    tmp_digit = G_UX_MSD(x);
    if (tmp_digit <= ONE_OVER_SQRT_2)
        exponent--;
    UX_DECR_EXPONENT(x, exponent);

    /* 
    ** In preporation for computing z_hi and z_lo, we compute z as
    ** 
    ** 		t0 <-- g + 1
    ** 		t1 <-- g - 1
    ** 		r <-- 2/(t0*ln2)
    ** 		z <-- t1*r
    **
    ** At this point, we can also compute the polynomial, z^3*p(z^2)
    */ 

    UX_COPY( UX_ONE, result);
    ADDSUB(x, result, ADD_SUB, tmp);
    DIVIDE( UX_TWO_OVER_LN2, &tmp[0], FULL_PRECISION, &r);
    MULTIPLY( &r,  &tmp[1], &z);

    /*
    ** If the binary exponent of z is i, the binary exponent of n is j, and k
    ** is the number of bits in a fraction digit, then we want to take the high
    ** k-(j-i) bits of z and combine them with n to create log2_hi.  If n is
    ** zero, just take log2_hi to be the high k bits of z.  We denote the
    ** high bits of z used to make log2_hi as integer, Z and let z_hi = 2^i*Z/K,
    ** where K = 2^k.
    **
    ** Get the necessary bits of z and compute n + high bits of z as the
    ** integer LOG2_HI
    */ 

    _Z = G_UX_MSD(&z);
    if (exponent == 0)
        {
        LOG2_HI = _Z;
        exponent = G_UX_EXPONENT(&z);
        sign = G_UX_SIGN(&z);
        }
    else
        { /* n != 0, convert n to unpacked */

        cnt = WORD_TO_UX(exponent, &tmp[2]);
        exponent = BITS_PER_UX_FRACTION_DIGIT_TYPE - cnt;
        cnt = exponent - G_UX_EXPONENT(&z);
        LOG2_HI = G_UX_MSD(&tmp[2]);
        sign = G_UX_SIGN(&tmp[2]);

        /* Take high bits from z */

        if (cnt >= BITS_PER_WORD)
            _Z = 0;
        else 
            {
            tmp_digit = (_Z >> cnt);
            _Z        = tmp_digit << cnt;
            tmp_digit = (G_UX_SIGN(&z) != G_UX_SIGN(&tmp[2])) ?
                         -tmp_digit : tmp_digit;
            LOG2_HI += tmp_digit;
            }
        }

    /*
    ** Compute log2_lo = z^3*p(z)
    */

    MULTIPLY( &z, &z, &tmp[2]);
    EVALUATE_RATIONAL(
        &tmp[2],
        POW_LOG2_COEF_ARRAY,
        POW_LOG2_COEF_ARRAY_DEGREE,
        NUMERATOR_FLAGS( POST_MULTIPLY ),
        &log2_lo);
    MULTIPLY( &z, &log2_lo, &log2_lo);


    /*
    ** If we denote the k most significant digit of ln2/2 as L (i.e.
    ** ln2/2 ~ L/(2*K)),
    ** then we can define
    ** 
    ** 		LO(ln2/2) = ln2/2 - L/(2*K)
    ** 
    ** Then we can obtain an approximation to z_lo by:
    ** 
    ** 	z_lo = z - z_hi
    ** 	     = t1/[t0*(ln2/2)] - z_hi 
    ** 	     = (t1 - t0*(ln2/2)*z_hi)/[t0*(ln2/2)]
    ** 	     = { t1 - t0*[L/(2*K) + LO(ln2/2)]*z_hi }/[t0*(ln2/2)]
    ** 	     = { t1 - t0*[2^(i-1)*(L*Z)/K^2 + LO(ln2/2)*z_hi}/[t0*(ln2/2)]
    ** 	     = { t1 - t0*[2^(i-1)*L*Z/K^2 + t0*z_hi*LO(ln2/2)] }/[t0*(ln2/2)]
    **
    ** We now define
    **
    ** 		u = 2^(i-1)*(L*Z)/K^2
    ** 
    ** So that z_lo can now be written as;
    **
    ** 	z_lo = { t1 - t0*[(2^i)*L*Z/K^2 + t0*z_hi*LO(ln2/2)] }/[t0*(ln2/2)]
    ** 	     = [ t1 - t0*( u + t0*z_hi*LO(ln2/2)) ]/[t0*(ln2/2)]
    ** 	     = [ (t1 - t0*u) - t0*z_hi*LO(ln2/2)]/[t0*(ln2/2)]
    ** 	     = (t1 - t0*u)/[t0*(ln2/2)] - t0*z_hi*LO(ln2/2)/[t0*(ln2/2)]
    ** 	     = (t1 - t0*u)*r - z_hi*LO(ln2/2)/(ln2/2) 
    ** 	     = (t1 - t0*u)*r - z_hi*2*LO(ln2)/ln2
    **
    ** With some care taken to prevent a loss of significance when computing
    ** t1 - t0*u.
    */

    if (_Z != 0)
        {
        /*
        ** We used some bits from Z so compute z_lo and add it to
        ** log2_lo.  Start the computation by getting u equal to the
        ** the high part of z*ln2 = 2^(i-1)*Z*L/K^2, also create z_hi
        ** in unpacked format
        */

        CLR_UX_LOW_FRACTION(&z);
        P_UX_MSD(&z, _Z);
        EXTENDED_DIGIT_MULTIPLY( _Z, MSD_OF_LN2, _Z, tmp_digit);
        UX_SET_SIGN_EXP_MSD( &u, G_UX_SIGN(&z), G_UX_EXPONENT(&z)-1, _Z);
        P_UX_FRACTION_DIGIT( &u, 1, tmp_digit);

        /*
        ** Compute t1 - t0*u carefully by using the extended multiply 
        **
        ** The value of z_lo is actually computed into the location
	** z so we can handle the Z = 0 (i.e. z_lo = z) case in the
        ** main flow
        */

        EXTENDED_MULTIPLY( &tmp[0], &u, &tmp[0], &tmp[2]);
        ADDSUB(&tmp[1], &tmp[0], SUB, &tmp[0]);
        ADDSUB(&tmp[0], &tmp[2], SUB, &tmp[0]);

        /*
        ** Now compute = (t0 - t1*u)*r - z_hi*(ln2_lo/ln2) into the same
        ** location as z.  This way the add for the Z == 0 and != 0 cases
        ** join up for the last add.
        */

        MULTIPLY(&tmp[0], &r, &tmp[0]);

        MULTIPLY(&z, UX_LN2_LO_OVER_LN2, &tmp[1]);
        ADDSUB(&tmp[0], &tmp[1], SUB, &z);
        }
    ADDSUB(&z, &log2_lo, ADD, &log2_lo);

    /*
    ** Compute I = rint(y*log2(x)) and h = y*log2(x) - I.  Start
    ** by obtaining I from y*log2_hi.
    **
    ** When x is very close to 1 and |y| is large, the current value
    ** of log2_hi doesn't have a enough significant bits to compute
    ** I.  So before we start, we take some of the high bits of log2_lo
    ** and put them in log2_hi
    */

    inc = G_UX_MSD(&log2_lo);
    cnt = exponent - G_UX_EXPONENT(&log2_lo);
    if ( cnt < BITS_PER_UX_FRACTION_DIGIT_TYPE)
        {
        tmp_digit = inc & MAKE_MASK(cnt, 0);
        P_UX_MSD(&log2_lo, tmp_digit);
        inc >>= cnt;
        inc = (sign ^ G_UX_SIGN(&log2_lo)) ? -inc : inc;
        LOG2_HI += inc;
        }

    /*
    ** Now that there are enough bits in log2_hi, compute I from the
    ** product of y*log2_hi.  In the process, screen for outrageous overflow
    ** or underflow and create an unpacked form of log2_hi for future use.
    */

    UX_SET_SIGN_EXP_MSD(&tmp[0], sign, exponent, LOG2_HI);
    exponent += G_UX_EXPONENT(y);
    if ( exponent > (signed) (F_EXP_WIDTH + 2))
        goto overflow_underflow;

    /*
    ** Get I by multiplying the high digit of y with LOG2_HI and rounding
    ** to the nearest integer.  We compute y*log2_hi(x) here so it can be
    ** shared with the code for I = 0 and I != 0 
    */

    I = 0;
    sign ^= G_UX_SIGN(y);
    EXTENDED_MULTIPLY( &tmp[0], y, &h, &tmp[0]);

    if (exponent >= 0)
        {
        UMULH( LOG2_HI, G_UX_MSD(y), I );
        cnt = BITS_PER_UX_FRACTION_DIGIT_TYPE - exponent;
        inc = SET_BIT(cnt - 1);
        tmp_digit = I + inc;
        inc += inc; 
        if (tmp_digit >= I)
            I = tmp_digit & -inc;
        else
            { /* A carry out occurred on the increment */
            cnt--;
            I = UX_MSB;
            exponent++;
            }

        /*
        ** Now we need to compute
        **
        **		h = y*log2(x) - I
        **		  = y*[log2_hi(x) + log2_lo(x)] - I
        **		  = [y*log2_hi(x) - I] + y*log2_lo(x)
        **
        ** for I != 0.
        */

        UX_SET_SIGN_EXP_MSD(&tmp[1], sign, exponent, I);
        ADDSUB(&h, &tmp[1], SUB, &h);
        ADDSUB(&h, &tmp[0], ADD, &h);
        }

    MULTIPLY(y, &log2_lo, &tmp[0]);
    ADDSUB(&tmp[0], &h, ADD, &h);

    /*
    ** Evaluate 2^h and do scaling
    */

    EVALUATE_RATIONAL(
        &h,
        POW2_COEF_ARRAY,
	POW2_COEF_ARRAY_DEGREE,
	NUMERATOR_FLAGS(STANDARD), 
        result);

    I >>= cnt;
    tmp_digit = -I;
    I = sign ? tmp_digit : I;
    UX_INCR_EXPONENT(result, I);
    return;

overflow_underflow:
    exponent = (sign ^ G_UX_SIGN(y)) ?
          UX_UNDERFLOW_EXPONENT : UX_OVERFLOW_EXPONENT;
    UX_SET_SIGN_EXP_MSD(result, 0, exponent, UX_MSB);
    return;
    }


/*
** C_UX_POW is the common interface routine is responsible for unpacking the
** arguments and processing exceptional input.  A big part of the exceptional
** input processing is the definition and accessing of the class-to-action
** array. However, there are two sets of exceptional cases that are not handled
** by the UNPACK routines.  They are:
** 
** 	(1) x is +/- Normal and y is +/- Infinity
** 	(2) x is -Normal or -Denormal and y is +/- Normal.
** 
** In the first case, we need to determine is |x| is less than, equal or
** greater than 1 and return 0, INVALID or Infinity respectively.  In the
** second case, we need to determine if y is an integer.  If not, signal
** invalid.  Otherwise, compute |x|^y and set the sign of the result according
** to the parity of y.
*/

#if !defined(C_UX_POW)
#   define C_UX_POW	__INTERNAL_NAME(C_ux_pow__)
#endif

static void
C_UX_POW(
  _X_FLOAT * packed_x, _X_FLOAT * packed_y,
  U_WORD const * class_to_action_array,
  _X_FLOAT * packed_result OPT_EXCEPTION_INFO_DECLARATION )
    {
    WORD x_fp_class, y_fp_class, index;
    UX_SIGN_TYPE sign;
    UX_EXPONENT_TYPE exponent;
    UX_FRACTION_DIGIT_TYPE tmp_digit;
    WORD underflow_error, overflow_error;
    WORD local_class_action_array[3], *p;
    UX_FLOAT unpacked_x, unpacked_y, unpacked_result, tmp;

    /*
    ** Initialize unpacked result to underflow.  We will use
    ** the underflow logic in the pack routine to report errors
    ** detected here.  Also initialize error codes
    */

    P_UX_EXPONENT(&unpacked_result, UX_UNDERFLOW_EXPONENT);
    P_UX_MSD(&unpacked_result, UX_MSB);
    underflow_error = POWER_UNDERFLOW;
    overflow_error  = POWER_POS_OVERFLOW;

    /* Unpack x and y */

    x_fp_class = UNPACK2(
        packed_x,
        packed_y,
        &unpacked_x,
        &unpacked_y,
        class_to_action_array,
        packed_result
        OPT_EXCEPTION_INFO_ARGUMENT );

    if (0 >= x_fp_class)
        return;

    /*
    ** OK, now we need to screen for the two sets of exceptional
    ** cases not covered by the class to action mappings.
    */

    sign = 0;
    y_fp_class = x_fp_class & MAKE_MASK(F_C_CLASS_BIT_WIDTH, 0);
    x_fp_class = ( x_fp_class >> F_C_CLASS_BIT_WIDTH) &
                              MAKE_MASK(F_C_CLASS_BIT_WIDTH, 0);

    if (F_C_INF == F_C_BASE_CLASS(y_fp_class))
        {
            exponent = G_UX_EXPONENT(&unpacked_x);
            if ((exponent == 1) && UX_FRACTION_IS_ONE_HALF(&unpacked_x))
                {
                underflow_error = POWER_ONE_TO_INF;
                goto pack_it;
                }
            sign = F_C_IS_NEG_CLASS(y_fp_class) ^ (exponent <= 0);
            if ((!sign) && F_C_IS_NEG_CLASS(x_fp_class))
                {
                underflow_error = POWER_NEG_BASE;
                goto pack_it;
                }
            tmp_digit = sign ? 0 : F_EXP_MASK;
            goto return_zero_or_inf;
        }
    else if (F_C_IS_NEG_CLASS(x_fp_class))
        {
        index = UX_RND_TO_INT(&unpacked_y, RZ_BIT_VECTOR | FRACTION_RESULT,
          NOT_USED, &tmp);
 
        if ((G_UX_MSD(&tmp) != 0) && (x_fp_class != F_C_NEG_INF) &&
            (x_fp_class != F_C_NEG_ZERO))
            {  /* y was not an integer */
            underflow_error = POWER_NEG_BASE;
            goto pack_it;
            }

        if (index & 1)
            {
            sign = UX_SIGN_BIT;
            overflow_error = POWER_NEG_OVERFLOW;
            }

        if (SET_BIT(x_fp_class) &
          ( SET_BIT(F_C_NEG_INF) | SET_BIT(F_C_NEG_ZERO) ))
            {

            tmp_digit = (((sign) && (G_UX_MSD(&tmp) != 0)) ? 
              F_SIGN_BIT_MASK : 0);
                          

return_zero_or_inf:

            P_X_DIGIT(packed_result, 0, tmp_digit);
            P_X_DIGIT(packed_result, 1, 0);

#           if (BITS_PER_DIGIT == 32)

                P_X_DIGIT(packed_result, 2, 0);
                P_X_DIGIT(packed_result, 3, 0);

#           endif

            return;
            }
        P_UX_SIGN(&unpacked_x, 0);
        }

    UX_POW(&unpacked_x, &unpacked_y, &unpacked_result);
    P_UX_SIGN(&unpacked_result, sign);

pack_it:

    PACK(
        &unpacked_result,
        packed_result,
        underflow_error,
        overflow_error
        OPT_EXCEPTION_INFO_ARGUMENT );
    }


#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_POW_NAME

X_XX_PROTO(F_ENTRY_NAME, packed_result, packed_x, packed_y)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_POW(
        PASS_ARG_X_FLOAT(packed_x),
        PASS_ARG_X_FLOAT(packed_y),
        ANSI_C_POW_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_POW_E_NAME

X_XX_PROTO(F_ENTRY_NAME, packed_result, packed_x, packed_y)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_POW(
        PASS_ARG_X_FLOAT(packed_x),
        PASS_ARG_X_FLOAT(packed_y),
        FORTRAN_POW_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }


/*
** UX_EXP2 is the internal routine that evaluates exp2 = 2^x.
**
** First, screen for certain overflow and underflow (expon > F_EXP_WIDTH + 3)
** and for tiny x's where 2^x ~ 1 (exponent < -F_PRECISION).  Then, isolate
** any integer part I of x (exponent will be > 0), convert it to UX and 
** subtract from x.  Use the rational approx of 2^f from the pow code.
** Finally, add (signed) I to the exponent of the result.
** 
*/


#if !defined(UX_EXP2)
#   define UX_EXP2           __INTERNAL_NAME(ux_exp2__)
#endif

static void
UX_EXP2( UX_FLOAT * x, UX_FLOAT * result)
    {
    WORD cnt;
    UX_SIGN_TYPE sign;
    UX_EXPONENT_TYPE exponent, scale;
    UX_FRACTION_DIGIT_TYPE  tmp_digit, I, inc;
    UX_FLOAT w,  h;

    exponent = G_UX_EXPONENT(x);
    sign = G_UX_SIGN(x);
    cnt = 0;

    if ( (UX_UNSIGNED_EXPONENT_TYPE) (exponent + 114) > (18 + 114))
      {   
        if ( exponent  > 0) 
        {      /* exponent > 18 , definite overflow or underflow */
           UX_COPY(x, result);
           scale = sign ? UX_UNDERFLOW_EXPONENT : UX_OVERFLOW_EXPONENT;
           P_UX_EXPONENT(result, scale);
	 }
        else 
        {    /* x is close to 0, just return 1 */
           UX_COPY( UX_ONE, result);          
        }
        return;           
        }
   
    I = 0;

    if (exponent >= 0) 
     {
        I = G_UX_MSD(x);
        cnt = BITS_PER_UX_FRACTION_DIGIT_TYPE - exponent;
        inc = SET_BIT(cnt - 1);
        I >>= (cnt - 1);
        I <<= (cnt - 1);

        tmp_digit = I + inc;
        inc += inc; 
        if (tmp_digit >= I)
            I = tmp_digit & -inc;
        else
            { /* A carry out occurred on the increment */
            cnt--;
            I = UX_MSB;
            exponent++;
            }
        UX_SET_SIGN_EXP_MSD(&w, sign, exponent, I);
        ADDSUB(x, &w, SUB, &h);
      }
    else
      {
        UX_COPY(x, &h);
      }

    EVALUATE_RATIONAL(
        &h,
        POW2_COEF_ARRAY,
        POW2_COEF_ARRAY_DEGREE,
        NUMERATOR_FLAGS(STANDARD), 
        result);

    I >>= cnt;
    tmp_digit = -I;
    I = sign ? tmp_digit : I;
    UX_INCR_EXPONENT(result, I);
    return;
   }



/*
** F_EXP2_NAME is the user level packed x-float routine
*/

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME    F_EXP2_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    WORD   fp_class;
    UX_FLOAT unpacked_argument, unpacked_result;
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    fp_class  = UNPACK(
        PASS_ARG_X_FLOAT(packed_argument),
        & unpacked_argument,
        EXP2_CLASS_TO_ACTION_MAP,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    if (0 > fp_class)
       RETURN_X_FLOAT(packed_result);

    UX_EXP2( &unpacked_argument, &unpacked_result);

    PACK(
        &unpacked_result,
        PASS_RET_X_FLOAT(packed_result),
        EXP2_UNDERFLOW,
        EXP2_OVERFLOW
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }




#if defined(MAKE_INCLUDE)

    @divert -append divertText

    precision = ceil(UX_PRECISION/8) + 4;

    START_TABLE;

    /*
    ** As previously noted, not all of the error cases can be encoded in the
    ** class to action mapping.  For these cases, we return unpacked results
    ** and left C_UX_POW figure out what to do.
    */

    TABLE_COMMENT("ansi-c class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ANSI_C_POW_CLASS_TO_ACTION_MAP");

	/* Index 0: for x, just unpack and get fp_class  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) + 0);

	/* Index 1: Class to index map */

    PRINT_64_TBL_ITEM( 
          CLASS_TO_INDEX( F_C_SIG_NAN,      2) +
          CLASS_TO_INDEX( F_C_QUIET_NAN,    3) +
          CLASS_TO_INDEX( F_C_POS_INF,      4) +
          CLASS_TO_INDEX( F_C_NEG_INF,      5) +
          CLASS_TO_INDEX( F_C_POS_NORM,     6) +
          CLASS_TO_INDEX( F_C_NEG_NORM,     7) +
          CLASS_TO_INDEX( F_C_POS_DENORM,   8) +
          CLASS_TO_INDEX( F_C_NEG_DENORM,   9) +
          CLASS_TO_INDEX( F_C_POS_ZERO,    10) +
          CLASS_TO_INDEX( F_C_NEG_ZERO ,   11) );

	/* Index 2: mapping for y when x is SNaN  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(10) +
	  CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
          CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_QUIET_NAN, 0) +
          CLASS_TO_ACTION( F_C_POS_INF,    RETURN_QUIET_NAN, 0) +
          CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_QUIET_NAN, 0) +
          CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_QUIET_NAN, 0) +
          CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_QUIET_NAN, 0) +
          CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_QUIET_NAN, 0) +
          CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_QUIET_NAN, 0) +
          CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     3) );

	/* Index 3: mapping for y when x is QNaN */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(9) +
	  CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_VALUE,     0) +
          CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
          CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
          CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     0) +
          CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     0) +
          CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_VALUE,     0) +
          CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
          CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
          CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     3) );

	/* Index 4: mapping for y when x is +Inf  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(8) +
	  CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
          CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
          CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     4) +
          CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     4) +
          CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     4) +
          CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     3) );

	/* Index 5: mapping for y when x is -Inf  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(7) +
          CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
          CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
          CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     3) );

	/* Index 6: mapping for y when x is +Norm  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(6) +
          CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
          CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
          CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     3) );

	/* Index 7: mapping for y when x is -Norm  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(5) +
          CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
          CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
          CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     3) );

	/* Index 8: mapping for y when x is +Denorm  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(4) +
          CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
          CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
          CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     4) +
          CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     3) );

	/* Index 9: mapping for y when x is -Denorm  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) +
          CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
          CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
          CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     3) );

	/* Index 10: mapping for y when x is +Zero  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) +
         CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
         CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
         CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     2) +
         CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     4) +
         CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     2) +
         CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ERROR,     6) +
         CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     2) +
         CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     6) +
         CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     3) +
         CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     3) );

	/* Index 11: mapping for y when x is -Zero  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
         CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
         CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
         CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     2) +
         CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     6) +
         CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ERROR,     5) +
         CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     2) +
         CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     6) +
         CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     3) +
         CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     3) );

         TABLE_COMMENT("data for the above mapping");
	     PRINT_U_TBL_ITEM( /* data 1 */              NULL );
	     PRINT_U_TBL_ITEM( /* data 2 */              ZERO );
	     PRINT_U_TBL_ITEM( /* data 3 */               ONE );
	     PRINT_U_TBL_ITEM( /* data 4 */               INF );
	     PRINT_U_TBL_ITEM( /* data 5 */    POWER_NEG_BASE );
	     PRINT_U_TBL_ITEM( /* data 6 */ POWER_ZERO_TO_NEG );


    TABLE_COMMENT("fortran class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "FORTRAN_POW_CLASS_TO_ACTION_MAP");

	/* Index 0: for x */

    PRINT_64_TBL_ITEM( 
          CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
	  CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) );

	/* Index 1: Index map for unpacking y */

    PRINT_64_TBL_ITEM( CLASS_TO_INDEX( F_C_SIG_NAN,     0) +
	  CLASS_TO_INDEX( F_C_QUIET_NAN,   0) +
	  CLASS_TO_INDEX( F_C_POS_INF,     3) +
	  CLASS_TO_INDEX( F_C_NEG_INF,     2) +
	  CLASS_TO_INDEX( F_C_POS_NORM,    4) +
	  CLASS_TO_INDEX( F_C_NEG_NORM,    2) +
	  CLASS_TO_INDEX( F_C_POS_DENORM,  5) +
	  CLASS_TO_INDEX( F_C_NEG_DENORM,  2) +
	  CLASS_TO_INDEX( F_C_POS_ZERO,    6) +
	  CLASS_TO_INDEX( F_C_NEG_ZERO ,   7) );

	/* Index 2: mapping for y when x is negative and not zero  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(6) +
          CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
          CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
          CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     5) );

	/* Index 3: mapping for y when x is +Inf  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(5) +
          CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
          CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
          CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     4) +
          CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     4) +
          CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     4) +
          CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     5) );

	/* Index 4: mapping for y when x is +Norm  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(4) +
          CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
          CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
          CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     3) );

	/* Index 5: mapping for y when x is +Denorm  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) +
          CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
          CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
          CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     4) +
          CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     3) +
          CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     3) );

	/* Index 6: mapping for y when x is +Zero  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) +
          CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
          CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
          CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     4) +
          CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ERROR,     6) +
          CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     6) +
          CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     5) );

	/* Index 7: mapping for y when x is -Zero  */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
          CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
          CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
          CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_NEGATIVE,  2) +
          CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     2) +
          CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     5) +
          CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_ERROR,     5) );

    TABLE_COMMENT("data for the above mapping");
	PRINT_U_TBL_ITEM( /* Data  1 */              NULL );
	PRINT_U_TBL_ITEM( /* Data  2 */              ZERO );
	PRINT_U_TBL_ITEM( /* Data  3 */               ONE );
	PRINT_U_TBL_ITEM( /* Data  4 */               INF );
	PRINT_U_TBL_ITEM( /* Data  5 */    POWER_NEG_BASE );
	PRINT_U_TBL_ITEM( /* Data  6 */ POWER_ZERO_TO_NEG );



    TABLE_COMMENT("exp2 class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "EXP2_CLASS_TO_ACTION_MAP\t");
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
              CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     3) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     1) );


    TABLE_COMMENT("Data for the class to action mappings");
    PRINT_U_TBL_ITEM( /* data 1 */ ONE  );
    PRINT_U_TBL_ITEM( /* data 2 */ EXP2_OF_NEG_INF );
    PRINT_U_TBL_ITEM( /* data 3 */ EXP2_OF_INF );




    TABLE_COMMENT("high word of sqrt(2) and ln2");
        tmp = trunc(bldexp(1/sqrt(2), BITS_PER_UX_FRACTION_DIGIT_TYPE));
        PRINT_UX_FRACTION_DIGIT_TBL_VDEF_ITEM( "ONE_OVER_SQRT_2\t\t", tmp);
        log2_hi = trunc(bldexp(log(2), BITS_PER_UX_FRACTION_DIGIT_TYPE));
        PRINT_UX_FRACTION_DIGIT_TBL_VDEF_ITEM( "MSD_OF_LN2\t\t", log2_hi);

    TABLE_COMMENT("1, 1/ln2 and log2_lo/ln2 in unpacked format");

        PRINT_UX_TBL_ADEF_ITEM( "UX_ONE\t\t\t", 1);
        tmp = 2/log(2);
        PRINT_UX_TBL_ADEF_ITEM( "UX_TWO_OVER_LN2\t\t", tmp);

        save_precision = precision;
        precision = ceil(3*UX_PRECISION/16) + 8;
        tmp = log(2);
        tmp = (tmp - bldexp(log2_hi, -BITS_PER_UX_FRACTION_DIGIT_TYPE))/tmp;
        PRINT_UX_TBL_ADEF_ITEM( "UX_LN2_LO_OVER_LN2\t", tmp);
        precision = save_precision;

    ln2 = log(2);
    scale      = ln2/2;
    zero_value = scale*scale/3;
    inv_scale  = 1/scale;
    function __log2(x)
        {
        if (x == 0)
            return zero_value;
        else
            return (inv_scale*atanh(x*scale) - x)/(x*x*x);
        }

    max_arg = 2*(sqrt(2) - 1)^2/log(2);

    save_precision = precision;
    precision = precision = ceil(UX_PRECISION/8) + 8;

    TABLE_COMMENT("Fixed point coefficients for log2 evaluation");
    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG,
        0, max_arg, __log2, UX_PRECISION, &degree, &ux_rational_coefs);

    precision = save_precision;

    PRINT_FIXED_128_TBL_ADEF("POW_LOG2_COEF_ARRAY\t");
    PRINT_WORD_DEF("POW_LOG2_COEF_ARRAY_DEGREE", degree);
    print_ux_rational_coefs(degree, 0, 0);


    scale      = log(2);
    function __2_pow(x)
        {
        if (x == 0)
            return 1;
        else
            return exp(x*scale);
        }

    max_arg = .5;

    save_precision = precision;
    precision = precision = ceil(UX_PRECISION/8) + 8;

    TABLE_COMMENT("Fixed point coefficients for 2^h evaluation");
    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT + REMES_LINEAR_ARG,
        -max_arg, max_arg, __2_pow, UX_PRECISION, &degree, &ux_rational_coefs);

    precision = save_precision;

    PRINT_FIXED_128_TBL_ADEF("POW2_COEF_ARRAY\t\t");
    PRINT_WORD_DEF("POW2_COEF_ARRAY_DEGREE\t", degree);
    print_ux_rational_coefs(degree, 0, 0);

    END_TABLE;

    @end_divert
    @eval my $tableText;						\
          my $outText    = MphocEval( GetStream( "divertText" ) );	\
          my $defineText = Egrep( "\\#define", $outText, \$tableText );	\
             $outText    = "$tableText\n\n$defineText";			\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),		\
                           "Definitions and constants for floating " .	\
                              "point power routines", __FILE__ );	\
             print "$headerText\n$outText\n";

#endif

