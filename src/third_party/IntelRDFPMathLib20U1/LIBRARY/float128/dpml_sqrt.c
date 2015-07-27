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

#if defined(FAST_SQRT) + defined(SQRT) + defined(RSQRT) + defined(MAKE_INCLUDE) != 1
#       error Exactly one of SQRT, FAST_SQRT, RSQRT, or MAKE_INCLUDE must be defined.
#endif

#if defined(FAST_SQRT)
#	define __ENTRY_NAME F_FAST_SQRT_NAME
#	define ___BASE_NAME   FAST_SQRT_BASE_NAME
#	define IF_SQRT(x) x
#	define IF_RSQRT(x)
#elif defined(RSQRT)
#	define __ENTRY_NAME F_RSQRT_NAME
#	define ___BASE_NAME   RSQRT_BASE_NAME
#	define IF_SQRT(x)
#	define IF_RSQRT(x) x
#elif defined(SQRT)
#	define __ENTRY_NAME F_SQRT_NAME
#	define ___BASE_NAME   SQRT_BASE_NAME
#	define IF_SQRT(x) x
#	define IF_RSQRT(x)
#endif

#if !defined(F_ENTRY_NAME)
#	define F_ENTRY_NAME __ENTRY_NAME
#endif
#if !defined(BASE_NAME)
#	define BASE_NAME ___BASE_NAME
#endif

#if !defined(BUILD_FILE_EXTENSION)
#	define BUILD_FILE_EXTENSION c
#endif


#include "dpml_private.h"

#if (DYNAMIC_ROUNDING_MODES) || (COMPILER == epc_cc)
#	define ESTABLISH_ROUND_TO_ZERO(old_mode) \
			INIT_FPU_STATE_AND_ROUND_TO_ZERO(old_mode)
#	define RESTORE_ROUNDING_MODE(old_mode) \
			RESTORE_FPU_STATE(old_mode)
#else
#	define ESTABLISH_ROUND_TO_ZERO(old_mode)
#	define RESTORE_ROUNDING_MODE(old_mode)
#endif


#if !defined(F_MUL_CHOPPED)

	/* This definition of F_MUL_CHOPPED is used for dynamic
	rounding modes and when no directed rounding is available.
	In the later case results will not be correctly rounded.  */

#	define F_MUL_CHOPPED(x,y,z) (z) = (x) * (y)

#endif


/*
** NUM_FRAC_BITS specifies the number of mantissa bits used for
** indexing the table (the table index also includes the low-order
** exponent bit).  NUM_FRAC_BITS also affects the table size:
**
**	sizeof(D_SQRT_TABLE_NAME) = (1 << (NUM_FRAC_BITS + 1))
**				    * (2*sizeof(float)+sizeof(double))
*/

#define NUM_FRAC_BITS 7
#define INDEX_MASK MAKE_MASK((NUM_FRAC_BITS + 1), 0)


#if (IEEE_FLOATING)

/*
**	LOC_OF_EXPON is the bit offset within u.B_SIGNED_HI_32 of the
**	low-order exponent bit of u.f, where u is a B_UNION.  (We assume
**	the highest bits of B_SIGNED_HI_32 hold the sign bit and exponent).
**
**	From LOC_OF_EXPON, EXP_BITS_OF_ONE_HALF and HI_EXP_BIT_MASK are derived.
*/

#       define LOC_OF_EXPON ((BITS_PER_LS_INT_TYPE - 1) - B_EXP_WIDTH)
#       define EXP_BITS_OF_ONE_HALF  ((U_LS_INT_TYPE)(B_EXP_BIAS-B_NORM-1) << LOC_OF_EXPON)
#	define HI_EXP_BIT_MASK   (MAKE_MASK(B_EXP_WIDTH-1, 1) << LOC_OF_EXPON)

#	define GET_SQRT_TABLE_INDEX(exp,index) \
		index = (exp >> (LOC_OF_EXPON - NUM_FRAC_BITS)); \
		index &= INDEX_MASK

/*
**	SAVE_EXP saves the exponent in a temporary so it can be used in
**	the INPUT_IS_ABNORMAL macro
*/

#	define SAVE_EXP(exp) save_exp = (exp)
#	define INPUT_IS_ABNORMAL \
		((U_LS_INT_TYPE)(save_exp-((LS_INT_TYPE)1 << LOC_OF_EXPON)) >= \
			(U_LS_INT_TYPE)hi_exp_mask)
#endif

#if (VAX_FLOATING)

#	define EXP_BITS_OF_ONE_HALF 0x4000
#	define HI_EXP_BIT_MASK 0x7fe0

#	define GET_SQRT_TABLE_INDEX(exp,index) \
		index = ((exp << 3) | ((U_INT_32)exp >> 29)); \
		index &= INDEX_MASK

#	define SAVE_EXP(exp)	/* INPUT_IS_ABNORMAL doesn't need it */
#	define INPUT_IS_ABNORMAL (x <= (F_TYPE)0.0)

#endif


#if ((ARCHITECTURE == alpha) || (BITS_PER_WORD == 64))

      /* We can do 64-bit stores */
      /* This is an optimization of the 'else' clause below */
#     if QUAD_PRECISION
#	   define STORE_EXP_TO_V_UNION \
		V_UNION_128_BIT_STORE
