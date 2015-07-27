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

#if defined(MAKE_COMMON)

#   undef  MAKE_INCLUDE
#   define MAKE_INCLUDE

#   if !defined(TABLE_NAME)
#       define TABLE_NAME	FOUR_OVER_PI_TABLE_NAME
#   endif

#   if !defined(BUILD_FILE_NAME)
#       define BUILD_FILE_NAME	FOUR_OVER_PI_BUILD_FILE_NAME
#   endif

#   if !defined(MP_FILE_NAME)
#       define MP_FILE_NAME	ADD_EXTENSION(FOUR_OVER_PI_BUILD_FILE_NAME,mp)
#   endif

#   define T_FLOAT	/* Need some float type defined for dpml_private.h */

#else

#   if defined(MAKE_INCLUDE)
#       undef  MAKE_COMMON
#   endif
#
#   if !defined(BASE_NAME)
#       define BASE_NAME       TRIG_REDUCE_BASE_NAME
#   endif

#endif

/*
 * If we not building the four_over_pi table, make sure that the name of the
 * the table is picked up from the file that contains the table
 */

#if !defined(MAKE_COMMON) && defined(FOUR_OVER_PI_TABLE_NAME)
#   error "FOUR_OVER_PI_TABLE_NAME cannot be specified without MAKE_COMMON"
#endif

#include "dpml_private.h"

#if !defined(NUM_INDEX_BITS)
#    define  NUM_INDEX_BITS	7
#endif

#if !defined(NUM_OCTANT_BITS)
#    define  NUM_OCTANT_BITS	10
#endif
#    define  MIN_OVERHANG	6

/*
 *  These flags indicate whether 'trig_reduce' has these optional
 *  parameters.
 */
#define VOC	0		/* have a 'variable-octant' parameter */
#define BIX	1		/* have a 'binary scaling' parameter */


/*
 * BASIC ALGORITHM:
 * ----------------
 *
 * It is assumed that this routine will be used VERY infrequently and
 * consequently the implementation contained here sacrifices some performance
 * for simplicity and uniformity.
 *
 * Let x' = x*2^bix + voc*(pi/4).  We want to produce
 *
 *		y = mod( x', pi/2 )/2^bix
 *
 * or equivalently,
 *
 *		I = nint( x'/(pi/2) )
 *		y = ( x' - I*(pi/2) )/2^bix
 *
 * We also want to produce an integer result containing the low bits of I 
 * (called the 'octant' bits) and some 'fractional' bits of I that can be
 * used as a table index (these are called the 'index' bits).  We also want
 * to compute and return y as two floating-point values, y = hi - lo, so
 * that lo provides some additional precision to the caller.
 *
 * More precisely,
 *
 *		x' = x*2^bix + voc*(pi/4)
 *		J = nint( x'/(pi/2) * 2^(NUM_INDEX_BITS+1) )
 *		I = floor( (J + 2^NUM_INDEX_BITS)/2^(NUM_INDEX_BITS+1) )
 *		y = ( x' - I*(pi/2) )/2^bix
 *		result = mod( J, 2^(NUM_OCTANT_BITS+NUM_INDEX_BITS) )
 *
 * [The following comments should be rewritten to be more precise.]
 *
 * Note that the reduce argument is in "radians".  For computational
 * purposes, it is convenient to first obtain the reduced argument in
 * cycles - i.e. compute y as
 *
 *              I' = trunc(x'/(pi/4))
 *              o  = low three bits I'
 *              z' = x' - I'
 *              z = z'     if o is even
 *                = z' - 1 if o is odd
 *              y = z*(pi/4)
 *
 * Note that z' is in fact the fraction bits of the quotient x'/(pi/4) =
 * (x + n*(pi/4))/(pi/4) = x/(p/4) + n, so that the reduction process can be
 * described by
 *
 *              o  = low three integer bits of [x/(pi/4) + n ]
 *              z' = fractional bits of (x/(pi/4))
 *              z = z'     if o is even                         (1)
 *                = z' - 1 if o is odd
 *              y = z*(pi/4)
 *
 * We see that the key operation is to compute x/(pi/4).  With this
 * in mind, let x = 2^n*f, where 2^v <= f < 2^(v+1) and f has P significant
 * bits.  If F is defined as F = 2^(P-v-1)*f, it follows that F is an integer.
 * Now
 *
 *              x/(pi/4) = x *(4/pi)
 *                       = (2^n*f) *(4/pi)
 *                       = [2^(n-P+v+1)]*[2^(P-v-1)*f] *(4/pi)
 *                       = [2^(n-P+v+1)]*F*(4/pi)
 *                       = F*{[2^(n-P+v+1)]*(4/pi)}
 *
 * Suppose that we have stored a large bit string that represents the value
 * of 4/pi, then we can obtain the value of 2^(n-P+v+1)*(4/pi) by moving the
 * binary point in 4/pi by n-P+v+1 places.  In particular, let
 *
 *              2^(n-P+v+1)*(4/pi) = J*8 + g
 *
 * That is, J is an integer formed from the first n-P+v-2 bits of 4/pi and
 * g is value formed by the remaining bits.  It follows that 
 *
 *              x/(pi/4) = F*{[2^(n-P+v+1)]*(4/pi)}
 *                       = F*(J*8 + g)
 *                       = F*J*8 + F*g
 *
 * Note that (1) implies that we need only compute x/(pi/4) modulo 8.  Noting
 * that F and J are integer, the above gives
 *
 *              x/(pi/4) (mod 8) = (F*J*8 + F*g) (mod 8)
 *                               = F*g (mod 8)
 *
 * At this point the algorithm for large argument reduction has the following
 * flavor:
 *
 *              (1) index into a precomputed bit string for 4/pi to
 *                  obtain g 
 *              (2) compute w = F*g (mod 8)
 *              (3) o <-- integer part of w + n
 *              (4) z' <-- fractional part of w
 *              (5) z = z'     if o is even
 *                    = z' - 1 if o is odd
 *              (6) y = z*(pi/4)
 *
 *			Algorithm I
 *			-----------
 *
 * The following sections describe the implementation issues associated with
 * each of the steps in algorithm I as well as present the code for the 
 * overall implementation.
 *
 *
 * THE 4/pi TABLE
 * --------------
 *
 * Step (1) of Algorithm I requires indexing into a bit string for 4/pi using
 * the exponent field of the argument.  Specifically, if n is the argument
 * exponent we want to shift the binary point of 4/pi by n - (P - v - 1) bits
 * to the right.  Since x can be as small as 1, it is possible that n - (P -
 * v - 1) is negative.  Thus to facilitate the indexing operation, it is
 * necessary for the bit string to have some leading 0's.
 *
 * Assume the bit string for 4/pi has T leading zeros and that the bits are
 * numbered in increasing order starting from 0.  I.e. the string looks like:
 *
 *	bit number: 0      T
 *	            00...001.01000101111.....
 *                          ^
 *                          |
 *		       binary point 
 *
 * From the above discussion, we want to shift the binary point of the bit
 * string P-v-1 bits to the right and extract g as some (as yet undetermined)
 * number of bits, starting o bits to the left of the shifted binary point.
 * Consequently, the position of the most significant bit we would like to
 * access is k = (T - 1) + [n - (P - v - 1)] - o = T + n - P + v - o.  Since
 * we want the bit position to be greater than or equal to zero, and we are
 * assuming that the argument is greater than or equal to 1 (i.e. n >= -v),
 * it follows that T >= P + o.  Since 4/pi is stored as bit string, it is
 * data type independent.  Consequently, the same table can be used for all
 * supported data types.  This means that the value of P used to determine T
 * should represent the largest precision supported.
 */

#define TYPE_MASK(x,y)	((1 << x) | (1 << y))

#if (FLOAT_TYPES & (TYPE_MASK(h_floating, x_floating)))

#   if (FLOAT_TYPES & ( 1 << x_floating))
#       define MAX_PRECISION	 (128 + 1)
#   else
#       define MAX_PRECISION	 Q_PRECISION
#   endif
#   define MAX_LOG2_MAX_FLOAT   (Q_MAX_BIN_EXP + Q_NORM + 1)

#elif (FLOAT_TYPES & (TYPE_MASK(g_floating, t_floating)))

#   define MAX_PRECISION	 D_PRECISION
#   define MAX_LOG2_MAX_FLOAT   (D_MAX_BIN_EXP + D_NORM + 1)

#elif (FLOAT_TYPES & (TYPE_MASK(f_floating, s_floating)))

#   define MAX_PRECISION	 S_PRECISION
#   define MAX_LOG2_MAX_FLOAT   (S_MAX_BIN_EXP + S_NORM + 1)

#endif

/*
 * Since most architectures do not efficiently support bit addressing, the
 * argument reduction routine assumes that the 4/pi bit string is stored
 * in L-bit "digits", where L will be specified later.  Getting the right bits
 * of 4/pi requires getting the set of "digits" that begin with the digit that
 * contains the leading bit and doing a sequence of shifts and logical ors.
 * The index of the digit that contains the initial bit is trunc(n/L) and the
 * bit position within that digit is n - L*trunc(n/L) = n % L. On some
 * architectures, obtaining both the quotient and remainder of an integer
 * division is faster than obtaining each one separately.  Consequently we
 * assume the existence of a div_rem operator.
 *
 * If the 4/pi table has been created, pick up the DIGIT definition from there
 * to ensure consistency between the table and the generated code.  Otherwise
 * use the default DIGIT definitions.
 */
   
