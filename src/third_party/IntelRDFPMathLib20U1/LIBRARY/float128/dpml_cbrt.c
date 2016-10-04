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

#define MAKE_COMMON 1

#if !defined(BUILD_FILE_NAME)
#   define BUILD_FILE_NAME  CBRT_BUILD_FILE_NAME
#endif

#if !defined(TMP_FILE)
#   define TMP_FILE             ADD_EXTENSION(BUILD_FILE_NAME,tmp) 
#endif

#define NEW_DPML_MACROS 1


#include "dpml_private.h"


#if !defined(F_ENTRY_NAME)
#   define F_ENTRY_NAME F_CBRT_NAME
#endif


#if !defined( BASE_NAME)
#   define BASE_NAME  CBRT_BASE_NAME
#endif
 
#if !defined(CBRT_BUILD_FILE_NAME)
#   define CBRT_BUILD_FILE_NAME  __D_TABLE_FILE_NAME(CBRT_BASE_NAME)
#endif

#if !defined(CBRT_TABLE_NAME)
#   define CBRT_TABLE_NAME   __D_TABLE_NAME(CBRT_BASE_NAME)
#endif


/*
 *  The algorithms used for the cbrt function are detailed in the X_FLOAT_NOTES
 *  file (notes 18.*).
 *
 *  The basic approach is to factor the input x into f * 2^n, where 
 *  1 <= f < 2  and  n = 3*m + i, where i = 0, 1, or 2.  Then 
 *
 *    cbrt(x) = cbrt(2^n * f)
 *            = cbrt(2^(3*m+i) * f)
 *            = 2^m * cbrt(2^i) * cbrt(f).
 *
 *  To get cbrt(f), we do a poly approx y = P(f), and then enough Newton's
 *  iterations to get the required accuracy.  We fetch 2^(i/3) from a table
 *  and multiply the result; then add m to the final exponent and adjust the
 *  final sign.
 *
 *  The generated file contains the poly coefficients and a small table of
 *  the roots, 2^(i/3), in full double precision and in lo parts  (the short
 *  hi part can be computed when the code executes).
 *  The generated file also contains a few other constants, and some #defines.
 *
 *  An optimization: instead of fetching 2^(i/3) as a floating point number
 *  and multiplying right away, we can fetch it into an integer register;
 *  add (sign + m) to its sign/exponent field;  and then move
 *  it to a floating point register.  On pipelined machines, this integer
 *  manipulation is done in parallel with the polynomial and/or Newton's
 *  iteration, so has "zero cost".  On sequential machines, these steps 
 *  have to be done at some point in time anyway.
 *
 *
 *  Single precision is implemented by computing a 9th degree double precision
 *  poly approx of cbrt(f).  The poly is good to about 30 bits, so we don't
 *  need any Newton's iterations.  We fetch the full-precision double 
 *  precision floating point number 2^(i/3) from the table, put it in an 
 *  integer variable, adjust its exponent and sign, move it to a floating
 *  point variable, multiply times the poly approx, and round back to
 *  single precision.
 *
 *
 *  Double and quad precision need to do Newton's iteration(s) after the 
 *  poly approx.  There are many choices of Newton's iterations for cbrt,
 *  with varying convergence.  Three interesting candidates with cubic 
 *  convergence are:
 *
 *  A:   y' =  y  -  (y/2) * (y^3 - f)            where y = P(f) ~ f^(1/3)
 *                           ---------
 *                          (y^3 + f/2)
 *
 *
 *  B:   y' =  y  -  f * z^3 * (y^3 - f) * (z*f + 7*y - 5* z^2 * y^2 *f) * 1/9
 *
 *           where y = P(f) ~ f^(1/3)  and  z = Q(f) ~ f^(-2/3)
 *
 *
 *  C:   y' =  z * f * (14 -  7 * z^3 * f^2  +  2 * z^6 * f^4 ) * 1/9
 *
 *           where y  ~ f^(1/3)  and  z ~ f^(-2/3)  as above.
 *
 *  
 *  The first Newton's iteration (A) comes from  y' =  y -  f(y)/f'(y),
 *  where f(y) = y^2 - x/y  maps out the curve   y^3 = x.  It has cubic
 *  convergence: the number of "good" bits in y' is 3*n, where 
 *  n = number of "good" bits we started with.  The initial approximation
 *  y = P(f)  approximates  f^(1/3).  The numerator  y^3 - f  can suffer
 *  massive cancellation error;  we'd have to compute it in extra precision.
 *
 *  The second Newton's iteration (B) is a variation on A.  It has the same
 *  "residual" y^3 - f, and has some extra factors that force the convergence
 *  to be cubic  (replace y with y*(1 + eps);  the rapidity of convergence
 *  depends on which powers of eps drop out).  In this case the number of
 *  "good" bits in y' is 3*n - 2.  This form of Newton's iteration needs two
 *  initial approximations:  y = P(f)  approximating f^(1/3)  and 
 *  z = Q(f) approximating f^(-2/3)  -  but we can get y from z by multiplying
 *  z * f.
 *
 *  The third Newton's iteration (C) is also a variation on A.  It has cubic
 *  convergence.  It has no divide, and also has no "residual".  The error
 *  is not small enough to use for double precision, but it is sufficient for
 *  a quick (40 bit) approximation for quad precision.
 *  
 *
 *  All three Newton's iterations have to be scaled by 2^(i/3).  We compute
 *  the residual y^3 - f in A and B very carefully to avoid cancellation:
 *    shorten y so that y*y is exact and a little short
 *    split the shortened y into hi and lo parts so that (y*y)*y_hi is
 *          also exact;
 *    y approximates f^1/3, so  (y*y)*y_hi - f is exact;
 *    then the residual
 *           y^3 - f ~  ((y*y)*y_hi - f) - (y*y)*y_lo
 *  Since C doesn't use the residual, there's no need to shorten or split y.
 *
 *  The first Newton's iteration has a divide, which is slow on Alpha (but
 *  not too bad in X_FLOAT).  The second Newton's iteration has no divide;
 *  it does have more arithmetic operations, which can be pipelined;
 *  furthermore, it requires two initial poly approximations, but we can avoid
 *  one of them by using y = f * z.  The third Newton's iteration is the
 *  fastest but least accurate.
 *
 *
 *  For double precision code, we use the second Newton's iteration, slightly 
 *  altered to maximize pipelining and to minimize cancellation; thus avoiding
 *  double precision floating divide.
 *
 *  We start with a degree 8 poly approx for z, to get 23 bits, and then
 *  compute y = f * z.  We shorten y to NUM_Y_BITS by masking out the
 *  low 32 bits  (n = 21).  Then split y into y_hi and y_lo, by masking out
 *  the low bits in the shortened y.
 *
 *  Let t = y * y.  t is exact and has NUM_Y_HI_BITS trailing zeros.  
 *  Then y^3 = t * y_hi  +  t * y_lo  is an extended precision quantity
 *  with NUM_Y_HI_BITS extra bits.  The first term has no roundoff error.
 *  We compute the residual as the sum of the exact first part and a 
 *  second part which is not exact but is small:
 *
 *      y^3 - f    =   (t * y_hi - f) + t * y_lo
 *
 *  There's a tradeoff here:  the shorter y is (n bits), the smaller
 *  the number of bits we get from the Newton's iteration  3*n - 2.
 *  We definitely want 3*n - 2 to be larger than F_PRECISION.  But the shorter
 *  y is, the more bits we can get in y_hi, and the more extra precision we
 *  get.  We definitely want n < F_PRECISION/2.  Another consideration is
 *  the accuracy of the initial approximation, which gives us the n "good" 
 *  bits to begin with:  z had only 23 bits, so y really has only 22.
 *
 *  Let n = NUM_Y_BITS = 21.  y_hi has D_PRECISION - 2 * n = 11 bits.  
 *  The Newton's iteration gives 3*n - 2 = 61 bits, which gives us a double
 *  precision cbrt with excellent error characteristics.
 *
 *  We have to scale up the Newton's iteration by 2^m * 2^(i/3).
 *  To do the scaling, let c_full and c_lo be the full precision and the lo 
 *  parts of 2^(i/3).  c_hi is computed = c_full - c_lo; c_lo is chosen so
 *  that c_hi is short and y * c_hi is exact.  Then the Newton's iteration
 *  including scaling is computed as
 *
 *    y'  =  y * (c_full - c_lo) +  
 *
 *            ( y * c_lo  + 
 *
 *                ((f - t*y_hi) - t*y_lo) *
 *                    ( (c_full * (7/9 * (f*z)*(z*z)))  * 
 *                                 (1/7 * (z*f) +  y + (z*z)*(5/7 * f)*t)) )
 *
 *  The Newton's iteration takes about 8 chimes after z (poly) is finished.
 *
 *
 *
 *  To do quad precision, since each X_FLOAT floating point operation is so
 *  slow, we do an initial approximation in double precision, convert the
 *  results to quad, and then do a quad precision Newton's iteration.
 *  We postpone adding m to the exponent until the last quad precision 
 *  operation; this simplifies the earlier steps.
 *
 *  Rather than actually calling double precision cbrt(), we cast f to 
 *  double precision df and compute a simplified double precision cbrt 
 *  approximation in-line.  We know 1 <= df < 2, so no need to normalize df 
 *  or process the exponent.  We start with a poly, then do the third
 *  Newton's iteration (C).  We scale times 2^(i/3) by loading the full 
 *  precision c_full into a double precision floating point register and doing
 *  a floating multiply.  Then split the double precision result dy into
 *  dy_hi and dy_lo, and convert all three to quad.
 *
 *  The cost of the double precision cbrt approx is probably comparable to
 *  one quad precision floating point operation.
 *
 *  After dy, dy_hi and dy_lo have been converted to y, y_hi, and y_lo, we
 *  need to do a Newton's iteration.  Each quad precision floating point 
 *  operation is costly, but a divide is relatively less than for double
 *  precision.  Therefore we use the first proposed Newton iteration, in
 *  the following decomposition:   t = y * y
 *
 *      y' =  y  -  y * (( t * y_hi - f) + t * y_lo)
 *                      ----------------------------
 *                        (t*y + t*y) + f
 *
 *  This requires 5 multiplies, 5 adds and 1 divide. We have to scale f 
 *  by 2^i, by adding i to the exponent.  Finally, add (sign + m) to the 
 *  sign/exponent field.
 *
 *
 *  The accuracy for all 3 precisions is quite satisfactory, just a little
 *  over .5 lsb.  The performance is more than twice as fast as the routines
 *  they replace.
 */




