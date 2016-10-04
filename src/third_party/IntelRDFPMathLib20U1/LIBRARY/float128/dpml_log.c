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

#define BASE_E  0
#define BASE_2  1
#define BASE_10 2

#if MAKE_COMMON && !defined(BUILD_FILE_NAME)

#   if (BASE_OF_LOG == BASE_2) || (LOG2)
#      define BUILD_FILE_NAME    F_LOG2_BUILD_FILE_NAME
#   elif (BASE_OF_LOG == BASE_10) || (LOG10 || FAST_LOG10)
#      define BUILD_FILE_NAME    F_LOG10_BUILD_FILE_NAME
#   else
#      define BUILD_FILE_NAME    F_LOG_BUILD_FILE_NAME
#   endif

#endif

#include "dpml_private.h"


#if MAKE_INCLUDE && LOG10
#   error "To generate table for base 10, define BASE_OF_LOG = BASE_10"
#endif

#if MAKE_INCLUDE && LOG2
#   error "To generate table for base 2, define BASE_OF_LOG = BASE_2"
#endif


#if MAKE_INCLUDE
#   if BASE_OF_LOG == BASE_2
#      define BASE_NAME        LOG2_BASE_NAME
#      define MPLOG            log2
#   elif BASE_OF_LOG == BASE_10
#      define BASE_NAME        LOG10_BASE_NAME
#      define MPLOG            log10
#   else
#      define BASE_NAME        LN_BASE_NAME
#      define MPLOG            log
#   endif

#   if MAKE_COMMON

#      if BASE_OF_LOG == BASE_10
#         define LOG_TABLE_NAME  F_LOG10_TABLE_NAME
#      elif BASE_OF_LOG == BASE_2
#         define LOG_TABLE_NAME  F_LOG2_TABLE_NAME
#      else
#         define LOG_TABLE_NAME  F_LOG_TABLE_NAME
#      endif

#   else
#      define LOG_TABLE_NAME  TABLE_NAME
#   endif

#endif



/*
 *  Compile time information, depending on which function is compiled:
 *     default function names and base names;  
 *     error codes for the exception dispatcher (note that the FAST functions
 *       do not invoke the exception dispatching mechanism);
 *     and other function-related symbolic constants, e.g. natural log or not,
 *       fast function or not.
 */

#undef  NATURAL
#undef  DO_LOG1P
#undef  FAST

#if !MAKE_INCLUDE

#  if FAST_LOG

#    define     _F_ENTRY_NAME           F_FAST_LN_NAME
#    define     BASE_NAME               FAST_LN_BASE_NAME

#    define     NATURAL                 1
#    define     FAST                    1

#  elif FAST_LOG10

#    define     _F_ENTRY_NAME           F_FAST_LOG10_NAME
#    define     BASE_NAME               FAST_LOG10_BASE_NAME

#    define     FAST                    1

#  elif LOG2

#    define     _F_ENTRY_NAME           F_LOG2_NAME
#    define     BASE_NAME               LOG2_BASE_NAME
#    define     LOG_ZERO                LOG2_OF_ZERO
#    define     LOG_NEGATIVE            LOG2_OF_NEGATIVE

#  elif LOG10

#    define     _F_ENTRY_NAME           F_LOG10_NAME
#    define     BASE_NAME               LOG10_BASE_NAME
#    define     LOG_ZERO                LOG10_OF_ZERO
#    define     LOG_NEGATIVE            LOG10_OF_NEGATIVE

#  elif LOG1P

#    define     _F_ENTRY_NAME           F_LOG1P_NAME
#    define     BASE_NAME               LN_BASE_NAME
#    define     LOG_ZERO                LOG1P_M1
#    define     LOG_NEGATIVE            LOG1P_LESS_M1

#    define     NATURAL                 1
#    define     DO_LOG1P                1

#  else

#    define     _F_ENTRY_NAME           F_LN_NAME
#    define     BASE_NAME               LN_BASE_NAME
#    define     LOG_ZERO                LOG_OF_ZERO
#    define     LOG_NEGATIVE            LOG_OF_NEGATIVE

#    define     NATURAL                 1

#  endif

#endif



#if MAKE_COMMON && QUAD_PRECISION
#   error "Shared tables not yet implemented for quad precision"
#endif

#if FAST && QUAD_PRECISION
#   error "Fast functions are not implemented in quad precision"
#endif

#if FAST && !MAKE_COMMON
#   error "Fast functions expect shared tables"
#endif

/*
 * SUMMARY OF THE ALGORITHM
 *
 *  The algorithm uses a large table for argument reduction, and then
 *  polynomial approximation.  Certain aspects of the algorithm were 
 *  suggested in an article by Peter Tang, in Transactions on Mathematical 
 *  Software, December 1990.
 *
 *  In general, log of x can be computed by first reducing x to a small range
 *  near 1, and computing the log of the reduced argument.  Write x = 2^m * f,
 *  where f is in the interval [1, 2], then  log(x) = m * log(2) + log(f).  
 *  Chop up the interval [1,2] into tiny subintervals [F(i), F(i+1)].  
 *  Determine which division point F(j) is closest to f; then  
 *     x = 2^m * f =  2^m *  F(j) * ( f/F(j) ),
 *  where the last factor is very close to 1.  Then 
 *     log(x) =  m * log(2) +  log(F(j)) +  log(f/F(j)) .
 *  Since w = f/F(j) is very close to 1, its log can be approximated with a 
 *  relatively small degree polynomial in w - 1 or (1-w)/(1+w)  (details in
 *  a following section).
 *
 *  When x is quite close to 1, however, the roundoff error in computing the
 *  variable w becomes visible, and can lead to quite large relative error.
 *  First, when x is very close to 1 (and, say x > 1), then m = 0 and F(j) = 1.
 *  The terms m * log(2) and log(F(j)) are both zero, so we need an accurate
 *  approximation for log(f/F(j)).  If x is a little farther from 1, so that
 *  j is a small positive integer, then m = 0 and F(j) is about 1 + j/2^LOG_K
 *  (depending on how F(j) is defined), and log(F(j)) is about j/2^LOG_K.
 *  The leading term of the polynomial in w can be as large as 1/2^(LOG_K + 1),
 *  which is not much smaller than log(F(j)) and possibly has opposite sign.
 *
 *  To finesse this problem, the current implementation of log uses a special
 *  path for x "near 1".  This approach is efficient because it permits a
 *  less stringent approximation for log(f/F(j)), provided that the cost
 *  of a branch is not too great.  An exception: the single precision fast
 *  logs use one path, because the error bound of the polynomial
 *  approximation for log(f/F(j)) is within the (relaxed) error requirements
 *  for fast functions.
 *
 * ONE PATH
 *  The accurate log functions have a ONE_PATH compile option (non-shared),
 *  which is appropriate in a situation that severely penalizes branching.
 *  In the ONE_PATH algorithm, the variable f/F(j) - 1 is computed very
 *  carefully in hi and lo parts, and the linear term of the polynomial
 *  is carefully split up into hi and lo parts and added to the respective
 *  hi and lo terms of the expression.  Unfortunately, the cost of this
 *  additional care has to be paid even when the care is not necessary,
 *  e.g. when x is large.
 *
 * TABLE CONSTANTS
 *  Both the ONE_PATH and the two path approaches store F(j), log(F(j)), 
 *  and 1/F(j) in a table, and index into the table using the leading
 *  LOG_K fraction bits of f.
 *
 *  In order to maintain the accuracy, computations use backup precision
 *  wherever possible.
 *  
 *  If no backup precision is available, log(F(j)) and (for the ONE_PATH
 *  algorithm) 1/F(j) are stored in the table in hi and lo parts.
 *  log(2) and, for base 2 and 10, log(e), are also given in hi and lo parts. 
 *  The hi part of log(2) is generated to have at least F_EXP_WIDTH trailing
 *  zero bits so that m * log(2) will be an exact product.  Similarly, 
 *  the hi part of log(F(j)) has enough trailing zeros so that
 *  when it is added to  m * log2_hi, no significant bits of log(F(j))_hi
 *  will be lost.  The lo parts of log(2) and log(F) are given in full 
 *  precision.  Any roundoff error generated in computations that involve 
 *  the lo parts will be shifted off.
 *
 *  All of these constants, the coefficients for the approximation polynomials,
 *  and the table of F(j) data are stored in the same array.
 *
 * LOG1P
 *  The computation of log1p(x) is similar to that for ln(x).  If x is not
 *  too close to zero, we compute ln(1+x) following Tang's recommendations
 *  to avoid roundoff error in the computation of (y - Fj)/Fj.  
 *  If x is within the interval [T1 - 1, T2 - 1], then x + 1 
 *  will be in the small interval [T1, T2] used in this implementation.  
 *  If x is really close to zero, return x.  The error cases are analogous 
 *  to those for natural log.
 *
 * POLYNOMIAL APPROXIMATIONS
 *  There are two standard approaches for polynomial approximation for the
 *  natural log, ln(w), where w is close to 1.
 *  1.  Write f = y + F, then f/F = 1 + y/F = 1 + z.  Using the Taylor's 
 *   expansion,  
 *       log(1 + z)  =  z - z^2/2 + z^3/3 + ... + (-1)^(n+1) * z^n / n + ...
 *  This suggests the minmax polynomial will be of the form
 *     Q(z)  = z +  C1*z^2 + C2*z^3 + ....
 *  This implementation computes the variable z by taking 
 *    z = (f-F) * (1/F),  where the reciprocal 1/F is stored in the F table.
 *
 *  2.  Write  z = ((f/F) - 1)/( (f/F) + 1) =  (f - F)/(f + F).
 *   The approximation for log(1 + f/F) can be derived from the Taylor's
 *   expansion:
 *      log(1 + f/F) =  2z + 2* z^3/3  + 2* z^5/5 + ...
 *   The factor 2 can be absorbed into z, so that the final form of the
 *   polynomial is
 *      log(1 + f/F) =  z +  z^3/12  +  z^5/80 + ...
 *   This suggests that the minmax polynomial will be
 *      z +  z * (C1* (z^2) + C2* (z^2)^2  + ...  =   z + z*P(z^2).
 *  
 *  To achieve comparable accuracy, more terms are required in the first
 *  approach's polynomial (the "reciprocal" approach) than in the second
 *  (the "quotient" approach), but the second approach requires a division
 *  in computing the approximation variable.
 *
 *  In this implementation, both the "normal path" (x not near 1) and the
 *  "near 1" path can be computed either with the first or second approach.
 *  By default, both paths use the approximation that avoids the use of
 *  floating point division.  These defaults can be overridden with compile
 *  options:
 *      -U USE_RECIP         for the "normal path"
 *      -U NO_DIVISIONS      for the "near 1 path"
 *  These choices are independent of one another.  The choices could also
 *  be tied to architecture considerations, e.g. the relative instruction
 *  speeds of divisions, and adds and multiplies.  In any case, coefficients
 *  for both sets of polynomials are generated in the non-shared include files
 *  at build time.
 *
 */

#if !defined(USE_RECIP)
#define USE_RECIP   1
#endif

#if !defined(NO_DIVISIONS)
#define NO_DIVISIONS 1
#endif


/*
 *  If the log base 10 or base 2 is required, since 
 *     log_base(x) = ln(x) / ln(base) =  ln(x) * log_base(e),
 *  each of the coefficients for the polynomial will be multiplied by
 *  by the appropriate base log of e.  These factors will be absorbed into
 *  the coefficients, except for the linear term z, which will be multiplied
 *  by log_base(e) in hi and lo parts.
 *
 *  All constants, including coefficients of the appropriate polynomials,
 *  are computed at build-time and are given in the shared or non-shared
 *  include files.
 */

#if ONE_PATH && MAKE_COMMON
#     error "ONE_PATH not implemented using shared tables"
#endif

#if ONE_PATH && SINGLE_PRECISION && !PRECISION_BACKUP_AVAILABLE
#     error "ONE_PATH requires using double precision as a backup"
#endif

/*
 * ERROR CASES
 *  Special cases (negative input, zero, NaNs, infinity, reserved operand)
 *  and denormalized numbers are screened out early in the routine, 
 *  by examining the sign and exponent fields.  The accurate routines
 *  use the DPML exception dispatching mechanism to raise the appropriate
 *  error and return the appropriate value.  
 *
 *      VAX format:
 *          x = 0   raises an error  ("log of zero")
 *          x < 0   raises an error  ("log of negative")
 *
 *      IEEE format:
 *          x = NaN  returns the NaN, without raising an error 
 *          x = +INFINITY returns +INFINITY, without raising an error.
 *          x = +denormal  is scaled, and the log is computed.
 *          x = +0  returns -INFINITY, through the exception dispatcher.
 *          x negative (including x = -INFINITY and x = -0) returns a NaN
 *              through the exception dispatcher.
 *
 *  The fast routines raise a floating point overflow for error cases.
 *  Input positive denorms are treated as zero in the fast routines (the
 *  accurate routines compute the correct result).  If x was a NaN or
 *  positive infinity, the fast routines return x.
 *
 *
 * ACCURACY
 *  The accuracy characteristics for this implementation (max error, in lsb):
 *
 * ln:        .55
 * log2:      .58
 * log10:     .56
 *
 * Fast ln and log10:   single precision < 1.0
 *                      double precision < 4.0
 */



