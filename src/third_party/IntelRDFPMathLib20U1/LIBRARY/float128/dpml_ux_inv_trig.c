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

#include <stdio.h>
/* File: dpml_ux_inv_trig.c */
/*
**
**
**
**  Facility:
**
**	DPML
**
**  Abstract:
**
**	This file contains the code for computing the radian and degree
**	inverse trig functions of an unpacked x-float value.  In addition,
**	it contains the user interface code for the pack x-float inverse
**	trig functions and the mphoc code for building the class to action
**	table.
**
**  Modification History:
**
**	1-001	Original version.  RNH 21-Sep-95
**	1-002	atan(2q0) bug fix. GWK 20-Nov-98
**      1-003   Fixed problem with quotient estimation in atan2 when the
**              high digits of y and x are equal. RNH 19-Apr-02
**      1-004   Added special intel specific switch in class to
**              action map. Added class to action map for atan2 and atan2d
**              when y is -0. SBN 22-Apr-2002.    
**      1-005   Modified unpacked_result to array of 2 in C_UX_ATAN2.
**              SBN 24-Apr-2002. 
**      1-006   Added interface macros. SBN 29-Apr-2002.
**      1-007   Changed type of diff from unsigned to signed in quotient
**              estimation. SBN 30-Apr-2002. 
**      1-008   Modified interface macros. SBN 15-May-2002.
**
** Build Info:
**
**	Preprocess this file with MAKE_INCLUDE defined to produce a .h
**	file containing the class-to-action map and appropriate constants
**	for the inverse trig functions.  Then compile this file with *NO*
**	defines to get the code for inverse trig functions.
*/

#define	BASE_NAME	inv_trig
#include "dpml_ux.h"

#if !defined(MAKE_INCLUDE)
#   include STR(BUILD_FILE_NAME)
#endif


/* 
** 1. BASIC DESIGN/ALGORITHMS
** --------------------------
** 
** The basic design of for the inverse trig functions relies on two evaluation
** routines, one for the atan family of functions and one for the asin/acos
** family.  Within each family, the differences between the degree and radian
** versions are handled by multiplying the radian result by 180/pi to get
** the degree result before rounding back to X_FLOAT precision.  It is possible
** to account for the radian/degree differences by using different sets of
** constants.  In order to discuss some of the design issues independent of the
** choice mechanism for dealing with the radian/degree differences, we will use
** the symbolic name CYCLE to refer to 180 or pi.
** 
** 
** 1.1 ATAN
** --------
** 
** We note that the atan(x) = atan2(x,1), so we will confine most of the
** discussion to the atan2 case.  The basic algorithm makes use of the
** following identities:
** 
** 		atan2(-y,x) = - atan2(y,x)			(1)
** 		atan2(y,-x) = CYCLE - atan(y,x)			(2)
** 		atan2(y, x) = atan(y/x)	x,y >= 0		(3)
** 		atan(z) = atan(a) + atan[(z - a)/(1 + a*z)	(4)
** 		atan(1/z) = CYCLE/2 - atan(z)			(5)
** 
** Items (1) through (3) imply that for the most part we need only deal with
** |y/x|.  In particular, based on the above we make the following definitions
** of i, C(i), S(i) and z(i), according to the size of |y/x|:
** 
** 	Size of |y/x|	 i	  c(i)	  s(i)        z(i)
** 	-------------	---	--------- ---- ------------------
** 	[0, 1/2)	 0	    0	    1         |y/x|
** 	[1/2, 2]	 1	 CYCLE/4    1  (|y|-|x|)/(|y|+|x|)
** 	(2, Inf)	 2	 CYCLE/2   -1         |x/y|
** 
** From which it follows that:
** 
** 		atan2(|y|,|x|) = c(i) + s(i)*atan(z(i))
** 
** where 0 <= z(i) < 1/2.  Using (2) we can extend the above table for negative
** x as
** 	Size of |y/x|	 i	   c(i)	  s(i)        z(i)
** 	--------------	---	--------- ---- ------------------
** 	[0, 1/2) x < 0	 3	CYCLE	   -1         |y/x|
** 	[1/2, 2] x < 0	 4	3*CYCLE/4  -1  (|y|-|x|)/(|y|+|x|)
** 	(2, Inf) x < 0	 5	CYCLE/2	    1         |x/y|
** 
** Finally, using (1) we have
** 
** 		atan2(y,x) = sign(y)*[c(i) + s(i)*atan(z(i))]
** 
** Based on the above, the general approach to evaluating atan2(y,x) is: 
** 
** 	(a) compute the exponent value, n, of y/x
** 	(b) Based on n and the sign of x, compute the index i, and the
** 	    value z(i).
** 	(c) compute atan(z(i)) using a rational approximation (see section
** 	    2)
** 	(d) based on i, compute c(i) + atan(z(i)) or c(i) - atan(z(i))
** 	(e) copy the sign of y onto the last result.
** 
** At this point we would like to discuss step (d) in more detail.  We note
** the following:
** 
** 		o c(i) = (i/4)*CYCLE      for i = 0, 1, 2
** 		o c(i+3) = (4-i)/4]*CYCLE for i = 0, 1, 2
** 		o s(i+3) = -s(i)          for i = 0, 1, 2
** 
** This implies that during the screening to determine the interval, we can
** determine c(i) and s(i) for i = 0, 1, or 2 and then adjust c(i) and s(i)
** to reflect the sign of x.  
** 
** 
** EVALUATE_RATIONAL depends on the reduced argument x satisfying
** |x| < 1 , and the coefficients decreasing.  If the coefficients
** don't decrease, shifting the exponent of the reduced argument 
** (effectively multiplying by 2, 4, or more) and pulling this factor
** out of the coefficients can then allow them to decrease.
** For atan, the reduced argument has its exponent shifted by 1,
** which effectively mutliplies it by 2.  If the argument is
** exactly 1/2, the shift makes it 1, and EVALUATE_RATIONAL won't work.
** So, we want to avoid a reduced argument of 1/2 for atan. 
**
** In order to call the polynomial evaluation routine with
** a reduced argument strictly less than 1/2 we check the
** value of the reduced argument after |y/x| is calculated.
**
** But rather than calculate |y/x|, its value is estimated by 
** calculating its exponentt.  The value of this exponent
** determines which of |y/x|, (|y|-|x|)/(|y|+|x|), or |x/y| is
** actually calculated and used as the reduced argument.  
** When the exponent is >1, the value is >= 2, and |x/y| is
** calculated as the reduced argument.  But if |y/x| is
** exactly = 2, |x/y| = 1/2, which should not be sent to the polynomial
** evaluation routine.
** So, the un-normalized exponent is checked, and decremented 
** if the most significant bit of the fraction field is 0.
** If the exponent is still >= 0, the initial reduced_argument = 1/2,
** so we want to use (|y|-|x|)/(|y|+|x|) = 1/3 instead
** To make this so:
**   (1) decrement the index
**   (2) un-toggle the sign bit
**   (3) change the reduced argument to 1/3 (via a table entry)
** 
** 
** 
** 2. ATAN/ATAN2 EVALUATION
** ------------------------
** 
** The atan family of functions call a common routine to unpack their arguments
** and invoke the evaluation routine UX_ATAN2.  For atan and atand, the 'x'
** argument passed to UX_ATAN2 is a null pointer.  UX_ATAN2 uses the null
** pointer to distinguish between an atan evaluation and an atan2 evaluation.
** Also, the null pointer is passed onto the divide routine, where it is
** implicitly treated as a pointer to the value 1.  In this way, very little
** special casing is required for atan cases being processed by UX_ATAN2.
*/ 