#if !defined(MAKE_COMMON)

#   undef   DIGIT_TYPE
#   undef   SIGNED_DIGIT_TYPE
#   undef   BITS_PER_DIGIT
#   define  DEFINES
#   include STR(FOUR_OVER_PI_BUILD_FILE_NAME)
#   undef   DEFINES

#endif

#define DIGIT_MASK(width,pos)	((( DIGIT_TYPE_CAST 1 << (width)) - 1) << (pos))
#define DIGIT_BIT(pos)		( DIGIT_TYPE_CAST 1 << (pos))
#if defined(MAKE_COMMON) || defined(MAKE_INCLUDE)
#define DIGIT_TYPE_CAST		/* MPHOC doesn't do casts */
#else
#define DIGIT_TYPE_CAST		(DIGIT_TYPE)
#endif


/*
 *  FOUR_OV_PI_ZERO_PAD_LEN is defined with the 4/pi table.
 */
#if !defined(FOUR_OV_PI_ZERO_PAD_LEN)
#define LOG2_4_OV_PI		1
#define FOUR_OV_PI_ZERO_PAD_LEN	(MAX_PRECISION - LOG2_4_OV_PI + NUM_OCTANT_BITS)
#endif

#define DIGIT_HEX_FMT_SPEC	PASTE_3(HEX_FORMAT_FOR_, BITS_PER_DIGIT, _BITS)


#define IS_POW_TWO(n)   (((n)&((n) - 1)) == 0)

#if !defined DIV_REM_BY_L
#   if IS_POW_TWO( BITS_PER_DIGIT )
#       define DIV_REM_BY_L(n,q,r)	(q) = (n) >> __LOG2(BITS_PER_DIGIT); \
					(r) = (n) & (BITS_PER_DIGIT - 1)
#   else
#       define DIV_REM_BY_L(n,q,r)	(q) = (n) / BITS_PER_DIGIT; \
					(r) = (n) - (q)*BITS_PER_DIGIT
#   endif
#endif

/*
 *  In case anything goes horribly wrong...
 */
#define fatal(message)  {                               \
    printf( "Fatal error: " message                     \
        "\n" "aborting at line " STR(__LINE__));        \
    /* exit(-1) */                                      \
    this_assignment = indicates_a_fatal_error;          \
                        }

#    define sMAC2	"; \\\n\t"
#    define MAC2	" \\\n\t"
#    define MAC3	"\n\n"


/******************************************************************************/
/*									      */
/*			Produce the four_ov_pi table			      */
/*									      */
/******************************************************************************/

#if defined(MAKE_COMMON)

    @divert divertText

#   undef  TABLE_WORD
#   define TABLE_WORD	PASTE_2(U_INT_,BITS_PER_DIGIT)

/*
 * The last issue associated with the 4/pi table is how many bits of 4/pi
 * are necessary?  Since the index into the table is essentially the exponent
 * of the argument less the number of bits of precision, the maximum number of
 * bits that can be skipped over is
 *
 *		MAX_EXP + (1 + v - P)
 *
 * Further, we require that the result be accurate to P + k bits, so that
 * we need at least that many additional bits.  Also, we need to guarantee
 * against a loss of significance.   For VAX F, D, G and H data types and
 * IEEE S and T data types, it has been verified that the number of leading
 * 0's and 1's does not exceed 5*P/4.
 *
 *      NOTE: The program used to establish this result along with
 *      a description of the algorithm is contained in a separate
 *      file (to be supplied at a later time.)
 *
 * It follows that the maximum number of bits required is
 *
 *		MAX_EXP + MAX_LEADING_0s_OR_1s + k + v + 1
 *
 * Additionally, we note that the above algorithm requires that we be able
 * to continually add digits to the generated product.  This requires that
 * we keep an extra P bit in the product for the next possible digits.
 * Consequently, the total number of bits required is actually
 *
 *		MAX_EXP + MAX_LEADING_0s_OR_1s + P + k + v + 1
 */

    /*
     * Determine the number of bits of 4/pi required and set number of
     * digits and mp precision;
     */

    max_leading_1s_or_0s = ceil(5*MAX_PRECISION/4);
    num_4_ov_pi_bits = MAX_LOG2_MAX_FLOAT + max_leading_1s_or_0s + MIN_OVERHANG +
       MAX_PRECISION + 1;
    total_bits = num_4_ov_pi_bits + FOUR_OV_PI_ZERO_PAD_LEN;
    precision = ceil(total_bits/MP_RADIX_BITS);

    /*
     * Get 4/pi and normalize the fraction field and adjust to include
     * leading zeros
     */
    t = 4/pi;
    t = bldexp(t, -(FOUR_OV_PI_ZERO_PAD_LEN + bexp(t)));
    /*
     * Since
     *	    bldexp(z,-bexp(z)) = z/(2*msb(z)), and
     *	    msb(4/pi)=1,
     * we now have
     *	    t = (2/pi)/2^PAD_LEN,
     * where PAD_LEN is a synonym for FOUR_OV_PI_ZERO_PAD_LEN.
     *
     * When the four_ov_pi table is interpreted as a fixed-point binary value,
     * with binary point at the start of the table, then
     *
     *	    four_ov_pi = (2/pi)/2^PAD_LEN.
     */

    /* Print out the table. */

    precision = ceil(BITS_PER_DIGIT/MP_RADIX_BITS) + 4;
    printf("\n#include \"dpml_private.h\"\n\n");
    printf("\n#ifndef DEFINES\n\n");

    START_GLOBAL_TABLE(TABLE_NAME, offset);

    /* print out hex table values so they fit on an 80 column page.  */

    num_4_ov_pi_digits = ceil(total_bits/BITS_PER_DIGIT);
    digits_per_row = floor(292/(BITS_PER_DIGIT + 16));

    for (n = 0; n < num_4_ov_pi_digits; n++) {
	if (mod(n,digits_per_row) == 0) printf ("\n       ");
        t = bldexp(t, BITS_PER_DIGIT);
        digit = floor(t);
        t -= digit;	/* NB: precision is reduced, but this works! */
        printf(" " DIGIT_HEX_FMT_SPEC ",", digit);
    }
    printf("\n\t");
    END_TABLE;

    printf("\n#else\n");

    printf("\n    /* Describe the trig_reduce interface */\n");
    printf("#   define NUM_INDEX_BITS\t\t%i\n", NUM_INDEX_BITS);
    printf("#   define NUM_OCTANT_BITS\t\t%i\n", NUM_OCTANT_BITS);
    printf("#   define VOC\t\t\t%i\n", VOC);
    printf("#   define BIX\t\t\t%i\n", BIX);
    printf("#   define MIN_OVERHANG\t\t%i\n", MIN_OVERHANG);

    printf("\n    /* Describe the table */\n");
    printf("#   define FOUR_OV_PI_ZERO_PAD_LEN\t%i\n", FOUR_OV_PI_ZERO_PAD_LEN);
    printf("#   define BITS_PER_DIGIT\t\t%i\n", BITS_PER_DIGIT);
    printf("#   define DIGIT_TYPE\t\t\tU_INT_%i\n", BITS_PER_DIGIT);
    printf("#   define SIGNED_DIGIT_TYPE\t\tINT_%i\n", BITS_PER_DIGIT);
    printf("#   undef  FOUR_OVER_PI_TABLE_NAME\n");
    printf("#   define FOUR_OVER_PI_TABLE_NAME\t" STR(TABLE_NAME) "\n");
    printf("    extern const " STR(DIGIT_TYPE) " FOUR_OVER_PI_TABLE_NAME[];\n");

    printf("\n#endif\n\n");

    @end_divert
    @eval my $outText = MphocEval( GetStream( "divertText" ) );		\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Table of 4/pi",				\
                              __FILE__ );				\
             print "$headerText\n\n$outText";

#endif


/******************************************************************************/
/*									      */
/*		Generate code for multi-precision multiplication	      */
/*									      */
/******************************************************************************/

#if defined(MAKE_INCLUDE) && !defined(MAKE_COMMON)

    @divert divertText

    /*
     *  Duplicate some definitions, to ensure consistency, and to make the
     *  include file more self-contained for the human reader's benefit.
     *  None of these definitions are strictly necessary
     */
    printf("\n    /* Describe the trig_reduce interface */\n");
    printf("#   define NUM_INDEX_BITS\t\t%i\n", NUM_INDEX_BITS);
    printf("#   define NUM_OCTANT_BITS\t\t%i\n", NUM_OCTANT_BITS);
    printf("#   define VOC\t\t\t%i\n", VOC);
    printf("#   define BIX\t\t\t%i\n", BIX);
    printf("#   define MIN_OVERHANG\t\t%i\n", MIN_OVERHANG);
    print;

    printf("\n    /* Describe the table */\n");
    printf("#   define FOUR_OV_PI_ZERO_PAD_LEN\t%i\n", FOUR_OV_PI_ZERO_PAD_LEN);
    print;

    printf("\n    /* Describe the datatypes used */\n");
    /*
     *  We print these in comments because they aren't necessarily identical to
     *  the definitions in dpml_private.h and/or PASTE(PLATFORM,_macros.h).
     */
    printf("/*\n");
    printf("#   define BITS_PER_DIGIT\t\t%i\n", BITS_PER_DIGIT);
    printf("#   define DIGIT_TYPE\t\t\tU_INT_%i\n", BITS_PER_DIGIT);
    printf("#   define SIGNED_DIGIT_TYPE\t\tINT_%i\n", BITS_PER_DIGIT);
    printf("#   define BITS_PER_WORD\t\t%i\n", BITS_PER_WORD);
    printf("*/\n");
    print;