/*
 *  If the table is built non-shared and ONE_PATH, the symbolic constant
 *  DO_ONE_PATH will get defined in the include file, to access the
 *  ONE_PATH algorithmic steps in the code.  Similarly, if the table is
 *  built shared, the symbolic constant DO_SHARED_TABLE will be defined
 *  in the generated include file.  These constants can be used to check
 *  that compile-time options are consistent with built-time options.
 */

#undef DO_SHARED_TABLE
#undef DO_ONE_PATH 


#if !MAKE_INCLUDE
#   if MAKE_COMMON
#      define TABLE_IS_EXTERNAL 1
#   else
#      undef TABLE_IS_EXTERNAL 
#   endif

#   include  STR(BUILD_FILE_NAME) 
#endif

#if LOG1P && DO_ONE_PATH
#     error Cannot use ONE_PATH algorithm for log1p
#endif

#if DO_ONE_PATH && !USE_RECIP
#     error "ONE_PATH requires using USE_RECIP option"
#endif

#if DO_SHARED_TABLE && !USE_RECIP
#     error "Shared table must have USE_RECIP true - no alternate polys"
#endif

#if DO_SHARED_TABLE && !NO_DIVISIONS
#     error "Shared table must have NO_DIVISIONS true - no alternate polys"
#endif


/*  
 * MPHOC code to generate the include file.
 *
 *  When processed by MPHOC, this code will generate arrays and definitions
 *  for various constants, coefficients, and the table of Fj data.
 *
 *  Since the coefficients and tables are different, depending on whether
 *  ONE_PATH or two paths are required, if the one path algorithm is desired,
 *  ONE_PATH must be specified at build-time, and non-shared tables must be
 *  selected.  The include file generated by ONE_PATH will set another
 *  symbolic constant, DO_ONE_PATH.
 *
 *  The arrays contain:
 *    - various constants, e.g. 1.0, log(2), log(e);
 *    - coefficients for all the approximating polynomials
 *    - the table of Fj data, which contains Fj, 1/Fj and log(Fj) in either
 *         hi and lo parts or in backup precision, in the appropriate base.
 *
 *  The endpoints for the "near 1" interval, T1 and T2, in a useful format,
 *  are also generated.
 *
 *  Logarithms are given in base e by default.  The compile options 
 *   BASE_OF_LOG = BASE_2  or BASE_10   generate the include files for the
 *  log2 and the log10 families of functions.
 *
 */

#if !defined(LOG_K)
#  if (!(MAKE_COMMON)) && PRECISION_BACKUP_AVAILABLE
#      define LOG_K   6
#  else
#      define LOG_K   7
#  endif
#endif

#   define TMP_FILE             ADD_EXTENSION(BUILD_FILE_NAME,tmp)


#if MAKE_COMMON
#   define START_TABLE(n,o)     START_GLOBAL_TABLE(n,o)
#else
#   define START_TABLE(n,o)     START_STATIC_TABLE(n,o)
#endif



#if MAKE_INCLUDE

@divert divertText


/*
 *  Various constants, e.g. log(2), will be built with EXP_WIDTH trailing 
 *  zeros, so that multiplying by the exponent will not lose any bits.
 */

#define PARTIAL_PRECISION     (B_PRECISION - B_EXP_WIDTH)



/*
 *  Print statements.
 */

#define PRINT_F_ITEM(a)     PRINT_1_TYPE_ENTRY(F_CHAR, a, offset)
#define PRINT_R_ITEM(a)     PRINT_1_TYPE_ENTRY(R_CHAR, a, offset)
#define PRINT_B_ITEM(a)     PRINT_1_TYPE_ENTRY(B_CHAR, a, offset)

#if MAKE_COMMON
#    define PRINT_POLY_ITEM(a)  PRINT_B_ITEM(a)
#else
#    define PRINT_POLY_ITEM(a)  PRINT_F_ITEM(a)
#endif

#define PRINT_TABLE_ITEM( a, b, c, d, i) \
      printf("\t/* %3i */", BYTES(offset));\
      printf("\t %#.4"STR(B_CHAR)",", a);\
      printf("\t %#.4"STR(B_CHAR)",", b);\
      printf("\t %#.4"STR(B_CHAR)",", c);\
      printf("\t %#.4"STR(B_CHAR)",", d);\
      printf("\t/* row %i */ \n", i); \
      offset += 4*BITS_PER_B_TYPE;

#define PRINT_ONE_PATH_TABLE_ITEM( a, b, c, d, z, i) \
      printf("\t/* %3i */", BYTES(offset));\
      printf("\t %#.4"STR(R_CHAR)",", a);\
      printf("\t %#.4"STR(R_CHAR)",", b);\
      printf("\t %#.4"STR(B_CHAR)",", c);\
      printf("\t %#.4"STR(B_CHAR)",", d);\
      printf("\t %#.4"STR(B_CHAR)",", z);\
      printf("\t/* row %i */ \n", i); \
      offset += 4*BITS_PER_B_TYPE;


#    define SET_MP_PREC(x)      precision = x
#    define WORKING_PRECISION   ceil( (B_PRECISION + 1) / MP_RADIX_BITS) + 2
#    define WORKING_REMES_PREC  ( ceil(2*B_PRECISION/MP_RADIX_BITS) + 5) + 10


/*
 *  MPHOC function for remes approximation of log(z), using the Taylor's
 *  series.
 */

   function log_x_plus_1_over_x()
        {
        if ($1 == 0)
            return 1;
        else {
           if (($1 < 1/MP_RADIX) && ($1 > - 1/MP_RADIX))
             return (logx1($1))/($1);
           else
            return log(1 + $1)/($1);
         }
        }


   function
    do_regular_coeffs()
{
    a = - $1;
    b = $1;
    remes_bits_of_accuracy = $2;

    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT + REMES_LINEAR_ARG,
          a, b, log_x_plus_1_over_x, remes_bits_of_accuracy, 
          &remes_degree_numer,  &remes_coeff_numer);

    for (i = 1; i <= remes_degree_numer ; i++) {
       y = remes_coeff_numer[i];
       PRINT_POLY_ITEM( y * log_of_e); 
     }

    return (remes_degree_numer);
 
 }

/*
 *  MPHOC function for remes approximation of log(z), using the variable
 *     z = (1 + x)/(1 - x)
 */

    function atanh_ov_x()
        {
        if ($1 == 0)
            return 2;
        else
            return 2*atanh($1)/($1);
        }


   function
    do_quot_coeffs()
{
    a = 0;
    b = $1;
    remes_bits_of_accuracy = $2;

    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG,
          a, b, atanh_ov_x, remes_bits_of_accuracy, 
          &remes_degree_numer,  &remes_coeff_numer);

    div_by_two = 2;
    for (i = 1; i <= remes_degree_numer ; i++) {
       div_by_two *= 4;
       y = remes_coeff_numer[i];
       PRINT_F_ITEM( y * log_of_e / div_by_two); 

     }

    return (remes_degree_numer);
 }



/*
 *  Constant log(2), either in backup precision or in hi and lo parts.
 *
 *  For base 2 and 10, constant log(e), either in backup precision or in
 *  hi and lo parts.  The hi and lo parts are given in two ways:
 *      LOGE_HI  and   LOGE_LO   are in full working precision
 *      LOGE_HI2 and   LOGE_LO2  are in half precision ("shortened").
 *   Note that LOGE_HI2 + LOGE_LO2 = LOGE_HI.
 */


    procedure do_constants() {

           v = MPLOG(2);

#if MAKE_COMMON || !PRECISION_BACKUP_AVAILABLE

     TABLE_COMMENT("log of 2 in hi and lo parts");

#if ONE_PATH
     y = bround(v, R_PRECISION);
#else
     y = rint(part_prec * v) / part_prec;
#endif
     z = v - y;     

     PRINT_TABLE_VALUE_DEFINE(LOG2_HI, LOG_TABLE_NAME, offset, B_TYPE);  
     PRINT_B_ITEM( y);
     PRINT_TABLE_VALUE_DEFINE(LOG2_LO, LOG_TABLE_NAME, offset, B_TYPE);  
     PRINT_B_ITEM( z);

#else

     PAD_IF_NEEDED(offset, BITS_PER_B_TYPE);
     TABLE_COMMENT("log of 2 in full precision");
     PRINT_TABLE_VALUE_DEFINE(LOG2_HI, LOG_TABLE_NAME, offset, B_TYPE);  
     PRINT_B_ITEM(v);           

#endif

#if (BASE_OF_LOG != BASE_E)
     TABLE_COMMENT("log of e, in hi and lo parts");
     a = bround(log_of_e, B_PRECISION);
     b = bround(log_of_e, F_PRECISION/2);
     c = log_of_e - a;
     d = log_of_e - b;

#  if PRECISION_BACKUP_AVAILABLE && !MAKE_COMMON
     PRINT_TABLE_VALUE_DEFINE(LOGE_HI, LOG_TABLE_NAME, offset, B_TYPE);  
     PRINT_B_ITEM(a);
     PRINT_TABLE_VALUE_DEFINE(LOGE_LO, LOG_TABLE_NAME, offset, B_TYPE);  
     PRINT_B_ITEM(c);
#  else
     PRINT_TABLE_VALUE_DEFINE(LOGE_HI, LOG_TABLE_NAME, offset, B_TYPE);  
     PRINT_B_ITEM(a);
     PRINT_TABLE_VALUE_DEFINE(LOGE_HI2, LOG_TABLE_NAME, offset, B_TYPE);
     PRINT_B_ITEM(b);
     PRINT_TABLE_VALUE_DEFINE(LOGE_LO, LOG_TABLE_NAME, offset, B_TYPE);  
     PRINT_B_ITEM(c);
     PRINT_TABLE_VALUE_DEFINE(LOGE_LO2, LOG_TABLE_NAME, offset, B_TYPE);
     PRINT_B_ITEM(d);

#  endif

#endif
         }