#define NUM_Y_BITS     21


/* 
 *  MPHOC code to do the polynomials and the table of cbrts: 2^(i/3),
 *  double precision floating point.
 *
 *  The hi part of 2^(i/3) will be loaded into an integer register; the
 *  exponent will be adjusted by m; then hi (and any lo) part of 2^(i/3)
 *  will be moved into a floating point register.  If BITS_PER_WORD >=
 *  BITS_PER_D_TYPE, then the entire 64-bit entry is moved as a unit between
 *  integer and floating point registers.  Otherwise, we have to fetch
 *  the lo part and store it into the "lo" part of a D_UNION.  The address
 *  of the lo 32-bits (endianness) is hidden in the D_HI_WORD and D_LO_WORD.
 */


#if MAKE_INCLUDE


#    define WORKING_PRECISION   ceil( (D_PRECISION + 1) / MP_RADIX_BITS) + 2
#    define REMES_PREC  ( ceil(2*D_PRECISION/MP_RADIX_BITS) + 5) + 10

#    define PRINT_D_ITEM(a)     PRINT_1_TYPE_ENTRY(D_CHAR, a, offset)


@divert divertText

   function
    do_cbrt(z)
{
    return cbrt(z);
  }


   function
    recip_cbrt(z)
{  
   auto t;

    t = cbrt(z);
    return 1/(t * t);
  }



   function
    cbrt_approx_poly(remes_bits_of_accuracy)
{

    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT + REMES_LINEAR_ARG,
          1.0, 2.0, do_cbrt, remes_bits_of_accuracy,
          &remes_degree_numer,  &remes_coeff_numer);

    for (i = 0; i <= remes_degree_numer ; i++) {
       y = remes_coeff_numer[i];
       PRINT_D_ITEM( y );
     }

    return (remes_degree_numer);

  }

   function
    recip_cbrt_poly(remes_bits_of_accuracy)
{

    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT + REMES_LINEAR_ARG,
          1.0, 2.0, recip_cbrt, remes_bits_of_accuracy,
          &remes_degree_numer,  &remes_coeff_numer);

    for (i = 0; i <= remes_degree_numer ; i++) {
       y = remes_coeff_numer[i];
       PRINT_D_ITEM( y );
     }

    return (remes_degree_numer);

  }