/*
 * COMPUTING F*g
 * -------------
 *
 * The goal of step (2) in Algorithm I is to produce a reduced argument that
 * is accurate to P + k bits, where k is minimum overhang in the polynominal
 * evaluation of the trig function.  Also, we need to get the octant bits, o.
 * Consequently, the value of w = F*g, must be accurately computed to P + k + 3
 * bits.  Note however, that if x is close to a multiple of pi/2 the reduced
 * argument will have a large number of leading zeros (in fixed point) and
 * consequently the actual number of required bits in w will depend upon the
 * input argument.  Since computing w is the most time consuming part of the
 * algorithm, we would like to compute the minimum number of bits possible.
 * Specifically, compute w to enough bits so that if x is not near a multiple
 * of pi/2, then the reduced argument will be accurate.  After w is computed,
 * we can check how close the original argument was to pi/2 by examining
 * the number of leading fractional 1's or 0's in w.  If there are too many
 * (i.e. the reduced argument will not have enough significant bits) then we
 * can compute additional bits of w.
 *
 * In order to compute F*g to P + k + 3 bits, we must perform some form of 
 * extended precision arithmetic.  For the sake of uniformity across data
 * types and architectures, the implementation described here computes F*g by
 * expressing F and g as fixed point values in "arrays" of some basic integer
 * unit of computation.  As indicated above, we shall refer to this integer
 * unit as a digit.  The choice of digit is arbitrary, however, it is best if
 * the double length product of two digits is efficiently computed.
 *
 * Now we need to represent w to at least P + k + o bits.  Since F has P
 * significant bits, if we use a finite precision approximation of g, call it
 * g', then the last P bits of the product F*g' are inaccurate.  Therefore
 * we need to represent g' to N = 2*P + k + o bits (as well as compute F*g'
 * to N bits).  If the number of bits in a digit is L, then F and g' must be
 * represented in at least ceil(P/L) and D = ceil(N/L) digits respectively.
 */

	/*
	 *  How many digits are in F, G, and W?
	 *  (num_req_bits is N in the dicussion above)
	 */
	num_f_digits =	ceil(BITS_PER_F_TYPE/BITS_PER_DIGIT);
	num_req_bits =	(2*F_PRECISION + MIN_OVERHANG + NUM_OCTANT_BITS);
	num_w_digits =	ceil(NUM_REQ_BITS/BITS_PER_DIGIT);
	num_g_digits =	num_w_digits;

	printf("#define NUM_F_DIGITS\t%i\n", num_f_digits);
	printf("#define NUM_G_DIGITS\t%i\n", num_g_digits);
	printf("#define NUM_W_DIGITS\t%i\n", num_w_digits);
	printf("#define NUM_REQ_BITS\t%i\n", num_req_bits);
	print;

	/*
	 *  Note that 'num_f_digits = ceil(F_PRECISION/BITS_PER_DIGIT)'
	 *  doesn't suffice; we declare _u.i[NUM_F_DIGITS], and expect to
	 *  get the sign and exponent from one of these 'f' digits.
	 */


/*
 * Now consider the computation of F*g' in terms of digits.  For the purpose
 * of discussion, suppose F requires 2 digits and g' requires 4 digits.
 * Then using "black board" arithmetic F*g' looks like:
 *
 *                              binary point
 *                               |
 *                               |
 *                               |
 *                             +--------+--------+--------+--------+
 *                         g': |   g1   |   g2   |   g3   |   g4   |
 *                             +--------+--------+--------+--------+
 *             +--------+--------+
 *          F: |   F1   |   F2   |
 *             +--------+--------+
 *          ----------------------------------------------------------
 *                               |               +--------+--------+
 *                               |               |      F2*g4      |
 *                               |      +--------+--------+--------+
 *                               |      |      F1*g4      |
 *                               |      +--------+--------+
 *                               |      |      F2*g3      |
 *                             +--------+--------+--------+
 *                             |      F1*g3      |
 *                             +--------+--------+
 *                             |      F2*g2      |
 *                    +--------+--------+--------+
 *                    |      F1*g2      |
 *                    +--------+--------+
 *                    |      F2*g1      |
 *           +--------+--------+--------+
 *           |      F1*g1      | |
 *           +--------+--------+ |
 *                               |
 *          ----------------------------------------------------------
 *           +--------+--------+--------+--------+--------+--------+
 *           |  Not required   |   w1   |   w2   |   w3   |   w4   |
 *           +--------+--------+--------+--------+--------+--------+
 *
 *                              Figure 1
 *                              --------
 *
 * The high two digits of the product are not required since we are interested
 * in the result modulo 8.
 *
 * In general the number of digits used to express g' will contain more
 * than N bits.  Let the number of bits in excess of N be M.  Then if x is
 * close to pi/2 and the number of leading fractional 0's or 1's in F*g' is
 * less than M, F*g' still contains enough significant bits to return an
 * accurate reduced argument.  Note that x will be close to pi/2 if o is
 * odd and z' has leading 1's or o is even and z' has leading 0's.  Note
 * further that the octant bits will be the high order 3 bits of one of the
 * most significant digit of the product.   Therefore there will be loss
 * of significance if w1 (in the picture above) has a binary representation
 * of the form
 *
 *                      +----------------------+
 *                      |xx00000...00000xxxxxxx|
 *                      +----------------------+
 *                              - or -
 *                      +----------------------+
 *                      |xx11111...11111xxxxxxx|
 *                      +----------------------+
 *                         |<-- M+2 -->|
 *
 * These two bit patterns can be detected by add and mask operations.
 *
 * Assuming that M+2 0's or 1's appear in w1, we know that there are not
 * enough significant bits in w to guarantee the accuracy of the answer.
 * Consequently, we need to generate more bits of w.  This can be done by
 * getting the next digit of g, computing the product of that digit with
 * F and adding it into the previous value of w.  This process can be repeated
 * until there are a sufficient number of significant bits.  Note that each
 * additional digit of g will add one digit (L bits) of significance to w.
 *
 * If the processes of adding additional significant bits is implemented in a
 * naive fashion, each time through the loop will require an additional digit
 * of storage.  Consider the situation where the first addition digit has
 * been added to w and there are still insufficient significant bits for
 * an accurate result.  This means that there are at least M + L leading
 * fractional 0's or 1's.  Then w must have the form
 *
 *              |<------------ D + 1 digits ---------->|
 *              +----------+----------+     +----------+
 *              |xx########|######xxxx| ... |xxxxxxxxxx|
 *              +----------+----------+     +----------+
 *                 |<-- M+L+2 -->|
 *
 * where the #'s indicate a string of 0's or 1's.  Since there are more than
 * L consecutive 0's or 1's, we can compress the representation of w by one
 * digit by removing L consecutive 0's or 1's from the first two digits
 * of w.  If this is done w will look like
 *
 *              |<-------------- D digits ------------>|
 *              +----------+----------+     +----------+
 *              |xx#####xxx|xxxxxxxxxx| ... |xxxxxxxxxx|
 *              +----------+----------+     +----------+
 *              -->|M+2|<--
 *
 * Which is the same as for when the first additional digit was added.
 * It follows that we need storage for only D+1 digits of w and a counter
 * indicating the number of additional digits that were added.
 *
 * To recap the above discussion, algorithm I is expanded as follows:
 *
 *               (1) s <-- 0
 *               (2) w <-- first D digits of F*g
 *               (3) if w has less than or equal to M leading fractional
 *                   0's or 1's, go to step 9
 *               (4) add an additional digit of F*g to w
 *               (5) if w has less than L leading leading fractional 0's
 *                   or 1's, go to step 9
 *               (6) Compact w by removing L 0's or 1's
 *               (7) s <-- s + 1
 *               (8) go to step 3.
 *               (9) o <-- integer part of w
 *              (10) z' <-- fractional part of w (taking into account what
 *		            ever compaction took place, i.e. what the current
 *			    value of s is.)
 *              (11) z = z'     if I is even
 *                   = z' - 1 if I is odd
 *              (12) y = z*(pi/4)
 *		
 *				Algorithm II
 *				------------
 *
 * The above loop has two exits.  An exit from step 3 yields an approximation
 * to w containing D digits while an exit from step 5 contains D+1 digits.
 * In the second case, there are fewer than L leading 0's and 1's and this
 * implies that there are enough "good" bits in the first D digits to generate
 * the return values.  Consequently, from either exit, it is sufficient to
 * use only the first D digits of w.
 *
 * The exposition above on the number of leading zeros was a little loose, in
 * that the leading zeros and ones will not always lie entirely in the digit
 * of w.  In general, there can be as many as L-1 extra bits, in which case,
 * we need to examine both the first and second word of w.
 */


/*
 *  bit_loss(s,t) prints the body of a macro definition that evaluates to true
 *  iff the specified bits of W are all 0's or all 1's.  The bits to be tested
 *  are specified by 's' and 't': the highest 's' bits aren't tested, the next
 *  't' bits are tested.  We require s + t <= 2*BITS_PER_DIGIT, so it suffices
 *  to examine (at most) MSD_OF_W and SECOND_MSD_OF_W.
 *
 *	|<---             W             --->
 *      +----------------------------------
 *      |s s s s T T T T T T T u u u u u u ...
 *      +----------------------------------
 *  Key:
 *	 s = high bits that are skipped
 *	 T = these bits are tested
 *	 u = remaining bits are untested
 */