/*
 *  There are 3 ways to build the table of F(j) and related data:
 *    1.  if ONE_PATH and DOUBLE_PRECISION or larger, we need to store  1/F(j)
 *        in hi and lo parts, to preserve accuracy.  So each row 
 *        of the table contains 5 items:
 *
 *          F(j)  = 1 + j/2^LOG_K     in "half" precision
 *          hi part of log(F(j))      in "half" precision
 *          full precision form of 1/F(j)
 *          lo part of 1/F(j)   where (1/F)_full - (1/F)_lo is "short" 
 *                                  (has around "half" precision - 3 bits)
 *          lo part of log(F(j))
 *
 *    2.  each row contains F(j), 1/F(j), and logF(j) in hi and lo parts.
 *        But F(j) can be
 *          F(j) =  1 + j/2^LOG_K + 1/2^(LOG_K+1)
 *
 *         or
 *
 *    3.    F(j) =  1 + j/2^LOG_K.
 *
 *        The first format allows indexing to be a little simpler; this
 *        is used in the non-shared table.  The second format allows the
 *        table to be used for single precision fast log, which does not
 *        have a special path near 1.  The second format is used in the
 *        shared table.
 */


    procedure do_table() {

#if (!SINGLE_PRECISION && ONE_PATH)

         TABLE_COMMENT("Table of F and log(F) hi in half-precision, ");
         TABLE_COMMENT(" and 1/F, 1/F lo, and log(F) lo in working precision");

           for (i = 0; i <= two_to_k; i++) {
              a = 1 + (i/two_to_k);
              b = 1/a;
              d = bround(b, R_PRECISION - 3);
              w = b - d;
              x = MPLOG(a);
              y = bround(x, R_PRECISION);
              z = x - y;
              PRINT_ONE_PATH_TABLE_ITEM( a, y, b, w, z, i);
            }

#else

         TABLE_COMMENT("Table of F, 1/F, and hi and lo log of F");

#  if (MAKE_COMMON || (ONE_PATH && SINGLE_PRECISION))

           for (i = 0; i <= two_to_k; i++) {
              a = 1 + (i/two_to_k);

#  else

           for (i = 0; i < two_to_k; i++) {
              a = 1 + (i/two_to_k) + (1/two_to_k_plus_1);

#  endif

              d = 1/a;
              x = MPLOG(a);
              y = rint(part_prec * x) / part_prec;
              z = x - y;
              PRINT_TABLE_ITEM( a, d, y, z, i);
            }

#endif
       }





/*
 *  This procedure generates T1 and T2 (the boundaries of the interval 
 *  "near 1") given as "integers" in the form  sign-expon-frac.
 *
 *  The values of T1 and T2 depend on LOG_K.  If x > 1 is very close to 1,
 *  say, m = 0, but j > 0, then
 *     log(x) = log(F(j))_hi + z + poly(z) + log(F(j))_lo
 *  where z = (x - F(j))* 1/F(j).  Although x - F(j) is exact, multiplying
 *  by 1/F causes a rounding error (3/2 lsb in the worst case).  log(F(j)) is
 *  approximately  j/2^LOG_K, and is given in extra precision.  If z is 
 *  computed only to working precision, then by forcing an alignment shift
 *  of at least 2 bits, the error in z is safely shifted off.  When using
 *  backup precision, a smaller alignment shift will work.
 *  The alignment shift used here is log2(SAFE_LIM) = 3  if no backup
 *  type exists, and an alignment shift of approximately 1 if there is 
 *  backup, leading to a max relative error of approx .56 lsb (double).  
 *  The smallest value of F(j) used is 1 + SAFE_LIM/2^LOG_K = T2, and 
 *  1 - SAFE_LIM/2^LOG_K for T1.  
 *
 *  In base 2 or 10, z is multiplied by log(e), which introduces an additional
 *  1 bit rounding error in the variable.  If we enlarge the interval [T1,T2]
 *  slightly, this error is also shifted off.
 *
 *  The size of the error is related to the alignment shift: an additional 
 *  shift of 1 bit reduces the (error - .5) by approximately 1/2.  
 *  This corresponds to doubling the size of the interval [T1, T2].
 *
 *  In the code, the hi word of input x is extracted into integer variable
 *  hi_x, with sign, exponent, and most significant fraction bits in the
 *  msb part of the integer.  VAX floating point will be shuffled to match
 *  this format, if there are insufficient fraction bits in the hi word.
 *  Then, we subtract and use unsigned integer compares
 *  to determine whether x was in the interval [T1, T2].
 */

#if IEEE_FLOATING
#   define R_SAFE_LIM  2
#else
#   define R_SAFE_LIM  4
#endif


#if BASE_OF_LOG == BASE_E
#   define B_SAFE_LIM  8.5
#else
#   define B_SAFE_LIM  9.75
#endif

#if PRECISION_BACKUP_AVAILABLE

#   define EXTRA_N      2
#   define EXTRA_N_Q    2
#   define EXTRA_A_Q    -2

#else

#   define EXTRA_N      4
#   if (QUAD_PRECISION && !NATURAL)
#      define EXTRA_N_Q    5
#   else
#      define EXTRA_N_Q    -1
#   endif
#   define EXTRA_A_Q    2

#endif



#if (VAX_FLOATING && (LOG_K > R_EXP_POS))
#   define R_FRAC_BITS_NEEDED   (R_EXP_POS + 16)
#else
#   define R_FRAC_BITS_NEEDED   (R_EXP_POS)
#endif

#define B_FRAC_BITS_NEEDED  (31 - B_EXP_WIDTH)


#define R_EXP_POS_64    R_EXP_POS
#define R_EXP_POS_32    R_EXP_POS

#define B_EXP_POS_64    (63 - B_EXP_WIDTH)
#define B_EXP_POS_32    (31 - B_EXP_WIDTH)

#define R_BIAS_M1    (R_EXP_BIAS - 1 - F_NORM)
#define B_BIAS_M1    (B_EXP_BIAS - 1 - F_NORM)

#if (F_PRECISION < 32)
#   define F_EXP_POS_64   R_EXP_POS_64
#   define F_EXP_POS_32   R_EXP_POS_32
#   define F_FRAC_BITS_NEEDED  R_FRAC_BITS_NEEDED
#   define F_SAFE_LIM  R_SAFE_LIM
#   define F_BIAS      R_BIAS_M1
#else
#   define F_EXP_POS_64   B_EXP_POS_64
#   define F_EXP_POS_32   B_EXP_POS_32
#   define F_FRAC_BITS_NEEDED  B_FRAC_BITS_NEEDED
#   define F_SAFE_LIM  B_SAFE_LIM
#   define F_BIAS      B_BIAS_M1
#endif





       procedure
       do_T_consts()
{

#if MAKE_COMMON

           x = 1 - (R_SAFE_LIM - 0.5)/2^(LOG_K );
           i = bexp(x);
           x = bround(x, R_FRAC_BITS_NEEDED + 1);
           frac1 = bextr(x, 2, R_FRAC_BITS_NEEDED + 1);

           y = 1 + R_SAFE_LIM/2^(LOG_K ) ;
           j = bexp(y);
           y = bround(y, R_FRAC_BITS_NEEDED + 1);
           frac2 = bextr(y, 2, R_FRAC_BITS_NEEDED + 1);

           a = ( (R_BIAS_M1 + i) * 2^R_EXP_POS_64) +
              frac1 * 2^(R_EXP_POS_64 - R_FRAC_BITS_NEEDED);
           b = ( (R_BIAS_M1 + j) * 2^R_EXP_POS_64) +
              frac2 * 2^(R_EXP_POS_64 - R_FRAC_BITS_NEEDED);

           printf("\n#define R_T1_64  (WORD) %#..16i  ", a);
           printf("\n#define R_T2_64  (WORD) %#..16i  ", b);


           c = ( (R_BIAS_M1 + i) * 2^R_EXP_POS_32) +
              frac1 * 2^(R_EXP_POS_32 - R_FRAC_BITS_NEEDED);
           d = ( (R_BIAS_M1 + j) * 2^R_EXP_POS_32) +
              frac2 * 2^(R_EXP_POS_32 - R_FRAC_BITS_NEEDED);

           printf("\n#define R_T1_32  (WORD) %#..16i  ", c);
           printf("\n#define R_T2_32  (WORD) %#..16i  ", d);


           x = 1 - (B_SAFE_LIM - 1)/2^(LOG_K );
           i = bexp(x);
           x = bround(x, B_FRAC_BITS_NEEDED + 1);
           frac1 = bextr(x, 2, B_FRAC_BITS_NEEDED + 1);

           y = 1 + B_SAFE_LIM/2^(LOG_K ) ;
           j = bexp(y);
           y = bround(y, B_FRAC_BITS_NEEDED + 1);
           frac2 = bextr(y, 2, B_FRAC_BITS_NEEDED + 1);

           a = ( (B_BIAS_M1 + i) * 2^B_EXP_POS_64) +
              frac1 * 2^(B_EXP_POS_64 - B_FRAC_BITS_NEEDED);
           b = ( (B_BIAS_M1 + j) * 2^B_EXP_POS_64) +
              frac2 * 2^(B_EXP_POS_64 - B_FRAC_BITS_NEEDED);

           printf("\n#define B_T1_64  (WORD) %#..16i  ", a);
           printf("\n#define B_T2_64  (WORD) %#..16i  ", b);


           c = ( (B_BIAS_M1 + i) * 2^B_EXP_POS_32) +
              frac1 * 2^(B_EXP_POS_32 - B_FRAC_BITS_NEEDED);
           d = ( (B_BIAS_M1 + j) * 2^B_EXP_POS_32) +
              frac2 * 2^(B_EXP_POS_32 - B_FRAC_BITS_NEEDED);

           printf("\n#define B_T1_32  (WORD) %#..16i  ", c);
           printf("\n#define B_T2_32  (WORD) %#..16i  ", d);


#else

           x = 1 - F_SAFE_LIM/2^(LOG_K );
           i = bexp(x);
           x = bround(x, F_FRAC_BITS_NEEDED + 1);
           frac1 = bextr(x, 2, F_FRAC_BITS_NEEDED + 1);

           y = 1 + F_SAFE_LIM/2^(LOG_K ) ;
           j = bexp(y);
           y = bround(y, F_FRAC_BITS_NEEDED + 1);
           frac2 = bextr(y, 2, F_FRAC_BITS_NEEDED + 1);

           a = ( (F_BIAS + i) * 2^F_EXP_POS_64) +
              frac1 * 2^(F_EXP_POS_64 - F_FRAC_BITS_NEEDED);
           b = ( (F_BIAS + j) * 2^F_EXP_POS_64) +
              frac2 * 2^(F_EXP_POS_64 - F_FRAC_BITS_NEEDED);

           printf("\n#define T1_64  (WORD) %#..16i  ", a);
           printf("\n#define T2_64  (WORD) %#..16i  ", b);


           c = ( (F_BIAS + i) * 2^F_EXP_POS_32) +
              frac1 * 2^(F_EXP_POS_32 - F_FRAC_BITS_NEEDED);
           d = ( (F_BIAS + j) * 2^F_EXP_POS_32) +
              frac2 * 2^(F_EXP_POS_32 - F_FRAC_BITS_NEEDED);

           printf("\n#define T1_32  (WORD) %#..16i  ", c);
           printf("\n#define T2_32  (WORD) %#..16i  ", d);

#endif

           printf("\n#define T2_MINUS_T1    (T2 - T1) \n");


         }





/*
 *  The MPHOC code itself.
 *
 *  The first constant is 1.0, used in scaling x to the interval [1,2).
 *  In the shared table, 1.0 is given in both double and single precision.
 *  The shared table also contains a big number which, when squared,
 *  generates an overflow.
 */

             
   working_prec = WORKING_PRECISION;
   SET_MP_PREC(working_prec);

   log_of_e = MPLOG(exp(1.0));
   part_prec = 2^PARTIAL_PRECISION;
   two_to_k = 2^LOG_K;
   two_to_k_plus_1 = 2^(LOG_K+1);

#if MAKE_COMMON
   printf("\n#include \"dpml_private.h\"\n\n");
#endif

   printf("\n#define LOG_TABLE_NAME "STR(LOG_TABLE_NAME));
   printf("\n#define TABLE_CONST  %i\n\n", LOG_K);

#if ONE_PATH
   printf("\n#define DO_ONE_PATH 1\n");
#endif

#if MAKE_COMMON
   printf("\n#define DO_SHARED_TABLE  1\n");
   printf("\n#if !TABLE_IS_EXTERNAL\n");
#endif

   START_TABLE(LOG_TABLE_NAME, offset);

#if MAKE_COMMON  

   TABLE_COMMENT("1.0 in double precision");
   PRINT_TABLE_VALUE_DEFINE(B_ONE, LOG_TABLE_NAME, offset, B_TYPE);
   PRINT_B_ITEM(1);

   TABLE_COMMENT("1.0 in single precision");
   PRINT_TABLE_VALUE_DEFINE(R_ONE, LOG_TABLE_NAME, offset, R_TYPE);
   PRINT_R_ITEM(1);

   PAD_IF_NEEDED(offset, BITS_PER_B_TYPE);

   TABLE_COMMENT("max float, to generate overflow in fast log");
   PRINT_TABLE_VALUE_DEFINE(OVF_LIM, LOG_TABLE_NAME, offset, B_TYPE);
   PRINT_B_ITEM(MPHOC_D_POS_HUGE);

   working_prec = WORKING_REMES_PREC;
   SET_MP_PREC(working_prec);

/*
 *  The shared table includes poly coefficients, in single and double
 *  precision, for both accurate and fast log.  Seven polynomials are
 *  generated:
 *      'accurate' single precision, near 1 and away from 1
 *      'fast' single precision, away from 1 (we use one path)
 *      'accurate' double precision, near 1 and away from 1
 *      'fast' double precision, near 1 and away from 1
 *
 *  Single precision coefficients are stored in double precision.
 *  The approximation interval for F_FLOAT is twice as large as for S_FLOAT,
 *  because of the interaction of table size, VAX float format, and indexing.
 *
 *  Double precision fast coefficients are split into two polynomials,
 *  to allow greater flexibility in adding terms, for performance.
 */

   
   TABLE_COMMENT("accurate poly coeffs, single precision, near 1");
   PRINT_TABLE_ADDRESS_DEFINE(R_POLY_ADD_ACC_NEAR, LOG_TABLE_NAME, 
        offset, B_TYPE);
   max_arg =  R_SAFE_LIM/(2^(LOG_K ));
   deg_sa_near = do_regular_coeffs(max_arg, R_PRECISION + 2);   


   TABLE_COMMENT("accurate/fast poly coeffs, single precision, away from 1");
   PRINT_TABLE_ADDRESS_DEFINE(R_POLY_ADD_AWAY, LOG_TABLE_NAME, 
        offset, B_TYPE);
#  if IEEE_FLOATING
   max_arg = 1/(2^(LOG_K + 1));
#  else
   max_arg = 1/(2^(LOG_K));
#  endif
   deg_sa_away = do_regular_coeffs(max_arg, R_PRECISION - 3 ); 


   TABLE_COMMENT("accurate poly coeffs, double precision, near 1");
   PRINT_TABLE_ADDRESS_DEFINE(B_POLY_ADD_ACC_NEAR, LOG_TABLE_NAME, 
        offset, B_TYPE);
   max_arg =  B_SAFE_LIM/(2^(LOG_K ));
   deg_da_near = do_regular_coeffs(max_arg, B_PRECISION + 4);   

   TABLE_COMMENT("constant 1, the linear coeff of the fast near 1 poly");
   PRINT_B_ITEM(log_of_e);

   TABLE_COMMENT("fast poly coeffs, double precision, near 1");
   PRINT_TABLE_ADDRESS_DEFINE(B_POLY_ADD_FAST_NEAR, LOG_TABLE_NAME, 
        offset, B_TYPE);
   max_arg =  B_SAFE_LIM/(2^(LOG_K ));
   deg_df_near = do_regular_coeffs(max_arg, B_PRECISION + 2);   

 
   TABLE_COMMENT("accurate poly coeffs, double precision, away from 1");
   PRINT_TABLE_ADDRESS_DEFINE(B_POLY_ADD_ACC_AWAY, LOG_TABLE_NAME, 
        offset, B_TYPE);
   max_arg = 1/(2^(LOG_K + 1));
   deg_da_away = do_regular_coeffs(max_arg, B_PRECISION - 2 );


   TABLE_COMMENT("fast poly coeffs, double precision, away from 1");
   PRINT_TABLE_ADDRESS_DEFINE(B_POLY_ADD_FAST_AWAY, LOG_TABLE_NAME, 
        offset, B_TYPE);
   max_arg = 1/(2^(LOG_K + 1));
   deg_df_away = do_regular_coeffs(max_arg, B_PRECISION - 8); 



#else

/*
 *  The non-shared poly coefficients are all given in working precision.
 *  They include either the ONE_PATH coefficients (one poly), or
 *  near-1 and away-from-1 coefficients, each given in the default expansion
 *  or the form that requires a divide (4 polys).
 */


   TABLE_COMMENT("1.0 in working precision");
   PRINT_TABLE_VALUE_DEFINE(F_ONE, LOG_TABLE_NAME, offset, F_TYPE);
   PRINT_F_ITEM(1);

   working_prec = WORKING_REMES_PREC;
   SET_MP_PREC(working_prec);


#   if ONE_PATH

   TABLE_COMMENT("poly coeffs for ONE_PATH");
   PRINT_TABLE_ADDRESS_DEFINE(POLY_ADDRESS_ONEP, LOG_TABLE_NAME, offset, F_TYPE);
   max_arg = 1/(2^(LOG_K + 1));
   deg_away = do_regular_coeffs(max_arg, F_PRECISION + 3);


 
#   else

   TABLE_COMMENT("poly coeffs, near 1");
   PRINT_TABLE_ADDRESS_DEFINE(POLY_ADDRESS_NEAR, LOG_TABLE_NAME, 
        offset, F_TYPE);
   max_arg =  F_SAFE_LIM/(2^(LOG_K ));
   deg_near = do_regular_coeffs(max_arg, F_PRECISION +  EXTRA_N);   


   TABLE_COMMENT("poly coeffs, quotient, near 1");
   PRINT_TABLE_ADDRESS_DEFINE(POLY_ADD_N_Q, LOG_TABLE_NAME, 
        offset, F_TYPE);
   max_arg = F_SAFE_LIM/2^(LOG_K);
   deg_near_q = do_quot_coeffs(max_arg, F_PRECISION + EXTRA_N_Q);   


   for (j = 0; j < deg_near_q ; j++) {
      printf("\n#define B%i POLY_ADD_N_Q[%i]", (2*j + 3), j);
    }

 
   TABLE_COMMENT("poly coeffs, away from 1");
   PRINT_TABLE_ADDRESS_DEFINE(POLY_ADDRESS_AWAY, LOG_TABLE_NAME, 
        offset, F_TYPE);
   max_arg = 1/(2^(LOG_K + 1));
   deg_away = do_regular_coeffs(max_arg, F_PRECISION - 2 );


   TABLE_COMMENT("poly coeffs, quotient, away from 1");
   PRINT_TABLE_ADDRESS_DEFINE(POLY_ADD_A_Q, LOG_TABLE_NAME, 
        offset, F_TYPE);
   max_arg = 1/2^LOG_K;
   deg_away_q = do_quot_coeffs(max_arg, F_PRECISION + EXTRA_A_Q );   

   for (j = 0; j < deg_away_q ; j++) {
      printf("\n#define C%i POLY_ADD_A_Q[%i]", (2*j + 3), j);
    }


#  endif

   printf("\n");
   PAD_IF_NEEDED(offset, BITS_PER_B_TYPE);

#endif

   working_prec = 2* WORKING_PRECISION;
   SET_MP_PREC(working_prec);

/*
 *  The constants include MPLOG(2), either in backup precision or in hi and
 *  lo parts, and MPLOG(e), in backup precision or in hi and lo parts.
 */

   do_constants();

/*
 *  The table of F(j), reciprocal of F(j), and logF(j).
 */

   printf("\n#define LOG_F_TABLE  %i \n", BYTES(offset) );       
   do_table();

   END_TABLE;

#if MAKE_COMMON
   printf("\n#else\n");
   printf("\n extern const TABLE_UNION "STR(LOG_TABLE_NAME)"[]; \n");
   printf("\n#endif\n");
#endif

/*
 *  The constants T1 and T2, which define the endpoints of the "near 1"
 *  interval.
 */
   
#if !ONE_PATH
   do_T_consts();
#endif

/*
#define GENP(addr, name) shell(STR(GENPOLY_EXECUTABLE)" \
      " offset=-%i  degree=%i " \
       "define=\"" STR(macro) "(x,y) y = \" " \
       "cn=\"" STR(table) "[%%d]\" "
*/


#if MAKE_COMMON

   shell(""STR(GENPOLY_EXECUTABLE)" one offset=-2  degree=%i c0=0 c1=0 c2=0 cn=\"R_POLY_ADD_ACC_NEAR[%%d]\" define=\"R_POLY_NEAR(x,y)  y = \" ", deg_sa_near + 1);



   shell(""STR(GENPOLY_EXECUTABLE)" one offset=-2  degree=%i c0=0 c1=0 cn=\"R_POLY_ADD_AWAY[%%d]\" define=\"R_POLY_AWAY(x,y)  y = \" ", deg_sa_away + 1);

   shell(""STR(GENPOLY_EXECUTABLE)" one offset=-2  degree=%i c0=0 c1=0 c2=0 cn=\"B_POLY_ADD_ACC_NEAR[%%d]\" define=\"B_POLY_NEAR(x,y)  y = \" ", deg_da_near + 1);

   shell(""STR(GENPOLY_EXECUTABLE)" one offset=-2  degree=%i c0=0  cn=\"B_POLY_ADD_FAST_NEAR[%%d]\" define=\"B_FAST_POLY_NEAR(x,y)  y = \" ", deg_df_near + 1);

   shell(""STR(GENPOLY_EXECUTABLE)" one offset=-2  degree=%i c0=0 c1=0 cn=\"B_POLY_ADD_ACC_AWAY[%%d]\" define=\"B_POLY_AWAY(x,y)  y = \" ", deg_da_away + 1);

   shell(""STR(GENPOLY_EXECUTABLE)" one offset=-2  degree=%i c0=0 c1=0  cn=\"B_POLY_ADD_FAST_AWAY[%%d]\" define=\"B_FAST_POLY_AWAY(x,y)  y = \" ",3);

   shell(""STR(GENPOLY_EXECUTABLE)" one offset=-2  degree=%i c0=0 c1=0 c2=0 c3=0 cn=\"B_POLY_ADD_FAST_AWAY[%%d]\" define=\"B_FAST_POLY_AWAY2(x,y)  y = \" ", deg_df_away + 1);



#else
#  if ONE_PATH

   shell(""STR(GENPOLY_EXECUTABLE)" one offset=-2  degree=%i c0=0 c1=0  cn=\"POLY_ADDRESS_ONEP[%%d]\" define=\"POLY_AWAY(x,y)  y = \" ", deg_away + 1);

#  else
   shell(""STR(GENPOLY_EXECUTABLE)" one offset=-2  degree=%i c0=0 c1=0 c2=0 cn=\"POLY_ADDRESS_NEAR[%%d]\" define=\"POLY_NEAR(x,y)  y = \" ", deg_near + 1);

   shell(""STR(GENPOLY_EXECUTABLE)" one odd  degree=%i  c1=0  cn=B%%d define=\"POLY_NEAR_Q(x,y)  y = \" ", 2*deg_near_q + 1);

   shell(""STR(GENPOLY_EXECUTABLE)" one offset=-2  degree=%i c0=0 c1=0 cn=\"POLY_ADDRESS_AWAY[%%d]\" define=\"POLY_AWAY(x,y)  y = \" ", deg_away + 1);

   shell(""STR(GENPOLY_EXECUTABLE)" one odd   degree=%i  c1=0  cn=C%%d define=\"POLY_AWAY_Q(x,y)  y = \" ", 2*deg_away_q + 1);

#  endif
#endif

   printf("\n\n");

@end_divert
@eval my $outText = MphocEval( GetStream( "divertText" ) ); 		\
      my $defineText = Egrep( "#define",  $outText, \$tableText );	\
      my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),		\
                       "Definitions and constants for " .		\
                       STR(F_ENTRY_NAME),  __FILE__);			\
         print "$headerText\n\n$tableText\n\n$defineText";	