#     else
#	   define STORE_EXP_TO_V_UNION \
		V_UNION_64_BIT_STORE
#     endif

#else

      /* Store it in 32-bits pieces */
#     if QUAD_PRECISION
#	   define STORE_EXP_TO_V_UNION \
		v.B_SIGNED_HI_32 = ((U_INT_32)exp) >> 1; \
		v.B_SIGNED_LO1_32 = 0; \
		v.B_SIGNED_LO2_32 = 0; \
		v.B_SIGNED_LO3_32 = 0
#     else
#	   define STORE_EXP_TO_V_UNION \
		v.B_SIGNED_HI_32 = ((U_INT_32)exp) >> 1; \
		v.B_SIGNED_LO_32 = 0
#     endif

#endif


/* This condition is complicated.  */

#if (VAX_FLOATING) == (ENDIANESS == little_endian)
#	define V_UNION_64_BIT_STORE \
		v.B_UNSIGNED_HI_64 = ((U_INT_64)(U_INT_32)exp) >> 1
#	define V_UNION_128_BIT_STORE \
		v.B_UNSIGNED_HI_64 = ((U_INT_64)(U_INT_32)exp) >> 1; \
                v.B_UNSIGNED_LO_64 = 0
#elif ((ARCHITECTURE == alpha) && defined(HAS_LOAD_WRONG_STORE_SIZE_PENALTY))
#	define V_UNION_64_BIT_STORE \
		v.B_UNSIGNED_HI_64 = ((U_WORD)exp) >> 1
#	define V_UNION_128_BIT_STORE \
		v.B_UNSIGNED_HI_64 = ((U_WORD)exp) >> 1; \
                v.B_UNSIGNED_LO_64 = 0
#else
#       define V_UNION_64_BIT_STORE \
                v.B_UNSIGNED_HI_64 = ((U_INT_64)(U_INT_32)exp) << 31
#       define V_UNION_128_BIT_STORE \
                v.B_UNSIGNED_HI_64 = ((U_INT_64)(U_INT_32)exp) << 31; \
                v.B_UNSIGNED_LO_64 = 0
#endif


/*
** The definitions of SQRT_COEF_STRUCT and D_SQRT_TABLE_NAME also
** appear in the generated .c file for the table.
*/
typedef struct {
	float a, b;
	double c;
} SQRT_COEF_STRUCT;

extern const SQRT_COEF_STRUCT D_SQRT_TABLE_NAME[(1<<(NUM_FRAC_BITS+1))];


/*
**  SCALE_AND_DO_INDEXED_POLY_APPROX
**
**	Inputs:
**		x		any number
**				= f * 2^(2*i+j)
**			where	1/2 <= f < 1, integer i and j,
**				and j = 0 or 1
**			ignoring f <= 0
**
**	Outputs:
**		half_scale	= 2^(i-1)	(SQRT, F_SQRT)
**
**		flah_scale	= 2^(1-i)	(RSQRT)
**				(the name is clear, albeit cute)
**
**		scaled_x	= f * 2^j
**			so	1/2 <= scaled_x < 2
**
**		y		~= 1/sqrt(scaled_x)
**
**			so	sqrt(x) ~= y * scaled_x * 2 * half_scale
**			and	1/sqrt(x) ~= y / (2 * half_scale)
**
**	Temporaries:
**		u, a, b, c, index
*/

#define SCALE_AND_DO_INDEXED_POLY_APPROX \
	u.f = (B_TYPE)x; \
	exp = u.B_HI_LS_INT_TYPE; \
	B_COPY_SIGN_AND_EXP((B_TYPE)x, half, y); \
	ASSERT( ((0.5 <= y) && (y < 1.0)) ); \
	GET_SQRT_TABLE_INDEX(exp,index); \
	b = (B_TYPE)D_SQRT_TABLE_NAME[index].b; \
	b *= y;	 \
	c = (B_TYPE)D_SQRT_TABLE_NAME[index].c; \
	lo_exp_bit_and_hi_frac = exp & ~hi_exp_mask; \
	u.B_HI_LS_INT_TYPE = (exp_of_one_half | lo_exp_bit_and_hi_frac); \
	c += b; \
	scaled_x = u.f; \
	ASSERT( (((0.5 <= scaled_x) && (scaled_x < 2.0)) || (scaled_x < 0.0)) ); \
	y *= y; \
	a = (B_TYPE)D_SQRT_TABLE_NAME[index].a; \
	SAVE_EXP(exp); \
	IF_SQRT ({ \
	    exp ^= lo_exp_bit_and_hi_frac; \
	    exp += exp_of_one_half; \
	}) \
	IF_RSQRT({ \
	    exp ^= lo_exp_bit_and_hi_frac; \
            exp = 3*exp_of_one_half - exp; \
	}) \
	y *= a; \
	STORE_EXP_TO_V_UNION; \
	y += c; \
	IF_SQRT ( half_scale = v.f ); \
	IF_RSQRT( flah_scale = v.f ); \
	/* end of SCALE_AND_DO_INDEXED_POLY_APPROX */



/*----------------------------------------------------------------------------*/
/*                      Tuckerman's Rounding                                  */
/*----------------------------------------------------------------------------*/