procedure bit_loss(s,t)
{
    auto b, i, m, p;
    b = s + t;			/* bits in both s and t together */
    printf(" /* %i,%i */ ", s, t);

    if (b < BITS_PER_DIGIT) {
        /*
         *  MSD_OF_W suffices
         */
        p = BITS_PER_DIGIT - b;	/* position of low bit of t in MSD_OF_W  */
        i = DIGIT_BIT(p);	/* to 'add 1' at position p */
        m = DIGIT_MASK(t, p);	/* mask of the t bits */
        printf(
	    MAC2 "(((MSD_OF_W + 0x%..16i) & 0x%..16i) == 0)"
	    MAC3, i, m-i);

    } else if (BITS_PER_DIGIT <= s && b < 2*BITS_PER_DIGIT) {
        /*
         *  SECOND_MSD_OF_W suffices
         */
        p = 2*BITS_PER_DIGIT - b;
        i = DIGIT_BIT(p);
        m = DIGIT_MASK(t, p);
        printf(
	    MAC2 "(((SECOND_MSD_OF_W + 0x%..16i) & 0x%..16i) == 0)"
	    MAC3, i, m-i);

    } else if (b <= 2*BITS_PER_DIGIT) {
	/*
	 *  Test bits in both MSD_OF_W and SECOND_MSD_OF_W.
	 */
	p = 2*BITS_PER_DIGIT - b;
	i = DIGIT_BIT(p);
	m = DIGIT_MASK(BITS_PER_DIGIT - p, p);
	printf("( ");
	printf(MAC2);
	if (m == i) printf("/* ");
	printf(
	    "(((SECOND_MSD_OF_W + 0x%..16i) & 0x%..16i) == 0)",
	    i, m-i);
	printf(" && ");
	if (m == i) printf(" */");
	m = DIGIT_MASK(BITS_PER_DIGIT - s, 0);
	printf(
	    MAC2 "(("
		 "( ((SIGNED_DIGIT_TYPE)SECOND_MSD_OF_W >> %i) - MSD_OF_W )"
		 " & 0x%..16i) == 0)",
	    p, m);
	printf(" )" MAC3);

    } else {
        fatal("bit_loss: s + t > 2*BITS_PER_DIGIT");
    }
}

	/*
	 *  Test for m+2 bits of all 0's or all 1's.
	 *  Recall that m is the number of bits we've got in W,
	 *  above and beyond what's required for accurate trig reduction.
	 */
	m = (num_w_digits*BITS_PER_DIGIT - num_req_bits);
	printf("#define W_HAS_M_BIT_LOSS ");
	bit_loss(0, NUM_OCTANT_BITS-1 + m+2);
#if 0
	/*
	 *  Test for L+1 bits of all 0's or all 1's.
	 *  It's insufficient to test just L bits, because then when
	 *  we 'collapse' L bits, we'd lose information -- we wouldn't
	 *  know whether they were all 0's or all 1's that were removed.
	 */
	printf("#define W_HAS_L_BIT_LOSS ");
	bit_loss(NUM_OCTANT_BITS-1, BITS_PER_DIGIT+1);
#endif

/*
 * The above algorithm for computing F*g contains a number of inefficiencies.
 * However, making the algorithm more efficient requires implementing
 * several special code paths to capitalize on specific conditions.  It was
 * felt that the efficiency gained by these special code paths did not warrant
 * the increase in code complexity.  For the sake of completeness however,
 * the possibilities are discussed here.
 *
 * First, the leading 0's and leading 1's cases are not symmetric.  Since
 * g' is obtain from g by truncation, adding more bits to w cannot increase
 * the number of leading 0's.  Consequently, if the initial cause for adding
 * more bits to w was due to leading 0's, one can predict a priori how many
 * additional digits to add to w.
 *
 * If, however, w initially had a string of leading 1's, than subsequent
 * digits of w could bring in more 1's, so determining how many additional
 * digits to process is an iterative procedure.  Also, it is possible that
 * computing additional digits of w will cause a leading 1's string to be
 * turned into a leading 0's string.
 *
 * Second, for any pass through the loop it's possible there are L+1 leading
 * 0's or 1's, but there are still sufficient significant  bits for the result.
 * In this case, the compaction and additional test could be avoided. 
 * (However, this will complicate the cycles to radian conversion.  See below)
 *
 * By way of putting these inefficiencies into perspective, for VAX f and g
 * format or IEEE s and t format, using a 32 bit digit, the initial
 * approximation to w contains 7 and 13 extra bits for single and double
 * precision respectively.  That means on a random basis, the loop is entered
 * less that 1% of the time in single precision and less than 1/100% of the
 * time in double.
 */

/*
 * DIGIT ARITHMETIC
 * ----------------
 *
 * In step (2) of Algorithm 2, we are computing the first D digits of the
 * product F*g.  From figure 1, we see that, (in general) we are computing
 * a 2*L bit product and incorporating it into the sum of previously computed
 * 2*L bit products.  If we think of F, g and w as multi-digit integers with
 * their digits numbered from least significant to most significant (starting
 * at zero) and denoting the i-th digit of F by F(i) and the j-th digit of
 * g by g(j), then the product in figure 1 can be obtained as follows:
 *
 *	t = 0;
 *	for (i = 0; i < num_g_digits; i++) {
 *	    for (j = 0; j < num_F_digits; j++)
 *	        t = t + F[j]*g[i]*2^(j*L)
 *	    w[i] = t mod 2^L;
 *	    t = (t >> L);            
 *	}
 *
 *			      Example 1
 *			      ---------
 *
 * Note that each time through the loop, t is accumulating the product g[i]*F
 * plus "the high digits" of g[i-1]*F.  It follows that t can be represented
 * in (num_F_digits + 1) digits.
 *
 * If F contains n digits, then the sum in the above loops looks like:
 *
 *	+--------+     +--------+--------+--------+--------+     +--------+ 
 *   t: |  t(n)  | ... | t(j+3) | t(j+2) | t(j+1) |  t(j)  | ... |  t(0)  |
 *	+--------+     +--------+--------+--------+--------+     +--------+ 
 *	                                 +--------+--------+
 *	 +                               |    F[j]*g[i]    |
 *	                                 +--------+--------+
 *     ----------------------------------------------------------------------
 *	+--------+     +--------+--------+--------+--------+     +--------+ 
 *   t: | t'(n)  | ... | t'(j+3)| t'(j+2)| t'(j+1)|  t'(j) | ... |  t(0)  |
 *	+--------+     +--------+--------+--------+--------+     +--------+ 
 *
 * Note that t(0) through t(j-1) are unaffected and that t(j+2) through
 * t(n) are affected only by the carry out when computing t'(j+1).  It
 * follows that if we keep the carry out of t'(j+1) as a separate quantity,
 * then the addition in the inner loop only affects two digits of t.  If
 * we denote the separate carry by c(j), the picture on the next iteration of
 * the loop (i.e. replace j by j+1) looks like:
 *
 *	+--------+     +--------+--------+--------+--------+     +--------+ 
 *   t: |  t(n)  | ... | t(j+3) | t(j+2) | t(j+1) |  t(j)  | ... |  t(0)  |
 *	+--------+     +--------+--------+--------+--------+     +--------+ 
 *	                        +--------+--------+
 *	                        |    F(i)*g(j+1)  |
 *	                        +--------+--------+
 *	                        +--------+
 *	 +                      |  c(j)  |
 *	                        +--------+
 *     ----------------------------------------------------------------------
 *	+--------+     +--------+--------+--------+--------+     +--------+ 
 *  t': |  t(n)  | ... | t(j+3) | t'(j+2)| t'(j+1)|  t(j)  | ... |  t(0)  |
 *	+--------+     +--------+--------+--------+--------+     +--------+ 
 *	               +--------+
 *	 +             | c(k+1) |
 *	               +--------+
 *
 *				Figure 1
 *				--------
 *
 * The above gives rise to the notion of a multiply/add primitive that has 5
 * inputs and 3 output: 
 *
 *	Inputs:		N, M	the most and least significant digits
 *				of t that are being added to
 *			C	the carry out from the previous mul/add
 *			A, B	The two digits that are to be multiplied
 *
 *	Outputs:	C'	The carry out of the final sum
 *			N',M'	The updated values of N and M.
 *
 * Recalling that the number of bits per digit is denoted by L, the mul/add
 * primitive is algebraicly defined by:
 *
 *		s  <-- (N + C)*2^L + A*B
 *		M' <-- s % 2^L
 *		N' <-- floor(s/2^L) % 2^L
 *		C' <-- floor(s/2^(2*L)) % 2^L
 *
 * Note that in example 1, there are several special cases of the mul/add
 * macro which might be faster depending on the values of i and j:
 *
 *	   i and j			Special case
 *	------------------	---------------------------------
 *	1) i = 0, j = 0		N = M = C = 0, C' = 0
 *	2) i = 0, j < n-1	N = C = 0, C' = 0
 *	3) i = 0, j = n-1	N = C = 0, C' = 0 and N' not needed
 *
 *	4) i > 0, j = 0		C = 0	
 *	5) i > 0, j < n-1	general case
 *	6) i > 0, j = n-1	N = 0, C' not needed
 *
 *	7) i + j = n-2		C' not needed
 *	8) i + j = n-1		C, N, C' and N' not needed
 *		
 * Note that cases 3 and 7 are functionally identical.  For purposes of this
 * discussion we will use the mnemonic XMUL to refer to producing a 2*L-bit
 * product from 2 L-bit digits and XADD/XADDC to refer to the addition of one 
 * 2*L-bit integer to another without/with producing a carry out.  With this
 * naming convention we denote the following 6 mul/add operations that
 * correspond to the 6 special cases as follows:
 *
 *	case	mul/add operator name
 *	----	---------------------
 *	 1)	 XMUL(A,B, N',M')
 *	 2)	 XMUL_ADD(A,B,M,N',M')
 *	 3)	 MUL_ADD(A,B,M,M')
 *	 4)	 XMUL_XADDC(A,B,N,M,C',N',M')
 *	 5)	 XMUL_XADDC_W_C_IN(C,A,B,N,M,C',N',M')
 *	 6)	 XMUL_XADD_W_C_IN(N,M,C,A,B,C',N',M')
 *
 * [XMUL_XADD_W_C_IN is described with more parameters than are actually used.]
 * [There are 8 cases, two of which are "functionally identical".  That leaves
 *  7 cases, but only 6 have a "mul/add operator name".]
 *
 * The mphoc code following these comments generates macros for computing the
 * initial multiplication of F*g as a function of the number of digits in both
 * F and g.  It assumes that NUM_F_DIGITS <= NUM_G_DIGITS
 */

    /*
     * The description of digit arithmetic above indicates that we need
     * NUM_F_DIGITS + 1 temporary locations to hold the intermediate products
     * and sums plus one extra for dealing with carries.  For adding
     * additional digits of the product F*g, we need at least 3 temporary
     * locations.
     */

    num_t_digits = max(3, num_f_digits + 2);

    /*
     *  Print macros for declaring the appropriate number of digits
     */