/*
 *  Make sure that both hi and lo words of 2^(i/3) are non-negative, to
 *  simplify the sign/exponent manipulation in the code.  If the lo word
 *  is 0 (i.e. 2^0), put in a tiny number instead, so that we can add 
 *  (sign + exp) to it.
 */ 

   procedure
    generate_root_table()
{

   for( i = 0; i <= 2; i++) {
     c = cbrt(2^i);
     PRINT_D_ITEM(c);

     c_hi = bchop(c, D_PRECISION - NUM_Y_BITS);
     c = c - c_hi;
     if (c == 0)
          c = 2^( D_MIN_BIN_EXP/2 );
     PRINT_D_ITEM(c);

   }
 }


       precision = REMES_PREC;

       printf("\n#include \"dpml_private.h\"\n\n");

       printf("\n#if !TABLE_IS_EXTERNAL\n\n");

       START_GLOBAL_TABLE(CBRT_TABLE_NAME,offset);

       TABLE_COMMENT("1.0 in double precision");

       PRINT_TABLE_VALUE_DEFINE(ONE_D, CBRT_TABLE_NAME, offset, D_TYPE);
       x = 1;
       PRINT_D_ITEM(x);


/*
 *  This poly gives a cbrt approx good to about 30 bits.  It will be used
 *  only for single precision.
 */

       TABLE_COMMENT("coeffs to approx cbrt(f)");

       PRINT_TABLE_ADDRESS_DEFINE(CBRT_POLY_ADDR, CBRT_TABLE_NAME,
        offset, D_TYPE);

       num_bits = S_PRECISION + 5;

       cbrt_deg = cbrt_approx_poly(num_bits);

       GENPOLY(CBRT_POLY_ADDR[%%d], CBRT_POLY(x), cbrt_deg);