/*
** Tuckerman's rounding is used to compute the correctly rounded sqrt(x).
** It's 'good to the last bit', or more precisely 'to within 1/2 lsb(sqrt(x))'.
** This is a short proof of Tuckerman's rounding.
**
** Let z be a machine-precision approximation to sqrt(x); then z+lsb(z) is the
** smallest representable number larger than z (NB: z-lsb(z) is the largest
** representable number less than z, _except_ when z is a power of 2).
** Within this proof, let [] represent _truncation_ to machine precision,
** and {} represent _rounding_ to machine precision.
**
** Note that for _any_ y (not necessarily representable in machine precision),
**
**      z + 1/2 lsb(z) <= y  <==>  z < {y}.
**
** For sqrt(x), we never have equality:
**      z + 1/2 lsb(z) <= sqrt(x)  ==>  z + 1/2 lsb(z) < sqrt(x),
** because if they were equal, we'd have:
**      (z + 1/2 lsb(z))^2 = x
** which is impossible, because to represent the left hand side requires more
** than twice the machine precision, while the right hand side is representable.
**
** Now the following statements are equivalent in turn:
**
**              z < {sqrt(x)}
**              z + 1/2 lsb(z) <= sqrt(x)
**              z + 1/2 lsb(z) < sqrt(x)
**              (z +  1/2 lsb(z))^2 < x
**              z (z + 1/2 lsb(z)) < x          (the reverse is proved below)
**              [ z (z + 1/2 lsb(z)) ] < x.
**
** To complete the reverse of the third inference above, suppose it were false.
** Then: z (z + 1/2 lsb(z)) < x <= (z +  1/2 lsb(z))^2.  The left hand side is
** some multiple of 1/2 lsb(z)^2.  The right hand side is only larger by
** d = 1/4 lsb(z)^2, so [rhs] = [rhs-d] = [lhs].  But the inequality implies
** [lhs] < x <= [rhs], and we have a contradiction.
**
** In conclusion,
**              z < {sqrt(x)}  <==>  [ z (z + 1/2 lsb(z)) ] < x.
*/

/*
** Here we cover another question:  How closely must y approximate sqrt(x) to
** ensure {y} = {sqrt(x)}, where x is a representable number?  We state without
** proof that the closest sqrt(x) approaches a value halfway between consecutive
** representable numbers occurs either when x is just larger than a power of 4,
** or just less than a power of 4.  We have:
**
**	sqrt(4^k*(1+lsb( 1 ))) = 2^k*(1 + lsb( 1 )/2 - lsb( 1 )^2/8 + ...), and
**	sqrt(4^k*(1-lsb(1/2))  = 2^k*(1 - lsb(1/2)/2 - lsb(1/2)^2/8 - ...).
**
** So if |y - sqrt(x)| < lsb(sqrt(x))^2/8 - O(lsb^3), {y} = {sqrt(x)}.
** For our purposes, this means that 50-bit accuracy (barely) suffices to
** produce a correctly-rounded 24-bit result, since (2^(1-24))^2/8 = 2^(1-50).
** After our Newton's iteration, we have nearly 53-bit accuracy.  All is well.
*/

/*----------------------------------------------------------------------------*/
/*			Computing 'x+' and 'x-'				      */
/*----------------------------------------------------------------------------*/

/*
** For Tuckerman's rounding, we need to compute the (machine-)representable
** numbers just after and before a representable x: 'x+' = x + lsb(x) and
** 'x-' = x - lsb(x-lsb(x)).  Letting '{}' denote rounding to machine precision,
** we compute these by:
**
**	'x+' = {x + {c x}}			(1)
**	'x-' = {x - {c x}}			(2)
**
** for some appropriate constant c, where neither x+{c x} nor x-{c x} are midway
** between two consecutive representable numbers.
**
** The weakest preconditions that satisfy the above are:
**
**	1/2 lsb(x) < {c x} < 3/2 lsb(x)		(1a), when x != 2^n(1-lsb(1/2))
**	1/2 lsb(x) < {c x} <  2  lsb(x)		(1b), when x = 2^n(1-lsb(1/2))
**	1/2 lsb(x) < {c x} < 3/2 lsb(x)		(2a), when x != 2^n
**	1/4 lsb(x) < {c x} < 3/4 lsb(x)		(2b), when x = 2^n
**
** For (1a), (1b), and (2a), we can take:
**
**	1/2 lsb(x)/x < c < 3/2 lsb(x)/x, which we can 'shrink' to simplify:
** 	1/2 lsb(1)/1 < c < 3/2 lsb(1)/2
**	1/2 lsb(1) < c < 3/4 lsb(1)
**
** For (2b), we require:
**
**	1/4 lsb(1) < c < 3/4 lsb(1)
**
** Thus, in any case, we can use any c in the range:
**
**	1/2 lsb(1) < c < 3/4 lsb(1)
**
** We choose the midpoint:
**
**	c = 5/8 lsb(1) = 5/8 2^(1-p) = 5/4 2^(-p)
**
** FWIW: It's possibly to compute 'x-' by:	'x-' = {x * (1-lsb(1/2))},
** but 'x+' isn't necessarily computed by:	'x+' = {x * (1+lsb(1))}. 
*/