#if !defined(UX_ATAN2)
#   define UX_ATAN2	__INTERNAL_NAME(ux_atan2__)
#endif

#define DEGREE_EVALUATION	((WORD) 1 << (BITS_PER_WORD - 1))
#define RADIAN_EVALUATION	0

#define ATAN_MAP_WIDTH		4
#define ATAN_MAP_FIELD(i,c)	((c) << ((i)*ATAN_MAP_WIDTH))
#define INV_TRIG_CONS(index)	\
		(UX_FLOAT *)((char *) INV_TRIG_CONS_BASE + (index))



void
UX_ATAN2(
  UX_FLOAT * unpacked_y,
  UX_FLOAT * unpacked_x,
  WORD       degree_radian_flag,
  UX_FLOAT * unpacked_result)
    {
    UX_FLOAT         tmp[2], red_arg, *aux_x, *tmp_ptr;
    WORD             index;
    UX_SIGN_TYPE     sign, sign_y;
    UX_EXPONENT_TYPE quotient_exp;
    UX_SIGNED_FRACTION_DIGIT_TYPE diff;

    /* Determine (estimate ?) the exponent of y/x */

    if (0 == unpacked_x)
        { /* This is a atan, rather than atan2 function */
        quotient_exp = G_UX_EXPONENT(unpacked_y);
        aux_x = UX_ONE;
        sign = 0;
        }
    else
        {
        quotient_exp = G_UX_EXPONENT(unpacked_y) - G_UX_EXPONENT(unpacked_x);
        aux_x = unpacked_x;
        sign = G_UX_SIGN(unpacked_x);
        P_UX_SIGN(unpacked_x, 0);
        diff = G_UX_MSD(unpacked_y) - G_UX_MSD(unpacked_x);
        if ( quotient_exp >= 0 )
        quotient_exp -= (diff == 0 && quotient_exp > 0 );
        quotient_exp += (diff >= 0);
        }

    /* Do argument reduction */

    index = sign ? 3*ATAN_MAP_WIDTH : 0;
    sign_y = G_UX_SIGN(unpacked_y);
    P_UX_SIGN(unpacked_y, 0);

    if (quotient_exp > 1)
        { /* reduced argument is x/y */
        index += 2*ATAN_MAP_WIDTH;
        tmp_ptr = unpacked_x;
        unpacked_x = unpacked_y;
        unpacked_y = tmp_ptr;
        sign ^= UX_SIGN_BIT;
        }
    else if (quotient_exp >= 0)
        { /* reduced argument is (y-x)/(y+x) */
        index += ATAN_MAP_WIDTH;
        ADDSUB(unpacked_y, aux_x,
          ADD_SUB | MAGNITUDE_ONLY | NO_NORMALIZATION, tmp);
        unpacked_y = &tmp[1];
        unpacked_x = &tmp[0];
        NORMALIZE(unpacked_y);
        }

    DIVIDE(unpacked_y, unpacked_x, FULL_PRECISION, &red_arg);

    /* force reduced argument to be less than 1/2 */

    quotient_exp = red_arg.exponent;
    if ( (UX_MSB & red_arg.fraction[0])  == 0) quotient_exp--;
    if ( quotient_exp >= 0 )
        {
        index -= ATAN_MAP_WIDTH;
        sign ^= UX_SIGN_BIT;
        red_arg = *UX_ONE_THIRD;
        }

    /* Evaluate the reduced argument */

    EVALUATE_RATIONAL(
        &red_arg,
        ATAN_COEF_ARRAY,
        ATAN_COEF_ARRAY_DEGREE,
        NUMERATOR_FLAGS(SQUARE_TERM | POST_MULTIPLY) |
          DENOMINATOR_FLAGS(SQUARE_TERM) | P_SCALE(1),
        unpacked_result);
 
    /* Add in the appropriate constant */

    UX_TOGGLE_SIGN(unpacked_result, sign);
    if (index)
        {
        index =
          ((ATAN_MAP_FIELD( 0, UX_ZERO_INDEX )            	+
            ATAN_MAP_FIELD( 1, UX_PI_OVER_4_INDEX )       	+
            ATAN_MAP_FIELD( 2, UX_PI_OVER_2_INDEX )      	+
            ATAN_MAP_FIELD( 3, UX_PI_INDEX )			+
            ATAN_MAP_FIELD( 4, UX_THREE_QUARTERS_PI_INDEX )	+
            ATAN_MAP_FIELD( 5, UX_PI_OVER_2_INDEX ) ) >> index) & 
            MAKE_MASK(ATAN_MAP_WIDTH, 3);

        NORMALIZE(unpacked_result);
        ADDSUB(
            INV_TRIG_CONS(index),
            unpacked_result,
            ADD | NO_NORMALIZATION,
            unpacked_result);
        }

    /* Convert to degrees if necessary */

    if (DEGREE_EVALUATION == degree_radian_flag)
        MULTIPLY( UX_RAD_TO_DEG, unpacked_result, unpacked_result);

    /* Determine final sign and return */

    P_UX_SIGN(unpacked_result, sign_y);
    return;
    }

