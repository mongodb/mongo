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

#define ENDIF	foo = 1;	/* Explain this */

/* File: dpml_pow_cons.c */
/*
**  Facility:
**
**	DPML
**
**  Abstract:
**
** 	This file is used to generate common include files for the 
**      DPML functions that are related to the exp function.  Currently
**	the generated file is shared by:
**
**		o exp (fast and accurate)
**		o pow (fast and accurate)
**		o expm1
**		o sinh and cosh
**
**	Where appropriate, this file also contains brief description of the
**	algorithms used in the above functions.
**
**  Modification History:
**
**	1-001	Initial implementation. Martha Jaffe 27-May-1994.
**
**	2-001	Initial implementation. RNH 01-Feb-95
**      2-002   Added hi-limit check const for exp2.  MJ 10-Dec-98
**      2-003   Added 'rm TMP_FILE'. RNH 04-Sep-2002
*/

/*
**  SUMMARY OF BUILD INFORMATION
**  ----------------------------
**
**  Since the total size of the constants and tables required to build the power
**  routines is large, by default we assume that the constants will be shared
**  whenever possible between data types and functions.  Switches are provided
**  to over-ride the default sharing behavior.
**
**  Also, there is a switch to determine if the argument reduction scheme for
**  the accurate power routine uses a divide operation or not.  The default is
**  to not use divide.
**
**  The following table summerizes the supportted switches
**
**	   Switch		                  Meaning
**	-----------	-------------------------------------------------
**	NO_FAST		Don't generate values for the fast routines.
**
**	NO_ACC		Don't generate values for the accurate routines.
**
**	ONE_TYPE	Only generate values for the specified type 
**
**	USE_DIVIDE	Generate constant necessary for doing the log argument
**			    reduction using division
**
**  The defualt values of the above switches are a function of data type:
**
**				     Default
**				---------------------
**		   Switch	Single  Double  Quad 
**		-----------	---------------------
**		NO_FAST		False	False	True
**		NO_ACC		False	False	False
**		ONE_TYPE	False	False	True
**		USE_DIVIDE	False	False	True
**
**
**		NOTE: when sharing the generated table between type,
**		the larger precision type must be specified when
**		processing this file.
**
**  In addition to the above build flags, users can also specify the size
**  (actually, the log2 of the size) of the exp and log tables by defining
**  POW2_K and LOG2_K respectively.  The default values are POW2_K = 8 and
**  LOG2_K = 7.  The implications of changing these values is discussed
**  below.  (Look for the string "DEFINING THE TABLE SIZES");
*/

#if defined X_FLOAT
#   define _X_FLT_DEF	1
#else
#   define _X_FLT_DEF	0
#endif

#if defined(NO_FAST)
#   undef   NO_FAST
#   define  NO_FAST	1
#else	
#   define  NO_FAST	_X_FLT_DEF
#endif

#if defined(NO_ACC)
#   undef   NO_ACC
#   define  NO_ACC	1
#else	
#   define  NO_ACC	0
#endif

#if defined(ONE_TYPE)
#   undef   ONE_TYPE
#   define  ONE_TYPE	1
#else	
#   define  ONE_TYPE	_X_FLT_DEF
#endif

#if defined(USE_DIVIDE)
#   undef   USE_DIVIDE
#   define  USE_DIVIDE	1
#else	
#   define  USE_DIVIDE	_X_FLT_DEF
#endif

#if NO_FAST && NO_ACC
#   error "ERROR:  Can't define both NO_FAST and NO_ACC"
#endif

#if USE_DIVIDE && NO_ACC
#   error "ERROR:  USE_DIVIDE only valid for accurate pow"
#endif

/*
 * MAKE_INCLUDE and MAKE_COMMON are always defined for this file.
 */

#undef  MAKE_INCLUDE
#define MAKE_INCLUDE

#undef  MAKE_COMMON
#define MAKE_COMMON

/*
 * Pick up default names
 */

#define	__POW_BASE_NAME		POW_BASE_NAME
#ifndef BASE_NAME
#    define BASE_NAME		__POW_BASE_NAME
#endif

#if defined(MAKE_COMMON)
#   define POW_TABLE_NAME	F_POW_TABLE_NAME 
#   define _BUILD_FILE_NAME	F_POW_BUILD_FILE_NAME
#else
#   define POW_TABLE_NAME	__F_TABLE_NAME(POW_TABLE_BASE_NAME)
#   define _BUILD_FILE_NAME	__BUILD_FILE_NAME(POW_TABLE_BASE_NAME)
#endif

#if !defined(BUILD_FILE_NAME)
#   define BUILD_FILE_NAME	_F_POW_BUILD_FILE_NAME
#endif

#if !defined(TABLE_NAME)
#   define TABLE_NAME		POW_TABLE_NAME
#endif

/*
 * Get default setting for table sizes
 */

#if !defined(LOG2_K)
#   define LOG2_K	7
#endif

#if !defined(POW2_K)
#   define POW2_K	8
#endif

/*
** Set types for default print macros.  Also set flag to pickup latest
** version of the mphoc macros.
*/

#define MP_T_TYPE	B_TYPE
#define MP_T_CHAR	B_CHAR
#define MP_T_PRECISION	B_PRECISION

#define NEW_DPML_MACROS	1

#include "dpml_private.h"
#include "dpml_pow.h"

#if !ONE_TYPE && (R_PRECISION + R_EXP_WIDTH + POW2_K - 1 > F_PRECISION)
#   error "ERROR: Floating types incompatible for shared tables"
#endif

/*
**  ORGANIZATION OF THE GENERATED FILE
**  ----------------------------------
**
**  The size of the table in generated file is quite large, and for the default
**  values, the single/double precision table is greater than 8k in size. In
**  order to help eliminate cache misses and ease finding problems with this
**  code and values in the tables, the table is laid out as follows:
**
**		+---------------------------------------+
**		|					|
**		|					|
**		|    table of 2^(j/2^POW2_K) values	|
**		|					|
**		|					|
**		+---------------------------------------+
**		|        Constants for fast exp		|
**		+---------------------------------------+
**		| Constants for 2^x portion of fast pow	|
**		+---------------------------------------+
**		| Constants for 2^x portion of acc pow	|
**		+---------------------------------------+
**		|         Constants for acc exp		|
**		+---------------------------------------+
**		|          Constants for expm1		|
**		+---------------------------------------+
**		|        Constants for sinh/cosh	|
**		+---------------------------------------+
**		|     Miscellaneous shared Constants 	|
**		+---------------------------------------+
**		|    Constants for log2 portion of pow	|
**		+---------------------------------------+
**		|					|
**		|					|
**		|  table of log(1 + j/2^LOG2_K) values	|
**		|					|
**		|					|
**		+---------------------------------------+
**
*/

@divert divertText

    /*
    ** GENERATING POLYNOMIAL COEFFICIENTS:
    ** -----------------------------------
    **
    ** All of the polynomial coefficients in this file are generated via the 
    ** Remes min/max error algorithm.  This algorithm takes as one of its input
    ** arguments, the function to be approximated, F(x).  For example, if we
    ** look at generating the exp and pow polynomials, F(x) can be one of e^x,
    ** (e^x - 1)/x, [e^x - (1 + x)]/x^2, 2^x, or (2^x - 1)/x.
    **
    ** In order to minimize the number of different functions defined for remes
    ** algorithm, we define F(x) as a polynomial evaluation routine, with an
    ** external (global) scale factor and initial term.  This not only reduces
    ** the number of functions that need to be defined, but also reduces the
    ** required MP precision in the calculation of the coefficients, since,
    ** the cancellation error in computations like e^x - 1 and log(x) -
    ** (x - x^2/2) have been eliminated.
    **
    ** Also, in order to insure the polynomial evaluation macro matches the
    ** coefficients, the invocation of genpoly that generates the evaluation
    ** macros is encoded as a macro definition at the time the coefficients
    ** are generated.  The macro is instantiated after the constant table is
    ** generated. 
    **
    ** Lastly, each set of coefficients is generated into the array 'coefs', so
    ** that it can be printed via a subroutine.  This requires that the 
    ** coefficients are printed immediately after they are generated.
    **/

#   define SET_POLY_GLOBALS(k, s, xs, fs)	\
		first_term       = (k);		\
		first_term_value = (s);		\
		x_scale          = (xs);	\
		final_scale      = (fs)

#   define PRINT_TBL_COM_ADEF_ARRAY(com, def, deg) \
		PRINT_TBL_COM_ADEF(com, def); \
		print_array(deg)

    procedure print_array(n)
        {
        for (i = 0; i <= n; i++)
            {
            PRINT_TBL_ITEM(coefs[i]);
            }
        }


#   define WORKING_PRECISION   (ceil(2*B_PRECISION/MP_RADIX_BITS) + 2)

    precision = WORKING_PRECISION;
    bit_precision = MP_RADIX_BITS*precision;

    /*
    ** Pick up definitions of common MP functions and print out the
    ** initial boiler plate for the generated file.  As part of the boiler
    ** plate, record the current definitions of the macro TABLE_NAME.
    ** Once that has been done, undefine TABLE_NAME so that we can define
    ** items in the generated file relative to the symbolic value TABLE_NAME
    ** rather than the actual value of TABLE_NAME
    */

#   include "mphoc_functions.h"

    printf(
         "\n"
            "/* Define default table name */\n"
         "\n"
            "#if !defined(TABLE_NAME)\n"
            "#   define TABLE_NAME\t" STR(TABLE_NAME) "\n"
            "#endif\n"
         "\n"
            "#include \"dpml_private.h\"\n"
         "\n");