#if defined(SQRT)
#   if (F_PRECISION == 24)
#	define ULP_FACTOR (F_TYPE)7.450580596923828125e-8
#   elif (F_PRECISION == 53)
#	define ULP_FACTOR (F_TYPE)1.387778780781445675529539585113525390625e-16
#   elif (F_PRECISION == 56)
#	define ULP_FACTOR (F_TYPE)1.7347234759768070944119244813919067382813e-17
#   elif (F_PRECISION == 113)
#	define ULP_FACTOR (F_TYPE)1.203706215242022408159986214115579574086314e-34
#   else
#	define ULP_FACTOR (F_TYPE)1.25/(F_POW_2(F_PRECISION))
#   endif
#endif



/*----------------------------------------------------------------------------*/
/*			Newton's Iteration     				      */
/*----------------------------------------------------------------------------*/

/*
    Newton's iteration for 1 / (nth root of x) is:

	y' = y + [ (1 - x * y^n) * y / n ]

    So, the iteration for 1 / sqrt(x) is:

	y' = y + [ (1 - x * y^2) * y * 0.5 ]

    If we want to do one iteration, multiply the result by x,
    and multiply the result by a scale factor we get:

	y' = scale   * x     * ( y + [ (1 - x * y^2) * y * 0.5 ] )
	y' = scale   * x * y * ( 1 + [ (1 - x * y^2) * 0.5 ] )
	y' = scale/2 * x * y * ( 2 + [ (1 - x * y^2) ] )        gives about 5/4 lsb error
	y' = scale/2 * x * y * ( 3 - x * y^2 )			gives about 8/4 lsb error

    So iterate to get better 1/sqrt(x) and multiply by x to get sqrt(x). 
*/

/*
**  For quad precision, we need additional Newton's iterations.
**  For lower precisions, the iteration (if needed) is embedded
**  in the ITERATE_AND_MAYBE_CHECK_LAST_BIT macro.
*/
#if QUAD_PRECISION

/*
**  NEWTONS_ITERATION
**
**	Inputs:
**		scaled_x	any number
**			ignoring scaled_x <= 0
**
**		y		~= 1/sqrt(scaled_x)
**
**	Outputs:
**		y		~= 1/sqrt(scaled_x)
**			y becomes a better approximation
**
**	Temporaries:
**		a, b, c
*/
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



/*----------------------------------------------------------------------------*/
/*			ITERATE_AND_MAYBE_CHECK_LAST_BIT		      */
/*----------------------------------------------------------------------------*/

#if 0		/* To make all arms 'elif's */
#elif FAST_SQRT && (F_PRECISION <= 24)

	/* Don't do a Newton's iteration */

#	define ITERATE_AND_MAYBE_CHECK_LAST_BIT \
		a = y * scaled_x; \
		b = half_scale + half_scale; \
		f_type_y = (F_TYPE)(a * b)

#	define RESULT f_type_y

#elif RSQRT && (F_PRECISION <= 24)

	/* Don't do a Newton's iteration */

#	define ITERATE_AND_MAYBE_CHECK_LAST_BIT \
		b = flah_scale + flah_scale; \
		f_type_y = (F_TYPE)(y * b)

#	define RESULT f_type_y

#elif SQRT && (F_PRECISION <= 24) && (B_PRECISION < 2*F_PRECISION)

	/* This case is unlikely enough that we will worry about it
	when we need to (if ever).  There is code in older versions of
	sqrt that does a tuckermans rounding on single prec values.  */

#	error "We need to worry about it now." 

#elif SQRT && (F_PRECISION <= 24) && (B_PRECISION >= 2*F_PRECISION)

	/* Make sure the last bit is correctly rounded by computing
	a double-precision result, and then rounding it to single.  */

#	define ITERATE_AND_MAYBE_CHECK_LAST_BIT \
		a = y * scaled_x; \
		b = a * y; \
		c = a * half_scale; \
		b = three - b; \
		f_type_y = (F_TYPE)(c * b)

#	define RESULT f_type_y

#elif RSQRT

	/* Do more accurate iteration (about 1 lsb error) */

#	define ITERATE_AND_MAYBE_CHECK_LAST_BIT \
		c = y * flah_scale; \
		f_type_y = (F_TYPE)((c+c)+c*(one-scaled_x*(y*y)));

#	define RESULT f_type_y

#elif RSQRT

	/* Do sloppy iteration (about 2 lsb error).
	y = (y * flah_scale) * (three - (y*scaled_x) * y) */

#	define ITERATE_AND_MAYBE_CHECK_LAST_BIT \
		a = y * scaled_x; \
		b = a * y; \
		c = y * flah_scale; \
		b = three - b; \
		y = c * b

#	define RESULT y

#elif FAST_SQRT

	/* Do sloppy iteration (about 2 lsb error).
	y = ((y*scaled_x) * half_scale) * (three - (y*scaled_x) * y) */

#	define ITERATE_AND_MAYBE_CHECK_LAST_BIT \
		a = y * scaled_x; \
		b = a * y; \
		c = a * half_scale; \
		b = three - b; \
		y = c * b

#	define RESULT y

#elif SQRT

	/* Do more accurate iteration and check last bit.
	[ NB: we compute ulp = 2*ULP_FACTOR*c, because y ~= 2*c.] */