/*
** C_UX_ATAN2 is the common processing routine for atanl, atan2l, atandl and
** atan2dl.  C_UX_ATAN2 unpacks the input arguments, calls UX_ATAN2 and then
** packs the results
*/

#if !defined(C_UX_ATAN2)
#   define C_UX_ATAN2	__INTERNAL_NAME(C_ux_atan2__)
#endif

static void
C_UX_ATAN2 (
  _X_FLOAT     * packed_y,
  _X_FLOAT     * packed_x,
  WORD           degree_radian_flag,
  U_WORD const * class_to_action_map,
  WORD           underflow_error,
  _X_FLOAT     * packed_result
  OPT_EXCEPTION_INFO_DECLARATION )
    {
    WORD    fp_class;
    UX_FLOAT unpacked_x, unpacked_y, unpacked_result[2];

    fp_class  = UNPACK2(
        packed_y,
        packed_x,
        & unpacked_y,
        & unpacked_x,
        class_to_action_map,
        packed_result
        OPT_EXCEPTION_INFO_ARGUMENT );

    if (0 > fp_class)
        return;

    UX_ATAN2(
        &unpacked_y,
        packed_x ? &unpacked_x : 0,
        degree_radian_flag,
        &unpacked_result[0]);

    PACK(
        &unpacked_result[0],
        packed_result,
        underflow_error,
        NOT_USED
        OPT_EXCEPTION_INFO_ARGUMENT );
    }

/*
** The following routines are the user level interfaces to the packed x-float
** atan family of routines
*/

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_ATAN_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_x)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_ATAN2(
        PASS_ARG_X_FLOAT(packed_x),
	NULL,
        RADIAN_EVALUATION,
        ATAN_CLASS_TO_ACTION_MAP,
        NOT_USED,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }


#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_ATAN2_NAME