#   undef TABLE_NAME

    printf("\n#if !DEFINE_SYMBOLIC_CONSTANTS\n\n");

    START_TABLE;

    /*
    **
    ** GENERAL DISCUSSION OF 2^x, e^x and 10^x
    ** ---------------------------------------
    **
    ** The computation of b^x for b = 2, e and 10 is based on a table look-up
    ** scheme, where the number of entries in the table is a power of 2,
    ** say 2^k.  Writing x*(lnb/ln2) as the sum of its integer, first k fraction
    ** bits and a reduced arguement we have:
    **
    **		x(lnb/ln2) = I + j/2^k + w,	|w| < 2^(k+1) 
    **
    ** Letting z = w*(ln2/lnb) = x - (I + j/2^k)*(ln2/lnb), the computation of
    ** e^x proceeds as:
    **
    **		b^x = 2^(x(lnb/ln2))
    **		    = 2^(I + j/2^k + w)
    **		    = 2^I * 2^(j/2^k) * 2^w
    **		    = 2^I * 2^(j/2^k) * e^z
    **		    = 2^I * 2^(j/2^k) * [ 1 + z*p(z) ]			(1)
    **
    ** In (1), the alignment shift between 1 and z*p(z) is at least k+1 bits,
    ** so if care is taken in computing 2^I*2^(j/2^k) high accuracy in the
    ** final answer is possible.  Toward this end, we suppose the values of 
    ** 2^(j/2^k) are stored in a table in hi and lo pieces, T(j) and L(j).
    ** Then (1) can be re-written as:
    **
    **		b^x = 2^I * 2^(j/2^k) * [ 1 + z*p(z) ]
    **		    = 2^I * [ T(j) + L(j) ] * [ 1 + z*p(z) ]
    **		    = 2^I * { T(j) + L(j) + [ T(j) + L(j) ]*z*p(z) }
    **		     
    ** There are various way to define T(j) and L(j) so that "extra"
    ** precision is obtained.  The definition we use here was chosen to
    ** optimize the performance of the fast exp and pow routines.  In
    ** particular:
    **
    **			T(j) = bround( 2^(j/2^k), F_PRECISION) 
    **			L(j) = 2^(j/2^k) - T(j)
    **
    ** With this definition, the term L(j)*z*p(z) is insignificant in the
    ** final sum and may be dropped, so that e^x can be approximated by:
    **
    **		b^x = 2^I * { T(j) + [ L(j) + T(j)*z*p(z) ] }	(2)
    **
    ** In order to expose more parallelism in the computation, rather than
    ** storing the values of T(j) and L(j) in the tables, we store T(j) and
    ** R(j) = L(j)/T(j) and write (2) as:
    **
    **		b^x = 2^I * { T(j) + [ L(j) + T(j)*z*p(z) ] }
    **		    = 2^I * { T(j) + T(j)* [ R(j) + z*p(z) ] }
    **		    = 2^I * T(j) + 2^I*T(j)* [ R(j) + z*p(z) ] 
    **		    = V(I,j) + V(I, j)* [ R(j) + z*p(z) ] 		(3)
    **
    ** where V(I,j) = 2^I * T(j).  Note that on pipelined architectures,
    ** R(j) + z*p(z) can be computed with the same latancy as z*p(z) and
    ** on architectures with multiple functional units V(I,j) can be computed
    ** in the integer unit while R(j) + z*p(z) is computed in the floating
    ** point unit.
    */

    /* 
    ** POW2 TABLE
    ** ----------
    **
    ** The pow2 table contains the 2^POW2_K th roots of 2, 2^(j/2^POW2_K).
    ** The table has a different form depending on whether backup precision
    ** is available or not.
    **
    ** When back up precision is not available, the table contain the values
    ** T(j) and R(j) as defined above.  When backup precision is available,
    ** only T(j) is stored.
    */

#   define __PRINT_TABLE_VALUE(tchar, value)			\
		printf( "\t/* %4i */ %#.4" STR(tchar) ",\n",	\
		  BYTES(MP_BIT_OFFSET), value);			\
		MP_BIT_OFFSET += CHAR_TO_BITS(tchar)

#   define __PRINT_TABLE_DEF(name, tchar, disp)				 \
		printf("#define " name "\t*((" STR(CHAR_TO_TYPE(tchar))	\
		     " *) ((char *) " STR(MP_TABLE_NAME) 		\
		     " + %i + (j)))\n",	BYTES(disp));			\
		disp += CHAR_TO_BITS(tchar)

#   if (USE_BACKUP)

#       define POW2_TABLE_BANNER					\
              "\n\t * Tj = 2^(j/2^POW2_K)"				\
              "\n\t *"							\
              "\n\t * offset                                   row"	\
	      "\n\t"	

#       define PRINT_POW2_TABLE_ACCESS_MACROS(disp)			\
        	PRINT_LOG_TABLE_DEF("GET_POW2(j)\t",   B_CHAR, disp)

#       define POW2_INDEX_POS		(__LOG2(BITS_PER_B_TYPE) - 3)

#       define PRINT_POW2_TABLE_ENTRY(j, Pj)				\
		printf( "\t/* %4i */ %#.4" STR(B_CHAR), ", /* %3i */",	\
		   BYTES(MP_BIT_OFFSET), Pj, j);			\
		MP_BIT_OFFSET += BITS_PER_B_TYPE

#   else /* USE_BACKUP */

#       define POW2_TABLE_BANNER					   \
	  "\n\t * Tj = 2^(j/2^POW2_K) and Rj = [2^(j/2^POW2_K) - Tj]/Tj."  \
          "\n\t *"							   \
          "\n\t * offset                            row"		   \
	  "\n\t"

#       define PRINT_POW2_TABLE_ACCESS_MACROS(disp)			  \
        	__PRINT_TABLE_DEF("POW2_HI(j)\t",          F_CHAR, disp); \
        	__PRINT_TABLE_DEF("POW2_LO_OV_POW2_HI(j)", F_CHAR, disp)

#       define POW2_INDEX_POS		(__LOG2(BITS_PER_F_TYPE) - 2)

#       define PRINT_POW2_TABLE_ENTRY(j, Pj)				\
		Pj_hi = bround(Pj, F_PRECISION);			\
		printf("\t/* %4i */ %#.4" STR(F_CHAR) ", /* %3i */\n",  \
		  BYTES(MP_BIT_OFFSET), Pj, j);				\
		MP_BIT_OFFSET += BITS_PER_F_TYPE;			\
		__PRINT_TABLE_VALUE(F_CHAR, (Pj - Pj_hi)/Pj)

#endif

    disp = MP_BIT_OFFSET;
    root_disp = disp;
    PRINT_POW2_TABLE_ACCESS_MACROS(disp);

    /*
    ** As noted above, the quantity V(I,j) = 2^I*T(j) is computed in an
    ** integer register.  The follow code prints out definitions for accessing
    ** T(j) an integer.  If the word size is smaller that the F_TYPE size, we
    ** need to access it in two pieces.  Make sure to take into account
    ** "endianess"
    */

    if (BITS_PER_WORD < BITS_PER_F_TYPE)
        {
        disp_lo = root_disp;
        if ((VAX_FLOATING) || (ENDIANESS == big_endian))
            disp_lo = root_disp + (BITS_PER_F_TYPE - BITS_PER_WORD);
        else
            root_disp += (BITS_PER_F_TYPE - BITS_PER_WORD);    

        /*
        ** If the word size is verfy small relative to the floating point
        ** type, get the low order bits in a F_UNION by loading the whole
        ** floating point type.  Otherwise, just load the low word
        */

        if (BITS_PER_WORD*2 < BITS_PER_F_TYPE)
            {
            printf("#define IPOW2_LO(u,j)\t\tu.f = "
              "*((B_TYPE *) ((char *) " STR(MP_TABLE_NAME)
               " + (j)))\n"); 
            }
        else
            {
            printf("#define IPOW2_LO(u,j)\t\tu.B_LO_WORD = "
              "*((WORD *) ((char *) " STR(MP_TABLE_NAME)
               " + %i + (j)))\n", BYTES(disp_lo)); 
            }
        }

    __PRINT_TABLE_DEF("IPOW2(j)\t", w, root_disp);

    printf("#define POW2_INDEX_POS\t\t%i \n",  POW2_INDEX_POS);

    TABLE_COMMENT( POW2_TABLE_BANNER );

    pow2_table_size = 2^POW2_K;
    for (j = 0; j < pow2_table_size; j++)
        {
        Pj = 2^(j/pow2_table_size);
        PRINT_POW2_TABLE_ENTRY( j, Pj);
        }

    /*
    ** Error Checking:
    ** ---------------
    **
    ** b^x can both underflow and overflow. Consequently some type of error
    ** check (screening) must eventually take place.  Since the appropriate
    ** timing and nature of the screening varies from function to function, it
    ** is discussed with the individual functions.
    **
    ** That said, all of the function using the pow2 table, have a "final"
    ** underflow/overflow check near the very end of the routine.  The check
    ** is based on the fact that the computation of V(I,j) is done in an
    ** integer register and provides a very good approximation to the final
    ** answer.  We can use integer comparisons on the bit pattern for V(I,j)
    ** to eliminate all potential overflows and underflows just prior to or
    ** just after the last floating point operation(s).
    */

    c = 2^(1/pow2_table_size);

    lo = F_HI_BITS_RND(2^(F_MIN_BIN_EXP + F_NORM + F_PRECISION + POW2_K)*c,
      MP_RP);
    hi = F_HI_BITS_RND(2^(F_MAX_BIN_EXP + F_NORM + 1)/c, MP_RM);
    PRINT_U_TBL_COM_VDEF_ITEM("F_PRECISION acc pow2 result range check",
      "POW2_LO_CHECK_F\t", lo);
    PRINT_U_TBL_VDEF_ITEM("POW2_HI_CHECK_F\t", hi - lo);
    PRINT_U_TBL_VDEF_ITEM("POW2_MAX_SCALE_F\t", hi);

    if (!ONE_TYPE)
        {
        lo = F_HI_BITS_RND(2^(R_MIN_BIN_EXP + R_NORM + R_PRECISION + POW2_K)*c,
           MP_RP);
        hi = F_HI_BITS_RND(2^(R_MAX_BIN_EXP + R_NORM + 1)/c, MP_RM);
        PRINT_U_TBL_COM_VDEF_ITEM("R_PRECISION acc pow2 result range check",
          "POW2_LO_CHECK_R\t", lo);
        PRINT_U_TBL_VDEF_ITEM("POW2_HI_CHECK_R\t", hi - lo);
        PRINT_U_TBL_VDEF_ITEM("POW2_MAX_SCALE_R\t", hi);
        }
    ENDIF

    /*
    ** Computation of I, j and w:
    ** --------------------------
    **
    ** From the above discussion, we see that at some point in the evaluation
    ** of b^x, we need to take a floating point value and break it into its
    ** integer part, high fraction bits and low fraction bits.  If z is the
    ** value we want to break apart, then the conceptual computation that is
    ** performed is:
    **
    **		t <-- rint(2^k*z)
    **		w = z - t/2^k
    **		m <-- (WORD) t
    **		i <-- m >> k
    **		j <-- m & (2^k - 1)
    **
    ** In actuality, the first three steps of the above is performed by taking
    ** z, adding and then subtracting a large positive constant, BIG. BIG is
    ** chosen so that the low order fraction bits of z are discarded due to
    ** the alignment shift leaving only the integer and high fraction bits.
    ** Specifically:
    **
    **		BIG <-- 3*2^(B_PRECISION - k - 2)
    **		u   <-- BIG + z
    **		fm  <-- u - BIG
    **
    ** Note that if B_PRECISION > 32 and the rounding mode is round to nearest,
    ** then the low order 32 bits of t are the twos complement representation
    ** m and fm = u/2^k.
    **
    **
    ** Polynomial Generation For 2^x, e^x and 10^x:
    ** --------------------------------------------
    **
    ** The coefficients for 2^x are based on the Taylor series expansion
    ** for e^x:
    **
    **          e^x  = 1 + x + x^2/2! + x^3/3! + ....
    **
    ** with the variable x replaced by x = z * ln2:
    **
    **          2^z = 1 + ln2*z + z^2*(ln2)^2/2! + z^3*(ln2)^3/3! + ....
    **              = 1 + z*(ln2 + z*(ln2)^2/2! + z^2*(ln2)^3/3! + ....)
    **              = 1 + z*P(z)
    **
    ** In both cases, the size of the argument being evaluated is dictated
    ** by k.
    */

    ln2 = log(2.0);
    recip_ln2 = 1/ln2;
    ln2_ov_ln10 = ln2/log(10.);
    ln10_ov_ln2 = log(10.0)/ln2;

    max_exp_x = .5/pow2_table_size;
    max_pow2_x = max_exp_x*ln2;

    /*
    ** The following function is used by the Remes algorithm to generate
    ** min/max coefficients for e^x and 2^x.  We can approximate e^x, e^x - 1
    ** and e^x - (1 + x) by specifying the (first_term, first_value) parameters
    ** as (0,1), (1, 1) and (2, .5) respectively.  By changing the x_scale and
    ** last scale values from 1 to appropiate powers of ln2, we can similarly
    ** evaluate 2^x, 2^x - 1 and 2^x - (1 + x*ln2)
    **
    */


    function e_to_x_poly(x)
        {
        auto s, z, k, t;

        s = first_term_value;

        if (x != 0)
            {
            k = first_term;
            z = x*x_scale;
            t = first_term_value;

            while(1)
                {
                k++;
                t = (t*z)/k;
                if ((bexp(s) - bexp(t)) > bit_precision)
                    break;
                s += t;
                }
            }
        ENDIF
        return s*final_scale;
        }

    /*
    ** All of the Remes invocations for exp/pow2 coeffient generations have
    ** the same form, so we make the corresponding code a macro.
    */