#endif



/*
 *   MACROS 
 */ 


/*
 *  Defines for the poly evaluations.
 */

#if DO_SHARED_TABLE

# if FAST

#   if SINGLE_PRECISION
#      define LO_POLY_AWAY  R_POLY_AWAY
#      define HI_POLY_AWAY
#      define POLY_NEAR

#   else
#      define LO_POLY_AWAY  B_FAST_POLY_AWAY
#      define HI_POLY_AWAY  B_FAST_POLY_AWAY2
#      define POLY_NEAR     B_FAST_POLY_NEAR


#   endif

# else

#   if SINGLE_PRECISION
#      define POLY_AWAY  R_POLY_AWAY
#      define POLY_NEAR  R_POLY_NEAR
#   else
#      define POLY_AWAY  B_POLY_AWAY
#      define POLY_NEAR  B_POLY_NEAR
#   endif

# endif

#endif

#if FAST
#   define EVAL_NEAR_POLY POLY_NEAR

#   define EVAL_LO_FAR_POLY LO_POLY_AWAY
#   define EVAL_HI_FAR_POLY HI_POLY_AWAY
#endif


#if USE_RECIP
#   define EVAL_FAR_POLY  POLY_AWAY
#else
#   define EVAL_FAR_POLY  POLY_AWAY_Q
#endif

#if NO_DIVISIONS
#  if DO_ONE_PATH
#     define EVAL_NEAR_POLY
#  else
#     define EVAL_NEAR_POLY  POLY_NEAR
#  endif

#else
#   define EVAL_NEAR_POLY  POLY_NEAR_Q
#endif


/*
 *  Macros for naming the endpoints of the "near 1" interval, depending
 *  on precision and BITS_PER_WORD.
 */

#if DO_SHARED_TABLE
#   if SINGLE_PRECISION
#      define T1_64  R_T1_64  
#      define T2_64  R_T2_64  
#      define T1_32  R_T1_32
#      define T2_32  R_T2_32  
#   elif DOUBLE_PRECISION
#      define T1_64  B_T1_64  
#      define T2_64  B_T2_64  
#      define T1_32  B_T1_32
#      define T2_32  B_T2_32  
#   endif

#endif


#if (BITS_PER_WORD == 64)
#   define T1    T1_64
#   define T2    T2_64
#else 
#   define T1    T1_32
#   define T2    T2_32
#endif 

#if DO_SHARED_TABLE 
#  if  SINGLE_PRECISION
#       define ONE R_ONE
#  else
#       define ONE B_ONE
#  endif
#else
#  define ONE F_ONE
#endif


/*
 *  Macros for fetching data from the F table.
 */


#if DO_ONE_PATH && !PRECISION_BACKUP_AVAILABLE