X_XX_PROTO(F_ENTRY_NAME, packed_result, packed_y, packed_x)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_ATAN2(
        PASS_ARG_X_FLOAT(packed_y),
    PASS_ARG_X_FLOAT(packed_x),
        RADIAN_EVALUATION,
        ATAN2_CLASS_TO_ACTION_MAP,
        ATAN2_UNDERFLOW,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_ATAND_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_x)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_ATAN2(
        PASS_ARG_X_FLOAT(packed_x),
	NULL,
        DEGREE_EVALUATION,
        ATAND_CLASS_TO_ACTION_MAP,
        NOT_USED,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define F_ENTRY_NAME	F_ATAND2_NAME

X_XX_PROTO(F_ENTRY_NAME, packed_result, packed_y, packed_x)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    C_UX_ATAN2(
        PASS_ARG_X_FLOAT(packed_y),
    PASS_ARG_X_FLOAT(packed_x),
        DEGREE_EVALUATION,
        ATAND2_CLASS_TO_ACTION_MAP,
        ATAND2_UNDERFLOW,
        PASS_RET_X_FLOAT(packed_result)
        OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

/* 
** 3.0 ASIN/ACOS
** -------------
** 
** The overall design for the asin/acos functions is remarkably similar to the
** atan functions.  The asin/acos evaluations are based on the following
** identities:
** 
** 		asin(-x) = -asin(x)				(1)
** 		asin(x)  = CYCLE/2 - 2*asin(sqrt((1-x)/2))	(2)
** 		acos(x)  = CYCLE/2 - asin(x)			(3)
** 
** As for atan, based on the above identities and the size of x, we can define
** quantities i, j, c(i), s(i), t(i) and z(i) such that
** 
** 	asin(x) or acos(x) = s(i)*[ c(i) + t(i)*2^j*asin(z(i))]
** 
** 
** 	Function      x	     i	s(i)    c(i)  t(i)  j     z(i)
** 	-------- ---------- ---	---- -------- ---- --- ------------------
** 	  asin   [-1, -1/2)  3   -1   CYCLE/2  -1   1  sqrt((1-|x|)/2)
** 	         [-1/2, 0)   2   -1      0      1   0       |x|
** 	         [0, 1/2)    0    1      0      1   0       |x|
** 	         [1/2, 1)    1    1   CYCLE/2  -1   1  sqrt((1-|x|)/2)
** 
** 	  acos   [-1, -1/2)  3    1    CYCLE   -1   1  sqrt((1-|x|)/2)
** 	         [-1/2, 0)   2    1   CYCLE/2   1   0       |x|
** 	         [0, 1/2)    0    1   CYCLE/2  -1   0       |x|
** 	         [1/2, 1)    1    1      0      1   1  sqrt((1-|x|)/2)
** 
** With the above in mind, the general approach to evaluating asin or acos is: 
** 
** 	(a) Based on the exponent and sign of x, compute the index i, and the
** 	    values of j and z(i).
** 	(b) compute w = asin(z(i)) using a rational approximation (see section
** 	    2)
** 	(c) increment the exponent of w by j.
** 	(d) based on i, compute s(i)*[c(i) + t(i)*w]
** 
** The algorithm for determining s(i), t(i) and c(i) for asin and acos is more
** complicated, so we resort to a "table look-up" scheme.  That is, We assume
** that there will be a array of _UX_FLOAT constants that contains the values
** CYCLE/4, CYCLE/2, 3*CYCLE/4 and CYCLE.  For each i in step (b), we can
** allocate a 7 bit field within a U_INT_32 value that encodes the index of
** c(i) in the constant table and the values of s(i) and t(i).  This allocation
** can be done at compile time, so that at run time, step (d) consists of
** accessing the appropriate 7 bit field and extracting s(i), t(i) and the
** index for c(i).  We assume that the 7 bit fields are allocated as
** 
** 		        7          2  1  0
** 			 +----------+--+--+
** 			 |  index   | s| t|
** 			 +----------+--+--+
** 
** with the first field starting at bit 4.  We further assume that the bit 0 is
** one for a degree evaluation and 0 otherwise.
** 
** 
** 4. ATAN AND ASIN EVALUATION
** ---------------------------
** 
** Both atan and asin are more efficiently evaluated using rational
** approximations than polynomial evaluations.  Extrapolating from the tables
** in Hart and the current x-float asin polynomial, the number of terms in a
** polynomial and a rational approximations are:
** 
** 		Function	polynomial	rational
** 		--------	----------	--------
** 		  asin		    32		 (10,10)
** 		  atan		    30		 (10,10)
** 
** 5. ASIN/ACOS EVALUATION
** -----------------------
** 
** Since asin and acos do not require unpacked interfaces, the user level
** routines do not unpack their arguments.  Instead they simply pass them on to
** the general asin/acos evaluation routine, UX_ASIN_ACOS.  The interface
** to UX_ASIN_ACOS is:
** 
** 	static void
** 	UX_ASIN_ACOS(
** 	    _X_FLOAT     * packed_argument,
** 	    WORD           index_map,
**          WORD           invalid_error,
**          U_WORD const * class_to_action_map,
** 	    _X_FLOAT     * packed_result);
** 
** where: 'index_map' is the 32 bit data item used to encode the c(i)'s, s(i)'s
** and t(i)'s defined in section 1; 'invalid_error' is the error code for the
** indicated error and 'class_to_action_array' is the mapping array for the
** given function.
*/ 


#define BIT_FROM_MAP(m,i)	(((m) << (BITS_PER_WORD - (i))) & UX_SIGN_BIT)

#define ASIN_MAP_WIDTH	6
#define ASIN_MAP_FIELD(i,c,t,s)	(((c) + 8*(t) + 4*(s)) << ((i)*ASIN_MAP_WIDTH))
#define G_MAP_INFO(m,i)		((m) >> (i))
#define G_S_FROM_ASIN_MAP(m)	((m & 4) ? UX_SIGN_BIT : 0 )
#define G_T_FROM_ASIN_MAP(m)	((m & 8) ? UX_SIGN_BIT : 0 )
#define G_C_FROM_ASIN_MAP(m)	INV_TRIG_CONS((m) & 0xf0)

#if !defined(UX_ASIN_ACOS)
#   define UX_ASIN_ACOS		__INTERNAL_NAME(ux_asin_acos__)
#endif

static void
UX_ASIN_ACOS(
  _X_FLOAT     * packed_argument,
  WORD           index_map,
  WORD           invalid_error,
  U_WORD const * class_to_action_map,
  _X_FLOAT     * packed_result
  OPT_EXCEPTION_INFO_DECLARATION )
    {
    WORD             fp_class, index, map;
    UX_FLOAT         * unpacked_argument, * unpacked_result, tmp[3];
    UX_SIGN_TYPE     sign;
    UX_EXPONENT_TYPE exponent, exponent_inc;

    unpacked_argument = &tmp[0];
    unpacked_result   = &tmp[1];

    fp_class  = UNPACK(
        packed_argument,
        unpacked_argument,
        class_to_action_map,
        packed_result
        OPT_EXCEPTION_INFO_ARGUMENT );

    if (0 > fp_class)
        return;

    /*
    ** Determine the index based on size of x and sign(x).  Also
    ** screen out arguments |x| > 1.
    */

    exponent = G_UX_EXPONENT(unpacked_argument);
    exponent_inc = 0;
    index = G_UX_SIGN( unpacked_argument) ? 2*ASIN_MAP_WIDTH : 0;
    P_UX_SIGN( unpacked_argument, 0);

    if (exponent >= 0)
        { /* |x| >= 1/2 */

        index += ASIN_MAP_WIDTH;
        if (exponent < 1)
            { /* 1/2 <= |argument| < 1, compute sqrt((1-x)/2) */
            exponent_inc = 1;
            ADDSUB( UX_ONE, unpacked_argument, SUB | MAGNITUDE_ONLY,
              unpacked_argument);
            UX_DECR_EXPONENT( unpacked_argument, 1);
            UX_SQRT( unpacked_argument, unpacked_argument);
            }

        /* separate |x| = 1 from |x| > 1 */

        else if ((exponent == 1) && UX_FRACTION_IS_ONE_HALF(unpacked_argument))

            /* |x| = 1, make "reduced argument" zero */
            UX_COPY(UX_ZERO, unpacked_argument);

        else
            { /* Force "overflow" to signal error */
            UX_SET_SIGN_EXP_MSD(unpacked_result, 0,
              UX_OVERFLOW_EXPONENT, UX_MSB);
            goto pack_it;
            }
        }

    EVALUATE_RATIONAL(
        unpacked_argument,
        ASIN_COEF_ARRAY,
        ASIN_COEF_ARRAY_DEGREE,
        NUMERATOR_FLAGS(SQUARE_TERM | POST_MULTIPLY | ALTERNATE_SIGN) |
          DENOMINATOR_FLAGS(SQUARE_TERM | ALTERNATE_SIGN) |
          P_SCALE(1),
        unpacked_result);

     /*
     ** Set sign for polynomial evaluation and scale by 2 if needed
     */

     index = G_MAP_INFO(index_map, index);
     P_UX_SIGN( unpacked_result, G_T_FROM_ASIN_MAP(index));
     UX_INCR_EXPONENT( unpacked_result, exponent_inc);

     /* Add in c(i) */

     ADDSUB( G_C_FROM_ASIN_MAP(index), unpacked_result,
       ADD | NO_NORMALIZATION, unpacked_result);

     /* Set sign of result and convert to degrees */

     P_UX_SIGN( unpacked_result, G_S_FROM_ASIN_MAP(index) );
     if (index_map & DEGREE_EVALUATION)
         MULTIPLY( unpacked_result, UX_RAD_TO_DEG, unpacked_result);

pack_it:
    PACK(
        unpacked_result,
        packed_result,
        NOT_USED,
        invalid_error
        OPT_EXCEPTION_INFO_ARGUMENT );
    }


/*
** The following routines are the user level interfaces to the packed
** asin, acos, asind and acosd routines.
*/

#undef  F_ENTRY_NAME
#define	F_ENTRY_NAME	F_ASIN_NAME

         		             /* Interval   Constant Index    t  s */
#define ASIN_INTERVAL_MAP ( ASIN_MAP_FIELD( 3,   UX_PI_OVER_2_INDEX, 1, 1) + \
			    ASIN_MAP_FIELD( 2,     UX_ZERO_INDEX,    0, 1) + \
			    ASIN_MAP_FIELD( 0,     UX_ZERO_INDEX,    0, 0) + \
			    ASIN_MAP_FIELD( 1,   UX_PI_OVER_2_INDEX, 1, 0) )

				     /* Interval    Constant Index    t  s */
#define ACOS_INTERVAL_MAP ( ASIN_MAP_FIELD( 3,        UX_PI_INDEX,    1, 0) + \
			    ASIN_MAP_FIELD( 2,    UX_PI_OVER_2_INDEX, 0, 0) + \
			    ASIN_MAP_FIELD( 0,    UX_PI_OVER_2_INDEX, 1, 0) + \
			    ASIN_MAP_FIELD( 1,       UX_ZERO_INDEX,   0, 0) )

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    UX_ASIN_ACOS(
         PASS_ARG_X_FLOAT(packed_argument),
         ASIN_INTERVAL_MAP + RADIAN_EVALUATION,
         ASIN_ARG_GT_ONE,
         ASIN_CLASS_TO_ACTION_MAP,
         PASS_RET_X_FLOAT(packed_result)
         OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define	F_ENTRY_NAME	F_ASIND_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    UX_ASIN_ACOS(
         PASS_ARG_X_FLOAT(packed_argument),
         ASIN_INTERVAL_MAP + DEGREE_EVALUATION,
         ASIN_ARG_GT_ONE,
         ASIND_CLASS_TO_ACTION_MAP,
         PASS_RET_X_FLOAT(packed_result)
         OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define	F_ENTRY_NAME	F_ACOS_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    UX_ASIN_ACOS(
         PASS_ARG_X_FLOAT(packed_argument),
         ACOS_INTERVAL_MAP + RADIAN_EVALUATION,
         ASIN_ARG_GT_ONE,
         ACOS_CLASS_TO_ACTION_MAP,
         PASS_RET_X_FLOAT(packed_result)
         OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }

#undef  F_ENTRY_NAME
#define	F_ENTRY_NAME	F_ACOSD_NAME

X_X_PROTO(F_ENTRY_NAME, packed_result, packed_argument)
    {
    EXCEPTION_INFO_DECL
    DECLARE_X_FLOAT(packed_result)

    INIT_EXCEPTION_INFO;
    UX_ASIN_ACOS(
         PASS_ARG_X_FLOAT(packed_argument),
         ACOS_INTERVAL_MAP + DEGREE_EVALUATION,
         ACOSD_ARG_GT_ONE,
         ACOSD_CLASS_TO_ACTION_MAP,
         PASS_RET_X_FLOAT(packed_result)
         OPT_EXCEPTION_INFO );

    RETURN_X_FLOAT(packed_result);

    }


/*
** MPHOC code for generatings the class-to-action mappings, rational
** coefficients and miscellaneous constants.
*/

#if defined(MAKE_INCLUDE)

    @divert -append divertText

    precision = ceil(UX_PRECISION/8) + 4;

#   undef  TABLE_NAME

    START_TABLE;

    TABLE_COMMENT("asin class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ASIN_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM(  CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );
        PRINT_U_TBL_ITEM( /* data 1 */ ASIN_ARG_GT_ONE );


    TABLE_COMMENT("acos class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ACOS_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM(  CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     2) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     2) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     2) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     2) );
        PRINT_U_TBL_ITEM( /* data 1 */ ACOS_ARG_GT_ONE );
        PRINT_U_TBL_ITEM( /* data 2 */ PI_OVER_2 );

    TABLE_COMMENT("asind class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ASIND_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM(  CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );
        PRINT_U_TBL_ITEM( /* data 1 */ ASIND_ARG_GT_ONE );


    TABLE_COMMENT("acosd class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ACOSD_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM(  CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     2) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     2) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     2) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     2) );
        PRINT_U_TBL_ITEM( /* data 1 */ ACOSD_ARG_GT_ONE );
        PRINT_U_TBL_ITEM( /* data 2 */ NINETY );


    TABLE_COMMENT("atan class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ATAN_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM(  CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  1) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );
        PRINT_U_TBL_ITEM( /* data 1 */ PI_OVER_2 );

    TABLE_COMMENT("atand class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ATAND_CLASS_TO_ACTION_MAP");
    PRINT_64_TBL_ITEM(  CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO ,  RETURN_VALUE,     0) );
        PRINT_U_TBL_ITEM( /* data 1 */ NINETY );


    TABLE_COMMENT("atan2(y,x) class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ATAN2_CLASS_TO_ACTION_MAP");

	  /* Index 0: class-to-action for y */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(8) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) );

	   /* Index 1: class-to-index mapping */

    PRINT_64_TBL_ITEM( 
	       CLASS_TO_INDEX( F_C_POS_INF,    2) +
	       CLASS_TO_INDEX( F_C_NEG_INF,    2) +
	       CLASS_TO_INDEX( F_C_POS_NORM,   3) +
	       CLASS_TO_INDEX( F_C_NEG_NORM,   3) +
	       CLASS_TO_INDEX( F_C_POS_DENORM, 3) +
	       CLASS_TO_INDEX( F_C_NEG_DENORM, 3) +
	       CLASS_TO_INDEX( F_C_POS_ZERO,   4) +
	       CLASS_TO_INDEX( F_C_NEG_ZERO,   5) );

          /* index 2: mapping for x when y is +/- Inf */

#if defined(INTEL_CLASS_ACTION)
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(4) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN,   1) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,       1) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_CPYSN_ARG_0, 2) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_CPYSN_ARG_0, 3) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_CPYSN_ARG_0, 4) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_CPYSN_ARG_0, 4) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_CPYSN_ARG_0, 4) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_CPYSN_ARG_0, 4) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_CPYSN_ARG_0, 4) +
              CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_CPYSN_ARG_0, 4) );