#   define PRINT_DECL_DEF(tag,name,k)	\
	/* define 'name'0 thru 'name''k-1' */ \
	printf("#define " tag STR(name) "0"); \
	for (i = 1; i < k; i++) printf(", " STR(name) "%i", i); \
	printf("\n")
    PRINT_DECL_DEF("G_DIGITS\t", g, num_g_digits);
    PRINT_DECL_DEF("F_DIGITS\t", F, num_f_digits);
    PRINT_DECL_DEF("TMP_DIGITS\t", t, num_t_digits);
#   undef PRINT_DECL_DEF
    print;

    /*
     * Print macros for referencing the most significant digits of F and g
     * as well as declaring the high temporary as the carry digit.
     */

    printf("#define MSD_OF_F\tF%i\n", num_f_digits - 1);
    printf("#define MSD_OF_W\tg%i\n", num_w_digits - 1);
    if (num_w_digits == 1)
        printf("#define SECOND_MSD_OF_W\tEXTRA_W_DIGIT\n");
    else
        printf("#define SECOND_MSD_OF_W\tg%i\n", num_w_digits - 2);
    printf("#define CARRY_DIGIT\tt%i\n", num_t_digits - 1);
    print;

    /*
     *  _PDP_SHUFFLE's will be needed for VAX floating-point datatypes
     *  if a DIGIT_TYPE crosses a 16-bit boundary.
     */
#   if (VAX_FLOATING && BITS_PER_DIGIT > 16)
	needs_pdp_shuffle = 1;
#   else
	needs_pdp_shuffle = 0;
#   endif

    /*
     *  GET_F_DIGITS(x) fetches the initial digits of f from x
     */
    printf("#define GET_F_DIGITS(x)"
	MAC2 "{"
	MAC2 "union { DIGIT_TYPE i[NUM_F_DIGITS]; F_TYPE f; } _u;"
        MAC2 "_u.f = x;");

    if (BITS_PER_F_TYPE < BITS_PER_DIGIT) {

        printf(MAC2 "F0 = _u.i[0] & DIGIT_MASK(%i, 0);", BITS_PER_F_TYPE);
	if (needs_pdp_shuffle) printf("_PDP_SHUFFLE(F0);");

    } else {

#       if (ENDIANESS == big_endian) || (VAX_FLOATING)
            j = 0;
            j_inc = 1;
#       else
            j = num_f_digits - 1;
            j_inc = -1;
#       endif

        for (i = num_f_digits - 1; i >= 0; i--) {
            printf(MAC2 "F%i = _u.i[%i]; ", i, j);
	    if (needs_pdp_shuffle) printf("_PDP_SHUFFLE(F%i);", i);
            j += j_inc;
        }

    }

    printf(MAC2 "}");
    printf(MAC3);


    /*
     *  GET_G_DIGITS_FROM_TABLE fetches the initial digits of g
     *  (and the extra_digit) from the table.
     */
    printf("#define GET_G_DIGITS_FROM_TABLE(p, extra_digit)");
#if 0
    /* Better performance with DEC C -- don't auto-increment! */
    for (i = num_g_digits - 1; i >= 0; i--)
        printf(MAC2 "g%i = p[%i]; ", i, num_g_digits - 1 - i);
    printf(MAC2 "extra_digit = p[%i]; ", num_g_digits);
    printf(MAC2 "p = p[num_g_digits]");
#else
    for (i = num_g_digits - 1; i >= 0; i--)
        printf(MAC2 "g%i = *p++; ", i);
    printf(MAC2 "extra_digit = *p++");
#endif
    printf(MAC3);

    /*
     *	Generate macro that aligns g bits
     *
     *	LEFT_SHIFT_G_DIGITS(lshift,BITS_PER_WORD-lshift,extra_digit) ==
     *		g = (g << lshift) | (extra_digit >> (BITS_PER_WORD-lshift)
     */
    printf("#define LEFT_SHIFT_G_DIGITS(lshift, rshift, extra_digit)");
    for (i = num_g_digits - 1; i > 0; i--)
        printf(MAC2 "g%i = (g%i << (lshift)) | (g%i >> (rshift));",
                       i,     i,                i-1);
    printf(MAC2 "g0 = (g0 << (lshift)) | (extra_digit >> (rshift))");
    printf(MAC3);


    /*
     *	MULTIPLY_F_AND_G_DIGITS(c) ==
     *		g = F * g
     */
    printf("#define MULTIPLY_F_AND_G_DIGITS(c)");

    if (num_g_digits == 1)

	printf("\t" "g0 = F0*g0\n");

    else if (num_f_digits == 1) {

        printf(MAC2 "XMUL(F0,g0,t0,g0)");

        for (i = 1; i < num_w_digits - 1; i++)
            printf(sMAC2 "XMUL_ADD(F0,g%i,t0,t0,g%i)", i, i);

        printf(sMAC2 "MUL_ADD(F0,g%i,t0,g%i)", i, i);

    } else {

        /* Get first product */
        printf(MAC2 "XMUL(g0,F0,t1,t0)");

        /*
         * Accumulate additional products until we use up all of the F
         * digits, or we no longer need the high digit of the XMUL.
         */

        msd_of_mul_add = 1;
        for (i = 1; i < num_f_digits; i++) {
            msd_of_mul_add++;
            if (msd_of_mul_add >= num_w_digits)
                break;
            printf(sMAC2 "XMUL_ADD(g0,F%i,t%i,t%i,t%i)", i, i, i+1, i);
        }

        /*
         * If we no longer needed the high digit of the XMUL before using
         * all of the F digits, add in the low bits of the final product.
         */
        if (msd_of_mul_add >= num_w_digits)
            printf(sMAC2 "MUL_ADD(g0,F%i,t%i)", i, i);

        /* Move the low bits of t to w */
	printf(sMAC2 "g0 = t0");

        /*
         * Now multiply by the remaining digits of g.  In the code that
         * follows, the digits of t are reused each time through the loop
         * modulo (NUM_F_DIGITS + 1).  For example, suppose NUM_F_DIGITS
         * is 3.  In the multiplications above, the digits of t (in most to
         * least significant order were t[3]:t[2]:t[1]:t[0].  In the first
         * iterations below the order is t[0]:t[3]:t[2]:t[1], and on the
         * next iteration t[1]:t[0]:t[3]:t[2], and so on.  The variables
         * hi, lo and first are used to track the order of the digits and
         * the least significant digit.  Note that the high tmp digit is
         * used as a carry digit.
         */

        for (i = 0; i < num_t_digits - 1; i++)
            next_index[i] = i + 1;
        next_index[num_t_digits - 2] = 0;

#   define UPDATE_DIGIT_INDEX(lo,hi)	lo = hi; hi = next_index[hi]

        first = 0;
        for (i = 1; i < num_w_digits; i++) {

            first = next_index[first];
            lo = first;
            hi = next_index[lo];
            msd_of_mul_add = i + 2;	/* msd is the carry out */

            if (msd_of_mul_add < num_w_digits)
                printf(sMAC2 "XMUL_XADDC(g%i,F0,t%i,t%i,c,t%i,t%i)",
                                              i,    hi, lo,   hi, lo);
            else if (msd_of_mul_add <= num_w_digits)
                printf(sMAC2 "XMUL_XADD(g%i,F0,t%i,t%i,t%i,t%i)",
                                             i,    hi, lo, hi, lo);
            else
                printf(sMAC2 "MUL_ADD(g%i,F0,t%i,t%i)",
                                          i,    lo,  lo);
            UPDATE_DIGIT_INDEX(lo,hi);

            for (j = 1; j < num_f_digits; j++) {

                msd_of_mul_add++;
                if (msd_of_mul_add < num_w_digits) {

                    if (j == (num_f_digits - 1))
                        printf(sMAC2 
                         "XMUL_XADDC(g%i,F%i,c,t%i,c,t%i,t%i)",
                                       i,  j,   lo,   hi, lo);
                    else
                        printf(sMAC2 
                         "XMUL_XADDC_W_C_IN(g%i,F%i,t%i,t%i,c,c,t%i,t%i)",
                                              i,  j, hi, lo,    hi, lo);

                } else if (msd_of_mul_add <= num_w_digits) {

                    if (j == (num_f_digits - 1))
                        printf(sMAC2 
                         "XMUL_XADD(g%i,F%i,c,t%i,t%i,t%i)",
                                      i,  j,   lo, hi, lo);
                    else
                        printf(sMAC2 
                         "XMUL_XADD_W_C_IN(g%i,F%i,t%i,t%i,c,t%i,t%i)",
                                             i,  j, hi, lo,   hi, lo);

                } else if (msd_of_mul_add <= num_w_digits + 1) {

                    printf(sMAC2 "MUL_ADD(g%i,F%i,t%i,t%i)",
                                            i,  j, lo, lo);
                } else
                    break;
                UPDATE_DIGIT_INDEX(lo,hi);
            }

            /* Move low digit of t to W */
	    printf(sMAC2 "g%i = t%i", i, first);
        }
    }
    print;
    print;

    /*
     * Generate the macro that multiplies F by an additional digit of g
     * and adds the product to w.
     */

    printf("#define GET_NEXT_PRODUCT(g, w, c)");
    if (num_g_digits == 1)

	printf("\t" "XMUL_XADD(g,F0,g0,w,g0,w)");

    else {

        printf(MAC2 "XMUL_XADDC(g,F0,g0,(DIGIT_TYPE)0,c,g0,w)");

        msd_of_mul_add = 1;
        for (i = 1; i < num_f_digits; i++) {
	    j = i-1;

            if (msd_of_mul_add < num_w_digits)
                printf(sMAC2
                  "XMUL_XADDC_W_C_IN(g,F%i,g%i,g%i,c,c,g%i,g%i)",
                                         i,  i,  j,      i, j);
            else if (msd_of_mul_add <= num_w_digits + 1)
                printf(sMAC2
                  "XMUL_XADD_W_C_IN(g,F%i,g%i,g%i,c,g%i,g%i)",
                                        i,  i,  j,    i, j);
            else if (msd_of_mul_add <= num_w_digits + 2)
                printf(sMAC2
                  "MUL_ADD(g,F%i,g%i,g%i)",
                               i,  j,  j);
            else
                break;
            msd_of_mul_add++;
        }
	printf(";");

        /*
         * If there was a carry out on the last add and we are not past the
         * last w digit, then the carry has to be propagated to the remaining
         * w digits as necessary.
         */

        if (msd_of_mul_add < num_w_digits) {
            if (msd_of_mul_add != (num_w_digits - 1)) {
                printf(MAC2 "if (c) ");
                i = msd_of_mul_add;
                while (i < num_w_digits - 1)
                    printf(MAC2 "if (++g%i == 0) ", i++);
                printf(MAC2 "g%i++", i);
            } else
                printf(MAC2 "g%i += c", i);
        }
    }
    printf(MAC3);

    /* Generate the macro that shifts w left by 1 digit */

    printf("#define LEFT_SHIFT_W_ONE_DIGIT(extra_w_digit)");
    for (i = num_w_digits - 1; i > 0; i--)
        printf(MAC2 "g%i = g%i;", i, i-1);
    printf(MAC2 "g0 = extra_w_digit" MAC3);

    print;

    @end_divert