#   define GEN_EXP_COEFS(max_x, prec, deg, com, tag)            \
                {                                                       \
                remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT +   \
                   REMES_LINEAR_ARG, -max_x, max_x, e_to_x_poly, prec,  \
                   &deg, &coefs);                                       \
                PRINT_TBL_COM_ADEF_ARRAY(com, tag, deg);                \
                }

    /*
    ** CONSTANTS FOR FAST EXP
    ** ----------------------
    **
    ** In fast exp, we use the identity e^x = 2^(x/ln2).  Since we would like
    ** to delay the screening for overflow and underflow for as long as
    ** possible (to increase parallelism) and since x/ln2 might overflow,
    ** we perform the initial calculation as:
    **
    **		w  <-- x*[ 1/(2^n*ln2) ]
    **		t  <-- BIG/2^n + w
    **		fm <-- t - BIG/2^n
    **		z  <-- w - fm
    **
    ** This produces a reduced argument, z, "scaled down" by 2^n.  We can
    ** compensate for the scale factor in z by adjusting the coefficients
    ** in the polynomial evaluation.
    **
    ** Note that if backup precision is not available, the compuation of
    ** z is more complicated that inidicate.  Specificly, we must compute
    ** w = x*[ 1/(2^n*ln2) ] to extra precision by break x and 1/(2^n*ln2)
    ** into high and low pieces.
    **
    ** Other than requiring that n >= 1, the exact choice of n in the above
    ** discussion is arbitrary.  We choose n = F_EXP_WIDTH because, we can
    ** then share the constants with the fast pow routine.  (See below)
    */

    scale_down = 2^-F_EXP_WIDTH;
    fast_big = 3*2^(B_PRECISION - POW2_K - 2 - F_EXP_WIDTH);
    printf("#define SCALE_DOWN_EXP\t%i \n",  F_EXP_WIDTH);

    if (!NO_FAST)
        {
        PRINT_TBL_COM_VDEF_ITEM("'big' for fast pow/exp rint computation",
          "FAST_BIG\t", fast_big);

        c = scale_down*recip_ln2;
        if (ONE_TYPE)
            {
            PRINT_TBL_COM_VDEF_ITEM("2^-F_EXP_WIDTH/ln2",
               "SCALE_DOWN_OVER_LN2\t", c);
            }
        else 
            {
            TABLE_COMMENT("2^-F_EXP_WIDTH/log(2) in full, hi, lo");
 
            c_hi = bround(c, F_PRECISION - F_HI_HALF_PRECISION - 2*LOG2_K + 1);
            PRINT_TBL_VDEF_ITEM("SCALE_DOWN_OV_LN2",  c);
            PRINT_TBL_VDEF_ITEM("SCALE_DOWN_OV_LN2_HI", c_hi);
            PRINT_TBL_VDEF_ITEM("SCALE_DOWN_OV_LN2_LO", c - c_hi);
            }

        /*
        ** For fast exp, we delay screening for overflow and underflow
        ** until just before the  polynomial evaluation.  At that point
        ** we have obtained the high bits of the input argument as an
        ** integer and can perform the screening with integer operations.
        */

        c = ln2*max(-(F_MIN_BIN_EXP + F_NORM), F_MAX_BIN_EXP + 1 + F_NORM);
        PRINT_U_TBL_COM_VDEF_ITEM("Fast exp F_PRECISION arg range check",
          "FAST_EXP_RANGE_CHECK_F", F_HI_BITS_RND(c, MP_RP));

        if (!ONE_TYPE)
            {
            c = ln2*max(-(R_MIN_BIN_EXP + R_NORM), R_MAX_BIN_EXP + 1 + R_NORM);
            PRINT_U_TBL_COM_VDEF_ITEM("Fast exp R_PRECISION arg range check",
              "FAST_EXP_RANGE_CHECK_R", F_HI_BITS_RND(c, MP_RP));
            }
        ENDIF

        /* 
        ** As noted above, the fast pow and exp routines scale there input
        ** argument down to avoid premature overflow and we need  to
        ** compensated for it in the polynomial coefficients.
        **
        ** The actual form of the polynomial evaluated depends on whether
        ** or not backup precision is available.  If it is, we use a polynomial
        ** for 2^x otherwise we use one for 2^x - 1
        */


        if (USE_BACKUP)
            {
            SET_POLY_GLOBALS(0, 1, ln2, 1);
            GEN_EXP_COEFS(max_pow2_x, F_PRECISION + 1, fast_pow2_deg_f,
              "F_PRECISION fast pow2 poly coeffs", "FAST_POW2_F\t")
            GENPOLY(FAST_POW2_F[%%d], FAST_POW2_POLY_F(x), fast_pow2_deg_f);
            }
        else
            {
            max_arg = max_pow2_x*scale_down;
            c = ln2/scale_down;

            SET_POLY_GLOBALS(0, 1, c, 1);
            GEN_EXP_COEFS(max_arg, F_PRECISION + 1, fast_pow2_deg_f,
              "F_PRECISION fast pow2 poly coeffs", "FAST_POW2_F\t")
            GENPOLY(FAST_POW2_F[%%d], FAST_POW2_POLY_F(x), fast_pow2_deg_f);

            if (!ONE_TYPE)
                {
                SET_POLY_GLOBALS(0, 1, ln2, 1);
                GEN_EXP_COEFS(max_pow2_x, R_PRECISION + 1, fast_pow2_deg_r,
                  "R_PRECISION fast pow2 poly coeffs", "FAST_POW2_R\t")
                GENPOLY(FAST_POW2_R[%%d], FAST_POW2_POLY_R(x), fast_pow2_deg_r);
                }
            ENDIF
            }
        }
    ENDIF

    /*
    ** CONSTANTS FOR 2^x EVALUATION IN FAST POW
    ** ----------------------------------------
    **
    ** In fast pow, we use the identity x^y = 2^(y*log2(x)).  As in fast exp,
    ** we would like to delay the screening for overflow and underflow for as
    ** long as possible but we need to avoid overflow when computing the
    ** product y*log2(x).  To do this, we scale y down by an appropriate
    ** power of 2 prior to performing the multiplication.  Since
    **
    **	   2^(F_MIN_BIN_EXP - F_PRECISION + 1) <= x < 2^F_MAX_BIN_EXP
    **
    ** It follows that 
    **
    **  (F_MIN_BIN_EXP - F_PRECISION + 1)*ln2 <= log2(x) < F_MAX_BIN_EXP*ln2
    **
    ** On the platforms currently supportted:
    **
    **	 2^F_EXP_WIDTH > | F_MIN_BIN_EXP-F_PRECISION+1 | >= | F_MAX_BIN_EXP |
    **
    ** So that log2(x) < 2^F_EXP_WIDTH.  Therefore, the product
    ** (y * 2^-F_EXP_WIDTH)*log2(x) is guarenteed not to overflow.  Note that
    ** (y * 2^-F_EXP_WIDTH) might underflow.  But in this case the correct
    ** result of x^y is 1 to machine precision.  So even if underflow occurs
    ** the correct result we be returned.
    **
    ** For fast pow, we delay any overflow underflow checks until just before
    ** the evaluation of exponential polynomial.  At that point we perform
    ** a gross level check on x and y to sceen out all guarenteed exceptions.
    ** Specifically we need to check for very large (positive or negative)
    ** y since these will cause guarenteed overflows or underflows.
    */

    acc_big = 3*2^(B_PRECISION - POW2_K - 2);

    if (!NO_FAST)
        {
        PRINT_TBL_COM_VDEF_ITEM(
           "Power of 2 to scale down y: 2^-F_EXP_WIDTH",
           "SCALE_DOWN\t", scale_down);

        tmp = as_int(acc_big, 32, F_EXP_WIDTH, MP_F_EXP_BIAS, MP_RZ);
        printf("#define ACC_BIG_HI_32\t\t0x%8.8.16i \n",  tmp + 1);
        tmp = as_int(fast_big, 32, F_EXP_WIDTH, MP_F_EXP_BIAS, MP_RZ);
        printf("#define FAST_BIG_HI_32\t\t0x%8.8.16i \n",  tmp + 1);
        }
    ENDIF

    /*
    ** CONSTANTS FOR 2^x EVALUATION IN ACCURATE POW
    ** ---------------------------------------------
    **
    ** In the accurate power routine, both x and y are screened prior to
    ** any computation, so it is unnecesary to scale y to avoid overflow,
    ** and consequently we don't need to compensate for the scale in the
    ** polynomial coefficients.  Also, in order to minimize the number of
    ** operations performed, the argument reduction is performed as
    ** z = (x - fm*LN2_HI) - fm*LN2_LO, when backup precision is not
    ** available.
    */

    if (!USE_BACKUP)
        { /* ln2_<hi,lo> are also used in the log2 part of pow */
        c_hi = bround(ln2, R_PRECISION);
        PRINT_TBL_COM_VDEF_ITEM("ln2 in hi/lo", "LN2_HI\t\t", c_hi);
        PRINT_TBL_VDEF_ITEM("LN2_LO\t\t", ln2 - c_hi);

        c_hi = bround(ln2_ov_ln10, R_PRECISION);
        PRINT_TBL_COM_VDEF_ITEM("ln2/ln10 in hi/lo", "LN2_OV_LN10_HI\t\t", c_hi);
        PRINT_TBL_VDEF_ITEM("LN2_OV_LN10_LO\t\t", ln2_ov_ln10 - c_hi);

        }

    if (!NO_ACC)
        {
        if (USE_BACKUP)
            { /* Approximate 2^x to extra precision */
            SET_POLY_GLOBALS(0, 1, ln2, 1);
            GEN_EXP_COEFS(max_pow2_x, F_PRECISION + POW2_K + 1, acc_pow2_deg_f,
              "F_PRECISION acc pow2 poly coeffs", "ACC_POW2_F\t")
            GENPOLY(ACC_POW2_F[%%d], ACC_POW2_POLY_F(x), acc_pow2_deg_f);
            }
        else
            { /* Approximate 2^x - 1 to base precision */
            SET_POLY_GLOBALS(1, 1, ln2, ln2);
            GEN_EXP_COEFS(max_pow2_x, F_PRECISION + 1, acc_pow2_deg_f,
              "F_PRECISION acc pow2 poly coeffs", "ACC_POW2_F\t");
            _GENPOLY(ACC_POW2_F[%%d], ACC_POW2_POLY_F(t,x), -1, c0=t,
              acc_pow2_deg_f + 1);

            if (!ONE_TYPE)
                {
                SET_POLY_GLOBALS(0, 1, ln2, 1);
                GEN_EXP_COEFS(max_pow2_x, R_PRECISION + POW2_K + 1,
                  acc_pow2_deg_r, "R_PRECISION acc pow2 poly coeffs",
                  "ACC_POW2_R\t")
                GENPOLY(ACC_POW2_R[%%d], ACC_POW2_POLY_R(x), acc_pow2_deg_r);
                }
            }
        }
    ENDIF

    /*
    ** CONSTANTS FOR ACCURATE EXP
    ** --------------------------
    **
    ** As with accurate power, accurate exp screens it argument prior to
    ** to any floating point calculation, so it is un-neccessary to scale
    ** the product x*(1/ln2).  This means that the value of BIG and the 
    ** polynomial coefficients also don't require any scaling
    */

    if (!NO_ACC)
        {
        PRINT_TBL_COM_VDEF_ITEM("'big' for accurate pow/exp rint computation",
          "ACC_BIG\t\t", acc_big);

        /*
        ** For accurate exp, the initial screening weeds out large arguments
        ** (guarenteed overflow or underflow), NaNs and Infinities and very
        ** small arguements (for which the final result is 1.)
        */

        if (IEEE_FLOATING)
            lo = (F_MIN_BIN_EXP + F_NORM - F_PRECISION)*ln2;
        else
            lo = (F_MIN_BIN_EXP + F_NORM)*ln2;
        hi = (F_MAX_BIN_EXP + F_NORM)*ln2 + log((2 - 2^-F_PRECISION));

        lo_check = F_HI_BITS_RND(2^-(F_PRECISION + 1), MP_RM);
        hi_check = F_HI_BITS_RND(max(-lo, hi),         MP_RP);

        TABLE_COMMENT("F_PRECISION argument and result sreening values");

        PRINT_U_TBL_VDEF_ITEM("EXP_LO_CHECK_F\t", lo_check);
        PRINT_U_TBL_VDEF_ITEM("EXP_HI_CHECK_F\t", hi_check - lo_check);

        if (!ONE_TYPE)
            {
            if (IEEE_FLOATING)
                lo = (R_MIN_BIN_EXP - R_NORM - R_PRECISION)*ln2;
            else
                lo = (R_MIN_BIN_EXP - R_NORM)*ln2;
            hi = (R_MAX_BIN_EXP - R_NORM)*ln2 + log((2 - 2^-R_PRECISION));

            lo_check = R_HI_BITS_RND(2^-(R_PRECISION + 1), MP_RM);
            hi_check = R_HI_BITS_RND(max(-lo, hi), MP_RP);

            TABLE_COMMENT(
              "R_PRECISION argument and result sreening values");

            PRINT_U_TBL_VDEF_ITEM("EXP_LO_CHECK_R\t", lo_check);
            PRINT_U_TBL_VDEF_ITEM("EXP_HI_CHECK_R\t", hi_check - lo_check);
            }
        ENDIF

        /*
        ** Similarly, for 2^x, initial screening to weed out large arguments
        ** (guaranteed overflow or underflow), NaNs and Infinities.
        */

        if (IEEE_FLOATING)
            lo = (F_MIN_BIN_EXP + F_NORM - F_PRECISION) ;
        else
            lo = (F_MIN_BIN_EXP + F_NORM);
        hi = (F_MAX_BIN_EXP + F_NORM) + log2((2 - 2^-F_PRECISION));

        hi_check = F_HI_BITS_RND(max(-lo, hi),         MP_RP);
        lo_check = F_HI_BITS_RND(2^-(F_PRECISION + 1), MP_RM);

        TABLE_COMMENT("F_PRECISION argument screening values for 2^x");

        PRINT_U_TBL_VDEF_ITEM("EXP2_HI_CHECK_F\t", hi_check - lo_check);

        if (!ONE_TYPE)
            {
            if (IEEE_FLOATING)
                lo = (R_MIN_BIN_EXP - R_NORM - R_PRECISION);
            else
                lo = (R_MIN_BIN_EXP - R_NORM);
            hi = (R_MAX_BIN_EXP - R_NORM) + log2((2 - 2^-R_PRECISION));

            hi_check = R_HI_BITS_RND(max(-lo, hi), MP_RP);
            lo_check = R_HI_BITS_RND(2^-(R_PRECISION + 1), MP_RM);

            TABLE_COMMENT(
              "R_PRECISION argument and result sreening values");

            PRINT_U_TBL_VDEF_ITEM("EXP2_HI_CHECK_R\t",hi_check - lo_check);
            }
        ENDIF

        /*
        ** Once again for the 10^x case
        */

        if (IEEE_FLOATING)
            lo = (F_MIN_BIN_EXP + F_NORM - F_PRECISION)*ln2_ov_ln10;
        else
            lo = (F_MIN_BIN_EXP + F_NORM)*ln2_ov_ln10;
        hi = (F_MAX_BIN_EXP + F_NORM)*ln2_ov_ln10 + log((2 - 2^-F_PRECISION));

        lo_check = F_HI_BITS_RND(2^-(F_PRECISION + 1), MP_RM);
        hi_check = F_HI_BITS_RND(max(-lo, hi),         MP_RP);

        TABLE_COMMENT("F_PRECISION argument and result sreening values for 10^x");

        PRINT_U_TBL_VDEF_ITEM("EXP10_LO_CHECK_F\t", lo_check);
        PRINT_U_TBL_VDEF_ITEM("EXP10_HI_CHECK_F\t", hi_check - lo_check);

        if (!ONE_TYPE)
            {
            if (IEEE_FLOATING)
                lo = (R_MIN_BIN_EXP - R_NORM - R_PRECISION)*ln2_ov_ln10;
            else
                lo = (R_MIN_BIN_EXP - R_NORM)*ln2_ov_ln10;
            hi = (R_MAX_BIN_EXP - R_NORM)*ln2_ov_ln10 + log((2 - 2^-R_PRECISION));

            lo_check = R_HI_BITS_RND(2^-(R_PRECISION + 1), MP_RM);
            hi_check = R_HI_BITS_RND(max(-lo, hi), MP_RP);

            TABLE_COMMENT(
              "R_PRECISION argument and result sreening values for 10^x");

            PRINT_U_TBL_VDEF_ITEM("EXP10_LO_CHECK_R\t", lo_check);
            PRINT_U_TBL_VDEF_ITEM("EXP10_HI_CHECK_R\t", hi_check - lo_check);
            }
        ENDIF


        /*
        ** When backup precision is available, accurate exp uses a polynomial
        ** for 2^x otherwise it uses one for e^x.
        **/

        if (USE_BACKUP)
            {
            SET_POLY_GLOBALS(0, 1, ln2, 1);
            GEN_EXP_COEFS(max_exp_x, F_PRECISION + POW2_K + 1, acc_exp_deg_f,
              "F_PRECISION acc exp poly coeffs", "ACC_EXP_F\t");
            GENPOLY(ACC_EXP_F[%%d], ACC_EXP_POLY_F(x), acc_exp_deg_f);
            }
        else
            {
            SET_POLY_GLOBALS(1, 1, 1, 1);
            GEN_EXP_COEFS(max_exp_x, F_PRECISION + 1, acc_exp_deg_f,
              "F_PRECISION acc exp poly coeffs", "ACC_EXP_F\t");
            _GENPOLY(ACC_EXP_F[%%d], ACC_EXP_POLY_F(t,x), -1, c0=t,
              acc_exp_deg_f + 1);

            /*
            **       NOTE: if (!ONE_TYPE) then ACC_EXP_POLY is identical
            **       to ACC_POW2_POLY
            */

            }
        }
    ENDIF

    /*
    ** CONSTANTS FOR EXPM1
    ** -------------------
    **
    ** For expm1, we essentially compute the accurate exp function and
    ** subtract 1.  However, to maintain accuracy in all cases, when
    ** backup precision is not available, we need to compute evaluate
    ** e^z as 1 + z + z^2*q(z) rather than as 1 + z*p(z)
    **
    ** Also, screening the input argument is a little more involved.  We need
    ** to screen for large arguments (both positive and negative) and small
    ** arguments (where a polynomial approximation is appropriate).
    **
    ** The bound for large positive arguments is the same as for exp.  For
    ** large negative arguments, we want to know where expm1(x) = -1 to 
    ** machine precision.  Because the check is done on both positive and
    ** negative arguments on a sign/magnitude value, it is done in two
    ** parts, one for the positive arguments and one for the negative
    ** arguments.
    **
    ** We arbitrarily define the polynomial range to have at least the same
    ** "effective" overhang as the table range.  ("Effective" overhang is
    ** actual overhang less the number of bits of error in the smaller term.)
    */

    expm1_max_poly_arg = 2/pow2_table_size;
    poly = F_HI_BITS_RND(expm1_max_poly_arg, MP_RM);
    lo = F_HI_BITS_RND((F_PRECISION + 1)*ln2, MP_RP);
    hi = F_HI_BITS_RND((F_MAX_BIN_EXP + F_NORM + 1)*ln2, MP_RP);

    PRINT_U_TBL_COM_VDEF_ITEM("F_PRECISION expm1 initial screening constants",
      "EXPM1_POLY_CHECK_F", poly);
    PRINT_U_TBL_VDEF_ITEM("EXPM1_HI_CHECK_F",  hi);
    PRINT_U_TBL_VDEF_ITEM("EXPM1_LO_CHECK_F",  lo);
      

    if (!ONE_TYPE)
        {
        poly = R_HI_BITS_RND(expm1_max_poly_arg, MP_RM);
        lo = R_HI_BITS_RND((R_PRECISION + 1)*ln2, MP_RM);
        hi = R_HI_BITS_RND((R_MAX_BIN_EXP + R_NORM + 1)*ln2, MP_RP);
        PRINT_U_TBL_COM_VDEF_ITEM(
          "R_PRECISION expm1 initial screening constants",
          "EXPM1_POLY_CHECK_R", poly);
        PRINT_U_TBL_VDEF_ITEM("EXPM1_HI_CHECK_R",  hi);
        PRINT_U_TBL_VDEF_ITEM("EXPM1_LO_CHECK_R",  lo);
        }
    ENDIF

    expm1_max_red_arg = 2/2^POW2_K;
    if (USE_BACKUP)
        {
        SET_POLY_GLOBALS(1, 1, 1, 1);
        GEN_EXP_COEFS(expm1_max_poly_arg, F_PRECISION + POW2_K,
          expm1_poly_deg_f, "F_PRECISION expm1 poly range poly coeffs",
          "EXPM1_F\t\t");
        _GENPOLY(EXPM1_F[%%d], EXPM1_POLY_F(x), -1, c0=0,
           expm1_poly_deg_f + 1);

        SET_POLY_GLOBALS(1, 1, ln2, ln2);
        GEN_EXP_COEFS(expm1_max_red_arg, F_PRECISION + POW2_K, expm1_red_deg_f,
          "F_PRECISION expm1 reduce range poly coeffs", "EXPM1_RED_F\t");
        _GENPOLY(EXPM1_RED_F[%%d], EXPM1_RED_POLY_F(x), -1, c0=0,
           expm1_red_deg_f + 1);
        }
    else
        {
        SET_POLY_GLOBALS(2, .5, 1, 1);

        GEN_EXP_COEFS(expm1_max_poly_arg, F_PRECISION + 1, expm1_poly_deg_f,
          "F_PRECISION expm1 poly range poly coeffs", "EXPM1_F\t\t");
        _GENPOLY(EXPM1_F[%%d], EXPM1_POLY_F(x) (x) +, -2, c0=0 c1=0,
          expm1_poly_deg_f + 2);

        GEN_EXP_COEFS(expm1_max_red_arg, F_PRECISION + 1, expm1_red_deg_f,
          "F_PRECISION expm1 reduce range poly coeffs", "EXPM1_RED_F\t");
        _GENPOLY(EXPM1_RED_F[%%d], EXPM1_RED_POLY_F(t,x), -2, c0=t c1=0,
          expm1_red_deg_f + 2);

        if (!ONE_TYPE)
            {
            SET_POLY_GLOBALS(1, 1, 1, 1);
            GEN_EXP_COEFS(expm1_max_poly_arg, R_PRECISION + POW2_K,
              expm1_poly_deg_r, "R_PRECISION expm1 poly range poly coeffs",
              "EXPM1_R\t\t");
            _GENPOLY(EXPM1_R[%%d], EXPM1_POLY_R(x), -1, c0=0,
               expm1_poly_deg_r + 1);

            SET_POLY_GLOBALS(1, 1, ln2, ln2);
            GEN_EXP_COEFS(expm1_max_red_arg, R_PRECISION + POW2_K,
              expm1_red_deg_r, "R_PRECISION expm1 reduce range poly coeffs",
              "EXPM1_RED_R\t");
            _GENPOLY(EXPM1_RED_R[%%d], EXPM1_RED_POLY_R(x), -1, c0=0,
               expm1_red_deg_r + 1);
            }
        ENDIF
        }

    /*
    ** CONSTANTS FOR SINH/COSH
    ** -----------------------
    **
    ** For sinh/cosh, we screen for large arguments (both positive and
    ** negative) and small arguments (where a polynomial approximation is
    ** appropriate).
    **
    ** The bound for large arguments is log(2*F_MAX).
    **
    ** We arbitrarily define the polynomial range to have at least the same
    ** "effective" overhang as the table range.  ("Effective" overhang is
    ** actual overhang less the number of bits of error in the smaller term.)
    */

    sinhcosh_max_poly_arg = sqrt(8/pow2_table_size);

    hi = F_HI_BITS_RND( (F_MAX_BIN_EXP + 1 + F_NORM)*ln2 +
      log((2 - 2^-(F_PRECISION - 1))), MP_RP);
    lo = F_HI_BITS_RND(sinhcosh_max_poly_arg, MP_RM);

    TABLE_COMMENT("F_PRECISION sinh/cosh argument screening constants");
    PRINT_U_TBL_VDEF_ITEM("SINHCOSH_OVERFLOW_CHECK_F", hi);
    PRINT_U_TBL_VDEF_ITEM("SINHCOSH_BIG_CHECK_F", hi - lo);
    PRINT_U_TBL_VDEF_ITEM("SINHCOSH_POLY_CHECK_F", lo);

    if (!ONE_TYPE)
        {
        hi = R_HI_BITS_RND((R_MAX_BIN_EXP + 1 - R_NORM)*ln2 +
          log((2 - 2^-(R_PRECISION - 1))), MP_RP);
        lo = R_HI_BITS_RND(sinhcosh_max_poly_arg, MP_RM);

        TABLE_COMMENT("R_PRECISION sinh/cosh argument screening constants");
        PRINT_U_TBL_VDEF_ITEM("SINHCOSH_OVERFLOW_CHECK_R", hi);
        PRINT_U_TBL_VDEF_ITEM("SINHCOSH_BIG_CHECK_R", hi - lo);
        PRINT_U_TBL_VDEF_ITEM("SINHCOSH_POLY_CHECK_R", lo);
        }
    ENDIF

    /*
    **
    ** The coefficients for sinh/cosh are based on the Taylor series expansions
    **
    **		sinh(x) = x + x^3/3! + x^5/5! ....
    **		        = x*[1 + x^2*P(x^2)]
    **
    **		cosh(x) = 1 + x^2/2! + x^4/4! ....
    **		        = 1 + x^2*Q(x^2)]
    **
    ** On the reduced range, the coefficients for accurate exp(x) are used and
    ** simply broken up into even and odd polynomials
    **
    ** The following function is used for the Remes approximation in much the
    ** same way as the e_to_x_poly() function is used.  That is by
    ** appropriately setting the values first_term, first_term_value, x_scale
    ** and final_scale, we can approximate, sinh(x), cosh(x), sinh(x) - x,
    ** cosh(x) - 1, sinh(x*ln2), cosh(x*ln2), ...
    */

    function sinh_cosh_poly(x)
        {
        auto s, z, k, t;

        s = first_term_value;

        if (x != 0)
            {
            k = first_term;
            z = (x*x)*x_scale;
            t = first_term_value;

            while(1)
                {
                k += 2;
                t = (t*z)/(k*k - k); 
                if ((bexp(s) - bexp(t)) > bit_precision)
                    break; 
                s += t; 
                }
            }
        ENDIF
        return s*final_scale;
        }

