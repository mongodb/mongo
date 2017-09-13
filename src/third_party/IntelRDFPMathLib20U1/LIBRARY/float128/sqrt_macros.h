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

#ifndef SQRT_MACROS_H
#define SQRT_MACROS_H


/* None of these macros screen for abnormal inputs.
They all assume positive finite values.  */


#define NUM_FRAC_BITS       7
#define INDEX_MASK        MAKE_MASK((NUM_FRAC_BITS + 1), 0)

#if (IEEE_FLOATING)

#       define IF_IEEE_FLOATING(x) x

#       define LOC_OF_EXPON ((BITS_PER_LS_INT_TYPE - 1) - B_EXP_WIDTH)
#       define EXP_BITS_OF_ONE_HALF  ((U_LS_INT_TYPE)(B_EXP_BIAS-B_NORM-1) << LOC_OF_EXPON)
#       define EXPON_MASK   MAKE_MASK(B_EXP_WIDTH, 0)
#	define HI_EXP_BIT_MASK   ((EXPON_MASK - 1) << LOC_OF_EXPON)
#	define GET_SQRT_TABLE_INDEX(exp,index) \
		index = (exp >> (LOC_OF_EXPON - NUM_FRAC_BITS)); \
		index &= INDEX_MASK

#   if ((ARCHITECTURE == alpha) && defined(HAS_LOAD_WRONG_STORE_SIZE_PENALTY))
#       define V_UNION_64_BIT_STORE \
                v.B_UNSIGNED_HI_64 = ((U_WORD)exp) >> 1
#       define V_UNION_128_BIT_STORE \
                v.B_UNSIGNED_HI_64 = ((U_WORD)exp) >> 1; \
                v.B_UNSIGNED_LO_64 = 0
#   else
#	define V_UNION_64_BIT_STORE \
		v.B_UNSIGNED_HI_64 = ((U_INT_64)(U_INT_32)exp) << 31
#	define V_UNION_128_BIT_STORE \
		v.B_UNSIGNED_HI_64 = ((U_INT_64)(U_INT_32)exp) << 31; \
                v.B_UNSIGNED_LO_64 = 0
#   endif

#else

#       define IF_IEEE_FLOATING(x) 

#	define EXP_BITS_OF_ONE_HALF 0x4000
#	define HI_EXP_BIT_MASK 0x7fe0
#	define GET_SQRT_TABLE_INDEX(exp,index) \
		index = ((exp << 3) | ((U_INT_32)exp >> 29)); \
		index &= INDEX_MASK
#	define V_UNION_64_BIT_STORE \
		v.B_UNSIGNED_HI_64 = ((U_INT_64)(U_INT_32)exp) >> 1
#	define V_UNION_128_BIT_STORE \
		v.B_UNSIGNED_HI_64 = ((U_INT_64)(U_INT_32)exp) >> 1 ;\
                v.B_UNSIGNED_LO_64 = 0

#endif

#if (ARCHITECTURE == alpha) || (BITS_PER_WORD == 64)
#     if QUAD_PRECISION
#	   define STORE_EXP_TO_V_UNION \
		V_UNION_128_BIT_STORE
#     else
#	   define STORE_EXP_TO_V_UNION \
		V_UNION_64_BIT_STORE
#     endif
#else
#     if QUAD_PRECISION
#	   define STORE_EXP_TO_V_UNION \
		v.B_SIGNED_HI_32 = ((U_INT_32)exp) >> 1; \
		v.B_SIGNED_LO1_32 = 0;\
		v.B_SIGNED_LO2_32 = 0;\
		v.B_SIGNED_LO3_32 = 0
#     else
#	   define STORE_EXP_TO_V_UNION \
		v.B_SIGNED_HI_32 = ((U_INT_32)exp) >> 1; \
		v.B_SIGNED_LO_32 = 0
#     endif
#endif

#if QUAD_PRECISION
#       define NEWTONS_ITERATION \
             a = y * scaled_x; \
             b = a * y; \
             b = one - b; \
             b *= y; \
             c = y + y; \
             c += b; \
             y = c * half

#else
#       define NEWTONS_ITERATION
#endif

#if QUAD_PRECISION
#       define NEWTONS_ITERATION_NO_SCALE(input) \
             a = y * (B_TYPE)(input); \
             b = a * y; \
             b = one - b; \
             b *= y; \
             c = y + y; \
             c += b; \
             y = c * half

#else
#       define NEWTONS_ITERATION_NO_SCALE(input)
#endif
 



typedef struct { float a, b; double c; } SQRT_COEF_STRUCT;
extern const SQRT_COEF_STRUCT D_SQRT_TABLE_NAME[];