#endif

/*
 * CONVERTING W TO FLOATING POINT
 * ------------------------------
 *
 * In converting w to floating point, we have to convert one digit at a
 * time in something like a Horner's scheme polynomial evaluation.
 *
 *      floating_w = S*S2*(w1 + S1*(w2 + S1*(w3 + S1*w4)))
 *
 * where S1 is 1/2^L and S2 = 1/2^(L-o) and S is the scale factor required
 * to compensate for the compaction of w during the looping phase.
 *
 * In addition to converting w to floating point format, we want to convert
 * from cycles to radians.  This involves multiplying by pi/4.  Thus the
 * reduced argument looks something like
 *
 *      reduced_arg = (pi/4)*S*S2*(w1 + S1*(w2 + S1*(w3 + S1*w4)))
 *
 * Since S2 and pi/4 are compile time constants, we can combine them and
 * eliminate one multiply.  Thus
 *
 *      reduced_arg = (S2*pi/4)*S*(w1 + S1*(w2 + S1*(w3 ... S1*wn)))    (2)
 *
 * Finally, note that S and S2 will be powers of two, so that the multiply
 * can be done either by adjusting the exponent or by multiplication.
 *
 * Recall that we would like to return the reduced argument with at least k
 * extra bits.  If there is a back-up data type, then the reduced argument
 * can be returned in that data type and equation (2) can be applied directly.
 * If there is no back-up data type, then both the conversion to floating
 * point and the conversion to radians must be carried out with some care
 * in the base precision.  Specifically the approach we will take will be to
 * break the floating point value of w and pi/4 into hi and lo pieces and
 * compute the reduced argument as
 *
 *              reduced_arg = (pi/4)*w
 *                          = (pi_ov_4_hi + pi_ov_4_lo)*(w_hi + w_lo)
 *                          = pi_ov_4_hi*w_hi + (pi_ov_4_lo*w_hi +
 *                               pi_ov_4_hi*w_lo + pi_ov_4_lo*w_lo)
 *                          = pi_ov_4_hi*w_hi + (pi_ov_4_lo*w_hi + pi_ov_4*w)
 *                          = r1 + r2
 *
 * where pi_over_4_hi and w_hi are chosen so that r1 is exact.  Having obtained
 * r1 and r2, we compute the high p bits of the reduced argument, r_hi, and the
 * remaining low bits, r_lo, as
 *
 *              r_hi = r1 + r2,         r_lo = r2 - (r_hi - r1)
 *
 * Recall from the description above, that at the point where the conversion to 
 * floating point takes place, w has less than L leading 0's or 1's.  If the
 * digit size and precision have the "right" relationship, it is relatively
 * easy to determine a short sequence of int ==> float converts that implement
 * the above algorithm.  However, if the digit size is small, since the number
 * of leading zeros is not known at compile time, the necessary sequence of 
 * conversions can be complicated.  To alleviate this complication, we will
 * normalize the bits of w.  This costs a little in performance in the case
 * where there is backup precision, but it greatly enhances portability.  The
 * normalization we will use has the "octant" bits in the high 3 bits of the
 * msd of w.  Assuming this normalization, the first n digits of w will
 * contain n*L - o good bits.  Since we want p + k good bits in the final
 * result, it follows that n = ceil(p+k+o).
 */

#if defined(MAKE_INCLUDE) && !defined(MAKE_COMMON)

    @divert -append divertText

    num_significant_w_digits = ceil((F_PRECISION + MIN_OVERHANG +
                                     NUM_OCTANT_BITS)/BITS_PER_DIGIT);

    n = min(num_significant_w_digits + 1, num_w_digits);
    lsd_of_w = num_w_digits - num_significant_w_digits;
    printf("#define LEFT_SHIFT_SIGNIFICANT_W_DIGITS(lshift,rshift)");
    if (num_w_digits == 1) {

        printf(MAC2 "g0 = (g0 << (lshift)) | (SECOND_MSD_OF_W >> (rshift));");
        printf(MAC2 "SECOND_MSD_OF_W <<= (lshift)\n\n");

    } else {

        for (i = num_w_digits - 1; i > lsd_of_w; i--)
            printf(MAC2 "g%i = (g%i << (lshift)) | (g%i >> (rshift));",
                           i,     i,                i-1);

        if (i > 0)
            printf(MAC2 "g%i = (g%i << (lshift)) | (g%i >> (rshift))",
                           i,     i,                i-1);
        else
            printf(MAC2 "g0 = (g0 << (lshift)) | (EXTRA_W_DIGIT >> (rshift))");
	printf(MAC3);

    }

#   if PRECISION_BACKUP_AVAILABLE

	/*
	 *  CVT_W_TO_B_TYPE(t) does as it says -- w is converted to a B_TYPE t.
         *  The 'binary point' in this conversion is just after MSD_OF_W (which
	 *  is treated as a signed digit).
	 */
        printf("#define CVT_W_TO_B_TYPE(t)");
	printf(MAC2 "t = TO_B_TYPE((SIGNED_DIGIT_TYPE) g%i)", num_w_digits-1);
        j = 0;
        for (i = num_w_digits-2; i >= lsd_of_w; i--)
            printf(MAC2 "  + SCALE_TAB(%i)*TO_B_TYPE(g%i)", j++, i);
        printf(MAC3);

	overhang = B_PRECISION - F_PRECISION;