#   define GET_F(j) *((R_TYPE *) ((char *)LOG_TABLE_NAME + LOG_F_TABLE + (j )))

#   define LOGF_HI(j) *((R_TYPE *) ((char *)LOG_TABLE_NAME + LOG_F_TABLE + (j ) + BYTES_PER_R_TYPE))

#   define RECIP_F_FULL(j) *((B_TYPE *) ((char *)LOG_TABLE_NAME + LOG_F_TABLE + (j ) + BYTES_PER_B_TYPE))

#   define RECIP_F_LO(j) *((B_TYPE *) ((char *)LOG_TABLE_NAME + LOG_F_TABLE + (j ) + 2*BYTES_PER_B_TYPE))

#   define LOGF_LO(j) *((B_TYPE *) ((char *)LOG_TABLE_NAME + LOG_F_TABLE + (j ) + 3*BYTES_PER_B_TYPE))

#else

#   define GET_F(j) *((B_TYPE *) ((char *)LOG_TABLE_NAME + LOG_F_TABLE + (j )))

#   define RECIP_F(j) *((B_TYPE *) ((char *)LOG_TABLE_NAME + LOG_F_TABLE + (j ) + BYTES_PER_B_TYPE))

#   define LOGF_HI(j) *((B_TYPE *) ((char *)LOG_TABLE_NAME + LOG_F_TABLE + (j ) + 2*BYTES_PER_B_TYPE))

#   define LOGF_LO(j) *((B_TYPE *) ((char *)LOG_TABLE_NAME + LOG_F_TABLE + (j ) + 3*BYTES_PER_B_TYPE))

#endif


/*
 *  For log1p, fetch the constant 1.0, and add it to x (provided that rounding
 *  modes are not "round up" and x is not fmax).  For IEEE, screen out NaNs
 *  and infinities, and also input near 0 (including denorms).
 */

#if DO_LOG1P

#   if VAX_FLOATING

#      define LOGP_CHANGE_VAR_AND_GET_ONE(x, tmpx, float_one, j, m, label) \
          float_one = ONE; \
          tmpx = x + float_one;

#   else

#     define LOWER_LIMIT  (F_EXP_BIAS - F_PRECISION - 2)
#     if DYNAMIC_ROUNDING_MODES

#         define LOGP_CHANGE_VAR_AND_GET_ONE(x, tmpx, float_one, j, m, label) \
              GET_EXP_WORD(x, j);  \
              float_one = ONE; \
              m = (j & F_EXP_MASK); \
              if (m <= ((U_WORD) LOWER_LIMIT << F_EXP_POS)) return (x); \
              if (m > ((U_WORD)F_MAX_BIASED_EXP << F_EXP_POS)) goto label; \
              if (m == ((U_WORD)F_MAX_BIASED_EXP << F_EXP_POS)) tmpx = x; \
              else  tmpx = x + float_one; 

#      else
#         define LOGP_CHANGE_VAR_AND_GET_ONE(x, tmpx, float_one, j, m, label) \
              GET_EXP_WORD(x, j);  \
              float_one = ONE; \
              m = (j & F_EXP_MASK); \
              if (m > ((U_WORD)F_MAX_BIASED_EXP << F_EXP_POS)) goto label; \
              if (m <= ((U_WORD) LOWER_LIMIT << F_EXP_POS)) return (x); \
              tmpx = x + float_one; 

#      endif
#    endif

#else
#    define  LOGP_CHANGE_VAR_AND_GET_ONE(x, tmpx, float_one, j, m, label)
#endif


/* 
 *  Macro GET_HI_WORD gets the sign, exponent, and hi fraction bits, of either
 *  the original x, or (for log1p) of temp_x.
 *
 *  VAX F format, accurate log, is treated specially, because we don't want
 *  to incur a penalty for using the "long" indexing, which costs 2 extra
 *  instructions.  Otherwise, for VAX format, if INDEX_BITS_NEEDED > 
 *  number of contiguous hi fraction bits, we do a PDP shuffle to get more
 *  fraction bits.  The fast F format log needs the first non-contiguous 
 *  fraction bit as "rounding" information, because it does not have a
 *  special path near 1 and so has to be careful.
 *
 *  The resulting integer word is used in screening for "near 1" and in 
 *  computing the index.  We also need to know the location of x's sign bit.
 */

#if DO_SHARED_TABLE && VAX_FLOATING && (TABLE_CONST == F_EXP_POS)
#  define SPECIAL_VAX 1
#else
#  define SPECIAL_VAX 0
#endif

#if DO_ONE_PATH || (DO_SHARED_TABLE && !SPECIAL_VAX)
#  define LONG_INDEX 1
#  define INDEX_BITS_NEEDED  (TABLE_CONST + 1)
#else
#  define LONG_INDEX 0
#  define INDEX_BITS_NEEDED  TABLE_CONST
#endif



#if VAX_FLOATING

#  if (INDEX_BITS_NEEDED > F_EXP_POS)
#     define CURRENT_EXP_POS  (BITS_PER_WORD - (F_EXP_WIDTH + 1))

#     define GET_HI_WORD(input, j)   GET_EXP_WORD(input, j);j = PDP_SHUFFLE(j);
#     define FINAL_SIGN_BIT_POSITION(j)   j = PDP_SHUFFLE(F_SIGN_BIT_MASK)

#  elif (SPECIAL_VAX && !FAST)

#     define HI_HALF  MAKE_MASK(16,0)

#     define CURRENT_EXP_POS  F_EXP_POS
#     define GET_HI_WORD(input, j)   GET_EXP_WORD(input, j);  j &= HI_HALF;
#     define FINAL_SIGN_BIT_POSITION(j)   j = (F_SIGN_BIT_MASK)

#  else
#     define CURRENT_EXP_POS  F_EXP_POS
#     define GET_HI_WORD(input, j)   GET_EXP_WORD(input, j);
#     define FINAL_SIGN_BIT_POSITION(j)   j = (F_SIGN_BIT_MASK)
#  endif

#elif IEEE_FLOATING
#  define  CURRENT_EXP_POS   F_EXP_POS
#  define  GET_HI_WORD(input, j)  GET_EXP_WORD(input, j)
#else
#  error Unsupported floating point format
#endif

/*
 *  Loads the constant 1.0 as early as possible, but after starting to fetch
 *  the hi word of x (except in log1p, which loads 1.0 even earlier).
 *  In fast log, also encourage the early loading of other constants.
 */

#if DO_LOG1P
#  define PRE_LOAD_ONE(z) 
/*
       clear = CLEAR_MASK; \
       index_mask = JMASK; \
       rounding_bit = ROUND_BIT;
 */
#else

#  if FAST

#      define PRE_LOAD_ONE(z)    z = ONE;\
          clear = CLEAR_MASK; \
          index_mask = JMASK; \
          rounding_bit = ROUND_BIT;

#  else

#      define PRE_LOAD_ONE(z)    z = ONE;

#  endif

 
#endif



/*
 *  Computes the index into the F_table, using the hi LOG_K fraction bits
 *  of x (really, from the integer hi word of x, PDP shuffled for double
 *  precision VAX).  The original hi part of x has been manipulated so that
 *  at least INDEX_BITS_NEEDED bits lie to the right of the exponent field, 
 *  down to (and including) the lsb of the integer.  The index is these leading
 *  INDEX_BITS_NEEDED fraction bits, multiplied by 2^4 or 2^5 for single or 
 *  double precision respectively (each row of the table has 4 floating point
 *  numbers, for a total of 16 or 32 bytes).
 *
 *  Indexing into the table is slightly different for ONE_PATH or two-path
 *  algorithm.  In the ONE_PATH case, F(j) = 1 + j/2^LOG_K.  So to find
 *  the nearest F(j) to a given scaled x, we need to round the fraction to
 *  to LOG_K bits, by clearing out the exponent field, adding 1 in the LOG_K +1
 *  position, and examining the first LOG_K fraction bits and the low order
 *  exponent bit.  In the two-path case, F(j) = 1 + j/2^LOG_K + 1/2^(LOG_K+1).
 *  So to find the nearest F(j), we need only look at the first LOG_K bits.
 */

#define LOG2_ITEMS_PER_TABLE_ENTRY   2

#if (BYTES_PER_B_TYPE <= 4) && !PRECISION_BACKUP_AVAILABLE
#define SHIFT_THE_INDEX   (2 + LOG2_ITEMS_PER_TABLE_ENTRY)
#elif (BYTES_PER_B_TYPE <= 8)
#define SHIFT_THE_INDEX   (3 + LOG2_ITEMS_PER_TABLE_ENTRY)
#elif (BYTES_PER_B_TYPE <= 16)
#define SHIFT_THE_INDEX   (4 + LOG2_ITEMS_PER_TABLE_ENTRY)
#else
#error Unknown floating point type
#endif


#define EXP_SIGN_MASK  MAKE_MASK((F_EXP_WIDTH + 1), 0)
#define SIGN_ONLY_MASK  MAKE_MASK(1, (F_EXP_WIDTH))

#if SPECIAL_VAX

#   define JMASK   MAKE_MASK(INDEX_BITS_NEEDED,(CURRENT_EXP_POS - TABLE_CONST))
#   define CLEAR_MASK  EXP_SIGN_MASK
#   define SHIFT_AMOUNT (SHIFT_THE_INDEX - (CURRENT_EXP_POS - TABLE_CONST))
#   define ROUND_BIT     ((U_WORD) 1 << (31))

#   if FAST

#     define SHIFT_GET_INDEX_AND_EXPONENT(hi_x, m, j) \
         rounding_bit &= hi_x; \
         j = (hi_x & index_mask); \
         j <<= SHIFT_AMOUNT; \
         other = ((rounding_bit) ? ((U_WORD)1 << SHIFT_AMOUNT) : 0); \
         j += other; \
         m = (hi_x >> CURRENT_EXP_POS); \
         m &= clear; 

#   else

#     define SHIFT_GET_INDEX_AND_EXPONENT(hi_x, m, j) \
         index_mask = JMASK; \
         j = (hi_x  & index_mask);  \
         j <<= SHIFT_AMOUNT; \
         m = (hi_x >> CURRENT_EXP_POS); 

#   endif

#elif LONG_INDEX

#  if (CURRENT_EXP_POS - (TABLE_CONST + SHIFT_THE_INDEX) >= 0)

#    define JMASK   MAKE_MASK(INDEX_BITS_NEEDED, SHIFT_THE_INDEX)
#    define CLEAR_MASK   MAKE_MASK(INDEX_BITS_NEEDED, SHIFT_THE_INDEX - 1)
#    define ROUND_BIT         ((U_WORD)1 << (SHIFT_THE_INDEX - 1))

#    define SHIFT_GET_INDEX_AND_EXPONENT(hi_x, m, j) \
          clear = CLEAR_MASK; \
          index_mask = JMASK; \
          rounding_bit = ROUND_BIT; \
      j = (hi_x >> (CURRENT_EXP_POS - (TABLE_CONST + SHIFT_THE_INDEX))); \
      m = hi_x >> CURRENT_EXP_POS; \
      j &= clear; \
      j += rounding_bit; \
      j &= index_mask; 

 
#  else

#    define JMASK  MAKE_MASK(INDEX_BITS_NEEDED,(CURRENT_EXP_POS - TABLE_CONST))
#    define CLEAR_MASK \
       MAKE_MASK(INDEX_BITS_NEEDED,(CURRENT_EXP_POS - INDEX_BITS_NEEDED))
#    define ROUND_BIT     ((U_WORD) 1 << (CURRENT_EXP_POS - INDEX_BITS_NEEDED))


#    define SHIFT_GET_INDEX_AND_EXPONENT(hi_x, m, j) \
          clear = CLEAR_MASK; \
          index_mask = JMASK; \
          rounding_bit = ROUND_BIT; \
       j = (hi_x & clear); \
       j += rounding_bit; \
       j &= index_mask; \
       j <<= (SHIFT_THE_INDEX - (CURRENT_EXP_POS - TABLE_CONST)); \
       m = (hi_x >> CURRENT_EXP_POS); \
       m &= EXP_SIGN_MASK;
  
#   endif

#else

#   if (CURRENT_EXP_POS - (TABLE_CONST + SHIFT_THE_INDEX) >= 0)

#     define JMASK   MAKE_MASK(INDEX_BITS_NEEDED, SHIFT_THE_INDEX)
#     define CLEAR_MASK 0
#     define ROUND_BIT 0

#     define SHIFT_GET_INDEX_AND_EXPONENT(hi_x, m, j) \
          index_mask = JMASK; \
       j = (hi_x >> (CURRENT_EXP_POS - (TABLE_CONST + SHIFT_THE_INDEX))); \
       j = (j & index_mask); \
       m = hi_x >> CURRENT_EXP_POS;