#   define GEN_SINH_COSH_COEFS(max_x, prec, deg, com, tag)		\
                {							\
		remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT +	\
		   REMES_SQUARE_ARG, 0, max_x, sinh_cosh_poly,		\
		    prec, &deg, &coefs);				\
		PRINT_TBL_COM_ADEF_ARRAY(com, tag, deg);		\
		}


    if (USE_BACKUP)
        {
        SET_POLY_GLOBALS(1, 1, 1, 1);
        GEN_SINH_COSH_COEFS(sinhcosh_max_poly_arg, F_PRECISION + POW2_K,
          sinh_poly_deg_f, "F_PRECISION sinh poly range poly coeffs",
          "SINH_F\t\t");
        _GENPOLY(SINH_F[%%d], SINH_POLY_F(x), -1, odd stride=2,
           2*sinh_poly_deg_f + 1);

        SET_POLY_GLOBALS(0, 1, 1, 1);
        GEN_SINH_COSH_COEFS(sinhcosh_max_poly_arg, F_PRECISION + POW2_K,
          cosh_poly_deg_f, "F_PRECISION cosh poly range poly coeffs",
          "COSH_F\t\t");
        _GENPOLY(COSH_F[%%d], COSH_POLY_F(x), -1, even stride=2,
           2*cosh_poly_deg_f);

        _GENPOLY(ACC_POW2_F[%%d], SINHCOSH_ODD_POLY_F(x), 0, odd,
           acc_pow2_deg_f);
        _GENPOLY(ACC_POW2_F[%%d], SINHCOSH_EVEN_POLY_F(x), 0, even,
           acc_pow2_deg_f);
        }
    else
        {
        SET_POLY_GLOBALS(3, 1/6, 1, 1);
        GEN_SINH_COSH_COEFS(sinhcosh_max_poly_arg, F_PRECISION + 1,
          sinh_poly_deg_f, "F_PRECISION sinh poly range poly coeffs",
          "SINH_F\t\t");
        _GENPOLY(SINH_F[%%d], SINH_POLY_F(x) (x) +, -3, odd stride=2 c1=0,
           2*sinh_poly_deg_f + 3);

        SET_POLY_GLOBALS(2, .5, 1, 1);
        GEN_SINH_COSH_COEFS(sinhcosh_max_poly_arg, F_PRECISION + 1,
          cosh_poly_deg_f, "F_PRECISION cosh poly range poly coeffs",
          "COSH_F\t\t");
        _GENPOLY(COSH_F[%%d], COSH_POLY_F(x) ONE +, -2, even stride=2 c0=0,
           2*cosh_poly_deg_f + 2);

        _GENPOLY(ACC_EXP_F[%%d], SINHCOSH_ODD_POLY_F(x), -1, odd,
           acc_exp_deg_f + 1);
        _GENPOLY(ACC_EXP_F[%%d], SINHCOSH_EVEN_POLY_F(x), -1, even c0=0,
           acc_exp_deg_f + 1);

        if (!ONE_TYPE)
            {
            SET_POLY_GLOBALS(1, 1, 1, 1);
            GEN_SINH_COSH_COEFS(sinhcosh_max_poly_arg, R_PRECISION + POW2_K,
              sinh_poly_deg_r, "R_PRECISION sinh poly range poly coeffs",
              "SINH_R\t\t");
            _GENPOLY(SINH_R[%%d], SINH_POLY_R(x), -1, odd stride=2,
               2*sinh_poly_deg_r + 1);

            SET_POLY_GLOBALS(0, 1, 1, 1);
            GEN_SINH_COSH_COEFS(sinhcosh_max_poly_arg, R_PRECISION + POW2_K,
              cosh_poly_deg_r, "R_PRECISION cosh poly range poly coeffs",
              "COSH_R\t\t");
            _GENPOLY(COSH_R[%%d], COSH_POLY_R(x), 0, even stride=2,
               2*cosh_poly_deg_r);

            _GENPOLY(ACC_POW2_R[%%d], SINHCOSH_ODD_POLY_R(x), 0, odd,
              acc_pow2_deg_r);
            _GENPOLY(ACC_POW2_R[%%d], SINHCOSH_EVEN_POLY_R(x), 0, even,
              acc_pow2_deg_r);
            }
        ENDIF
        }

    /*
    ** MISCELLANEOUS SHARED CONSTANTS:
    ** -------------------------------
    **
    ** This section of MP code records the current build parameters that
    ** must be passed on to the functions that use the generated table and
    ** also generates constants that are not assocaiated with any particular
    ** function.  Begin by recording the current build parameters.
    */ 

    printf("#define LOG2_K\t\t\t%i\n",   LOG2_K);
    printf("#define POW2_K\t\t\t%i\n",   POW2_K);
    printf("#define NO_FAST\t\t\t%i\n",  NO_FAST);
    printf("#define NO_ACC\t\t\t%i\n",   NO_ACC);
    printf("#define USE_DIVIDE\t\t%i\n", USE_DIVIDE);

    /*
    ** Generate a floating point 1.0 for use in expm1 and scaling the input
    ** argument in the power functions.  Also generate 1/ln2 for scaling the
    ** input argument in exp, expm1 and sinh/cosh and .5 for near
    ** overflow/underflow fixup. 
    */


    PRINT_TBL_COM_VDEF_ITEM("B_PRECISION .5, 1.0 and 2.0", "HALF\t\t", .5);
    PRINT_TBL_VDEF_ITEM("ONE\t\t", 1.0);
    PRINT_TBL_VDEF_ITEM("TWO\t\t", 2.0);

    PRINT_TBL_COM_VDEF_ITEM("B_PRECISION max float", "MAX_FLOAT\t",
      MP_MAX_FLOAT);

    PRINT_TBL_COM_VDEF_ITEM("1/ln2 in B_PRECISION", "RECIP_LN2\t", recip_ln2);


    /*
    ** GENERAL DISCUSSION OF x^y AND log2(x)
    ** -------------------------------------
    **
    ** This implementation computes the power x^y in three conceptual stages:
    **   
    **     o  compute log2(x), with some extra bits of precision
    **     o  multiply  y * log2(x), maintaining the extra precision
    **     o  evaluate 2 ^ product.
    **
    ** In the actual implementations, the first two steps are combined.
    **
    **
    ** DEFINING THE TABLE SIZES:
    ** -------------------------
    **
    ** The evaluation of log2(x) and 2^product both use table look-up schemes
    ** to increase accuracy and performance.  The number of extra bits of
    ** precision required for log2(x) is F_EXP_WIDTH - 1 + POW2_K, where
    ** 2^POW2_K is the number of entries in the 2^x table (See the previous
    ** discussion on 2^x).
    **
    ** The total amount of extra precision in the log2(x) computation is a
    ** function of the log2 table size and the argument reduction scheme used.
    ** By way of explaination, consider calculating log2(f) for f in the
    ** interval [1,2).  Let the table size for the log2 evaluation be 2^LOG2_K
    ** and let j the integer such that Fj = 1 + j/2^LOG2_K is closest to f. 
    ** With the above definitions, we consider two possible argument reduction
    ** schemes:
    **
    **	With  :     z = (f - Fj)/(f + Fj)
    **	divide:	    log2(f) = log2(Fj) + (2/ln2)*[z + z^3/3 + z^5/5 + ...]
    **		 
    **	Without:    w = (f - Fj)/Fj
    **   divide:    log2(f) = log2(Fj) + (1/ln2)*[w - w^2/2 + w^3/3 - ... ]
    **		
    ** The worst case senario for accuracy is when f = 1 + 1/2^(LOG2_K + 1).
    ** This implies that log2(Fj) = 0 and that we can only get extended
    ** precision in the log2 computation by computing the first "few" terms
    ** of the series in extended precision.
    **
    ** In the "with divide" case, we compute z in extended precision, and the
    ** amount of extra precision in the final result is (essentially) the
    ** alignment shift between z and z^3/3, or 2*LOG2_K + 5.
    **
    ** In the "without divide" case, we compute s = w - w^2/2 in extended
    ** precision,  and the amount of extra precision in the final result is
    ** (essentially) the alignment shift between s and w^3/3, or 2*LOG2_K + 3.
    **
    ** If we are only considering accuracy, then we should chose LOG2_K and
    ** POW2_K according to the relationship:
    **
    **		2*LOG2_K + R = F_EXP_WIDTH - 1 + POW2_K
    **
    ** where R is 5 or 3 depending on whether the argument reduction is uses a
    ** divide or not.  However, since the power table is used for fast exp and
    ** regular exp (and possibly log2 and fast log2) the values of LOG2_K and
    ** POW2_K may be taken to be bigger than those prescribed by the above
    ** relation to increase the performance of any or all of the routines
    ** dependent upon the table.  In particular, the default values of LOG2_K
    ** and POW2_K do not satify the above relationship, but were chosen to
    ** optimize the performance of fast exp and fast pow.
    **
    **
    ** COMPUTATION OF LOG2(x)
    ** ----------------------
    **
    ** The computation of log2(x) proceeds as follows:
    **
    **		log2(2^I*f) = I + log2(f)
    **		            = I + log2(Fj) + log2(f/Fj)
    **			    = I + log2(Fj) + p(z)
    **
    ** where f is in [1, 2 ), Fj = 1 + j/2^LOG2_K and z is the "reduced"
    ** argument (using one of the two methods described above) and p is
    ** is a polynomial.  The form of p depends on the reduction methods.
    **
    ** 		NOTE: A more detailed discussion of the follow
    **		two sections is contained in dpml_pow.c
    **
    ** 
    ** Reduction With Divides:
    ** -----------------------
    **
    ** If the argument reduction for log2(x) is going to use a divide, then
    ** we need to compute z = [(f - Fj)/(f + Fj)]*(2/ln2) and p(z) is evaluated
    ** as:
    **
    **		p(z) = z + z^3*q(z^2)
    **
    ** where
    **
    **	    q(t) = (ln2/2)^2 * sum{ [t*(ln2/2)^2]^n/(2n+3) | n = 0, 1, ... }
    **
    ** It is necessary to compute z extra precision.  If no backup precision
    ** is available, then z must be computed in hi and lo pieces in order to
    ** obtain required accuracy for log2(x).  In this case the computation
    ** proceeds as follows:
    **
    **		t    = f - Fj
    **		s    = (f + Fj)
    **		r    = 1/s
    **		z    = t*r
    **		z_hi = hi_bits(z)
    **		f_hi = hi_bits(f)
    **		f_lo = lo_bits(f)
    ** 		z_lo = {([(f_hi - Fj)*hi_bits(2/ln2) - z_hi*s] +
    **		          f_lo*hi_bits(2/ln2)) +
    **		             [t*lo_bits(2/ln2) - z_hi*f_lo]}*g;
    **
    **
    ** Reduction Without Divides:
    ** --------------------------
    **
    ** If the argument reduction for log2(x) is not going to use a divide, then
    ** we need to compute z = (f - Fj)/(Fj*ln2) and p(z) is evaluated
    ** as:
    **
    **		p(z) = z - z^2*ln2/2 + z^3*q(z)
    **
    ** where
    **
    **	    q(t) = -(ln2)^2 * sum{ [-t*ln2]^n/(n+3) | n = 0, 1, ... }
    **
    ** It is necessary to compute s = z - z^2*ln2/2 to extra precision.  If no
    ** backup precision is available, then s must be computed in hi and lo
    ** pieces in order to obtain required accuracy for log2(x).  In this case
    ** the computation proceeds as follows:
    **
    **		t    = f - Fj
    **		z    = t*(1/(Fj*ln2))
    **		g    = Fj*Fj*(ln2/2)
    **		u    = 2*Fj
    **		s    = (u - t)*t*g
    **		s_hi = hi_bits(s)
    **		v    = Fj*s_hi
    **		t_hi = hi_bits(t)
    **		t_lo = lo_bits(t)
    **          s_lo = {[u*(t - v*hi_bits(ln2)) + t_hi^2] +
    **                     [t_lo*(t + t_hi) - u*v*lo_bits(ln2))]}*g
    **
    ** For the fast pow routine, we use the "no divide" reduction.  However,
    ** we "cheat" on the accuracy of final result by computing the polynomial
    ** as
    **		p(z) = z_hi + z_lo  - z*q(z)
    **
    ** where
    **
    **	    q(t) = ln2 * sum{ [-t*ln2]^n/(n+2) | n = 0, 1, ... }
    **
    */

    /*
    ** CONSTANTS FOR LOG2
    ** ------------------
    **
    ** When no backup is available, computing the reduced arguement requires
    ** 2/ln2 in hi an lo pieces or ln2/2 in full precision and ln2 in hi
    ** and lo pieces, depending on whether divide is used or not.
    */

    if (!USE_BACKUP)
        {
        if (USE_DIVIDE)
            {
            c = 2*recip_ln2;
            PRINT_TBL_COM_VDEF_ITEM("2/ln2 in F_PRECISION and hi/lo",
              "TWO_OVER_LN2\t", c);
            c_hi = bround(c, R_PRECISION);
            PRINT_TBL_VDEF_ITEM("TWO_OVER_LN2_HI\t", c_hi);
            PRINT_TBL_VDEF_ITEM("TWO_OVER_LN2_LO\t", c - c_hi);
            }
        else
            {
            PRINT_TBL_COM_VDEF_ITEM("ln2/2 in F_PRECISION",
              "LN2_OVER_TWO\t", .5*ln2);
            }
        }
    ENDIF


    /*
    ** Log Polynomials:
    ** ----------------
    **
    ** As indicated above, we use two different polynomial log evaluations
    ** depending on whether division is used or not.  when using a divide:
    **
    **        ln(F/Fj) =  2z + 2*z^3/3 + 2*z^5/5 + ....,  z = (F - Fj)/(x + Fj)
    **
    **  or letting u = 2*z/ln2,
    **
    **	      log2(F/Fj) = u + u^3*ln2^2/12 + u^5*ln2^4/80 + ....,
    **	         =  u + u^3*(ln2^2/12 + u^2*ln2^4/80 + u^4*ln2^6/448....)
    **	         =  u + u^3*P(u^2)	( if no backup precision )
    **	         =  u*Q(u^2)		( if backup precision    )
    */

    function divide_log2_poly(x)
        {
        auto s, z, k, u, t;

        s = first_term_value;

        if (x != 0)
            {
            k = 2*first_term + 1;
            z = (x*x)*x_scale;
            t = z;

            while(1)
                {
                k += 2;
                u = t/k; 
                if ((bexp(s) - bexp(u)) > bit_precision)
                    break; 
                s += u; 
                t *= z;
                }
            }
        ENDIF
        return s*final_scale;
        }

    /*
    ** When not using a divide:
    **
    **        ln(F/Fj) = w - w^2/2 + w^3/3 ... , w = (F - Fj)/Fj
    **
    ** For the accurate pow, we let v = w/ln2, and write the above as:
    **
    **        log2(F/Fj) = v - v^2*ln2/2 + v^3*ln2^2/3 ... 
    **	                 = (v - v^2*ln2/2) + v^3*(ln2^2/3 - v*ln2^3/4  ...)
    **	                 = (v - v^2*ln2/2) + v^3*P(v)   ( if no backup prec )
    **
    ** For fast pow we write the power series as:
    **
    **        log2(F/Fj) = v - v^2*ln2/2 + v^3*ln2^2/3 ... 
    **	                 = v + v^2*(-ln2/2 + v*ln2^2/3 - v^2*ln2^2/4 + ...)
    **	                 = v + v^2*P(v)   ( if no backup prec )
    **
    ** If backup precision is available we can write the series as
    **
    **        log2(F/Fj) = v - v^2*ln2/2 + v^3*ln2^2/3 ... 
    **	                 = v*P(v)
    **
    ** Note that whether using the divide or non-divide form, the reduced
    ** argument is most negative, when j = 1 and F = F0; and is most positive
    ** when j = 0 and F = F1.
    */

    function no_divide_log2_poly(x)
        {
        auto s, z, k, u, t;

        s = first_term_value;
        if (x != 0)
            {
            k = first_term + 2;
            z = x*x_scale;
            t = z;
            while(1)
                {
                u = t/k; 
                if (bexp(s) - bexp(u) > bit_precision)
                    break; 
                s += u; 
                t *= z;
                k++;
                }
            }
        ENDIF
        return s*final_scale;
        }