#else
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(4) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN,   1) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,       1) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,       2) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,       2) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_CPYSN_ARG_0, 4) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_CPYSN_ARG_0, 4) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_CPYSN_ARG_0, 4) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_CPYSN_ARG_0, 4) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_CPYSN_ARG_0, 4) +
              CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_CPYSN_ARG_0, 4) );
#endif

          /* index 3: mapping for x when y is +/-Norm or +/-Denorm */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN,   1) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,       1) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_CPYSN_ARG_0, 5) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_CPYSN_ARG_0, 6) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,    1) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,    1) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,    1) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,    1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_CPYSN_ARG_0, 4) +
              CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_CPYSN_ARG_0, 4) );

#if defined(INTEL_CLASS_ACTION)
          /* index 4: mapping for x when y is +Zero */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN,   1) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,       1) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,       0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_CPYSN_ARG_0, 6) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,       0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_CPYSN_ARG_0, 4) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,       0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_CPYSN_ARG_0, 6) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,       1) +
              CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_VALUE,       6) );


          /* index 5: mapping for x when y is -Zero */
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN,   1) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,       1) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,       0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_CPYSN_ARG_0, 6) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,       0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_CPYSN_ARG_0, 6) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,       0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_CPYSN_ARG_0, 6) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,       0) +
              CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_CPYSN_ARG_0, 6) );