#	define DECLARE_old_mode U_WORD old_mode;
#	define DECLARE_ulp_stuff F_TYPE ulp, y_less_1_ulp, y_plus_1_ulp;
#	define ITERATE_AND_MAYBE_CHECK_LAST_BIT \
		a = y * scaled_x; \
		ulp = 2.0*ULP_FACTOR; \
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
		ESTABLISH_ROUND_TO_ZERO(old_mode); \
		F_MUL_CHOPPED(y, y_less_1_ulp, a); \
		F_MUL_CHOPPED(y, y_plus_1_ulp, b); \
		RESTORE_ROUNDING_MODE(old_mode); \
		y = ((a >= x) ? y_less_1_ulp : y); \
		y = ((b <  x) ? y_plus_1_ulp : y);			

#	define RESULT y

#else

	error "Can't define ITERATE_AND_MAYBE_CHECK_LAST_BIT"

#endif


#ifndef DECLARE_old_mode
#define DECLARE_old_mode
#endif
#ifndef DECLARE_ulp_stuff
#define DECLARE_ulp_stuff
#endif



/*----------------------------------------------------------------------------*/
/*                      The Function Itself!                                  */
/*----------------------------------------------------------------------------*/



F_TYPE F_ENTRY_NAME(F_TYPE x)
{
        EXCEPTION_RECORD_DECLARATION
	B_UNION u, v;

	F_TYPE f_type_y;
	B_TYPE y, a, b, c;
	B_TYPE scaled_x;
	B_TYPE IF_SQRT (half_scale)
	       IF_RSQRT(flah_scale);
	const B_TYPE half  = (B_TYPE)0.5;
	const B_TYPE one   = (B_TYPE)1.0;
	const B_TYPE three = (B_TYPE)3.0;
	DECLARE_old_mode
	DECLARE_ulp_stuff

	LS_INT_TYPE   exp, save_exp;
	U_LS_INT_TYPE index;
	U_LS_INT_TYPE lo_exp_bit_and_hi_frac;
	U_LS_INT_TYPE hi_exp_mask = HI_EXP_BIT_MASK; 
	U_LS_INT_TYPE exp_of_one_half = EXP_BITS_OF_ONE_HALF; 

#if defined(HAS_SQRT_INSTRUCTION) && ( FAST_SQRT || SQRT ) && ( SINGLE_PRECISION || DOUBLE_PRECISION )
	u.f = (B_TYPE)x;
	save_exp = u.B_HI_LS_INT_TYPE;

	if INPUT_IS_ABNORMAL
		goto abnormal_input;

	F_HW_SQRT(x,RESULT);

	return RESULT;
#else
	SCALE_AND_DO_INDEXED_POLY_APPROX; 

	if INPUT_IS_ABNORMAL
		goto abnormal_input;

        NEWTONS_ITERATION;
        NEWTONS_ITERATION;

	ITERATE_AND_MAYBE_CHECK_LAST_BIT; 

	return RESULT; 
#endif


abnormal_input:

#if VAX_FLOATING

	/* x is either 0 or negative   */

	if (x == (F_TYPE)0.0) {
#if RSQRT
		GET_EXCEPTION_RESULT_1(RSQRT_OF_POS_ZERO, x, RESULT);
#else
		RESULT = x;
#endif
	} else {
		GET_EXCEPTION_RESULT_1(SQRT_OF_NEGATIVE, x, RESULT);
	}
	return RESULT; 


#elif (IEEE_FLOATING)
 
	F_CLASSIFY(x, index);

	switch (index) {

            case F_C_SIG_NAN:
            case F_C_QUIET_NAN:
               RESULT = x;
               return RESULT;
               break;

#if RSQRT

            case F_C_POS_INF:
               RESULT = (F_TYPE)0.0;
               return RESULT;
               break;
            case F_C_POS_ZERO:
               GET_EXCEPTION_RESULT_1(RSQRT_OF_POS_ZERO, x, RESULT);
               return RESULT;
               break;
            case F_C_NEG_ZERO:
               GET_EXCEPTION_RESULT_1(RSQRT_OF_NEG_ZERO, x, RESULT);
               return RESULT;
               break;

#else

            case F_C_POS_INF:
            case F_C_POS_ZERO:
            case F_C_NEG_ZERO:
               RESULT = x;
               return RESULT;
               break;

#endif

            case F_C_NEG_INF:
            case F_C_NEG_NORM:
            case F_C_NEG_DENORM:
		GET_EXCEPTION_RESULT_1(SQRT_OF_NEGATIVE, x, RESULT);
                return RESULT;
                break;

            default:

                /* must be positive denorm */

		F_MAKE_FLOAT(
                   ((WORD) (2*F_PRECISION + 1) << F_EXP_POS), f_type_y);
		F_COPY_SIGN_AND_EXP(x, f_type_y, x);
		x -= f_type_y;

#if defined(HAS_SQRT_INSTRUCTION) && ( FAST_SQRT || SQRT ) && ( SINGLE_PRECISION || DOUBLE_PRECISION )
		F_HW_SQRT(x,RESULT);
#else
		SCALE_AND_DO_INDEXED_POLY_APPROX;

                NEWTONS_ITERATION;
                NEWTONS_ITERATION;
	
		ITERATE_AND_MAYBE_CHECK_LAST_BIT;
#endif

		/* Scale down again (up for RSQRT) */

		IF_SQRT ( SUB_FROM_EXP_FIELD(RESULT, F_PRECISION) );
		IF_RSQRT(   ADD_TO_EXP_FIELD(RESULT, F_PRECISION) );
		return RESULT;
                break;
	}

#endif

}  /* sqrt */