#   define __GEN_LOG_COEFS(term, min, max, func, prec, deg, com, tag)	\
		{							\
		remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT + 	\
		  (term), min, max, func, prec, &deg, &coefs);		\
		PRINT_TBL_COM_ADEF_ARRAY(com, tag, deg);		\
		}

#   define GEN_DIV_LOG_COEFS(max, prec, deg, com, tag)			\
			__GEN_LOG_COEFS(REMES_SQUARE_ARG, 0., max,	\
			    divide_log2_poly, prec, deg, com, tag)

#   define GEN_NO_DIV_LOG_COEFS(min, max, prec, deg, com, tag)	\
			__GEN_LOG_COEFS(REMES_LINEAR_ARG, min, max,	\
			     no_divide_log2_poly, prec, deg, com, tag)

    log2_table_size = 2^LOG2_K;
    min_arg = -1/((2*log2_table_size + 2)*ln2);
    max_arg =  1/(2*log2_table_size*ln2);

    if (!NO_ACC)
        {
        if (USE_DIVIDE)
            {
            c = ln2/2;
            max_div_arg = 2/((4*log2_table_size + 1)*ln2);
            if (USE_BACKUP)
                {
                SET_POLY_GLOBALS(0, 1, c*c, 1);
                GEN_DIV_LOG_COEFS(max_div_arg, F_PRECISION + 2*LOG2_K + 3,
                  acc_log2_deg_f, "F_PRECISION acc log2 poly coeffs",
                  "ACC_LOG2_F\t");
                _GENPOLY(ACC_LOG2_F[%%d], ACC_LOG2_POLY_F(t,x), 0,
                  odd stride=2 c0=t, 2*acc_log2_deg_f + 1);
                }
            else
                {
                SET_POLY_GLOBALS(1, 1/3, c*c, c*c);
                GEN_DIV_LOG_COEFS(max_div_arg, F_PRECISION + 1, acc_log2_deg_f,
                  "F_PRECISION acc log2 poly coeffs", "ACC_LOG2_F\t");
                _GENPOLY(ACC_LOG2_F[%%d], ACC_LOG2_POLY_F(t,x), -3,
                  odd stride=2 c0=t c1=0, 2*acc_log2_deg_f + 3);

                if (!ONE_TYPE)
                    {
                    /* Get R_PRECISION coefficients - backup prec assumed. */
                    SET_POLY_GLOBALS(0, 1, c*c, 1);
                    GEN_DIV_LOG_COEFS(max_div_arg, R_PRECISION + 2*LOG2_K + 3,
                      acc_log2_deg_r, "R_PRECISION acc log2 poly coeffs",
                      "ACC_LOG2_R\t");
                    _GENPOLY(ACC_LOG2_R[%%d], ACC_LOG2_POLY_R(t,x), 0,
                      odd stride=2 c0=t, 2*acc_log2_deg_r + 1);
                    }
                ENDIF
                }
            }
        else /* !USE_DIVIDE */
            {
            if (USE_BACKUP)
                {
                SET_POLY_GLOBALS(0, 1, -c, 1);
                GEN_NO_DIV_LOG_COEFS(min_arg, max_arg, F_PRECISION + LOG2_K + 3,
                  acc_log2_deg_f, "F_PRECISION acc log2 poly coeffs",
                  "ACC_LOG2_F\t");
                 _GENPOLY(ACC_LOG2_F[%%d], ACC_LOG2_POLY_F(t,x), -1, c0=t,
                   acc_log2_deg_f);
                 }
            else
                {
                SET_POLY_GLOBALS(2, 1/3, -ln2, ln2*ln2);
                GEN_NO_DIV_LOG_COEFS(min_arg, max_arg, F_PRECISION + 1,
                  acc_log2_deg_f, "F_PRECISION acc log2 poly coeffs",
                  "ACC_LOG2_F\t");
                _GENPOLY(ACC_LOG2_F[%%d], ACC_LOG2_POLY_F(t,x), -3,
                  c0=t c1=0 c2=0, acc_log2_deg_f + 3);
                }

            if (!ONE_TYPE)
                {
                /*
                ** backup precision is assumed. Also, we can combine the
                ** addition of the hi bits of log2(x) with the polynomial
                ** evaluation.
                */

                SET_POLY_GLOBALS(0, 1, -ln2, 1);
                GEN_NO_DIV_LOG_COEFS(min_arg, max_arg, R_PRECISION + LOG2_K + 3,
                  acc_log2_deg_r, "R_PRECISION acc log2 poly coeffs",
                  "ACC_LOG2_R\t");
                _GENPOLY(ACC_LOG2_R[%%d], ACC_LOG2_POLY_R(t,x), -1, c0=t,
                   acc_log2_deg_r + 1);
                }
            ENDIF
            }
        }
    ENDIF

    if (!NO_FAST)
        {
        /*
        ** We assume that we are not using the divide reduction for the
        ** fast case.  Additionally, we assume that if backup precision
        ** is available, the fast polynomial is the same as the accurate
        ** polynomial except that the first two terms are computed
        ** separately and added in afterwards.
        */

        if (USE_BACKUP)
            printf("#define FAST_LOG2_POLY_F\t\tACC_LOG2_POLY_F\n");
        else
            {
            SET_POLY_GLOBALS(1, 1/2, -ln2, -ln2);
            GEN_NO_DIV_LOG_COEFS(min_arg, max_arg, F_PRECISION + 1,
              fast_log2_deg_f, "F_PRECISION fast log2 poly coeffs",
              "FAST_LOG2_F\t");
            _GENPOLY(FAST_LOG2_F[%%d], FAST_LOG2_POLY_F(t,x), -2,
              c0=t c1=0 c2=0, fast_log2_deg_f + 2);

            if (!ONE_TYPE)
                {
                _GENPOLY(ACC_LOG2_R[%%d], FAST_LOG2_POLY_R(t,x), -1,
                   c0=t c1=0 c2=0, acc_log2_deg_r + 1);
                }
            }
        }
    ENDIF

    /*
    ** THE LOG2 TABLE
    ** ----------------
    **
    ** The actual format of the log2 table depends on whether it will be shared
    ** between functions and/or data types and whether or not backup precision
    ** is available.  In general, for j = 0, 1, ... 2^LOG2_K, the table needs to
    ** contain the following values:
    **
    ** 		Fj = 1 + j/2^LOG2_K
    **		Rj = 1/(Fj*ln2)
    **		Lj = log2(Fj)
    **
    ** If there is no back-up data type available, then the values Rj and Lj
    ** need to be stored in hi and lo pieces.  The following table gives the
    ** required table values:
    **
    **		Function		  Fj  Rj  Rj_hi Rj_lo Lj  Lj_hi Lj_lo
    **	---------------------------------+---+---------------+---------------+
    **	fast pow / backup		 | x | x             | x             |
    **	acc pow  / backup / divide	 | x |               | x             |
    **	acc pow  / backup / no divide	 | x | x             | x             |
    **	fast pow / no backup		 | x |      x     x  |      x     x  |
    **	acc pow  / no backup / divide	 | x |               |      x     x  |
    **	acc pow  / no backup / no divide | x | x             |      x     x  |
    **	---------------------------------+---+---------------+---------------+
    ** 
    ** Based on the above table and the number of possible combinations
    ** for sharing of the table, the log table can have many different formats.
    ** In the interest of time and simplicity, only the two combination
    ** suitable for building the DPML on Alpha are inlcude here.
    */