#   else

#    define JMASK  MAKE_MASK(INDEX_BITS_NEEDED,(CURRENT_EXP_POS - TABLE_CONST))
#    define CLEAR_MASK EXP_SIGN_MASK
#    define ROUND_BIT 0

#    define SHIFT_GET_INDEX_AND_EXPONENT(hi_x, m, j) \
          clear = CLEAR_MASK; \
          index_mask = JMASK; \
       j = (hi_x  & index_mask);  \
       j <<= (SHIFT_THE_INDEX - (CURRENT_EXP_POS - TABLE_CONST)); \
       m = (hi_x >> CURRENT_EXP_POS); \
       m &= clear;

#  endif

#endif



/*
 *  Screen out bad x.
 *
 *  In principle, for VAX format, we could screen with 
 *      "if (x <= 0) goto label"
 *  but in practice, integer compares are faster and have less impact on
 *  code scheduling than floating compares.
 *
 *  Bad x: m = sign/exponent has been shifted to the right. 
 *  For VAX double precision, if m = 0, x was zero; if m < 0, x was negative.
 *  For VAX single precision, need to make sure any fraction bits to the
 *    left of the exponent were zeroed out.
 *  For IEEE, screen out negatives and MAX_EXPONENT as well.
 */

#if VAX_FLOATING

#   if SPECIAL_VAX && !FAST

#     define SCREEN_OUT_BAD_X(hi_x, label) \
        if ((U_WORD)((hi_x >> CURRENT_EXP_POS) - 1) >=  \
              F_MAX_BIASED_EXP) goto label

#   elif (INDEX_BITS_NEEDED <= F_EXP_POS)

#     define SCREEN_OUT_BAD_X(hi_x, label) \
        if ((U_WORD)(((hi_x >> CURRENT_EXP_POS) & EXP_SIGN_MASK) - 1) >=  \
              F_MAX_BIASED_EXP) goto label

#   else

#     define  SCREEN_OUT_BAD_X(hi_x, label)   if ((WORD) hi_x <= 0) goto label

#   endif



#elif IEEE_FLOATING


#   define  SCREEN_OUT_BAD_X(hi_x, label) \
      if ((U_WORD)((hi_x >> CURRENT_EXP_POS) - 1) >= F_MAX_BIASED_EXP)\
         goto label

#else
#   error Unsupported floating point format
#endif

/*
 *  Compute the unbiased, IEEE-style exponent, in integer position.
 */

#define FINAL_VERSION_OF_EXPONENT(m)    m -= (F_EXP_BIAS - F_NORM)

/* 
 *  "Shortens" a variable to half precision or so, so that products of
 *  "short" variables will be exact.  There are two flavors of "shorten":
 *     add and subtract a BIG number to clear out a specific number
 *         number of bits.  the original number must be known to be in
 *         a given range.
 *     cast to smaller precision, so that the "shortened" number has lots
 *         of trailing zeros, or is flushed to zero.
 *  When backup precision is available (we assume this is the case for
 *  single precision), no need to shorten variables.
 */

#define BIG  F_POW_2(F_PRECISION - (F_PRECISION - TABLE_CONST)/2 - 1)


#if F_COPY_SIGN_IS_FAST

#       define SHORTEN(z, w) { \
                F_TYPE tmp = BIG; \
                w = z; \
                F_COPY_SIGN(tmp, z, tmp); \
                ADD_SUB_BIG(w, tmp); \
}

#else

#       define SHORTEN(z, w) { \
                F_TYPE tmp = BIG; \
                w = z; \
                tmp = ( (z > 0) ? tmp : -(tmp) ); \
                ADD_SUB_BIG(w, tmp); \
}

#endif


#define  SHORTEN2(z, w)      SHORTEN_VIA_CASTS(z, w)




/*
 *  There are two approximation polynomials for log:
 *     using reciprocal:  f/F(j) - 1 = (f - F(j))*(1/F(j))
 *   and
 *     using quotient:  2*(f - F(j))/(f + F(j))
 *  where f is the scaled-down x.
 *
 *  The macro PREPARE_VARIABLE_FOR_POLY chooses one approach or the other,
 *  depending on the current value of USE_RECIP.
 *  In the first case, 
 *     variable = (f - Fj) * (1/Fj)  where the reciprocal is fetched from
 *         the table.
 *  In the second case, compute 
 *     variable = 2*(f - Fj)/(f + Fj).
 *
 *
 *  In the ONE_PATH case, we do a variant of the USE_RECIP which computes
 *  the variable in hi and lo parts.  First, the reciprocal is stored in
 *  full precision and as a lo part, in the table.  A first approximation
 *  to y = (f - Fj)*(1/Fj) is computed, using the full reciprocal (1/F)_full.
 *  y is good enough to use in the polynomial, but we need something better
 *  for the linear term.  Compute 1/F_hi = 1/F_full - 1/F_lo.
 *
 *  Shorten t = (f - Fj) into a hi part, and subtract to get a lo part:
 *    t_hi = shorten(t);    t_lo = t - t_hi;
 *  Then a better approximation to the product (f - Fj)*(1/Fj) consists of
 *  the pieces
 *     (t_hi * 1/F_hi)  and  ( t_lo * 1/F_hi +  (t_hi + t_lo)*1/F_lo )
 *  
 *
 *  The variable for the approximation for log1p is also more complicated
 *  than the standard variable for log, because f = the scaled (1 + x)
 *  minus F(j) must be computed accurately.
 *  
 *  m is the IEEE-style exponent for  1 + x, and f = 2^(-m) * (1 + x).
 *  Fj is the nearest division point to f, as above.  Then t is f - F(j),
 *  computed as:
 *  
 *     m = -2, -3, ...:     t = f - Fj
 *     m = -1 :             t = (2 - Fj) + 2x
 *     m = 0 :              t = (1 - Fj) + x
 *     m = 1, 2, ... PREC-1:   t  ((2^-m) - Fj) + (2^-m)*x
 *     m = PREC, PREC+1:    t = ((2^-m)*x - Fj) + (2^-m)
 *     m = PREC+2, PREC+3, ... t = f - Fj.
 *
 *  Once t has been computed, the approximation variable is either
 *   t*(1/Fj)  or  2t/(t + 2Fj).
 */

#define SCALE_DOWN(j, tmp) \
        B_MAKE_FLOAT(((U_WORD)(B_EXP_BIAS - F_NORM - j) << B_EXP_POS), tmp)

#if DO_LOG1P

#  define SET_UP_FOR_LOGP_POLY(j, Fj, tmp, one, x, m, tmpx, y)  \
    if (m == -1) \
           y = ((B_TYPE) 2.0 - Fj) + (B_TYPE) (x + x); \
    else if (m == 0)\
           y = ((B_TYPE) one - Fj) + (B_TYPE) x; \
    else if ((m > 0) && (m < F_PRECISION))  \
       { SCALE_DOWN(m, tmp); \
        y = (tmp - Fj) + tmp * (B_TYPE) x; } \
    else if ((m == F_PRECISION) || (m == (F_PRECISION + 1))) \
        { SCALE_DOWN(m, tmp); \
        y = (tmp * (B_TYPE) x - Fj) + tmp; } \
    else  y = (B_TYPE) tmpx - Fj; 


#  if  USE_RECIP

#    define PREPARE_VARIABLE_FOR_FAR_POLY(j, Fj, tmp, one, x, m, tmpx, z, y) \
      SET_UP_FOR_LOGP_POLY(j, Fj, tmp, one, x, m, tmpx, y); \
      tmp = RECIP_F(j);  y = y * tmp; 

#  else

#    define PREPARE_VARIABLE_FOR_FAR_POLY(j, Fj, tmp, one, x, m, tmpx, z, y) \
      SET_UP_FOR_LOGP_POLY(j, Fj, tmp, one, x, m, tmpx, y); \
      tmp = (y + Fj) + Fj;  y = y/tmp ; y += y; 

#  endif

#else
               /* not LOG1P */

#  if DO_ONE_PATH && !PRECISION_BACKUP_AVAILABLE

#   define PREPARE_VARIABLE_FOR_FAR_POLY(j, Fj, tmp, one, x, m, tmpx, z, y) \
     tmp = (B_TYPE) RECIP_F_FULL(j);   tmpx -=  Fj;\
     x = RECIP_F_LO(j);    y = tmpx * tmp ;    tmp -= x; \
     SHORTEN2(tmpx, z);   x *= tmpx;   tmpx -= z; \
     z *=  tmp;   x += tmp * tmpx; 

#  else

#    if  USE_RECIP
#      define PREPARE_VARIABLE_FOR_FAR_POLY(j, Fj, tmp, one, x, m, tmpx, z, y)\
          tmp = RECIP_F(j);  y = (B_TYPE) tmpx - Fj;   y *= tmp; 
#    else
#      define PREPARE_VARIABLE_FOR_FAR_POLY(j, Fj, tmp, one, x, m, tmpx, z, y)\
          tmp = (B_TYPE) tmpx - Fj;  \
          y = (B_TYPE) tmpx + Fj;   y = tmp/y;   y += y; 
#    endif

#  endif
#endif


/*
 *  Add the linear term or a reasonable facsimile to m*log2 + logF, to
 *  the hi or lo parts as appropriate.
 *
 *  In the ONE_PATH algorithm, the linear term is split into a "hi" part
 *  that is added to (m*log2 + log(F))_hi  -  which can range in size from
 *  just a little larger than the linear term, to very large - and a "lo"
 *  part which is added to (m*log2 + log(F))_lo + poly.
 */

#if NATURAL

#  if DO_ONE_PATH && !PRECISION_BACKUP_AVAILABLE 
#    define ADD_LINEAR_TERM_TO_LOG_F(w, v, x, t, tmpx, y, z) \
       w += x;  \
       t = v + z;  tmpx = t - v;   v = t;   w += z - tmpx;

#  else
#    define ADD_LINEAR_TERM_TO_LOG_F(w, v, x, t, tmpx, y, z)        w += y;
#  endif

#else

#  if DO_ONE_PATH && !PRECISION_BACKUP_AVAILABLE
#    define  ADD_LINEAR_TERM_TO_LOG_F(w, v, x, t, tmpx, y, z) \
      SHORTEN2(z, tmpx);   z -= tmpx; \
      w += z * LOGE_HI;   w += tmpx * LOGE_LO2;   w += x * LOGE_HI; \
      z = tmpx * LOGE_HI2;   t = v + z;   z -= t - v;  v = t;  w += z;

#  else
#   define  ADD_LINEAR_TERM_TO_LOG_F(w, v, x, t, tmpx, y, z) \
       w += y * LOGE_HI;
#  endif
#endif



/*
 *  If x was close to 1, the polynomial approximation uses either x - 1
 *  (for LOG1P, x itself), or the quotient  z = 2*(x - 1)/(x + 1) 
 *  (for LOG1P, x/2 + x) as the variable for the polynomial approximation.
 *
 *  Since the quotient will not be exact, in general, the macro
 *  computes a rough estimate of z.  The error in this computed z is
 *  calculated in another macro GET_ACCURATE_LO_PART_OF_QUOTIENT.
 *  The current macro computes the quotient z, and also f = x - 1, and
 *  x = 1/(x + 1).  Both f and x are used later in GET_ACCURATE_LO_PART macro.
 */


#if DO_LOG1P

#  define TWO  (B_TYPE) 2.0

#  if NO_DIVISIONS
#      define PREPARE_VARIABLE_NEAR_1(x, one, w, tmp)   w = (B_TYPE) x
#  else
#    if PRECISION_BACKUP_AVAILABLE
#       define PREPARE_VARIABLE_NEAR_1(x, one, w, tmp)\
           tmp = (B_TYPE) x;  w = tmp + TWO;  w = tmp/w;   w += w;

#    else
#       define PREPARE_VARIABLE_NEAR_1(x, one, w, tmp)\
           tmp = x;  x = x + TWO;  x = one/x;  w = tmp * x;  w += w;
#    endif
#  endif

#else

#  if NO_DIVISIONS
#    define PREPARE_VARIABLE_NEAR_1(x, one, w, tmp)   w = (B_TYPE)(x - one)
#  else
#    if PRECISION_BACKUP_AVAILABLE
#      define PREPARE_VARIABLE_NEAR_1(x, one, w, tmp) \
        tmp = (B_TYPE)(x - one);   w = (B_TYPE)(x + one);  w = tmp/w;   w += w;
#    else
#      define PREPARE_VARIABLE_NEAR_1(x, one, w, tmp) \
         tmp = x - one;   x = x + one;   x = one/x;  w = tmp * x;  w += w;