#   else

	/*
	 *  CVT_W_TO_HI_LO(hi, lo, tmp_digit) converts w to two F_TYPEs:
	 *  hi and lo, with the same conventions as CVT_W_TO_B_TYPE.
	 *  The high part is 'shortened' to half_precision, to make
	 *  hi*PI_OVER_4_HI exact (PI_OVER_4_HI = bround(pi/4,half_precision). 
	 *
	 *  For hi, we'll take the 1+half_precision high bits of w (recall
	 *  that the highest bit is just a 'sign' bit).
	 */
        half_precision = floor(F_PRECISION/2);
	hi_bits = 1+half_precision;
        num_digits_per_half_precision = ceil(hi_bits/BITS_PER_DIGIT);
        extra_bits = num_digits_per_half_precision*BITS_PER_DIGIT - hi_bits;
	/*
	 *  The digit containing the lowest of the hi_bits is split --
	 *  move the low bits to tmp_digit, and keep the rest.
	 */
        half_precision_digit = num_w_digits - num_digits_per_half_precision;
        printf("#define CVT_W_TO_HI_LO(hi, lo, tmp_digit)");
	printf(MAC2 "tmp_digit = g%i & " DIGIT_HEX_FMT_SPEC ";",
            half_precision_digit, (1 << extra_bits) - 1);
        printf(MAC2 "g%i ^= tmp_digit;", half_precision_digit);
	/*
	 *  Now compute hi and lo.  Note that we needn't worry about inexact
	 *  conversions from DIGIT_TYPE to F_TYPE.
	 */
	if (half_precision_digit < lsd_of_w) fatal("we never set lo");
	j = 0;
        for (i = num_w_digits - 1; i >= lsd_of_w; i--) {
            if (j == 0)
		printf(MAC2 "hi = TO_F_TYPE((SIGNED_DIGIT_TYPE) g%i)", i);
	    else
                printf(MAC2 "   + SCALE_TAB(%i)*g%i", j-1, i);
	    if (i == half_precision_digit) {
		printf(sMAC2 "lo = TO_F_TYPE(tmp_digit)");
		if (j > 0) printf("*SCALE_TAB(%i)", j-1);
	    }
	    j++;
	}
        printf(MAC3);

	overhang = half_precision + F_PRECISION;

#endif

    if (MIN_OVERHANG >= overhang)
	fatal("MIN_OVERHANG too big");

    /* Make sure there are enough good bits in pi/4 */
    precision = ceil(3/2*(F_PRECISION+MIN_OVERHANG)/MP_RADIX_BITS) + 4;
    pi_over_4 = pi/4;

    START_STATIC_TABLE(TABLE_NAME, offset);
    pi_offset = BYTES(offset);


#   if PRECISION_BACKUP_AVAILABLE

#       define PRINT_ENTRY(value)	PRINT_1_TYPE_ENTRY(B_CHAR,value,offset)
#       define ENTRY_TYPE		B_TYPE

        TABLE_COMMENT("pi/4");
        PRINT_ENTRY(pi_over_4);

#   else

#       define PRINT_ENTRY(value)	PRINT_1_TYPE_ENTRY(F_CHAR,value,offset)
#       define ENTRY_TYPE		F_TYPE
    
        hi = bround(pi_over_4, half_precision);
        lo = pi_over_4 - hi;
        TABLE_COMMENT("pi/4 in hi and lo pieces");
        PRINT_ENTRY(hi);
        pi_lo_offset = BYTES(offset);
        PRINT_ENTRY(lo);

#   endif

    scale_offset = BYTES(offset);
    s1 = 2^(-BITS_PER_DIGIT);
    t = s1;

    TABLE_COMMENT("Powers of 2^-BITS_PER_DIGIT");
    for (i = 1; i < num_significant_w_digits; i++) {
        PRINT_ENTRY(t);
        t *= s1;
    }

    END_TABLE;
    
    printf("#define TRIG_RED_TABLE_NAME\t" STR(TABLE_NAME) "\n");

#   define AT_OFFSET  "(((char*)TRIG_RED_TABLE_NAME) + %i)"
#   if PRECISION_BACKUP_AVAILABLE
        printf("#define PI_OVER_4 "
	    "*((" STR(ENTRY_TYPE) "*)" AT_OFFSET ")\n", pi_offset);
#   else
        printf("#define PI_OVER_4_HI "
	    "*((" STR(ENTRY_TYPE) "*)" AT_OFFSET ")\n", pi_offset);
        printf("#define PI_OVER_4_LO "
	    "*((" STR(ENTRY_TYPE) "*)" AT_OFFSET ")\n", pi_lo_offset);
#   endif

    printf("#define SCALE_TAB(j) " 
	"*(((" STR(ENTRY_TYPE) "*)" AT_OFFSET ") + j)\n", scale_offset);

    @end_divert
    @eval my $outText    = MphocEval( GetStream( "divertText" ) );	\
          my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),         \
                           "Definitions multi-preicion arithmetic " .,	\
                              "used for large argument reduction",	\
                                __FILE__ );				\
             print "$headerText\n\n$outText";

#endif

#if !defined(MAKE_INCLUDE)
#   define DEFINES
#   include STR(BUILD_FILE_NAME)
#endif

#define	TMP_DIGIT	t0
#define	EXTRA_W_DIGIT	t1

#define	TO_B_TYPE(x)	((B_TYPE) (x))
#define	TO_F_TYPE(x)	((F_TYPE) (x))


/*
 * If the DIGIT_TYPE and WORD are not the same size, or VAX data types are
 * being used, then the bit positions and masks used for accessing the fields
 * of floating point values must be adjusted. I.e. we can't use the definitions
 * in f_format.h, we need to have new definitions, relative to the digit size.
 */

#define DIV_UP(x,y)	   (((x)+(y)-1)/(y))	/* = ceil(x/y) */
#define _F_EXP_POS	   (BITS_PER_F_TYPE - F_EXP_WIDTH - 1)
#define	DIGITS_PER_F_TYPE  DIV_UP(BITS_PER_F_TYPE, BITS_PER_DIGIT)
#define EXP_DIGIT_POS   (_F_EXP_POS - (DIGITS_PER_F_TYPE - 1)*BITS_PER_DIGIT)

#if (EXP_DIGIT_POS < 0) || (EXP_DIGIT_POS + F_EXP_WITDH > BITS_PER_DIGIT)
#   error "Digit size inappropriate for floating point data type"
#endif

#if !defined F_ENTRY_NAME
#   define F_ENTRY_NAME	F_TRIG_REDUCE_NAME
#endif

/*
 * For VAX data types, the fraction field needs to be "unshuffled" if the
 * digit size is greater than 16 bits.  The following definitions are
 * essentially the same as the code in f_format.h that defines the
 * PDP_SHUFFLE macro, except that here, the choices are made on DIGIT_SIZE
 * rather than word size.
 */

#if (VAX_FLOATING && BITS_PER_DIGIT > 16)

#   if (BITS_PER_DIGIT == 32)
#       define _PDP_SHUFFLE(i) \
	    (i) = ( ((DIGIT_TYPE)(i)<<16) \
	          | ((DIGIT_TYPE)(i)>>16) )

#   elif (BITS_PER_F_TYPE == 32 && 32 <= BITS_PER_DIGIT)
#       define _PDP_SHUFFLE(i) \
            (i) = ( ((DIGIT_TYPE)((i) & 0xffff) << 16) \
		  | ((DIGIT_TYPE)(i) >> 16) )

#   elif (BITS_PER_DIGIT == 64)
#       define _PDP_SHUFFLE(i) \
	    (i) = (  ((DIGIT_TYPE)(i) << 48) \
                  |  ((DIGIT_TYPE)(i) >> 48) \
                  | (((DIGIT_TYPE)(i) >> 16) & ((DIGIT_TYPE)0xffff << 16)) \
                  | (((DIGIT_TYPE)(i) << 16) & ((DIGIT_TYPE)0xffff << 32)) )
#   else
#       error "_PDP_SHUFFLE macro not defined for this digit size."
#   endif
#else
#   define _PDP_SHUFFLE(i)	/* a No-op */
#endif

/*
 *  After the initial multiply, we'll grab some bits, either one or two digits,
 *  depending on what will fit in a WORD.  We expect these bits will suffice
 *  for 'result', but we check to be sure.
 */
#if BITS_PER_WORD >= 2*BITS_PER_DIGIT
#   define GRAB_HIGH (((WORD)MSD_OF_W << BITS_PER_DIGIT) | SECOND_MSD_OF_W)
#   define NUM_GRABBED_BITS (2*BITS_PER_DIGIT)
#else
#   define GRAB_HIGH MSD_OF_W
#   define NUM_GRABBED_BITS BITS_PER_DIGIT
#endif
#if          NUM_OCTANT_BITS + NUM_INDEX_BITS > NUM_GRABBED_BITS
#   error -- NUM_OCTANT_BITS + NUM_INDEX_BITS > NUM_GRABBED_BITS
#endif

/*
 *  This is a refugee from dpml_private.h
 */
#define F_EXP_OF_ONE (F_EXP_BIAS - F_NORM)

/******************************************************************************/