#else
          /* index 4: mapping for x when y is +Zero */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN,   1) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,       1) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,       0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_CPYSN_ARG_0, 6) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,       0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_CPYSN_ARG_0, 4) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,       0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_CPYSN_ARG_0, 6) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,       3) +
              CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_ERROR,       3) );

          /* index 5: mapping for x when y is -Zero */
    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN,   1) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,       1) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,       0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_CPYSN_ARG_0, 6) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,       0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_CPYSN_ARG_0, 6) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,       0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_CPYSN_ARG_0, 6) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,       3) +
              CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_ERROR,       3) );
#endif
	PRINT_U_TBL_ITEM( /* data 1 */            NULL );
#if defined(INTEL_CLASS_ACTION)
	PRINT_U_TBL_ITEM( /* data 2 */       PI_OVER_4 );
	PRINT_U_TBL_ITEM( /* data 3 */ THREE_PI_OVER_4 );
#else
	PRINT_U_TBL_ITEM( /* data 2 */  ATAN2_BOTH_INF );
	PRINT_U_TBL_ITEM( /* data 3 */ ATAN2_BOTH_ZERO );
#endif
	PRINT_U_TBL_ITEM( /* data 4 */       PI_OVER_2 );
	PRINT_U_TBL_ITEM( /* data 5 */            ZERO );
	PRINT_U_TBL_ITEM( /* data 6 */              PI );


    TABLE_COMMENT("atan2d(y,x) class-to-action-mapping");
    PRINT_CLASS_TO_ACTION_TBL_DEF( "ATAND2_CLASS_TO_ACTION_MAP");

	  /* Index 0: class-to-action for y */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(7) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 0) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     0) );

	   /* Index 1: class-to-index mapping */

    PRINT_64_TBL_ITEM( 
	       CLASS_TO_INDEX( F_C_POS_INF,    2) +
	       CLASS_TO_INDEX( F_C_NEG_INF,    3) +
	       CLASS_TO_INDEX( F_C_POS_NORM,   4) +
	       CLASS_TO_INDEX( F_C_NEG_NORM,   5) +
	       CLASS_TO_INDEX( F_C_POS_DENORM, 4) +
	       CLASS_TO_INDEX( F_C_NEG_DENORM, 5) +
	       CLASS_TO_INDEX( F_C_POS_ZERO,   6) +
	       CLASS_TO_INDEX( F_C_NEG_ZERO,   7) );

          /* index 2: mapping for x when y is +Inf */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(6) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     4) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     4) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     4) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_VALUE,     4) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     4) +
              CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_VALUE,     4) );

          /* index 3: mapping for x when y is -Inf */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(5) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_ERROR,     2) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_NEGATIVE,  4) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_NEGATIVE,  4) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_NEGATIVE,  4) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_NEGATIVE,  4) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_NEGATIVE,  4) +
              CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_NEGATIVE,  4) );

          /* index 4: mapping for x when y is +Norm or +Denorm */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(4) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     5) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     6) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  1) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  1) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  1) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_VALUE,     4) +
              CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_VALUE,     4) );

          /* index 5: mapping for x when y is -Norm or -Denorm */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(3) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_NEGATIVE,  5) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  6) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_UNPACKED,  1) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_UNPACKED,  1) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_UNPACKED,  1) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_UNPACKED,  1) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_NEGATIVE,  4) +
              CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_NEGATIVE,  4) );

          /* index 6: mapping for x when y is +Zero */

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(2) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_VALUE,     6) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_VALUE,     4) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_VALUE,     6) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     3) +
              CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_ERROR,     3) );

          /* index 7: mapping for x when y is -Zero */