#   if (ONE_TYPE && NO_FAST && !USE_BACKUP && USE_DIVIDE)

        /*
        ** These macros build the log table for a single, accurate power
        ** function when backup precision is not available and division is
        ** used.  (This is the quad-precision case)
        */

#       define LOG_TABLE_BANNER						\
	  "\n\t * Fj, hi(log2(Fj)) and lo(log2(Fj) in base precision"	\
          "\n\t *\n\t * offset"						\
          "                                                     row"	\
	  "\n\t"

#       define PRINT_LOG_TABLE_ACCESS_MACROS(disp)			\
		printf("#define POW_EVAL_FLAGS\t\tUSE_DIVIDE\n");	\
        	__PRINT_TABLE_DEF("GET_F(j)\t",     F_CHAR, disp);	\
        	__PRINT_TABLE_DEF("LOG_F_HI(j)\t",  F_CHAR, disp);	\
        	__PRINT_TABLE_DEF("LOG_F_LO(j)\t",  F_CHAR, disp)

#       define LOG_INDEX_BASE_POS	(__LOG2(BITS_PER_F_TYPE) - 3)
#       define LOG_INDEX_SCALE		3

#       define PRINT_LOG_TABLE_ENTRY(j, Fj, Rj, Lj)			\
		printf( "\t/* %4i */ %#.4" STR(F_CHAR) ", /* %3i */\n",	\
		    BYTES(MP_BIT_OFFSET), Fj, j);			\
		MP_BIT_OFFSET += BITS_PER_F_TYPE;			\
		Lj_hi = bround(Lj, F_HI_HALF_PRECISION);		\
		__PRINT_TABLE_VALUE(F_CHAR, Lj_hi);			\
		__PRINT_TABLE_VALUE(F_CHAR, Lj - Lj_hi)