WORD
F_ENTRY_NAME(F_TYPE x,
#   if VOC
	WORD voc,
#   endif
#   if BIX
	WORD bix,
#   endif
	F_TYPE *hi, F_TYPE *lo)
{
    WORD octant, offset, scale, j;
    DIGIT_TYPE F_DIGITS;		/* declare F0, ... Fm		*/
    DIGIT_TYPE G_DIGITS;		/* declare g0, ... gn		*/
    DIGIT_TYPE TMP_DIGITS;		/* declare t0, ... tm+1		*/
    DIGIT_TYPE next_g_digit;
    const DIGIT_TYPE *p;

    /*
     *  We have the following definitions and identities:
     *
     *	    2^bexp(t) = 2*msb(t) = 2*2^floor(log2(abs(t))), and
     *	    bldexp(x,k) = x*2^k.
     */

    /*
     *	Get the fraction bits and exponent field as integers into F.
     *	Isolate the biased exponent and sign.
     *	Clear the sign and exponent bits and restore the hidden bit.
     *  Get the exponent (sans sign), and add in bix.
     */
    GET_F_DIGITS(x);
#if 0
    TMP_DIGIT = MSD_OF_F & DIGIT_MASK(F_EXP_WIDTH + 1, EXP_DIGIT_POS);
#else
    TMP_DIGIT = MSD_OF_F & DIGIT_MASK(BITS_PER_DIGIT - EXP_DIGIT_POS,
						       EXP_DIGIT_POS);
#endif
    MSD_OF_F = (MSD_OF_F ^ TMP_DIGIT) | DIGIT_BIT(EXP_DIGIT_POS);
    TMP_DIGIT = (TMP_DIGIT >> EXP_DIGIT_POS) & DIGIT_MASK(F_EXP_WIDTH,0);
#   if BIX
    TMP_DIGIT += bix;
#   endif
    /*
     *  As a multi-precision integer,
     *      F == abs(x)/msb(x) * 2^(F_PRECISION-1)
     *	      == abs(x)*2^(1-bexp(x)) * 2^(F_PRECISION-1)
     *	      == abs(x)*2^(F_PRECISION-bexp(x))
     *  TMP_DIGIT == bexp(x)-1 + F_EXP_OF_ONE + bix
     */

    /*
     *	Use the exponent to get the bit offset of the first
     *	interesting bit in the 4/pi table.
     */
    offset = TMP_DIGIT - F_EXP_OF_ONE - (F_PRECISION-1)
		- (NUM_OCTANT_BITS-1)
		+ FOUR_OV_PI_ZERO_PAD_LEN;

    /*
     *  A negative offset would have us access memory before the start of
     *	the 4/pi table.  This indicates that the x was pretty small already,
     *	so we'll make a quick exit.
     *	NB:  We neither test nor account for negative x.  Nut we should.
     */
    if (offset < 0) {
        *hi = x;
        *lo = 0;
#	if VOC
	    return voc << NUM_INDEX_BITS;
#	else
	    return 0;
#	endif
    }

    /*
     *	Get the address of the digit containing the first interesting bit,
     *  and its bit offset within that digit.  Load G from the the table,
     *	shifting the digits by that bit offset, so that the interesting bit
     *	will become the high bit of G.
     */
    DIV_REM_BY_L(offset, j, offset);
    p = &FOUR_OVER_PI_TABLE_NAME[j];
    GET_G_DIGITS_FROM_TABLE(p, next_g_digit);
    if (offset) {
        j = BITS_PER_DIGIT - offset;
        LEFT_SHIFT_G_DIGITS(offset, j, next_g_digit);
    }
    /*
     *  When g is interpreted as a fixed-point binary number,
     *  with binary point at the left, we now have: ...TBS....
     *
     *  We'll multiply g by F, modulo 1.  Recall that
     *	    F == abs(x)*2^(F_PRECISION-bexp(x))
     *
     *  mod(F*g,1) == ...TBS....
     */

    /*
     *  The extended-precision multiply: w = F*g.
     */
    MULTIPLY_F_AND_G_DIGITS( /* F_DIGITS, G_DIGITS, T_DIGITS, */ CARRY_DIGIT );

    /* 
     *	Add in the variable octant.
     */
#   if VOC
    MSD_OF_W += (DIGIT_TYPE)voc << (BITS_PER_DIGIT - NUM_OCTANT_BITS);
#   endif

    /*
     *  Grab the high bits of w (save them in octant).
     *  Then sign-extend the low octant bit.
     */
    octant = GRAB_HIGH;
    TMP_DIGIT = MSD_OF_W << (NUM_OCTANT_BITS - 1);
    MSD_OF_W = ((SIGNED_DIGIT_TYPE) TMP_DIGIT) >> (NUM_OCTANT_BITS - 1);

    scale = 0;

    do {
	/*
	 *  If there isn't enough significance in w, then:
	 *  get more bits from the table, form the new digit into TMP_DIGIT,
	 *  and add the partial product F*TMP_DIGIT to w.
	 *
	 *  Once W_HAS_M_BIT_LOSS becomes false, it'll stay false, and we'll
	 *  do no more partial products.  But we'll stay in the loop so the
	 *  left shifts will ensure MSD_OF_W is not all 0's or 1's.
	 */
        if (W_HAS_M_BIT_LOSS) {
            TMP_DIGIT = next_g_digit;
            next_g_digit = *p++;
            if (offset) TMP_DIGIT = (TMP_DIGIT << offset) | (next_g_digit >> j);
            GET_NEXT_PRODUCT(TMP_DIGIT, EXTRA_W_DIGIT, CARRY_DIGIT);
	}

        /*
         *  We're done if the there are fewer than L+1 bits of 0's or 1's.
         */
	TMP_DIGIT = (SIGNED_DIGIT_TYPE)SECOND_MSD_OF_W >> (BITS_PER_DIGIT-1);
	if (MSD_OF_W != TMP_DIGIT) break;

        /*
         *  Shift w left a digit, and keep w*2^scale invariant.
         */
        LEFT_SHIFT_W_ONE_DIGIT(EXTRA_W_DIGIT);
        scale -= BITS_PER_DIGIT;

    } while (1);


    /*
     *  GET_NEXT_PRODUCT may have produced carrys into MSD_OF_W which need to
     *  be reflected in 'octant'.  First, get the high bits of w, aligning the
     *  binary point with what we have in 'octant'.
     */
#   define smaller_of(a,b) ((a) < (b) ? (a) : (b))
    offset = GRAB_HIGH;
    offset >>= smaller_of(-scale, BITS_PER_WORD-1);
    /*
     *  Determine the amount by which to increase 'octant', and increase it.
     *  Then shift 'octant' right, to discard the extra bits it's carrying.
     */
    offset = (offset - octant) & MAKE_MASK(NUM_GRABBED_BITS-NUM_OCTANT_BITS, 0);
    octant += offset;
    octant >>= NUM_GRABBED_BITS - (NUM_OCTANT_BITS + NUM_INDEX_BITS);

    /*
     *  Increase the significant bits in w by shifting it left until (so that)
     *  the two high bits of w differ.  For 'positive' MSD_OF_W, the high 1 bit
     *  is at bit position floor(log2(MSD_OF_W)); for 'negative' MSD_OF_W, the
     *  high 0 bit is at bit position floor(log2(~MSD_OF_W)).  We compute
     *  j = bit position + 2, so the sign-extended low j bits equal MSD_OF_W;
     *  thus, we can safely shift w left by BITS_PER_DIGIT - j bits.
     *
     *	[In truth, if the conversion to F_TYPE rounds up to the next 'octave',
     *  the two high bits of w won't differ, but the third will; we'll have
     *  one less bit of significance, but that's okay].
     *
     *  The standard trick for finding the highest bit set in an unsigned int
     *  is to convert to floating, and extract the exponent.  This trick won't
     *  work if the integer is zero.
     */
    TMP_DIGIT = MSD_OF_W ^ ((SIGNED_DIGIT_TYPE)MSD_OF_W >> (BITS_PER_DIGIT-1));
    j = 1;
    if (TMP_DIGIT) {
        F_TYPE f_type_tmp;
        f_type_tmp = (SIGNED_DIGIT_TYPE)TMP_DIGIT;
        GET_EXP_WORD(f_type_tmp, j);
        j = ((j >> F_EXP_POS) & MAKE_MASK(F_EXP_WIDTH, 0)) - F_EXP_OF_ONE + 2;
    }

    offset = BITS_PER_DIGIT - j;
    if (offset) {
        LEFT_SHIFT_SIGNIFICANT_W_DIGITS(offset, j);
        scale -= offset;
    }

    /*
     *  We scaled x up by 2^bix; now scale down by bix.
     */
#   if BIX
    scale -= bix;
#   endif

    /*
     *  Originally the 'binary point' was after the high NUM_OCTANT_BITS in w.
     *  CVT_W_TO_{B_TYPE,HI_LO} places it after the high BITS_PER_DIGIT bits.
     */
    scale -= BITS_PER_DIGIT - NUM_OCTANT_BITS;

    /*
     *  We're almost done.  Just convert to floating point and then to radians.
     */
#if PRECISION_BACKUP_AVAILABLE
  {
    B_UNION ub;
    B_TYPE t;

    ub.f = PI_OVER_4;
    ub.B_HI_WORD += (scale << B_EXP_POS);
    CVT_W_TO_B_TYPE(t);
    t *= ub.f;
    if (x >= (F_TYPE)0.0) {
        *hi = t;
        *lo = *hi - t;
    } else {
	*hi = -t;
	*lo = t + *hi;
	octant = ((1 << (NUM_OCTANT_BITS+NUM_INDEX_BITS))-1) - octant;
    }
  }
#else
  {
    F_UNION uf;
    F_TYPE t, s, u, v, r;

    CVT_W_TO_HI_LO(t, s, TMP_DIGIT);
    
    uf.f = 0.;
    uf.F_HI_WORD = ALIGN_W_EXP_FIELD(F_EXP_OF_ONE + scale);
    u = uf.f;

    v = u*PI_OVER_4_LO;
    u = u*PI_OVER_4_HI;
    
    s = s*(u + v) + t*v;
    r = t*u;

    t = r + s;
    s = (t - r) - s;

    if (x >= (F_TYPE)0.0) {
        *hi = t;
        *lo = s;
    } else {
	*hi = -t;
	*lo = -s;
	octant = ((1 << (NUM_OCTANT_BITS+NUM_INDEX_BITS))-1) - octant;
    }
  }
#endif

    return octant;
}