#    endif

#  endif
#endif


/*
 *  Shortens the "near 1" variable, in preparation for adding the linear term
 *  carefully to the rest of the polynomial.
 *
 *  When backup precision is available, no need to shorten.  When using the
 *  quotient approximation near 1, the lo part is computed much more carefully,
 *  in the macro GET_ACCURATE_LO_PART.
 */

#if PRECISION_BACKUP_AVAILABLE
#   define SHORTEN_IF_NECESSARY(z, hi, lo)
#else

#  if NO_DIVISIONS
#    define SHORTEN_IF_NECESSARY(z, hi, lo)   SHORTEN(z, hi);  lo = z - hi;
#  else
#    define SHORTEN_IF_NECESSARY(z, hi, lo)   SHORTEN2(z, hi);
#  endif
#endif


/* 
 *  This macro extends the precision of the quotient u = 2(x - 1)/(x + 1), 
 *  where x is the original input to the log function and x is near 1.
 *
 *  x lies in a small interval around 1, [T1, T2] .  
 *  f was already computed as   f = x - 1.  Because x was near 1, f is exact.
 *  u_hi was already computed by taking a preliminary approximation to
 *     the quotient u, and then "shortening" u to roughly half precision.
 *  g was already computed as g = 1/(x + 1).  Clearly g is not exact.
 *
 *  We need to find u_lo so that u = u_hi + u_lo.
 *  Compute  f_hi = "shortened" f, and  f_lo = f - f_hi.
 *  Both f_hi and f_lo are exact, and u_hi is exact (at least
 *  what there is of it).
 *
 *  Multiply the basic equation    u = 2(x - 1)/(x + 1)  through by (x + 1):
 *
 *    (x + 1) * u = (x + 1) * (u_hi + u_lo) = 2 * f .
 *
 *  Replace f by (f_hi + f_lo), and solve for u_lo:
 *  
 *   (x + 1) * u_lo = 2 * f - (x + 1) * u_hi  =  2 * f - (f + 2) * u_hi =
 *           2 * f - ( f_hi * u_hi - f_lo * u_hi - 2 * u_hi).
 *
 *  All the terms on the right hand side are either exact or are products
 *  of two "short" quantities (and are therefore exact).  Grouping the
 *  terms in order of size, and dividing through by (x + 1):
 *
 *    u_lo  =  [ ( 2 * (f - u_hi) - f_hi * u_hi) - f_lo * u_hi ] * g.
 *
 *  This macro returns u_lo in the parameter f.
 */

#if (NO_DIVISIONS || PRECISION_BACKUP_AVAILABLE)
#  define GET_ACCURATE_LO_PART_OF_QUOTIENT(f, u_hi, g) 
#else
#  define GET_ACCURATE_LO_PART_OF_QUOTIENT(f, u_hi, g) \
   {  B_TYPE f_hi, f_lo; \
       SHORTEN2(f, f_hi); \
       f_lo = f - f_hi; \
       f -= u_hi; \
       f += f; \
       f_hi *= u_hi; \
       f -= f_hi; \
       f_lo *= u_hi; \
       f -= f_lo; \
       f *= g;        }
#endif


/*
 *  Combine poly term, linear term, and the second order term (if NO_DIVISIONS)
 *  and compute the final result.
 *
 *  When backup precision is available, a direct sum of the terms
 *    z - z*z/2 + poly    is sufficient.
 *
 *  When using the variable z = x - 1 (or x, in LOG1P), we have shortened
 *   z into z_hi + z_lo.  The sum of the first two terms is computed as
 *    [ z - (z_hi * z_hi)/2 ]  +   poly - ( (z + z_lo)*z_lo)/2
 *  where the first term is exact (note that z has (LOG_K + 1) trailing zeros).
 *
 *  When using the quotient variable, we add in the "fixed up" part of the
 *  linear term in the variable "extra".  There is no square term in the 
 *  quotient variable approximation.
 *
 *  In base 10 and base 2, the linear term must be multiplied carefully by
 *  log(e) and the result added carefully to the polynomial.
 */

#define HALF  ( (F_TYPE) 0.5)


#if PRECISION_BACKUP_AVAILABLE

#    if NATURAL
#       if NO_DIVISIONS
#         define SUBTRACT_SQ_TERM_AND_COMBINE(t, lin, short, lo, tmp, extra) \
             lin -= (lin * lin)*HALF; \
             t += lin; 
#       else
#         define SUBTRACT_SQ_TERM_AND_COMBINE(t, lin, short, lo, tmp, extra) \
             t += lin; 

#       endif

#    else
#       if NO_DIVISIONS
#         define SUBTRACT_SQ_TERM_AND_COMBINE(t, lin, short, lo, tmp, extra) \
             lin -= (lin * lin)*HALF; \
             t += (lin * LOGE_HI);
#       else
#         define SUBTRACT_SQ_TERM_AND_COMBINE(t, lin, short, lo, tmp, extra) \
             t += (lin * LOGE_LO); \
             t += (lin * LOGE_HI);
#       endif

#    endif


#else

#    if NATURAL
#       if !NO_DIVISIONS
#          define SUBTRACT_SQ_TERM_AND_COMBINE(t, lin, short, lo, tmp, extra) \
              t += extra; \
              t += short;
#       else

#       if DO_LOG1P
#          define SUBTRACT_SQ_TERM_AND_COMBINE(t, lin, short, lo, tmp, extra) \
              t -= ((lin + short)*lo)*HALF; \
              short -= (short * short)*HALF; \
              t += lo; \
              t += short;
#       else

#          define SUBTRACT_SQ_TERM_AND_COMBINE(t, lin, short, lo, tmp, extra) \
              t -= ((lin + short)*lo)*HALF; \
              lin -= (short * short)*HALF; \
              t += lin;
#       endif
#       endif
#    else
#       if NO_DIVISIONS
#          define SUBTRACT_SQ_TERM_AND_COMBINE(t, lin, short, lo, tmp, extra) \
              tmp = ((lin + short)*lo)*HALF; \
              t -= tmp * LOGE_HI;\
              lin -= (short * short)*HALF; \
              SHORTEN2(lin, short); \
              lo = lin - short; \
              t += lo * LOGE_HI; \
              t += short * LOGE_LO2; \
              t += short * LOGE_HI2;         
#       else
#          define SUBTRACT_SQ_TERM_AND_COMBINE(t, lin, short, lo, tmp, extra) \
              t += extra * LOGE_LO2; \
              tmp = short * LOGE_LO2; \
              tmp += extra * LOGE_HI2; \
              t += tmp; \
              t += short * LOGE_HI2;
#       endif
#    endif
#endif



/*
 *  Looks at the exponent field of x in hi_x, to see if it is zero.
 */

#define WHERE_IS_EXPON_NOW  (CURRENT_EXP_POS - F_EXP_POS)
#define  ZERO_EXPON(j)   ( !( (j) & (F_EXP_MASK << WHERE_IS_EXPON_NOW)) )



#if !defined F_ENTRY_NAME
#   define F_ENTRY_NAME       _F_ENTRY_NAME
#endif


#if !FAST

/* 
 *  The code for logarithm.
 */

F_TYPE
F_ENTRY_NAME(F_TYPE x)