#   elif !(ONE_TYPE || NO_FAST || NO_ACC || USE_DIVIDE)

        /*
        ** These macros build the log table for a shared table for both
        ** accurate and fast pow in two types, the larger of which has no
        ** backup precision and no divide is used.
        */

#       define LOG_TABLE_BANNER						    \
	  "\n\t * Fj, Rj = 1/(Fj*ln2) and Lj = log2(Fj).  Lj and Rj are"    \
	  "\n\t * given in hi and low parts.  Fj and the hi part or Lj are" \
	  "\n\t * in reduced precision; Rj, lo(Rj) and lo(Lj) in standard"  \
          "\n\t * precision with hi(Rj) = Rj - lo(Rj)"			    \
          "\n\t *"							    \
          "\n\t * offset                             row"		    \
	  "\n\t"

#       define PRINT_LOG_TABLE_ACCESS_MACROS(disp)			\
        	__PRINT_TABLE_DEF("GET_F(j)\t",      R_CHAR, disp);	\
        	__PRINT_TABLE_DEF("LOG_F_HI(j)\t",   R_CHAR, disp);	\
        	__PRINT_TABLE_DEF("RECIP_F(j)\t",    F_CHAR, disp);	\
        	__PRINT_TABLE_DEF("RECIP_F_LO(j)\t", F_CHAR, disp);	\
        	__PRINT_TABLE_DEF("LOG_F_LO(j)\t",   F_CHAR, disp)