/*
 *  This poly gives around 22 bit approx of 1/cbrt(f)^2.  It is used as
 *  the initial approx in the two double precision Newton's iterations,
 *  which are used in double and quad precision respectively.
 */

       TABLE_COMMENT("coeffs to approx 1/cbrt(f)^2");

       PRINT_TABLE_ADDRESS_DEFINE(REC_CBRT_POLY_ADDR, CBRT_TABLE_NAME,
        offset, D_TYPE);

       num_bits = 2*D_PRECISION/5;

       recip_cbrt_deg = recip_cbrt_poly(num_bits);

       GENPOLY(REC_CBRT_POLY_ADDR[%%d], RECIP_CBRT_POLY(x), recip_cbrt_deg);



       precision = WORKING_PRECISION;

       TABLE_COMMENT("cube roots of 2^i, i = 0,1,2  in full and lo");

       printf("\n#define OFFSET_OF_CBRTS_OF_2  %i \n", BYTES(offset) );

       generate_root_table();

 
       TABLE_COMMENT("Numerical constants");


       PRINT_TABLE_VALUE_DEFINE(BIG_QUAD, CBRT_TABLE_NAME, offset, D_TYPE);

       x = 2^(D_PRECISION - (Q_PRECISION - 2*D_PRECISION));

       PRINT_D_ITEM(x);


       PRINT_TABLE_VALUE_DEFINE(SEVEN_NINTHS, CBRT_TABLE_NAME, offset, D_TYPE);
       x = 7/9;
       PRINT_D_ITEM(x);

       PRINT_TABLE_VALUE_DEFINE(ONE_SEVENTH, CBRT_TABLE_NAME, offset, D_TYPE);
       x = 1/7;
       PRINT_D_ITEM(x);

       PRINT_TABLE_VALUE_DEFINE(FIVE_SEVENTHS, CBRT_TABLE_NAME, offset,D_TYPE);
       x = 5/7;
       PRINT_D_ITEM(x);

       PRINT_TABLE_VALUE_DEFINE(FOURTEEN, CBRT_TABLE_NAME, offset, D_TYPE);
       x = 14;
       PRINT_D_ITEM(x);

       PRINT_TABLE_VALUE_DEFINE(SEVEN, CBRT_TABLE_NAME, offset, D_TYPE);
       x = 7;
       PRINT_D_ITEM(x);

       PRINT_TABLE_VALUE_DEFINE(NINTH, CBRT_TABLE_NAME, offset, D_TYPE);
       x = 1/9;
       PRINT_D_ITEM(x);




       END_TABLE;