#if defined(INTEL_CLASS_ACTION)

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  6) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_NEGATIVE,  6) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_NEGATIVE,  6) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     3) +
              CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_NEGATIVE,  6) );
#else

    PRINT_64_TBL_ITEM( CLASS_TO_ACTION_DISP(1) +
	      CLASS_TO_ACTION( F_C_SIG_NAN,    RETURN_QUIET_NAN, 1) +
              CLASS_TO_ACTION( F_C_QUIET_NAN,  RETURN_VALUE,     1) +
              CLASS_TO_ACTION( F_C_POS_INF,    RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_INF,    RETURN_NEGATIVE,  6) +
              CLASS_TO_ACTION( F_C_POS_DENORM, RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_DENORM, RETURN_NEGATIVE,  6) +
              CLASS_TO_ACTION( F_C_POS_NORM,   RETURN_VALUE,     0) +
              CLASS_TO_ACTION( F_C_NEG_NORM,   RETURN_NEGATIVE,  6) +
              CLASS_TO_ACTION( F_C_POS_ZERO,   RETURN_ERROR,     3) +
              CLASS_TO_ACTION( F_C_NEG_ZERO,   RETURN_ERROR,     3 ) );
#endif

	PRINT_U_TBL_ITEM( /* data 1 */             NULL );
	PRINT_U_TBL_ITEM( /* data 2 */  ATAND2_BOTH_INF );
	PRINT_U_TBL_ITEM( /* data 3 */ ATAND2_BOTH_ZERO );
	PRINT_U_TBL_ITEM( /* data 4 */           NINETY );
	PRINT_U_TBL_ITEM( /* data 5 */             ZERO );
	PRINT_U_TBL_ITEM( /* data 6 */       ONE_EIGHTY );

    /*
    ** The following code generates the "table" of constants that the
    ** UX_ATAN2 and UX_ASIN_ACOS routines index into find the appropriate
    ** additive term.  As each value is added to the table, its offset
    ** (in bytes) is computed and recorded as a #define.
    */

    TABLE_COMMENT("0, pi/4, pi/2, 3pi/4, pi in unpacked format");
    PRINT_UX_TBL_ADEF("INV_TRIG_CONS_BASE\t");
    inv_trig_cons_base = MP_BIT_OFFSET;