#define SQRT_FIRST_PART(input) \
	B_UNION u, v; \
	B_TYPE y, a, b, c; \
	B_TYPE scaled_x, half_scale; \
	B_TYPE half  = (B_TYPE)0.5; \
	B_TYPE one   = (B_TYPE)1.0; \
	B_TYPE three = (B_TYPE)3.0; \
	F_TYPE ulp, y_less_1_ulp, y_plus_1_ulp; \
	F_TYPE f_type_y, truncated_y, truncated_product; \
        LS_INT_TYPE exp; \
        U_LS_INT_TYPE orig_rounding_mode; \
        U_LS_INT_TYPE index; \
        U_LS_INT_TYPE lo_exp_bit_and_hi_frac; \
        U_LS_INT_TYPE hi_exp_mask = HI_EXP_BIT_MASK; \
        U_LS_INT_TYPE exp_of_one_half = EXP_BITS_OF_ONE_HALF; \
	u.f = (B_TYPE)(input); \
	exp = u.B_HI_LS_INT_TYPE; \
	B_COPY_SIGN_AND_EXP((B_TYPE)(input), half, y); \
	GET_SQRT_TABLE_INDEX(exp,index); \
	b = (B_TYPE)D_SQRT_TABLE_NAME[index].b; \
	b *= y;	 \
	c = (B_TYPE)D_SQRT_TABLE_NAME[index].c; \
	lo_exp_bit_and_hi_frac = exp & ~hi_exp_mask; \
	u.B_HI_LS_INT_TYPE = exp_of_one_half | lo_exp_bit_and_hi_frac; \
	c += b; \
	scaled_x = u.f; \
	y *= y; \
	a = (B_TYPE)D_SQRT_TABLE_NAME[index].a; \
	exp ^= lo_exp_bit_and_hi_frac; \
	exp += exp_of_one_half; \
	y *= a; \
	STORE_EXP_TO_V_UNION; \
	y += c; \
	half_scale = v.f


#define B_HALF_PREC_SQRT(input, result) { \
	SQRT_FIRST_PART(input); \
	a = scaled_x * y; \
	b = half_scale + half_scale; \
	y = a * b; \
	(result) = (B_TYPE)y; \
}

#if (DYNAMIC_ROUNDING_MODES && !FAST_SQRT)

#	define ESTABLISH_KNOWN_ROUNDING_MODE(old_mode) INIT_FPU_STATE_AND_ROUND_TO_ZERO(old_mode)
#	define RESTORE_ORIGINAL_ROUNDING_MODE(old_mode) RESTORE_FPU_STATE(old_mode)

#else

#	define ESTABLISH_KNOWN_ROUNDING_MODE(old_mode)
#	define RESTORE_ORIGINAL_ROUNDING_MODE(old_mode)

#endif


#if !defined(F_MUL_CHOPPED)
#	define F_MUL_CHOPPED(x,y,z) (z) = (x) * (y)
#endif


#if ( (F_PRECISION == 24) && PRECISION_BACKUP_AVAILABLE )


	/* Make sure the last bit is correctly rounded by computing
	a double-precision result, and then rounding it to single.  */


#		define ITERATE_AND_MAYBE_CHECK_LAST_BIT(input) \
			a = y * scaled_x; \
			b = a * y; \
			c = a * half_scale; \
			b = three - b; \
			f_type_y = (F_TYPE)(c * b)
			
#		define RESULT f_type_y


#else

#   undef  ULP_FACTOR
#   if (F_PRECISION == 53)

#	define ULP_FACTOR (F_TYPE)2.775557561562891351e-16 /* 1.25 * 2^(1 - F_PRECISION) */

#   elif QUAD_PRECISION

#       define ULP_FACTOR 1.9259299443872358530559779425849273185381e-34

#   else
# 	error Unsupported F_PRECISION.
#   endif



/* Newton's iteration for 1 / (nth root of x) is:

	y' = y + [ (1 - x * y^n) * y / n ]

So, the iteration for 1 / sqrt(x) is:

	y' = y + [ (1 - x * y^2) * y * 0.5 ]

If we want to do one iteration and multiply the result by x
and multiply the result by a scale factor we get:

	y' = scale   * x     * ( y + [ (1 - x * y^2) * y * 0.5 ] )
	y' = scale   * x * y * ( 1 + [ (1 - x * y^2) * 0.5 ] )
	y' = scale/2 * x * y * ( 2 + [ (1 - x * y^2) ] )        gives about 5/4 lsb error
	y' = scale/2 * x * y * ( 3 - x * y^2 )					gives about 8/4 lsb error

So iterate to get better 1/sqrt(x) and multiply by x to get sqrt(x).  */