#       define LOG_INDEX_BASE_POS	(__LOG2(BITS_PER_F_TYPE) - 1)
#       define LOG_INDEX_SCALE		1

#       define PRINT_LOG_TABLE_ENTRY(j, Fj, Rj, Lj)			\
		Lj_hi = bround(Lj, R_PRECISION);			\
		printf( "\t/* %4i */ %#.4" STR(R_CHAR) ", %#.4"		\
		  STR(R_CHAR) ", /* %3i */\n", BYTES(MP_BIT_OFFSET),	\
		  Fj, Lj_hi, j);					\
		MP_BIT_OFFSET += 2*BITS_PER_R_TYPE;			\
		__PRINT_TABLE_VALUE(F_CHAR, Rj);			\
		__PRINT_TABLE_VALUE(F_CHAR, Rj - bround(Rj, LOG2_K));	\
		__PRINT_TABLE_VALUE(F_CHAR, Lj - Lj_hi)
#   else

#       error "ERROR: Log table generation for this set of switches NYI"

#   endif

    disp = MP_BIT_OFFSET;
    PRINT_LOG_TABLE_ACCESS_MACROS(disp);

    printf("#define LOG_INDEX_BASE_POS\t%i \n",  LOG_INDEX_BASE_POS);
    printf("#define LOG_INDEX_SCALE\t\t%i \n",   LOG_INDEX_SCALE);

    TABLE_COMMENT( LOG_TABLE_BANNER );

    for (i = 0; i <= log2_table_size; i++)
        {
        Fj = 1 + (i/log2_table_size);
        Rj = 1/(Fj*ln2);
        Lj = log2(Fj);
        PRINT_LOG_TABLE_ENTRY( i, Fj, Rj, Lj);
        }

    END_TABLE;

    printf(  "#else\n"
           "\n    extern const "STR(B_TYPE)" "STR(MP_TABLE_NAME)"[%i]; \n"
           "\n#endif\n\n",
        MP_BIT_OFFSET/BITS_PER_F_TYPE - 1);

@end_divert
@eval my $outText = MphocEval( GetStream( "divertText" ) );		\
      my $defineText = Egrep( "#define",  $outText, \$tableText );	\
      my $polyText   = Egrep( STR(GENPOLY_EXECUTABLE), $tableText, 	\
                              \$tableText );				\
         $polyText = GenPoly( $polyText );				\
         $outText  = "$tableText\n\n$defineText\n\n$polyText";		\
      my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),		\
                       "Definitions and constants for " .		\
                       "power and related functions", __FILE__);	\
         print "$headerText\n\n$outText";

   /*  end of the MAKE_INCLUDE mphoc code section */