/*
 *  Declaring the size of the table in the "extern" will allow the compiler
 *  to generate memory accesses more freely.
 */


       printf("\n#else\n");
       printf("\n extern const TABLE_UNION "STR(CBRT_TABLE_NAME)"[%i]; \n",
          offset/BITS_PER_TABLE_WORD); 
       printf("\n#endif\n");


       printf("\n\n");
       
@end_divert

@eval my $outText = MphocEval( GetStream( "divertText" ) ); 		\
      my $defineText = Egrep( "#define",  $outText, \$tableText );	\
      my $polyText   = Egrep( STR(GENPOLY_EXECUTABLE), $tableText,	\
                              \$tableText );				\
         $polyText = GenPoly( $polyText );				\
      my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),		\
                       "Definitions and constants for " .		\
                       STR(F_ENTRY_NAME),  __FILE__);			\
         print "$headerText\n\n$tableText\n\n$defineText\n\n$polyText";	


/*
 * if not MAKE_INCLUDE
 */
#else          



/*
 *  Macros for the code.
 */





#define TABLE_IS_EXTERNAL 1 

#include STR(BUILD_FILE_NAME)


/*
 *  Fetch D_PRECISION 1.0 from the table early to use in COPYSIGN_EXP, and
 *  also to encourage early calculation of the table's address and fetching
 *  coeffs for the poly, etc.  Quad precision uses constant 1.0 instead,
 *  for better performance.
 */


#if QUAD_PRECISION

#   define ONE_CONST  (B_TYPE)1.0 

#else

#   define ONE_CONST  ONE_D

#endif

/*
 *  Macros to facilitate integer division
 *
 *  Instead of unbiasing the exponent right away, we add and later subtract
 *  small corrective quantities (ADD_ADJUST, SUB_ADJUST) to get rid of the
 *  BIAS/3 exactly:
 *
 *    (true_expon + BIAS + ADD_ADJUST)*(1/3) - SUB_ADJUST =  true_expon/3
 *
 *  true_expon + BIAS >= 0, so we can do unsigned arithmetic, which has
 *  better performance.
 *
 */


#define SUB_ADJUST  (F_PRECISION + F_EXP_BIAS + 2)/3

#define ADD_ADJUST  (3*(SUB_ADJUST) - F_EXP_BIAS + (WORD)F_NORM)

/*
 *  Instead of doing integer division, we can multiply by an integer that
 *  corresponds to 1/3 in "fixed point".
 *
 *  If the number is small enough and in the right form, the compiler may
 *  optimize the multiply into shifts and adds.
 */


#if SINGLE_PRECISION

#   define ONE_THIRD  0x11
#   define SHIFT_PROD 9

#elif DOUBLE_PRECISION

#   define ONE_THIRD  0x111
#   define SHIFT_PROD 13

#elif QUAD_PRECISION

#   define ONE_THIRD  0x1111
#   define SHIFT_PROD 17

#endif


#define DIV_BY_3(num)   (( (10 * num) * ONE_THIRD  +  num) >> SHIFT_PROD)




/*
 *  Macros to shorten y and split y into y_hi and y_lo.  Only used for double
 *  precision, and for the first Newton's iteration for X_FLOAT, which is
 *  also done in double precision.
 *
 *  y is double precision floating point number, containing about 22 good
 *  bits of approximation to cbrt(f).
 *
 *  To shorten y, we zero out the low 32 bits of y (y has 53 - 32 = 21 bits).
 *  Then mask out the hi 11 bits of y to get y_hi  (11 bits).
 *  Then y_lo = y - y_hi  (10 bits).
 *
 *  This decomposition of y  (21 = 11 + 10) turns out to give optimal accuracy
 *  in the subsequent Newton's iteration.
 *
 *  Instead of shortening y with masks, etc., we could use floating point
 *  instructions:  cast y to single precision, then add/sub a suitable BIG
 *  constant.
 */