#   define PRINT_BYTE_OFFSET(name)			\
		printf("#define " name "\t%i\n",	\
		   BYTES(MP_BIT_OFFSET - inv_trig_cons_base))

    PRINT_UX_TBL_ADEF("UX_ZERO\t\t\t\t" );
    PRINT_BYTE_OFFSET( "UX_ZERO_INDEX\t\t" );         PRINT_UX_TBL_ITEM(0);
    PRINT_BYTE_OFFSET( "UX_PI_OVER_4_INDEX\t" );      PRINT_UX_TBL_ITEM(pi/4);
    PRINT_BYTE_OFFSET( "UX_PI_OVER_2_INDEX\t" );      PRINT_UX_TBL_ITEM(pi/2);
    PRINT_BYTE_OFFSET( "UX_THREE_QUARTERS_PI_INDEX"); PRINT_UX_TBL_ITEM(3*pi/4);
    PRINT_BYTE_OFFSET( "UX_PI_INDEX\t\t" );           PRINT_UX_TBL_ITEM(pi);

    /* Miscellaneous constants */

    TABLE_COMMENT("1, 180/pi, 1/3 in unpacked format");
    PRINT_UX_TBL_ADEF_ITEM("UX_ONE\t\t\t", 1);
    PRINT_UX_TBL_ADEF_ITEM("UX_RAD_TO_DEG\t\t", 180/pi );
    PRINT_UX_TBL_ADEF_ITEM("UX_ONE_THIRD\t\t", 1/3 );

    /*
    ** Get the rational coefficients for atan.  Since the reduced argument
    ** is always less that 1/2, we can scale the argument up by 2, which
    ** puts more leading zeros in the coefficients and there by promotes
    ** early exits from the polynomial loop
    */

    function __atan(z)
        {
        auto x;

        x = .5*z;
        if (x == 0)
            return 1.;
        else
            return atan(x)/x;
        }

    save_precision = precision;
    precision = ceil(2*UX_PRECISION/8);

    max_arg = 2*(1/2);

    remes(REMES_FIND_RATIONAL + REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG,
       0, max_arg, __atan, UX_PRECISION, &num_degree, &den_degree,
       &ux_rational_coefs);
    precision = save_precision;

    TABLE_COMMENT("Fixed point coefficients for atan evaluation");
    PRINT_FIXED_128_TBL_ADEF("ATAN_COEF_ARRAY\t\t");
    degree = print_ux_rational_coefs(num_degree, den_degree, -1);
    PRINT_WORD_DEF("ATAN_COEF_ARRAY_DEGREE\t", degree );

    /* One more time for asin rational coefficients.  Again, we scale by 2 */

    function __asin(z)
        {
        auto x;

        x = .5*z;
        if (x == 0)
            return 1.;
        else
            return asin(x)/x;
        }

    save_precision = precision;
    precision = ceil(2*UX_PRECISION/8);

    max_arg = 2*(1/2);

    remes(REMES_FIND_RATIONAL + REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG,
       0, max_arg, __asin, UX_PRECISION, &num_degree, &den_degree,
       &ux_rational_coefs);
    precision = save_precision;

    TABLE_COMMENT("Fixed point coefficients for asin evaluation");
    PRINT_FIXED_128_TBL_ADEF("ASIN_COEF_ARRAY\t\t");
    degree = print_ux_rational_coefs(num_degree, den_degree, -1);
    PRINT_WORD_DEF("ASIN_COEF_ARRAY_DEGREE\t", degree );

    END_TABLE;

    @end_divert
    @eval my $tableText;						\
          my $outText    = MphocEval( GetStream( "divertText" ) );	\
          my $defineText = Egrep( "#define", $outText, \$tableText );	\
             $outText    = "$tableText\n\n$defineText";			\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions and constants inverse " . 	\
                              "trigonomic routines", __FILE__ );	\
             print "$headerText\n\n$outText\n";

#endif