/*----------------------------------------------------------------------------*/
/*                      MPHOC code to generate the table                      */
/*----------------------------------------------------------------------------*/


#if MAKE_INCLUDE

#undef  F_NAME_SUFFIX
#define F_NAME_SUFFIX TABLE_SUFFIX

@divert divertText


	/*
	** Print header information.
	*/
	print;
	print "#include \"dpml_private.h\"";
	print;
	print "#define NUM_FRAC_BITS ", STR(NUM_FRAC_BITS);
	print;
	/*
	** The definitions of SQRT_COEF_STRUCT and D_SQRT_TABLE_NAME also
	** appear in the code.
	*/
	print "typedef struct {";
	print "	float a, b;";
	print "	double c;";
	print "} SQRT_COEF_STRUCT;";
	print;
	print "const SQRT_COEF_STRUCT D_SQRT_TABLE_NAME[(1<<(NUM_FRAC_BITS+1))] = {";
	print;

/*
** Generate and print the polynomial coefficients.
*/
function rsqrt_f(r) { return 1/sqrt(r); }

precision = ceil( (D_PRECISION + 16)/MP_RADIX_BITS );

/*
**  For each half fo the table, ...
*/
for (h = 1; h <= 2; h++) {

    xaa = 0.5;
    xbb = 1.0;
    xkk = 1.0/h;
    print;
    printf("/*\n**\t");
    printf("a*x^2 + b*x + c");
    printf(" ~= sqrt(%5r/x),\t\t%5r <= x < %5r", xkk, xaa, xbb);
    printf("\n*/\n");

    for (i = 0; i < 2^NUM_FRAC_BITS; i++) {
	xa = xaa + (xbb-xaa) *   i  /2^NUM_FRAC_BITS;
	xb = xaa + (xbb-xaa) * (i+1)/2^NUM_FRAC_BITS;
	/*
	** Determine a minimum-error quadratic approximation to
	** sqrt(xkk/x) in the range xa <= x <= xb.  (This doesn't
	** minimize the error after a Newton's iteration; that'd
	** require a weighting function of x^(1/4), a needless
	** complication for this single-precision approximation).
	*/
        tol = S_PRECISION+2;
        flags = 0;
        err = remes(flags, xa, xb, rsqrt_f, tol, &degree, &rsqrt_c);
        if (degree != 2) print("*** degree = %i\n", degree);
	for (j = 0; j <= degree; j++)
	    rsqrt_c[j] = rsqrt_c[j] * sqrt(xkk);
	/*
	** Now round the x^2 and x coefficients to single precision,
	** by subtracting Chebyshev polynomials.  The additional error
	** is negligible (less than 3%; e.g., if the polynomial was good to
	** 27 bits, it's degraded to only 27-log2(1.03) = 26.96 bits).
	**
	** The algebra is simplified by expressing the range xa..xb in terms of
	** the range's midpoint and radius.
	*/
	xm = (xb + xa)/2;
	xr = (xb - xa)/2;
	z = xm / xr;
	/*
	** The Chebyshev polynomials we subtract are multiples of:
	**
	**	w	  <->	(x-xm)/xr
	**	1-2*w^2	  <->	1-2*((x-xm)/xr)^2
	**
	** The x terms are collected, scaled (by t), and subtracted from the
	** polynomial coefficients.
	**
	** First we subtract (a multiple of) the 2nd degree Chebyshev polynomial
	** to produce a new polynomial with the desired (representable in single
	** precision) 2nd degree polynomial coefficient.  This minimizes the
	** maximum absolute error between the 'Remes' polynomial and the new
	** polynomial (since the difference is a Chebyshev polynomial, which
	** has the 'equal ripple' property).
	** 
	** Then we subtract (a multiple of) the 1st degree Chebyshev polynomial
	** to produce a new polynomial with the desired (representable in single
	** precision) 1st degree coefficient.  This minimizes the maximum
	** absolute error between the previous polynomial and the newer one
	** (under the constraints of having the same 2nd degree coefficient,
	** and the desired 1st degree coefficient).  The 0th degree coefficient
	** is rounded to double precision (somebody's got to!), and this has
	** no significant effect on the single precision result.
	**
	** Is the resulting polynomial optimal?  Nope; nobody claims it is.
	** Is it 'best' in some sense?  Yes -- the theory is clear and the code
	** is short (disregarding this phillipic).  Is it close enough?  Yep.
	** Why?  That's a good question....
	**
	** To see why this works, consider the polynomial for 1/sqrt(x) for
	** 1 <= x < 1+2^-7,
	**
	**	0.37... x^2 + -1.24... x + 1.87...
	**
	** Simply rounding the x coefficient to 24 bits may corrupt the result
	** of the polynomial by as much as (1+2^-7) * 0.5*s_lsb(1.24), where
	** s_lsb(z) = 2^floor(log2(|z|) + 1 - 24) is the value of z's least
	** significant bit when z is expressed in single precision.  This is
	** as much as 2^-24, which is 2*s_lsb(1/sqrt(x)) -- two single-precision
	** lsb of the result!  Rounding the x^2 coefficient has similar effects,
	** affecting the result by 1/2 single-precision lsb.  We can do better.
	**
	** If rounding increases the x coefficient by t, |t| <= 0.5*lsb(1.24),
	** the corruption can be partly compensated by adjusting the constant
	** coefficient, decreasing it by (for example) t*(1 + 1+2^-7)/2.
	** The corruption is then:
	**
	**	t*( x - (1+1+2^-7)/2 )
	**
	** Since 1 <= x < 1+2^-7, and |t| <= 0.5*lsb(1.24), we have:
	**
	**	| t*( x - (1+1+2^-7)/2 ) | <= 0.5*lsb(1.24) * 2^-8 = 2^(-24 -8)
	**
	** which is only 0.0078125*s_lsb(1/sqrt(x)) -- a factor of 256 smaller
	** than the corruption from simply rounding the x coefficient. 
	**
	** To minimize the (absolute value of the) maximum corruption, we add
	** a multiple of a Chebyshev polynomial, for the particular range of x,
	** because Chebyshev polynomials are 'minimax' (or 'equal ripple')
	** polynomials.
	** For the range -1 <= w <= 1, the Chebyshev polynomials are:
	**
	**	1,  w,  2*w^2-1,  4*w^3-3*w,  ....
	** 
	** To convert these to polynomials in x for the range a <= x <= b,
	** substitute (x-m)/r, with m = (b+a)/2, r = (b-a)/2, and z = m/r.
	** The Chebyshev polynomials become:
	**
	**	1, x/r - z, 2*(x/r)^2 - 4*z*(x/r) + 2*z^2-1,
	**	4*(x/r)^3 - 12*z*(x/r)^2 + (12*z^2-3)*(x/r) - 4*z^3+3*z, ....
	**
	** For 1 <= x < 1+2^-7, these are:
	**
	**	1, 2^8*x - (2^8+1), 2^17*x^2 - (2^18+2^10)*x + (2^17+2^10+1),
	**	2^26*x^3 - 3*(2^26+2^18)*x^2 + 3*(2^26+2^19+3*2^8)*x
	**			- (2^26+3*2^18+2^11+2^8+1), ....
	**
	** Each of these are 'equal ripple', oscillating between +/-1.  We see
	** our previous adjustment, ( x - (1+1+2^-7)/2 ), appear here with a
	** factor of 2^8.  Scaling it by t*2^-8 gives our previous result; this
	** scaling also reduces the 'ripple' to +/-t*2^-8.
	**
	** When we use the 2nd degree Chebyshev polynomial to round the 2nd
	** degree coefficient to single precision, we must scale the polynomial
	** by a factor of t*2^-17, where here |t| <= 0.5*lsb(0.37).  This means
	** that the effect of this corruption, the size of the 'ripple', is less
	** than 0.5*lsb(0.37)*2^-17 = 2^-43, or 2^-18*s_lsb(1/sqrt(x)).  This is
	** far better than the the 1/2 lsb we got when we simply rounded the x^2
	** coefficient.
	** 
	** Can this technique be applied to other polynomial coefficients?
	** It is an invention of my own conception developed outside the term
	** of my contract, and for which I've received no compensation.
	*/
	t = rsqrt_c[2] - bround(rsqrt_c[2], S_PRECISION);
	rsqrt_c[2] = rsqrt_c[2] - t;
	rsqrt_c[1] = rsqrt_c[1] + t * 2*z * xr;
	rsqrt_c[0] = rsqrt_c[0] + t * (0.5-z^2) * xr^2;
	t = rsqrt_c[1] - bround(rsqrt_c[1], S_PRECISION);
	rsqrt_c[2] = rsqrt_c[2];
	rsqrt_c[1] = rsqrt_c[1] - t;
	rsqrt_c[0] = rsqrt_c[0] + t * z * xr;
	t = rsqrt_c[0] - bround(rsqrt_c[1], D_PRECISION);
        printf("{\t%.10r,\t%.10r,\t%.20r\t},\n",
	    rsqrt_c[2], rsqrt_c[1], rsqrt_c[0]);
    }
}

	/*
	** Print the trailer.
	*/
	print;
	print "};";
	print;