#if VAX_FLOATING
#      define YHI_MASK ((U_WORD)  0xfe00ffff)
#else
#      define NUM_Y_HI_BITS 11
#      define YHI_MASK \
        MAKE_MASK((D_EXP_WIDTH + NUM_Y_HI_BITS + 1), D_EXP_POS - NUM_Y_HI_BITS)

#endif

#if (BITS_PER_WORD == 64)
#   if IEEE_FLOATING
#      define CLEAR_LO1_32_MASK     MAKE_MASK(32,32)
#   else
#      define CLEAR_LO1_32_MASK     MAKE_MASK(32,0)
#   endif
#else
#      define CLEAR_LO1_32_MASK     0
#endif

#define SPLIT_UP_Y(input_y, utmp, output_y, y_var_hi) \
       utmp.f = input_y; \
       utmp.D_UNSIGNED_LO1_WORD &= CLEAR_LO1_32_MASK; \
       output_y = utmp.f; \
       utmp.D_HI_WORD &= YHI_MASK; \
       y_var_hi = utmp.f;


/*
 *  Macros to fetch the full precision words and possibly also the lo words
 *  of 2^(i/3) from the table, either as D_HI_WORD in an integer register, 
 *  or as a double precision floating point number.
 *
 *  The table items are double precision (64-bits).  If BITS_PER_WORD is 32,
 *  fetch the lo 32 bits into the D_LO_WORD of a D_UNION (thus hiding the
 *  endianness). 
 *
 *  Single precision code fetches only the full precision root, as integer.
 *  Quad precision code also fetches only the full precision root, as double.
 *  Double precision code fetches both hi and lo parts of 2^(i/3), parameter
 *  'which' in the macro.
 */


#if BITS_PER_WORD < BITS_PER_D_TYPE
#   define IF_SMALL_WORD(x)   x
#else
#   define IF_SMALL_WORD(x)
#endif


#define GET_ROOT(index,utmp,which,hiword) \
   hiword = ((U_WORD) root->which.D_HI_WORD); \
   IF_SMALL_WORD(ROOT_LO(index,which,utmp));


#define ROOT_LO(index,which,utmp) \
  utmp.D_LO_WORD = ((WORD) root->which.D_LO_WORD);


#define GET_ROOT_AS_FLOAT(index,flt) \
 flt = ((D_TYPE)root->full.f);



#define ROOT_TO_FLOAT(hiword,utmp,floating) \
    utmp.D_HI_WORD = hiword; \
    floating = utmp.f;


typedef struct { D_UNION full, lo ;} CUBE_ROOT_TABLE_ITEM;



/*
 *  Constants for screening and for adjusting the denorm exponent.
 */


#define LSB_OF_EXPON  ((U_WORD)1 << F_EXP_POS)
#define HI_WORD_OF_HALF  ((U_WORD)(F_EXP_BIAS - 1) << F_EXP_POS)

#if IEEE_FLOATING
#   define SIGN_MASK_EXT  (-F_SIGN_BIT_MASK)
#else
#   define SIGN_MASK_EXT  F_SIGN_BIT_MASK
#endif


/*
 *  The code.
 */