{
  EXCEPTION_RECORD_DECLARATION
  WORD m,  hi_x, j;
  WORD clear, index_mask, rounding_bit;
  U_WORD screen;
  F_TYPE float_one, temp_x;

/*
 *  In log1p, start by getting the constant 1.0.  Check if x is really tiny,
 *  e.g. denorm, by looking at the exponent - if so, return x.  Then, add 1
 *  to x, provided that this won't overflow (only a danger if the rounding mode
 *  is "round to pos infinity" and x was FMAX).
 */

    LOGP_CHANGE_VAR_AND_GET_ONE(x, temp_x, float_one, hi_x, m, bad_x);
/*
 *  Fetch the sign, exponent, and highest fraction bits as an integer
 *  in hi_x.  VAX format numbers are massaged so that the fraction bits that
 *  were not adjacent to the sign and exponent are either ignored (single
 *  precision) or are swapped into the "lower" half of the integer word
 *  (double precision).  
 */

#if LOG1P
   GET_HI_WORD(temp_x, hi_x) ; 
#else
   GET_HI_WORD(x, hi_x) ; 
#endif


/*
 *  Load the constant 1.0 as early as possible, forcing early computation
 *  of the address of the array (in log1p, done already).
 */

    PRE_LOAD_ONE(float_one);

/*  
 *  Now, screen x to see if it is in the interval [T1, T2], "near 1".
 *  For efficiency, the comparison is done with an unsigned integer compare:  
 *  (x - T1) < (T2 - T1).
 */

#if !DO_ONE_PATH
   screen = (U_WORD) (hi_x - T1);

   if ( screen < T2_MINUS_T1 ) goto near_1; 
#endif

   {                
     B_TYPE w, t, y, v, z;      

/* 
 *  Normalize (a copy of) the fraction field of x to have a value between 
 *  1 and 2, by putting the exponent of 1.0 into the exponent field, 
 *  either with COPY_SIGN_EXP or directly.  
 */

#if DO_LOG1P
     F_COPY_SIGN_AND_EXP(temp_x, float_one, temp_x);
#else
     F_COPY_SIGN_AND_EXP(x, float_one, temp_x);
#endif


/*
 *  x is not in the interval [T1, T2], but it still might be negative,
 *  zero, infinity or NaN.  On the way to screening these out, shift the
 *  exponent and fraction field to the right to isolate the leading 
 *  INDEX_BITS_NEEDED fraction bits, in order to get the index of the jth row.
 */

     SHIFT_GET_INDEX_AND_EXPONENT(hi_x, m, j);

/*
 *  Continue shifting down, to isolate the exponent.  Screen out the special
 *  cases with another unsigned integer compare, to see if sign = 1, or 
 *  exponent = 0, or exponent = MAX (IEEE only).  Note that if x's exponent
 *  was zero, subtracting 1 makes it look like a large (unsigned) integer.
 */

     SCREEN_OUT_BAD_X(hi_x, bad_x);

/*
 *  So x is OK.  Get the unbiased, IEEE-style exponent m.
 */
     FINAL_VERSION_OF_EXPONENT(m);


#if !defined( LOG1p )
   denorms_rejoin:
#endif

/*
 *  Fetch the division point F(j), which is the closest table entry to x.
 *   abs(x - F(j) ) < 1/ 2^(LOG_K+1).
 */

     t = (B_TYPE) GET_F(j);

/*
 *  The variable for the approximation polynomial is either
 *     (scaled_x - F(j)) * 1/F(j)   where F(j) is fetched from the table
 *  (when USE_RECIP is true)
 *  or    
 *     2(x - F(j))/(x + F(j)).
 *  Use the latter only when divides are relatively fast.
 */

    PREPARE_VARIABLE_FOR_FAR_POLY(j, t, w, float_one, x, m, temp_x, z, y); 

/* 
 *  Compute  m*log(2) + log(F), in hi and lo parts:
 *    m*log(2)_lo + log(F)_lo
 *    m*log(2)_hi + log(F)_hi
 *
 *  The log(F) is fetched from the F_table.  The base of these logs is 
 *  2, 10 or e, as appropriate.
 */

    v = (B_TYPE) m;

#if !PRECISION_BACKUP_AVAILABLE
        w = LOG2_LO;             
        w *= v;
        w += LOGF_LO(j);             /* m*log(2)_lo + log(F)_lo   */

        v *= LOG2_HI;
        v += (B_TYPE) LOGF_HI(j);             /* m*log(2)_hi + log(F)_hi   */

#else
        w = LOG2_HI;
        w *= v;
        w += LOGF_HI(j);
#endif

/*
 *  The first order term of the polynomial, (loge)*y, is added to log(F).
 *  In the ONE_PATH algorithm, this must be done very carefully, in order
 *  not to lose accuracy.
 */

     ADD_LINEAR_TERM_TO_LOG_F(w, v, x, t, temp_x, y, z);

/*  
 *  t = poly(y)
 */

     EVAL_FAR_POLY(y, t);

/*
 *  Combine the poly with log(F).
 */

     w += t;                      

#if !PRECISION_BACKUP_AVAILABLE
        w += v;
#endif

/*
 *  So, if x = 2^m * fraction = 2^m * (F + rest) = 2^m * F * f,
 *
 *    log(x) = m * log(2) + log(F) + log(f) =   
 *
 *       (m * log(2)_hi + log(F)_hi) +
 *           (f * log(e)_hi + ( f*log(e)_lo + poly(f) +
 *              ( m * log(2)_lo + log(F)_lo ) ) )
 *
 *   where these terms are given in descending size.
 */

     return ((F_TYPE) w );

    }                 /* end x not in interval */

/*  
 *  The approximation for x near 1, in the interval (T1, T2), involves
 *  computing the variable for approximation with PREPARE_VARIABLE_NEAR_1,
 *  to get either z = x - 1 or  z = 2(x-1)/(x+1), and then splitting z
 *  into hi and lo parts z_hi and z_lo using the SHORTEN macro.
 *
 *  In the first, NO_DIVISIONS approach, it's important to split z carefully
 *  so that z_hi has N bits.  Because x was near 1, x - 1 is no smaller 
 *  than 2^(-F_PRECISION + 1) (except in log1p).  
 *  x - 1 has some trailing zeros; in fact, the smaller z is, the more 
 *  trailing zeros.
 *
 *  We can perserve accuracy in the approximation 
 *         ln(1 + z) = z - z^2/2 + z^3/3 - ....
 *  by splitting the second term z^2/2 into
 *    z_hi^2/2 (exact)  +  z_lo*(z + z_hi)/2.
 *
 *  The smaller z is, the greater the alignment shift between z_hi^2/2 and z,
 *  but then the more trailing zeros z has.  So z - z_hi^2/2 is exact, 
 *  provided that the number of bits is small enough ( < F_PRECISION/2 - 1).
 *
 *  In log1p, we use z_hi - (z_hi^2/2).  We also know that z is no smaller
 *  than 2^(-F_PRECISION + 1).
 *
 *
 *  When NO_DIVISIONS is FALSE, the quotient 2*(x-1)/(x+1) is computed
 *  a second time, using "shortened" variables to preserve accuracy by
 *  guaranteeing that products are exact.
 *
 *  Let f = x - 1  and f1 = shortened f.   f2 = f - f1 is exact.
 *    u = 2(x-1)/(x + 1) is not exact.
 *    Let u1 = shortened u.
 *  The macro GET_ACCURATE_LO_PART_OF_QUOTIENT  computes u2 = u - u1, 
 *  by reconstructing u itself in extra accuracy.
 *
 *
 *  In both approaches, single precision uses double precision as a backup type
 *  in critical steps.
 */

#if !DO_ONE_PATH

  near_1:


#if DO_LOG1P
      if (temp_x == float_one) return (x);
#endif

     { 
        B_TYPE  t, z, w, v, y;

/*
 *   z =  x - 1   or  2(x-1)/(x+1).   In the second case,  x is returned
 *   as  x/(1+x), and y is returned as x - 1.  These variables will be
 *   used later in getting a more accurate version of the quotient.
 */

          PREPARE_VARIABLE_NEAR_1(x, float_one, z, y);
/*
 *  If no backup precision is available, split z into hi and lo parts.
 */

          SHORTEN_IF_NECESSARY(z, w, v);
/*
 *  t = poly(z).  Does not include linear or square terms.
 */

          EVAL_NEAR_POLY(z, t); 

/*
 *  An accurate estimate of the lo part of the approximation variable is
 *  returned in y  (NO_DIVISIONS = FALSE case).
 */

          GET_ACCURATE_LO_PART_OF_QUOTIENT(y, w, x);

/*
 *  Combine the first and second order terms (using hi and lo if needed)
 *  with the remainder of the poly.
 */

          SUBTRACT_SQ_TERM_AND_COMBINE(t, z, w, v, temp_x, y);

          return ( (F_TYPE) t);




      }     /* end of x in (T1, T2) */

#endif
                       /* end "good" points */
/*  "Bad" points:
 *    1. if sign is negative but exponent is zero
 *        could be -0  (IEEE) :   return NaN, via exception dispatcher
 *        or -denorm (IEEE) :     return NaN and raise error
 *        or reserved operand (VAX) :  return x and raise error
 *    2. if sign is negative and exponent is not zero
 *        could be NaN (IEEE) :  return NaN itself
 *        or -infinity (IEEE) :  return NaN and raise error
 *        or negative number :     return NaN (IEEE) and raise error
 *    3. if sign is positive but exponent is zero
 *        could be true zero :   return -infinity (VAX, raise error)
 *        or pos denorm (IEEE) : a valid case!  scale x by 2^PRECISION
 *          and subtract PRECISION from the exponent.
 *    4. if sign is positive and exponent = EMAX (for IEEE only)
 *        could be NaN : return NaN
 *        or pos infinity : return x
 */


  bad_x:  

#if IEEE_FLOATING

     /* Check for negative arguement */
     if ((hi_x & F_SIGN_BIT_MASK) != 0) {

# if COMPATIBILITY_MODE
              if ZERO_EXPON(hi_x) {    /* sign = negative, exponent is zero 
                                          could be -0 or -denorm */

                       GET_EXCEPTION_RESULT_1(LOG_NEGATIVE, x, x); 
                                           return x;
		       }

               else  {             /* sign = negative, exponent not zero */

                  F_SET_FLAG_IF_NAN(x, m);
                  if (!m)    /* -inf or negative: return NaN, error */
                      GET_EXCEPTION_RESULT_1(LOG_NEGATIVE, x, x)
                                  return x;
		  }
# else  
		F_SET_FLAG_IF_NAN( x, m ) ;
		if ( m )
		    return x ;
		else {
		    WORD func_error_word ;
		    func_error_word = ERROR_WORD( STATUS_INVALID,
						  NEG_HUGE_INDEX,
						  NAN_INDEX,
						  F_TYPE_ENUM,
						  DPML_EDOM,
						  SIGNAL_LOGZERNEG ) ;
		    RETURN_EXCEPTION_RESULT_1( func_error_word, x, F_F, _FpCodeLog ) ;
		    }
# endif
            }                      

      else if ZERO_EXPON(hi_x) {   /*  sign = positive, expon = zero */

#if DO_LOG1P

#   if COMPATIBILITY_MODE
	/*  +0: return -inf, via RAISE */
	GET_EXCEPTION_RESULT_1(LOG_ZERO, x, x); 
	return x;
#   else  
	{   WORD func_error_word ;
	    func_error_word = ERROR_WORD( STATUS_OVERFLOW,
					  NEG_HUGE_INDEX,
					  NEG_INFINITY_INDEX,
					  F_TYPE_ENUM,
					  DPML_ERANGE,
					  SIGNAL_LOGZERNEG ) ;
	    RETURN_EXCEPTION_RESULT_1( func_error_word, x, F_F, _FpCodeLog ) ;
	    }
#   endif

	}

#else  /*  if DO_LOG1P  */

/*
 *  Push in a known exponent to check if x was zero without doing a floating
 *  compare, and (if not) to scale x to be between 2.0 and 2^PRECISION.
 */

        DENORM_TO_NORM(x, temp_x);

	if (temp_x == 0.0) {       /*  +0: should return -inf, via RAISE */

# if COMPATIBILITY_MODE
	    GET_EXCEPTION_RESULT_1(LOG_ZERO, x, x); 
	    return x;
# else  
	    WORD func_error_word ;
	    func_error_word = ERROR_WORD( STATUS_OVERFLOW,
					  NEG_HUGE_INDEX,
					  NEG_INFINITY_INDEX,
					  F_TYPE_ENUM,
					  DPML_ERANGE,
					  SIGNAL_LOGZERNEG ) ;
	    RETURN_EXCEPTION_RESULT_1( func_error_word, x, F_F, _FpCodeLog ) ;
# endif

             } else {     /* x is positive denorm - scale and compute log */


                 GET_HI_WORD(temp_x, hi_x) ;  
                                        /* compute the index again */  


                 F_COPY_SIGN_AND_EXP(temp_x, float_one, temp_x);

                 SHIFT_GET_INDEX_AND_EXPONENT(hi_x, m, j);

                 FINAL_VERSION_OF_EXPONENT(m);                

                 m -= __LOG2_DENORM_SCALE;

                 goto denorms_rejoin;
		 }
  }

#endif
       else   {                     /* sign = positive, expon = MAX */
                                    /* pos inf or NaN: just return x, no err */
               return (x);
             }


#elif VAX_FLOATING

     FINAL_SIGN_BIT_POSITION(m);
     if ((hi_x & m) != 0) {        /* sign = negative. */
                                   /* if exponent is zero, reserved operand */
                                   /* if exponent not zero, negative number */

                  GET_EXCEPTION_RESULT_1(LOG_NEGATIVE, x, x)
                         return x;
           }

       else if ZERO_EXPON(hi_x) {    /*  sign = positive, expon = zero */
                                    /*  zero:  raise error */
                  GET_EXCEPTION_RESULT_1(LOG_ZERO, x, x); 
                                  return x;
           }

#else
#error  Unsupported floating point format
#endif

 }       /* end of logarithm */


#else

/*
 *  Fast logarithm.  The algorithmic steps are the same as in accurate log,
 *  but in slightly different order.
 */
 

F_TYPE
F_ENTRY_NAME(F_TYPE x)

{ WORD m,  hi_x, j;
  WORD clear, index_mask, rounding_bit, other;
  U_WORD screen; 
  F_TYPE float_one, tx;
  B_TYPE  w, w1, w4, t, y, v, z;

printf("x = %8.8x\n", (int *) &x);
   GET_HI_WORD(x, hi_x);

   PRE_LOAD_ONE(float_one);

   F_COPY_SIGN_AND_EXP(x, float_one, tx);

   w1 = LOG2_HI;

#if DOUBLE_PRECISION
   w = LOG2_LO;
#endif

   SHIFT_GET_INDEX_AND_EXPONENT(hi_x, m, j);
   FINAL_VERSION_OF_EXPONENT(m);

   v = (B_TYPE) m;

#if DOUBLE_PRECISION
   screen = (U_WORD) (hi_x - T1);
#endif

   t = (B_TYPE) GET_F(j);

   z = RECIP_F(j); 
   y = (B_TYPE) tx - t;       /* f - Fj */

#if DOUBLE_PRECISION
   if ( screen < T2_MINUS_T1 ) goto near_1; 
#endif

   y *= z;                    /* y= (f - Fj)*recip */

   SCREEN_OUT_BAD_X(hi_x, bad_x);  

   w1 *= v;                   /* m*log2_hi */
#if DOUBLE_PRECISION
   w *= v;                    /* m*log2_lo */
#endif

   EVAL_LO_FAR_POLY(y, z);

#if DOUBLE_PRECISION
   EVAL_HI_FAR_POLY(y, w4);
#endif


#if SINGLE_PRECISION 
#  if NATURAL

       w = y + (B_TYPE) LOGF_HI(j);
       v = w + w1;

#  else

       w1 += (B_TYPE) LOGF_HI(j);
       v = w1 + y*LOGE_HI;

#  endif
#else
#  if NATURAL


   v = w1 + (B_TYPE) LOGF_HI(j);     /* m*log(2)_hi + log(F)_hi   */
   w += LOGF_LO(j);             /* m*log(2)_lo + log(F)_lo   */
   v += y;                    /* hi sum + y */

#  else

   v = w1 + (B_TYPE) LOGF_HI(j);     /* m*log(2)_hi + log(F)_hi   */
   w += LOGF_LO(j);             /* m*log(2)_lo + log(F)_lo   */
   v += y * LOGE_HI;                    /* hi sum + y */

#  endif
#endif

   v += z;                       /* hi sum + hi poly */

#if DOUBLE_PRECISION

   w += w4;                      /* lo sum + lo poly */
   v += w;

#endif

   return( (F_TYPE) v);

/*
 *  In fast log, only double precision has the near 1 code path.
 */

#if DOUBLE_PRECISION
 near_1:
    z = (B_TYPE)(x - float_one);

    EVAL_NEAR_POLY(z, v);

    return(v);

#endif

/*
 *  Error cases:
 *    hi_x holds the sign, exponent, some fraction bits of x, and,
 *    if VAX format and PDP-shuffle was not performed, possibly some
 *    fraction bits to the left of the exponent.
 *
 *    If +Inf or NaN return x.  Otherwise signal error
 *
 *    We raise an error by generating an overflow in floating multiply.
 */


bad_x:

#   if IEEE_FLOATING

#       define EXP_MASK  MAKE_MASK(F_EXP_WIDTH, CURRENT_EXP_POS)

        m = hi_x & EXP_MASK;
        if (m == EXP_MASK)
            { /* x is NaN or Inf - Check sign for -Inf */
            F_SET_FLAG_IF_NAN(x, j);
            if (j || ((hi_x ^ m) == 0))
                 /* x was NaN or +Inf */
                 return x;
             }

#   endif

    return ((F_TYPE) (OVF_LIM * OVF_LIM));

    return (x);


}


#endif