@end_divert
@eval my $outText = MphocEval( GetStream( "divertText" ) );		\
     my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),		\
                       "Double precision square root table", __FILE__);	\
     print "$headerText\n\n$outText";


#endif  /* MAKE_INCLUDE */


/*----------------------------------------------------------------------------*/
/*                              Testing                                       */
/*----------------------------------------------------------------------------*/

#if MAKE_MTC


@divert > dpml_sqrt.mtc


build default = "sqrt.a";

function SINGLE_SQRT      = F_CHAR F_SQRT_NAME(F_CHAR.v.r); 
function FAST_SINGLE_SQRT = F_CHAR F_FAST_SQRT_NAME(F_CHAR.v.r); 
function DOUBLE_SQRT      = B_CHAR B_SQRT_NAME(B_CHAR.v.r);
function FAST_DOUBLE_SQRT = B_CHAR B_FAST_SQRT_NAME(B_CHAR.v.r);
function MP_SQRT          = void mp_sqrt(m.r.r, m.r.w);



type SQRT_ACCURACY = accuracy
	error = lsb;
	stats = max;
	points = 1024;
;


domain SINGLE_SQRT_DENORMS  = { [ 0.0 , 1e-37  ]:uniform:10001 } ;
domain DOUBLE_SQRT_DENORMS  = { [ 0.0 , 1e-307 ]:uniform:10001 } ;
domain SINGLE_SQRT_ACCURACY = { [ 0.0 , 17.0   ]:uniform:100001 } ;
domain DOUBLE_SQRT_ACCURACY = { [ 0.0 , 17.0   ]:uniform:100001 } ;