F_TYPE
F_ENTRY_NAME(F_TYPE x)
{
     WORD j, m, k, sign;

     U_WORD  um, i, uj;

     B_TYPE f, y, one, z;

     B_TYPE r, y_hi, y_lo, c_hi, c_lo, t, w;

#if QUAD_PRECISION
     D_TYPE dy, dy_hi, dy_lo, df, dz, dr, dt, dw;
#endif

     F_UNION work_u;

     D_UNION stk_tmp_u, stk_tmp_v;

     CUBE_ROOT_TABLE_ITEM * root;


/*
 *  First, reduce x to f, where 1 <= f < 2.  f will be the variable for
 *  the poly.   Get the hi word of x and isolate the (biased) exponent 
 *  and the sign.
 *
 *  Screen out x = 0 and the IEEE denorms, infinities and NaNs.
 *
 *  The variable sign holds either F_SIGN_BIT_MASK (sign extended) or 0.
 *  For single and double precision, it will be convenient for sign to be 
 *  D_SIGN_BIT_MASK instead - modify the single precision variable shortly.
 */

     one =  ONE_CONST;

     B_COPY_SIGN_AND_EXP((B_TYPE) x, one, f);

     work_u.f = x;

     j = work_u.F_SIGNED_HI_WORD; 

     sign = (j & SIGN_MASK_EXT);


#if IEEE_FLOATING

     i = MAKE_MASK( (F_EXP_WIDTH - 1), (F_EXP_POS + 1) ); 

     if ( (WORD)( (j + LSB_OF_EXPON) & i ) == 0 )

           goto special;

#else

     i = F_EXP_MASK; 

     if ((j & i) == 0)

          return x;

#endif


/*
 * Denorm cases rejoin the normal path here.
 *
 * Start the poly as soon as possible.
 *
 * But in the meantime, add the constant to the exponent field, fix its sign,
 * and prepare to start the "division"  expon * 1/3.
 *
 * Single precision replaces the 'sign' variable with 
 *     sign << (D_EXP_POS - F_EXP_POS).
 * Double precision case follows the poly with a Newton's iteration.
 * Quad precision casts f to double and does the poly and one Newton's
 * iteration in double precision.
 */


   normal_path:

     j -= sign;



#if SINGLE_PRECISION

     z = CBRT_POLY(f);  

     sign = ((sign) ? D_SIGN_BIT_MASK : 0);

#elif DOUBLE_PRECISION

     z = RECIP_CBRT_POLY(f);  

     w = f * z;


     SPLIT_UP_Y(w, stk_tmp_u, y, y_hi);


     y_lo = y - y_hi;

     r = w * ((z * z) * SEVEN_NINTHS);

     t = y * y;

     w = ( ONE_SEVENTH * w + y - (((z * z) *(FIVE_SEVENTHS * f)) * t ) );

     t =  (f - t * y_hi) - (t * y_lo)  ;


#elif QUAD_PRECISION

     df = (D_TYPE) f;

     dz = RECIP_CBRT_POLY(df);

     dw = df * dz;

     dt = dw * dw;      

     dr = dz * dz;

     dr += dr;

     dy =  FOURTEEN - ((SEVEN * dz)* dt);

     dy += ( (dt * dt) * dr );

     dy *= (dw * NINTH);


#endif

/*
 *  While the poly and Newton's iteration are executing, divide the (biased)
 *  exponent by 3 and compute the remainder mod 3.  We added a constant to the
 *  biased exponent so that the BIAS etc. will be divisible by 3.  We
 *  also subtract the constant/3 from the quotient.
 *
 *  The true exponent = 3*m + i, where i = 0, 1 or 2.
 *
 *  Later, the final cbrt approx will be multiplied by 2^(i/3), 
 *  and (sign + m) will be added to the final exponent.
 */


     uj = (U_WORD) (j >> F_EXP_POS);

#if VAX_FLOATING
     uj &= MAKE_MASK(F_EXP_WIDTH,0);
#endif

     uj += ADD_ADJUST;

     um = DIV_BY_3(uj);

     i = uj - 3*um;

     m = (WORD) (um - SUB_ADJUST);


     root =   (CUBE_ROOT_TABLE_ITEM *) ((char *) CBRT_TABLE_NAME +
               OFFSET_OF_CBRTS_OF_2 + (i * sizeof(CUBE_ROOT_TABLE_ITEM) ))  ;


     m <<= (B_EXP_POS);

     m += sign;


/*
 *  While the poly and Newton's iteration still continue to execute ...
 *
 *  Fetch 2^(i/3) from the table.  T(i) = 2^(i/3) consists of double precision
 *  entries, stored in full precision and lo parts.
 *
 *  For single and double precision, fetch the "full word" of T(i) into an
 *  integer register.  We've already shifted m into "exponent position" and
 *  added the sign; we add this to T(i) and move T(i) to a floating register.
 *
 *  Single precision requires only the full root T(i), in double precision.
 *  Double precision requires full T(i) as well as a short hi part and a 
 *  lo part.  The algorithm requires the "hi" part quite late, so plenty of
 *  time to compute it from full - lo.  We need to add (sign + m) to the
 *  sign/exponent field of both T(i)_full and T(i)_lo.  Note that both 
 *  T(i)_full and T(i)_lo are positive, so that adding (sign + exp) works.
 *
 *  For quad precision, we'll fix up m and the sign much later.  Just fetch 
 *  T(i) in full precision, as a floating point number.
 *
 *  If only the hi 32 bits of T(i) fit into an integer register (IF_SMALL_WORD)
 *  we also fetch the lo 32 bits of T(i) from the table and put it into the
 *  D_UNION that we'll use to create the floating point number.
 */


#if  SINGLE_PRECISION

     GET_ROOT(i, stk_tmp_u, full, j);

     j += m;

     ROOT_TO_FLOAT(j,stk_tmp_u,c_hi);


#elif DOUBLE_PRECISION

     GET_ROOT(i, stk_tmp_u, full, j); 

     GET_ROOT(i, stk_tmp_v, lo, k);

     j += m;

     k += m; 

     ROOT_TO_FLOAT(j,stk_tmp_u,c_hi);

     ROOT_TO_FLOAT(k,stk_tmp_v,c_lo);


#elif QUAD_PRECISION

     GET_ROOT_AS_FLOAT(i, dr);


#endif


/*
 *  Single and double precision are nearly finished.  Put the floating T(i)
 *  together with the poly (single) or the pieces of the Newton's iteration
 *  (double precision).
 *
 *  Quad precision needs a Newton's iteration in quad.  First, multiply the
 *  double precision Newton's iteration parts together with the double
 *  precision T(i).  Then split the double precision result dy into hi and
 *  lo parts, dy_hi and dy_lo.  Convert the three double precision numbers
 *  to quad precision and do another Newton's iteration.  
 *  Add m to the result's exponent and correct the sign.
 */


#if SINGLE_PRECISION

     z = c_hi * z;

#elif DOUBLE_PRECISION


     z = y*(c_hi - c_lo)  + (y * c_lo + (((c_hi * r) * w) * t) );


#elif QUAD_PRECISION

     dy *= dr;


     dy_lo = BIG_QUAD;

     dy_hi = dy;

     ADD_SUB_BIG(dy_hi, dy_lo);

     dy_lo = dy - dy_hi;


     y = (F_TYPE) dy;

     y_hi = (F_TYPE) dy_hi;

     y_lo = (F_TYPE) dy_lo;


     t = y * y;     


     ADD_TO_EXP_FIELD(f, i);

     z = ((t * y_hi) - f) + t * y_lo; 


     w = t * y;

     w += w;

     w += f;


     z *= y;

     z = z/w;

     z = y - z;

     work_u.f = z;

     work_u.F_HI_WORD += m;

     z = work_u.f;

#endif


/*
 *  Done!  Cast the result to F_PRECISION and return.
 */


     return (F_TYPE) z;


/*
 *  Processing of IEEE special cases: zeros, denorms, infinities, NaNs.
 *
 *    j =    hi word of x; 
 *    one =  (B_TYPE) 1.0;
 *    f =    "fraction field" of x - denorms have a spurious hidden bit set.
 *
 *  If exponent == EXP_MASK, x must be a NaN or signed infinity.  We really
 *  should transform signalling NaNs into quiet NaNs - for now, just return x.
 *
 *  Otherwise, x is zero or denormalized.  f is x's fraction with hidden bit
 *  presumed to be set and with exponent of 1.0.  Subtract f - 1, fetch the
 *  hi word.  If x was really zero, f is now 1, and the exponent of f - 1
 *  is zero.  Otherwise, the difference represents the number of bits to 
 *  shift the original denormalized number.  Subtract it from x's old 
 *  exponent - it will be negative, but we'll add the ADD_ADJUST to make it
 *  positive and we'll subtract out the (propagated) negative sign.
 *
 *  Prepare the new fraction f and we're ready to compute the poly.
 */


   special:

     if ((j & F_EXP_MASK) == F_EXP_MASK)

        return x;

     work_u.f = (F_TYPE)(f - one) ;

     k = work_u.F_SIGNED_HI_WORD;

     k &= F_EXP_MASK;

     if (k == 0)
       return x;

     B_COPY_SIGN_AND_EXP((B_TYPE)work_u.f, one, f);

     j = j + ( k - HI_WORD_OF_HALF );

     goto normal_path;


   }


/*
 * not MAKE_INCLUDE
 */
#endif