#		define ITERATE_AND_MAYBE_CHECK_LAST_BIT(input) \
			a = y * scaled_x; \
			ulp = ULP_FACTOR; \
			b = a * y; \
			c = a * half_scale; \
			b = one - b; \
			a = c + c; \
			b = c * b; \
			ulp *= c; \
			y = a + b; \
			y_less_1_ulp = y - ulp; \
			ASSERT( y_less_1_ulp < y ); \
			y_plus_1_ulp = y + ulp; \
			ASSERT( y_plus_1_ulp > y ); \
			ESTABLISH_KNOWN_ROUNDING_MODE(orig_rounding_mode); \
			F_MUL_CHOPPED(y, y_less_1_ulp, a); \
			F_MUL_CHOPPED(y, y_plus_1_ulp, b); \
			RESTORE_ORIGINAL_ROUNDING_MODE(orig_rounding_mode); \
			y = ((a >= input) ? y_less_1_ulp : y); \
			y = ((b <  input) ? y_plus_1_ulp : y); \
			
#		define RESULT y

#endif


#if (SINGLE_PRECISION)

#	define F_SQRT(input, result) { \
		SQRT_FIRST_PART(input); \
		a = scaled_x * y; \
		b = half_scale + half_scale; \
		y = a * b; \
		(result) = (F_TYPE)y; \
	}

#	define F_PRECISE_SQRT(input, result) { \
		SQRT_FIRST_PART(input); \
		NEWTONS_ITERATION; \
		NEWTONS_ITERATION; \
		ITERATE_AND_MAYBE_CHECK_LAST_BIT(input); \
		(result) = (F_TYPE) RESULT; \
	}

#	define F_SQRT_2_LSB(input,result) F_SQRT(input,result)

#	define F_SQRT_2_LSB_NO_SCALE_FINISH_ITERATION(input) \
		y *= (B_TYPE)(input) \
		y += y;

#else

#	define F_SQRT(input, result) { \
		SQRT_FIRST_PART(input); \
                NEWTONS_ITERATION;\
                NEWTONS_ITERATION;\
		a = scaled_x * y; \
		b = a * y; \
		c = a * half_scale; \
		b = one - b; \
		a = c + c; \
		b = c * b; \
		y = a + b; \
		(result) = (F_TYPE)y; \
	}

#	define F_PRECISE_SQRT(input, result) { \
		SQRT_FIRST_PART(input); \
                NEWTONS_ITERATION;\
                NEWTONS_ITERATION;\
		ITERATE_AND_MAYBE_CHECK_LAST_BIT(input); \
		(result) = (F_TYPE) RESULT; \
	}

#	define F_SQRT_2_LSB(input, result) { \
		SQRT_FIRST_PART(input); \
                NEWTONS_ITERATION; \
                NEWTONS_ITERATION;\
		a = scaled_x * y; \
		b = a * y; \
		c = a * half_scale; \
		b = three - b; \
		y = c * b; \
		(result) = (F_TYPE)y; \
	}

#	define F_SQRT_2_LSB_NO_SCALE_FINISH_ITERATION(input) \
                NEWTONS_ITERATION_NO_SCALE(input);\
                NEWTONS_ITERATION_NO_SCALE(input);\
		a = (B_TYPE)(input) * y; \
		b = a * y; \
		b = three - b; \
		y = a * b

#endif


/* The F_SQRT_2_LSB_NO_SCALE macro avoids most scaling (i.e. 0.5 <= input < 2.0).
The input for the polynomial is still scaled, however, because the
coefficients have a scale factor built into them.  */

#define F_SQRT_2_LSB_NO_SCALE_TIMES_2(input, result) { \
	B_UNION u; \
	B_TYPE y, a, b, c; \
	B_TYPE half  = (B_TYPE)0.5; \
	B_TYPE one   = (B_TYPE)1.0; \
	B_TYPE three = (B_TYPE)3.0; \
	LS_INT_TYPE exp;  \
	U_LS_INT_TYPE index; \
	u.f = (B_TYPE)(input); \
	exp = u.B_HI_LS_INT_TYPE; \
	B_COPY_SIGN_AND_EXP((B_TYPE)(input), half, y); \
	GET_SQRT_TABLE_INDEX(exp,index); \
	b = (B_TYPE)D_SQRT_TABLE_NAME[index].b; \
	b *= y;	 \
	c = (B_TYPE)D_SQRT_TABLE_NAME[index].c; \
	c += b; \
	y *= y; \
	a = (B_TYPE)D_SQRT_TABLE_NAME[index].a; \
	y *= a; \
	y += c; \
	F_SQRT_2_LSB_NO_SCALE_FINISH_ITERATION(input); \
	(result) = (F_TYPE)y; \
}

#endif  /* SQRT_MACROS_H */