domain SQRT_KEYPOINTS =
	lsb = 0.5; { 2.0 | der } { 5.0 | der } { 10.0 | der }
	lsb = 0.5; { MTC_POS_TINY | der } { MTC_POS_HUGE | der }
	{ 0.0 | 0.0 } { 1.0 | 1.0 } { MTC_NEG_ZERO | MTC_NEG_ZERO }
	{ MTC_POS_INFINITY | MTC_POS_INFINITY } { MTC_NAN | MTC_NAN }
;

domain FAST_SINGLE_SQRT_KEYPOINTS =
	lsb = 1.0; { 2.0 | der } { 5.0 | der } { 10.0 | der }
	lsb = 1.0; { MTC_POS_TINY | der } { MTC_POS_HUGE | der }
	{ 0.0 | 0.0 } { 1.0 | 1.0 } { MTC_NEG_ZERO | MTC_NEG_ZERO }
	{ MTC_POS_INFINITY | MTC_POS_INFINITY } { MTC_NAN | MTC_NAN }
;

domain FAST_DOUBLE_SQRT_KEYPOINTS =
	lsb = 2.0; { 2.0 | der } { 5.0 | der } { 10.0 | der }
	lsb = 2.0; { MTC_POS_TINY | der } { MTC_POS_HUGE | der }
	{ 0.0 | 0.0 } { 1.0 | 1.0 } { MTC_NEG_ZERO | MTC_NEG_ZERO }
	{ MTC_POS_INFINITY | MTC_POS_INFINITY } { MTC_NAN | MTC_NAN }
;


test sqrt_acc_sd =
	type   = SQRT_ACCURACY;
	domain = SINGLE_SQRT_ACCURACY;
	function            = SINGLE_SQRT; 
	comparison_function = FAST_DOUBLE_SQRT;
	output = 
		file = "sqrt_acc_sd.out";
	;
; 

test sqrt_denorm_acc_sd =
	type   = SQRT_ACCURACY;
	domain = SINGLE_SQRT_DENORMS;
	function            = SINGLE_SQRT; 
	comparison_function = FAST_DOUBLE_SQRT;
	output = 
		file = "sqrt_denorm_acc_sd.out";
	;
; 

test fast_sqrt_acc_sd =
	type   = SQRT_ACCURACY;
	domain = SINGLE_SQRT_ACCURACY;
	function            = FAST_SINGLE_SQRT; 
	comparison_function = FAST_DOUBLE_SQRT;
	output = 
		file = "fast_sqrt_acc_sd.out";
	;
; 


test sqrt_acc_dm =
	type   = SQRT_ACCURACY;
	domain = DOUBLE_SQRT_ACCURACY;
 	function            = DOUBLE_SQRT;
	comparison_function = MP_SQRT;
	output =
		file = "sqrt_acc_dm.out";
	;
; 

test sqrt_denorm_acc_dm =
	type   = SQRT_ACCURACY;
	domain = DOUBLE_SQRT_DENORMS;
 	function            = DOUBLE_SQRT;
	comparison_function = MP_SQRT;
	output =
		file = "sqrt_denorm_acc_dm.out";
	;
; 

test fast_sqrt_acc_dm =
	type   = SQRT_ACCURACY;
	domain = DOUBLE_SQRT_ACCURACY;
 	function            = FAST_DOUBLE_SQRT;
	comparison_function = MP_SQRT;
	output =
		file = "fast_sqrt_acc_dm.out";
	;
; 


test sqrt_key_sd =
    type   = key_point; 
    domain = SQRT_KEYPOINTS; 
    function            = SINGLE_SQRT;
    comparison_function = DOUBLE_SQRT;
    output =
        file = "sqrt_key_sd.out"  ;
        style = verbose;
    ;
;

test sqrt_key_dm =
    type   = key_point; 
    domain = SQRT_KEYPOINTS; 
    function            = DOUBLE_SQRT;
    comparison_function = MP_SQRT;
    output =
        file = "sqrt_key_dm.out"  ;
        style = verbose;
    ;
;

test fast_sqrt_key_sd =
    type   = key_point; 
    domain = FAST_SINGLE_SQRT_KEYPOINTS; 
    function            = FAST_SINGLE_SQRT;
    comparison_function = FAST_DOUBLE_SQRT;
    output =
        file = "fast_sqrt_key_sd.out"  ;
        style = verbose;
    ;
;

test fast_sqrt_key_dm =
    type   = key_point; 
    domain = FAST_DOUBLE_SQRT_KEYPOINTS; 
    function            = FAST_DOUBLE_SQRT;
    comparison_function = MP_SQRT;
    output =
        file = "fast_sqrt_key_dm.out"  ;
        style = verbose;
    ;
;


@end_divert


#endif  /* MAKE_MTC */


