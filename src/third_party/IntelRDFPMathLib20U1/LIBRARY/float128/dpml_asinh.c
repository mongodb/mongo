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

#include "dpml_private.h"
#include "sqrt_macros.h"

#undef  MAKE_ASINH
#undef  MAKE_ACOSH

#if defined(ASINH)
#       define  MAKE_ASINH
#       define  BASE_NAME       ASINH_BASE_NAME
#       define  _F_ENTRY_NAME    F_ASINH_NAME
#elif defined(ACOSH)
#       define  BASE_NAME       ACOSH_BASE_NAME
#       define  _F_ENTRY_NAME    F_ACOSH_NAME
#else
#       error "Must have one of ASINH, ACOSH defined"
#endif

#if !defined(F_ENTRY_NAME)
#   define F_ENTRY_NAME	_F_ENTRY_NAME
#endif

/*
Arcsinh & Arccosh  
--------------------------------------


    This source can be compiled into both Arcsine and Arccosine
    routines. The definitions necessary to create the function follow.


    Function Generation:

    Along with any standard compile time definitions required  by  the  dpml
    the following items should be defined on the compilation command line to
    create the indicated routine.

        Arcsinh :       ASINH
            
        Arccosh :       ACOSH
            

    To create each routine's 'include' file an initial compilation should be
    done using the following definition in addition to those above.

        MAKE_INCLUDE


    Selectable Build-time Parameters:

    The definitions below define the minimum  "overhang"  limits  for  those
    ranges  of  the  routine  with  adjustable accuracy bounds.  The numbers
    specified in  the  definitions  are  the  number  of  binary  digits  of
    overhang.   A  complete  discussion  of  these  values  and their use is
    included in the individual routine documentation.
*/

#define POLY_RANGE_OVERHANG    5 
#define REDUCE_RANGE_OVERHANG  5 
#define ASYM_RANGE_OVERHANG    7 
#define LARGE_RANGE_OVERHANG   7





#if !defined(MAKE_INCLUDE) 
#include STR(BUILD_FILE_NAME)
#endif

/*
Arcsinh
--------------------------

    The Arcsinh designs described here are the result  of
    an effort to create a fast Arcsinh routine with error bounds near 1/2
    lsb.  The  inherent  conflict  is  that,  to  create  fast  routines  we
    generally  need  to  give  up  some accuracy, and conversely, to increase
    accuracy we often must give up speed.  As a  result,  the  design  we're
    presenting  defines a user (builder) configureable routine.  That is, it
    is set up such that the builder of a routine  may  choose,  through  the
    proper  setting  of  parameters, the degree of accuracy of the generated
    routine and hence, indirectly, its speed.


    The Design:

    The overall domain of the Arcsinh function has been divided  up  into
    six regions or paths as follows:

            (1)        (2)         (3)         (4)       (5)     (6) 
        |--------|------------|-----------|-----------|-------|----------|
        0  small   polynomial   reduction   asymptotic  large    huge 

    (Note:   Although  the  domain  of  Arcsinh  extends  from  -infinite to 
    +infinite,  the  problem can be considered one of only positive arguments 
    through the application of the identity asinh(-x) = - asinh(x).  )


    Within each region a unique approximation to the Arcsinh function  is
    used.   Each is chosen for its error characteristics, efficiency and the
    range over which it can be applied.

    1. Small region:      

                asinh(x) = x                         (x <= max_small)

        Within the "small" region the Arcsinh function is approximated as
        asinh(x)  =  x. This is a very quick approximation but it may only be
        applied to small input values.  There is effectively  no  associated
        storage  costs.   By limiting the magnitude of x the error bound can
        be limited to <= 1/2 lsb.

    2. Polynomial region:     

        Within the "polynomial" region the function is approximated as

                asinh(x) = x (1 + x^2 P(x))       (max_small_x <x <= max_poly_x)

        where P(x) is a minimax polynomial approximation to (asinh(x)/x -1)/x^2,
        given by Remes' algorithm and max_poly_x is the upper bound of the 
        polynomial region whose value satisfies: 

                (asinh(x)-x)/asinh(x) <= 2^(-POLY_RANGE_OVERHANG)


    3. Reduction region:   

        In this region, asinh(x) is computed by

                asinh(x) = asinh(x0) + asinh(x*sqrt(1+x0^2)-x0*sqrt(1+x^2))

                                max_poly_x < x <= max_reduce_x

        i.e. asinh(x) is computed as the sum of two quantities:   asinh(x0),  
        and a reduced value asinh(t), where
                
                t = x * sqrt(1+x0^2) - x0*sqrt(1+x^2).

        This approach incurs the cost  of  calculating  t  and also an 
        lookup table. The values x0, asinh(x0) and sqrt(1+xo^2) are
        stored in the table to reduce the run time cost. Increased accuracy
        and efficiency are gained by choosing the asinh(x0) table values such 
        that  they  have  a predetermined number of trailing 0's or 1's beyond 
        the extent of the floating point precision.  This reduces the error 
        bound  and  avoids the need to perform an extended addition between 
        the two quantities.  The error bounds here can be established at a value
        close to 1/2 lsb.


    4. Asymptotic region: In this region, asinh(x) is computed as:

                asinh(x) = ln(2x) + 1/4 x^-2 - 3/16 x^-4

                        + 5/96 x^-6 -...

                        where max_reduce_x < x <= max_asym_x

       The upperbound of the reduction region, max_reduce_x, ( or the lowerbound
       of the asymptotic region) is determined by finding a smallest x such that

                ((asinh(x) - ln(2x)) / asinh(x)) < 2^-(ASYM_RANGE_OVERHANG)


    5. Large region: In this region, asinh(x) is computed as:

                asinh(x) = ln(2x),               where max_asym_x < x <= HUGE/2

       where HUGE is the largest floating number represented by the machine
       and max_asym_x is determined by

        (asinh(x) - ln(2x))/asinh(x) < 2^-(F_PRECISION+LARGE_RANGE_OVERHANG+1)


    6. Huge region: In this region, to avoid overflow, asinh(x) is computed as:

                asinh(x) = ln(2) + log(x),       where HUGE/2 < x <= HUGE


    Special cases:  

    Infinities and Nans passed as input result in an Infinities or
    Nans being returned.



    Configuring the implementation:

    For polynomial, reduction, asymptotic, large and huge regions (paths 2, 3,
    4, 5 and 6), the implementation has been set up so  that the builder can 
    control the accuracy. This is accomplished by allowing the builder to 
    specify a lower limit to the floating-point alignment shift of the operation
    which significantly affects round-off error in that range.  By establishing
    an alignment shift the builder  determines  the  relative  accuracy  of  the
    approximation  and  thereby determines the effective rate at which the
    routine will execute.

    In the case of the approximations used for Arcsinh we have the  following
    situations:

    Polynomial region:

        The function is computed as

                x + x^3 P(x)

        where P(x) is approximately

                -1/6 + 3/40 x^2 + ....

        Thus overhang is 

                (asinh(x) - x) / asinh(x)

        or approximately x^2 P(x).


    Reduced region: 

        In this region the result are computed by 

        asinh(x) = asinh(xi) + asinh(ti),

        where ti = x * sqrt(1+xi^2) - xi * sqrt(1+x^2). Obviously, we
        have asinh(ti) = mx - mi, where mx = asinh(x) and mi = asinh(xi).
        Thus the overhang is determined by

                (mx-mi)/mi.

        The minimum alignment shift can be properly controlled by adjusting 
        the size of interval (m1 - m0) and m0.

    The builder of the Arcsinh routines can specify the  overhang  limits
    for  each  of  the  above paths (The "small" regions have
    error bounds  pre-established  to  within  1/2  lsb).   The  larger  the
    overhang is,  the  more  accurate of the results are. However, larger
    overhang generates larger size of the lookup table. The values of these
    overhangs are defined within the main source file header and can be set
    there.



    Design Specifics:

    The following sections discuss the design and implementation details  of
    each path.


    Note:


    Interlaced along with the documentation is the source code necessary  to
    generate  the boundary points, constants, etc.  Understanding the design
    should not require an understanding of the source.
*/


#if defined(MAKE_INCLUDE)

@divert divertText

    /*
        The following command establishes the working precision required for
        accurate  computations  in  the  current  floating  point precision.

        Note:  For most all our work, the required precision is  bounded  by
        the  accuracy  needed to generate the "accurate" table values of the
        'reduce' range.  There we need an accuracy equivalent of the current
        floating  point  precision plus the number of bits in the "accurate"
        value overhang.

        precision =
            ceil( (2*floating_precision +
                   bits of overhang in the reduce range +
                   7-bit potential shift in a normalized MP number) / MP_radix)
    */

        working_prec = ceil( (2*F_PRECISION + REDUCE_RANGE_OVERHANG + MP_RADIX_BITS-1) / MP_RADIX_BITS ) + 1;

#define SET_MP_PREC(x)  precision = x
#define RND_TO_FMT(x)   bround(x, F_PRECISION)
#define PRINT_POLY_TERM(x)    printf("        %#.4" STR(F_CHAR) ", \n", (x) )


/*      INC_BIT & DEC_BIT increment and decrement x,  respectively,  by  one
        bit at position p.
*/

#if defined(MAKE_ASINH)

#define BINARY_EXP(x)   (bexp(x) - 1)
#define DEC_BIT(x, p)   x -= 2^(BINARY_EXP(x) - p)
#define INC_BIT(x, p)   x += 2^(BINARY_EXP(x) - p)
#define TRUNCATE(x, p)  bchop(x, p)



            /* "index" determines the lookup index of a floating point value.
               It extracts  the  exponent and necessary fraction bits of a
               floating point number and returns them as an integer.
            */

function index()
{
    v = $1;

                                /* get base 2 exponent of value */
    n = BINARY_EXP(v);     
                                /* get first K fraction bits of value (NOT
                                   including the hidden bit) as an integer */
    f = bextr(v, 2, K+1);

                                /* index = ((bias + n - norm)<< K) + f
                                   as an integer */
    return ( (F_EXP_BIAS + n - F_NORM) * 2^K) + f;
}


            /* "make_accurate" determines a value, v, such that asinh(v) has
               trailing 1's or 0's between the limit of the current precision
               and the extent of the specified overhang.
               lower_bound < v <= argument.
            */

function make_accurate()
{
    v = $1;  lower_bound = $2;  upper_bound = $3; overhang = $4;

    ones_mask = (2^overhang) - 1;
    v1 = v2 = v;
    while (v1 > lower_bound || v2 < upper_bound) {
        if (v1 > lower_bound) {
        bits = bextr( asinh(v1), (F_PRECISION + 1), (F_PRECISION + overhang) );
        if ( (bits == 0) || (bits == ones_mask) )
            return v1;
        DEC_BIT(v1, (F_PRECISION - 1));
        };
        if (v2 < upper_bound) {
        INC_BIT(v2, (F_PRECISION - 1));
        bits = bextr( asinh(v2), (F_PRECISION + 1), (F_PRECISION + overhang) );
        if ( (bits == 0) || (bits == ones_mask) )
            return v2;
        }
    }

    printf("Couldn't find an ACCURATE value \n");
    return -1;
}

        SET_MP_PREC(working_prec);

    /*
        The "small" region:

        Within the "small" region asinh(x) is approximated as  asinh(x)  =  x.
        The reasoning follows:

        Given the Taylor Series expansion to asinh(x)

           asinh(x) = x - x^3 (1/6) + x^5 (3/40) + ...,  for x < 1.        

        we find that successive terms of  the  series  decrease  rapidly  in
        magnitude,  and  that  as x goes to 0, the relative distance between
        individual terms of the series becomes greater.  It is meaningful to
        ask, at what point does x^3/6, and hence all succeeding terms of the
        series, become irrelevant with respect to the magnitude of x given a
        fixed  floating point precision?  I.e. when is the ratio of x^3/6 to
        x less than 1 / 2^(precision + 1)?  Solving we find:

        let,
            p = current floating point precision.
        
            (1/6) * (x^3)/x  <  1/2^(p+1)   ==>
        
                    x  <  sqrt( 6/2^(p+1) )

        So, when x  <  sqrt(  6/2^(p+1)  ),  asinh(x)  correctly  rounded  is
        equivalent  to  x. The value sqrt( 6/2^(p+1) ) will thus be made the
        upper bound of the "small" region.

        error bounds:

        Since x is equivalent to asinh(x) correctly rounded, the error  bound
        for this approximation is 1/2 lsb.
*/

    max_small_x = sqrt( 6 / 2^(F_PRECISION + 1) );


/*
        The "polynomial" region:

        Within the "polynomial" region, Arcsinh is approximated as
        asinh(x) = x  -  x^3/6 + x^5 (3/40) + ..., or rather a truncated
        polynomial approximation to this series.  This is a reasonably quick
        approximation   and  has  storage  costs  limited  to  that  of  the
        coefficients.

        error bounds:

        The error bound for this  approach  is  roughly  determined  by  two
        things:

          - The overhang between the first two terms of the series (or

            more  exactly,  between  x and the sum of the remaining terms of
            the series = x - asinh(x)).

          - The accuracy of the term largest in magnitude.

        The overhang between the terms is a function of x and,  in  general,
        will  decrease  as  x gets larger.  The largest term is x and, as an
        input argument, is assumed to be exact.  This implies  that  we  can
        enforce  an  error  bound  of our choosing (greater than 1/2 lsb) by
        limiting the size of our input argument such that  the  leading  two
        terms maintain a chosen overhang.

        If our desired overhang limit is V, we can compute a maximum X  for
        which  that overhang is satisfied by determining when the following
        is true:

            (X - asinh(X))/asinh(X) <  2^-V

        This point X will be upper bound of the "polynomial" range.

        Note:
            Since X can not be computed analytically  it  must  be  computed
            numerically (e.g Newton's method).



        The following code determines the polynomial range
*/ 
    
    rho = 2^-(POLY_RANGE_OVERHANG);
    c = (1 - rho);
    x = 0.5;
    error = 1;
    while (error > 2^-(2*F_PRECISION)) {
        f = asinh(x) - x * c;
        f1 = (1/sqrt(1+x*x)) - c; 
        next_x = x - f/f1;
        error = abs(f);
        x = next_x;
    }
    max_poly_x = x; 

    
    
        /* The following code determines the upper bound of the reduced range 
        (or the lower bound of the asymptotic region.) In the asymptotic region, 
        asinh(x) is determined by the following formula:
                                           
                asinh(x) = ln(2x) + 1/4 x^-2 - 3/16 x^-4

                        + 5/96 x^-6 -...

        The maximum of the reduction region( or the minimum of the asymptotic
        region) is determined by finding a minimal x such that

                ((asinh(x) - ln(2x)) / asinh(x)) < 2^-(ASYM_RANGE_OVERHANG);
        */ 

    rho = 2^-(ASYM_RANGE_OVERHANG);
    c = (1 - rho);
    x = 2.5;
    error = 1;
    while (error > 2^-(2*F_PRECISION)) {
        f = c * asinh(x) - log(2*x);
        f1 = c * (1/sqrt(1+x*x)) - 1/x ; 
        next_x = x - f/f1;
        error = abs(f);
        x = next_x;
    }
    max_reduce_x = x;



        /* The following code determines the upperbound of the asymptotic 

        region.  ( or the lower bound of the large region.) Within this region, 
        asinh(x) is approximated by

                sign(x) asinh(x) = ln(2|x|) + 1/4 x^-2 - 3/16 x^-4

                        + 5/96 x^-6 -...

        The upperbound of the asymptotic region( or the lower bound of the large 
        region) is determined by finding a minimal x such that

        (asinh(x) - ln(2x))/asinh(x) < 2^-(F_PRECISION+LARGE_RANGE_OVERHANG+1)
        */

    SET_MP_PREC(2*working_prec+1);
    rho = 2^-(F_PRECISION+LARGE_RANGE_OVERHANG+1);
    c = (1 - rho);
    x = 2.5;
    error = 1;
    while (error > 2^-(2*F_PRECISION)) {
        f = c * asinh(x) - log(2*x);
        f1 = c * (1/sqrt(1+x*x)) - 1/x ; 
        next_x = x - f/f1;
        error = abs(f);
        x = next_x;
    }
    max_asym_x = x;


        /* The following code determines and prints the terms of the
        asymptotic expansion in the asymptotic region.

                asinh(x) = ln(2x) + 1/4 x^-2 - 3/16 x^-4

                        + 5/96 x^-6 -...
        */


        /* Approximation to the function:  x^2(asinh(x) - ln(2x)) */
    function asinh_asym_func()
    {
        x = $1;
        if (x == 0)
            return 1/4;
        else
            return ((x*x)*(asinh(x) - ln(2*x)));
    }


    old_precision = precision;
    precision = ceil(2*F_PRECISION/MP_RADIX_BITS) + 5;


    remes(REMES_FIND_POLYNOMIAL+ REMES_RELATIVE_WEIGHT+ REMES_RECIP_SQUARE_ARG,
          max_reduce_x, max_asym_x, asinh_asym_func, (F_PRECISION + 1),
          &poly_deg, &poly_coef);

    printf("#define NUM_ASYM_TERM               %i\n", poly_deg + 1 );

    printf("#define EVALUATE_ASYM_RANGE_POLYNOMIAL(x,c,y) \\\n");
    printf("                         POLY_%i(x,c,y)\n", poly_deg + 1);

    printf("static const TABLE_UNION asym_range_coef[] = { \n");
    for (i = 0; i <= poly_deg; i++)
         printf("\t%#.4" STR(F_CHAR) ",\n", poly_coef[i] );
    printf("}; \n\n");
    precision = old_precision;



        /* The "reduction" region: Within the "reduction" region asinh(x) is 
        approximated as  

                asinh(x)  = asinh(m)  +  asinh(t), 

                        where t = x*sqrt(1+m*m) - m*sqrt(1+x*x).

        error bounds:

        As in the "polynomial" regions the error bound  here  is
        roughly a function of:

          - The overhang between the final addition  of  asinh(m)  and  
            asinh(t).

          - The accuracy of the dominating (larger) value, asinh(m).

        As for the overhang, given some value m, t will decrease
        as x moves closer to m.  This in turn implies asinh(t)
        becomes smaller.  Thus given some x in the "reduce" range we can
        chose some m near x such that asinh(m) + asinh(t) has
        at least the desired alignment shift.

        As for the accuracy of asinh(m), we can chose the m above  such  that
        asinh(m)  has  trailing  0's  or 1's from the boundary of the current
        precision to the extent of the chosen overhang.   This  will  reduce
        the  overall  error  in our computation.  These asinh(m) are known as
        "accurate" values.

        This implies that we can roughly enforce what error bound we  choose
        (greater than 1/2 lsb).



        Determining the "m" and asinh(m) tables:

        Ensuring  a  certain  overhang,  k,  between  asinh(m)   and  
        asinh(t):

        From the identity above, it is obvious that asinh(t) is the
        difference between asinh(x) and asinh(m). Thus, if we choose a
        net of equal length subintervals in [asinh(max_poly_x), 
        asihn(max_reduce_x)] such that the size of subinterval is sufficiently 
        small, we can ensure the alignment shift.

                asinh(t)/asinh(m) = (ax - am)/am,

        where   ax = asinh(x) and am = asinh(m) and (ax-am) <= 1/2 of the
        subinterval size.
  

        The generation of these "m",s and x's divides the  reduce  range  up
        into subintervals like the following:


            |--|--|---|---|----|----|-- ... --|
            Xo Mo X1  M1  X2   M2   X3        Xn

        For each input x such that Xi < x <= Xi+1 the  asinh(x)  is  computed
        using   Mi   and   asinh(Mi).    Note  that  these  subdivisions  are
        non-uniform.  As x moves to the right the  relative  size  of  these
        intervals is increasing.

        Indexing the "m" and asinh(m) table:

        Given an x which lies within the 'reduce' range we need an efficient
        way  of  determining which "m" and asinh(m) should be used in our our
        calculations.  We will accomplish this using a  second  table.   For
        each  value  of  x such that Xi < x <= Xi+1 we will use the exponent
        bits of x and a certain number of fraction bits (enough to  uniquely
        characterize  which  subinterval  x  lies within) to act as an index
        into a second  "indexing"  table.   The  values  stored  within  the
        "indexing"  table will in turn point to the appropriate value of "m"
        (and asinh(m)) to use for the current x.


        Mapping input arguments to the appropriate region:

        As described throughout, each input argument x maps to  one  of  the
        three  different  approximations  for  asinh(x).   An efficient way of
        determining which approximation should be used for an input x is  to
        calculate  the  "index" of each argument x (as we do in the 'reduce'
        region) and use it to make the choice of approximation.
        This  simply  involves  determining the index value for the boundary
        points of each region and then comparing  the  index  of  the  input
        argument x to these values and branching accordingly.


        The indicies:

        Indices consist of exponents & fraction bits to uniquely characterize
        an interval.  The number of fraction bits indicates table size.

        Calculation of the number of fraction bits needed for the index:

        It is desirable to minimize the number  of  fraction  bits  used  to
        address  the  "indexing"  table.   Each  additional  bit we use will
        increases the size of the "indexing" table by a power of 2. (This is
        not  completely  true.   Depending  on  what value is chosen for the
        upper bound of the 'reduce' range it may not be necessary  to  store
        indexes  for  values  at  the far end of the range.  The increase in
        table size, however, is still on the order of a power of 2.)

        Since the size of the intervals Xi <--> Xi+1 decrease as x  goes  to
        one,  we  find  that  the smallest interval for which we need to
        uniquely specify each x is the interval X(n-1)  <-->  Xn. The minimum
        number  of  fraction  bits  we need to characterize this interval is
        given by the overhang difference  between  X(n-1) and Xn.  Thus,  the
        minimum number of fraction bits, k, required to satisfy our index is
        given by:

            (X(n-1) - Xn)/Xn = 1/2^k

            k = floor( log2 ((Xn - X(n-1))/Xn) )

        as the number of fraction bits required for our index.


        Mapping the index:

        Realizing that an arguments exponent and fraction bits are going  to
        be  looked  at as an integer index to the "indexing" table leaves an
        issue of addressing.  Since we want  Xo,  the  lower  bound  of  the
        'reduce'  range, to map to the first element of the "indexing" table
        it is necessary to map the generated index of Xo down to zero.  This
        is  accomplished by predetermining the index of Xo and using it as a
        offset (subtracting it off) from the generated  index  of  Xo.
        If  we  subtract this offset from all generated indexes we can
        map the indexes of the 'reduce' range into a table  look-up  address
        between 0 and tablesize-1.


        The following code:
        1. compute the minimum number of "index" bits required
           to accurately determine the mapping of input values to table
           values in the 'reduce' range.
        2. compute the accurate table.
        */
            

    rho = 2^-REDUCE_RANGE_OVERHANG;
    max_poly_ax = asinh(max_poly_x);
    x0 = max_poly_x;
    ax0 = max_poly_ax;
    max_reduce_ax = asinh(max_reduce_x);
    max_K = 1;
    max_delta_ax = 0.0;
    n = 0;
    printf("static const TABLE_UNION asinh_tab[] = { \n");

    while(ax0 < max_reduce_ax) {
        delta_ax = rho * ax0;
        ax1 = ax0 + delta_ax;
        x1 = sinh(ax1);
        if (ax1 > max_reduce_ax) { 
                ax1 = max_reduce_ax;
                delta_ax = max_reduce_ax - ax0;
                axm = ax0 + delta_ax/2;
                x1 = max_reduce_x;
                xm = sinh(axm);
                xm = make_accurate( RND_TO_FMT(xm), x0, x1, REDUCE_RANGE_OVERHANG);
                printf("\t%#.4" STR(F_CHAR) ",", xm);
                printf("\t%#.4" STR(F_CHAR) ",", asinh(xm));
                cosh_asinh_value = sqrt(1.0 + xm * xm);
                printf("\t%#.4" STR(F_CHAR) ",\n", cosh_asinh_value);
                n++;
                if (max_delta_ax < delta_ax) max_delta_ax = delta_ax;
                break;
        }
        axm = ax0 + delta_ax/2;
        xm = sinh(axm);
        xm = make_accurate( RND_TO_FMT(xm), x0, x1, REDUCE_RANGE_OVERHANG);
        printf("\t%#.4" STR(F_CHAR) ",", xm);
        printf("\t%#.4" STR(F_CHAR) ",", asinh(xm));
        cosh_asinh_value = sqrt(1.0 + xm * xm);
        printf("\t%#.4" STR(F_CHAR) ",\n", cosh_asinh_value);
        delta_x = x1 - x0;
        K = abs( floor ( log2 (delta_x/x0)));
        if (max_K < K) max_K = K;
        if (max_delta_ax < delta_ax) max_delta_ax = delta_ax;
        ax0 = ax1;
        x0 = x1;
        n++;
    }
    count = n;
    printf("}; \n\n");
    printf("#define TABLE_ENTRY_SIZE   %i \n", count);
    max_delta_x = sinh(max_delta_ax);
    K = max_K;
    K = K - 1;
    printf("#define K   %i \n", K);

            /* computation of the "index" represented by the maximum
               value we will evaluate in the 'polynomial' range.  Since the
               'reduce' range begins beyond this value, we will use this
               "index" to map all "indicies" so that those of the 'reduce'
               range will take on values between 0 and the table size.
            */

    offset = index( TRUNCATE(max_poly_x, K+1) );
    printf("#define OFFSET_IND   %i \n", offset);

    DEC_BIT(max_small_x, K);

    printf("#define MAX_SMALL_INDEX %i \n", (index(max_small_x)-offset));

    printf("#define MAX_POLY_INDEX %i \n", (index(max_poly_x)-offset));

    printf("#define MAX_REDUCE_INDEX %i \n", (index(max_reduce_x)-offset));

    printf("#define MAX_ASYM_INDEX %i \n", (index(max_asym_x)-offset));

    half_huge_x = MPHOC_F_POS_HUGE/2;

    printf("#define HALF_HUGE_INDEX %i \n", (index(half_huge_x)-offset));



            /* computation of the "accurate" table "indecies" for values
               within the 'reduce' range.
            */

    rho = 2^-REDUCE_RANGE_OVERHANG;

    if (count <= 256)
        printf("static const U_INT_8 asinh_index_table[] = { ");
    else
        printf("static const U_INT_16 asinh_index_table[] = { ");
    bytes_used = 0;
    i = 0;
    j = 0;
    m = 0;
    x0 = max_poly_x;
    ax0 = asinh(x0);

    while(ax0 < max_reduce_ax) {
        delta_ax = rho * ax0;
        ax1 = ax0 + delta_ax;
        if (ax1 > max_reduce_ax) { 
                ax1 = max_reduce_ax;
                delta_ax = max_reduce_ax - ax0;
        }
        x1 = sinh(ax1);
        b = TRUNCATE( x0, K+1);
        next_b = TRUNCATE( x1, K+1);
        cur_index = b;
        while (cur_index < next_b) {
            if ((j++ % 8) == 0) {
                printf("\n\t");
                bytes_used = 1;
            }
            else
                bytes_used++;
            printf("%i,  ", i);
            INC_BIT(cur_index, K);
        }
        i++;
        ax0 = ax1;
        x0 = x1;
    }
    i--;
    /*
     *  Pad this table up to avoid alignment problem on HP machine.
     */
    bytes_to_pad = 8 - bytes_used;
    for (k = 0; k < bytes_to_pad; k++)
        printf("0,  " );

    printf("\n}; \n\n");




    /* Generate the coefficients for the 'polynomial' range polynomial */


        /* Approximation to the function:  (asinh(x) - x) / x^3 */
    function asinh_func()
    {
        if ($1 == 0)
            return -1/6;
        else
            return (asinh($1) - ($1))/($1 * $1 * $1);
    }


    old_precision = precision;
    precision = ceil(2*F_PRECISION/MP_RADIX_BITS) + 5;


    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG,
          0.0, max_poly_x, asinh_func, (F_PRECISION),
          &poly_deg, &poly_coef);

    printf("#define EVALUATE_POLY_RANGE_POLYNOMIAL(x,c,y) \\\n");
    printf("                         ODD_POLY_%i_U(x,c,y)\n", 2 * poly_deg + 3);

    printf("static const TABLE_UNION poly_range_coef[] = { \n");
    for (i = 0; i <= poly_deg; i++)
         printf("\t%#.4" STR(F_CHAR) ",\n", poly_coef[i] );
    printf("}; \n\n");



    /* Generate the coefficients for the 'reduce' range polynomial */

    remes(REMES_FIND_POLYNOMIAL + REMES_RELATIVE_WEIGHT + REMES_SQUARE_ARG,
          0.0, max_delta_x, asinh_func, (F_PRECISION + 1),
          &poly_deg, &poly_coef);

    printf("#define EVALUATE_REDUCE_RANGE_POLYNOMIAL(x,c,y) \\\n");
    printf("                         ODD_POLY_%i_U(x,c,y)\n", 2 * poly_deg + 3);

    printf("static const TABLE_UNION reduce_range_coef[] = { \n");
    for (i = 0; i <= poly_deg; i++)
         printf("\t%#.4" STR(F_CHAR) ",\n", poly_coef[i] );
    printf("}; \n\n");


    precision = old_precision;



#else /* ACOSH */

        /* The computation of ACOSH is devided into four regions:

        1. Direct region: In this region, ACOSH(x) is computed as

                acosh(x) = log(x + sqrt((x-1)*(x+1)))
                
                        where 1 < x <= max_direct_x.


        2. Asymptotic regions: In this region, ACOSH(x) is computed as

                acosh(x) = ln(2x) - 1/4 x^-2 - 3/16 x^-4

                        - 5/96 x^-6 -...

                        where max_direct_x < x <= max_asym_x.

        3. Large region: In this region, ACOSH(x) is computed as

                acosh(x) = log(2x).

                        where max_asym_x < x <= HUGE/2. 


        4. Huge region: In this region, ACOSH(x) is computed as

                acosh(x) = ln(2) + log(x).

                        where  HUGE/2 < x <= HUGE. 
        */


        /* The following code determines the upper bound of the direct region
        (or the lower bound of the asymptotic region). In the asymptotic region,
        asinh(x) is determined by the following formula:
                                           
                acosh(x) = ln(2x) - 1/4 x^-2 - 3/16 x^-4

                         - 5/96 x^-6 -...

        The upper bound of the direct region region is determined by finding 
        the smallest x such that

                ((ln(2x)-acosh(x)) / acosh(x)) < 2^-(ASYM_RANGE_OVERHANG);
        */ 

    function root_func() {
       acosh_x = acosh($1);
       return (ln(2*($1)) - acosh_x)/acosh_x;
    }

    lo = binc(1,(F_PRECISION - 1));
    hi = 10;
    rho = 2^-(ASYM_RANGE_OVERHANG);
    max_direct_x = find_root(MP_FIND_ROOT_NO_DERIV, lo, hi, rho, root_func);

    printf("static const TABLE_UNION max_direct_x[] = { %#.4" STR(F_CHAR) " };\n\n", max_direct_x);



        /* The following code determines the the upper bound of the asymptotic 
        region.(or the lower bound of the large region.) In the large region,
        acosh(x) is approximated by

                acosh(x) = ln(2x).

        Thus, the lower bound of the large region (or max_asym_x) is determined 
        by finding the smallest x such that

          -(ln(2x)-acosh(x))/acosh(x) < 2^(-F_PRECISION+LARGE_RANGE_OVER_HANG+1)
        */

    SET_MP_PREC(2 * working_prec+1);
    rho = 2^-(F_PRECISION+LARGE_RANGE_OVERHANG+1);
    c = (1 + rho);
    x = 2.5;
    error = 1;
    while (error > 2^-(2*F_PRECISION)) {
        f = log(2*x) - c * acosh(x);
        f1 = 1/x - c * (1/sqrt(x*x-1)); 
        next_x = x - f/f1;
        error = abs(f);
        x = next_x;
    }
    max_asym_x = x;

    printf("static const TABLE_UNION max_asym_x[] = { %#.4" STR(F_CHAR) " };\n\n", max_asym_x);


        /* The computation in asymptotic and large region is more efficient
        than the computation in the direct region if the number of the
        terms in the asymptotic expansion is kept reasonably small. The
        following procedure computes and prints the terms of the
        asymptotic series. 
        */ 

        /* Approximation to the function:  -[x^2(acosh(x) - ln(2x))] */
    function acosh_func()
    {
        x = $1;
        if (x == 0)
            return 1/4;
        else
            return -((x*x)*(acosh(x) - ln(2*x)));
    }


    old_precision = precision;
    precision = ceil(2*F_PRECISION/MP_RADIX_BITS) + 5;


    remes(REMES_FIND_POLYNOMIAL+ REMES_RELATIVE_WEIGHT+ REMES_RECIP_SQUARE_ARG,
          max_direct_x, max_asym_x, acosh_func, (F_PRECISION + 1),
          &poly_deg, &poly_coef);

    printf("#define EVALUATE_ASYM_RANGE_POLYNOMIAL(x,c,y) \\\n");
    printf("                         POLY_%i(x,c,y)\n", poly_deg + 1);

    printf("static const TABLE_UNION asym_range_coef[] = { \n");
    for (i = 0; i <= poly_deg; i++)
         printf("\t%#.4" STR(F_CHAR) ",\n", poly_coef[i] );
    printf("}; \n\n");
    precision = old_precision;


    half_huge_x = MPHOC_F_POS_HUGE/2;
    printf("static const TABLE_UNION half_huge_x[] = { %#.4" STR(F_CHAR) " };\n\n", half_huge_x);

#endif /* MAKE_ASINH */

        /* compute correctly rounded "high" and "low" parts of log(2) */

    SET_MP_PREC(2 * working_prec);      
    log_2 = log(2);
    printf("static const TABLE_UNION log_2[] = {\n");
    printf("        %#.4" STR(F_CHAR) " \n", log_2);
    printf("};\n\n");

    SET_MP_PREC(working_prec);  


        /* the following values are defined for use in performing
        automated testing with MTC.
        */

#ifdef MTC_DEFS  
    huge_x = (MPHOC_F_POS_HUGE/10)*9;
    half_huge_x = MPHOC_F_POS_HUGE/2;
#ifdef MAKE_ASINH  
    printf("#define MAX_SMALL_PT        m:%m\n", max_small_x );
    printf("#define MAX_POLY_PT         m:%m\n", max_poly_x );
    printf("#define MAX_REDUCE_PT       m:%m\n", max_reduce_x );
    printf("#define MAX_ASYM_PT         m:%m\n", max_asym_x );
    printf("#define HUGE_PT             m:%m\n", huge_x );
    printf("#define HALF_HUGE_PT        m:%m\n", half_huge_x );
#else
    printf("#define MAX_DIRECT_PT       m:%m\n", max_direct_x );
    printf("#define MAX_ASYM_PT         m:%m\n", max_asym_x );
    printf("#define HUGE_PT             m:%m\n", huge_x );
    printf("#define HALF_HUGE_PT        m:%m\n", half_huge_x );
#endif
#endif

@end_divert
@eval my $outText    = MphocEval( GetStream( "divertText" ) );	\
      my $headerText = GetHeaderText( STR(BUILD_FILE_NAME),     \
                       "Definitions and constants for "	.	\
                       STR(F_ENTRY_NAME), __FILE__);		\
         print "$headerText\n$outText";

#endif  /* MAKE_INCLUDE */




typedef struct { F_TYPE x_value, asinh_value, cosh_asinh_value;} TABLE_ENTRY;



        /* macros specific to asinh */

#define  MAX_DIRECT_X           *(F_TYPE *)max_direct_x
#define  MAX_ASYM_X             *(F_TYPE *)max_asym_x
#define  HALF_HUGE_X            *(F_TYPE *)half_huge_x

#define  POLY_RANGE_COEF        (F_TYPE *)poly_range_coef
#define  REDUCE_RANGE_COEF      (F_TYPE *)reduce_range_coef
#define  ASYM_RANGE_COEF        (F_TYPE *)asym_range_coef

#define  ASINH_TABLE(indx)      *((TABLE_ENTRY *)asinh_tab + indx)
#define  LOG_2                  *(F_TYPE *)log_2

#if VAX_FLOATING
#   if BITS_PER_WORD == 32
#       define INDEX_SHIFT	(F_EXP_POS + (BITS_PER_WORD - 16) - K)
#   else
#       define INDEX_SHIFT	(F_EXP_POS + (BITS_PER_F_TYPE - 16) - K)
#   endif
#   define IEEE_ONLY(x)
#elif IEEE_FLOATING
#   define INDEX_SHIFT	(F_EXP_POS - K)
#   define IEEE_ONLY(x)	x
#endif


#define GET_INDEX(exp_word, indx)       (indx) = (exp_word >> INDEX_SHIFT) - OFFSET_IND;

#define GET_MID_POINT(indx, m, sqrt_one_add_m2) \
                        (indx) = asinh_index_table[(indx)]; \
                        (m) = (ASINH_TABLE(indx)).x_value; \
                        (sqrt_one_add_m2) = (ASINH_TABLE(indx)).cosh_asinh_value

        

#define ADD_ASINH_TABLE_VALUE(x, indx, y)   \
                                        (y) = (F_TYPE)((ASINH_TABLE(indx)).asinh_value + (x))

#if defined(MAKE_ASINH)

F_F_PROTO( F_LN_NAME ) ;

F_TYPE
F_ENTRY_NAME (F_TYPE x)
{
    EXCEPTION_RECORD_DECLARATION
    F_TYPE z, m, t, u, sqrt_one_add_m2, sqrt_one_add_z2, one_add_z2;
    F_SIGN_TYPE sign;
    WORD indx;
    U_WORD exp_word_z;

    F_SAVE_SIGN_AND_GET_ABS(x, sign, z);
    GET_EXP_WORD(z, exp_word_z);
    exp_word_z = PDP_SHUFFLE(exp_word_z);
    if (F_EXP_WORD_IS_INFINITE_OR_NAN(exp_word_z)) return(x); 

    GET_INDEX(exp_word_z, indx);
    if (indx >= MAX_REDUCE_INDEX) goto asym_or_ln2x;

        /* Reduced region */

    if (indx > MAX_POLY_INDEX) {
        GET_MID_POINT(indx, m, sqrt_one_add_m2);
        one_add_z2 = ((F_TYPE) 1.0 + z*z);
        t = (z - m)*(z + m); 
        F_HW_OR_SW_SQRT(one_add_z2, sqrt_one_add_z2);
        t = t/(z * sqrt_one_add_m2 + m * sqrt_one_add_z2); 
        EVALUATE_REDUCE_RANGE_POLYNOMIAL(t, REDUCE_RANGE_COEF, z);
        ADD_ASINH_TABLE_VALUE(z, indx, t); 
        F_NEGATE_IF_SIGN_NEG(sign, t);
        return t;
    }

        /* Polynomial region */

    else if (indx > MAX_SMALL_INDEX) {
        EVALUATE_POLY_RANGE_POLYNOMIAL(z, POLY_RANGE_COEF, t);
        F_NEGATE_IF_SIGN_NEG(sign, t);
        return(t);
    }

        /* Small region */

    else return (x);
        

asym_or_ln2x:

        /* Asymptotic region */

        if (indx < MAX_ASYM_INDEX) {
                u = 1/(z*z);
                EVALUATE_ASYM_RANGE_POLYNOMIAL(u, ASYM_RANGE_COEF, t);
                t += F_LN_NAME(2*z);
                F_NEGATE_IF_SIGN_NEG(sign, t);
                return(t);
        } 

        /* Large region */

        else if ( indx < HALF_HUGE_INDEX) {
                t = F_LN_NAME(2*z);
                F_NEGATE_IF_SIGN_NEG(sign, t);
                return(t);
        } 

        /* Huge region */

        else {
                t = LOG_2
                      + F_LN_NAME(z);
                F_NEGATE_IF_SIGN_NEG(sign, t);
                return t;
        }
}

#else /* ACOSH */

F_F_PROTO( F_LN_NAME ) ;
F_F_PROTO( F_LOG1P_NAME ) ;

F_TYPE
F_ENTRY_NAME (F_TYPE x)
{
    EXCEPTION_RECORD_DECLARATION
    F_TYPE t, x2_sub_one, sqrt_x2_sub_one;
    F_TYPE u;
    U_WORD exp_word_x;


        GET_EXP_WORD(x, exp_word_x);
	IEEE_ONLY(if (F_EXP_WORD_IS_ABNORMAL(exp_word_x)) goto non_finite_x;)

        if (x <= 1.0) goto out_of_range_or_one;

        if (x > MAX_DIRECT_X) goto asym_or_ln2x;
        else {
                /* Direct region */

                x2_sub_one = (x - (F_TYPE)1.0) * (x + (F_TYPE)1.0);
                F_HW_OR_SW_SQRT(x2_sub_one, sqrt_x2_sub_one);
                u = x - (F_TYPE)1.0;
                u += sqrt_x2_sub_one;
                t = F_LOG1P_NAME(u);
                return(t);
        }

asym_or_ln2x:

        if (x < MAX_ASYM_X) {

                /* Asymptotic region */

                u = 1/(x*x);
                EVALUATE_ASYM_RANGE_POLYNOMIAL(u, ASYM_RANGE_COEF, t);
                t = F_LN_NAME(2*x) - t;
                return(t);
        }
        else if (  x < HALF_HUGE_X) {

                /* Large region */

                t = F_LN_NAME(2*x);
                return(t);
        } 
        else {
                /* Huge region */

                t = LOG_2
                    + F_LN_NAME(x);
                return t;
        }

out_of_range_or_one:

                if (x != 1.0) goto invalid_argument;
                return (F_TYPE) 0.0;       

#if IEEE_FLOATING

non_finite_x:

    F_CLASSIFY(x, exp_word_x);
    if ((exp_word_x == F_C_POS_INF) || (F_C_BASE_CLASS(exp_word_x) == F_C_NAN))
        return x;

#endif

invalid_argument:

    GET_EXCEPTION_RESULT_1(ACOSH_ARG_LT_ONE, x, t);
    return t;

}

#endif /* MAKE_ASINH */



#ifdef MTC

#ifdef MAKE_ASINH
@divert > dpml_asinh.mtc
#else
@divert > dpml_acosh.mtc
#endif
/*
**  accuracy and key point tests for single 
**  and double precision asinh or acosh functions.
*/

#ifdef MAKE_ASINH

#if (F_PRECISION==24)
    build asinh_build = STR(PASTE(F_ENTRY_NAME , .o)) "my_asinh.o" "my_logf.o"
"my_log.o" "dpml_globals.o" "dpml_exception.o" "dpml_sqrt_s_table.o"
"dpml_sqrt_t_table.o";
    function asinh_func = F_CHAR F_ENTRY_NAME( F_CHAR.v.r );
    comparison_function asinh_backup = B_CHAR my_asinh( B_CHAR.v.r );

#else
    build asinh_build = STR(PASTE(F_ENTRY_NAME , .o)) "my_log.o" 
"dpml_globals.o" "dpml_exception.o" "dpml_sqrt_t_table.o";
    function asinh_func = F_CHAR F_ENTRY_NAME( F_CHAR.v.r );
    comparison_function asinh_backup = void mp_asinh(m.r.r, m.r.w);
#endif

domain asinh_accuracy =
  { [ 0 , MAX_SMALL_PT ):uniform:1000 }  
  { [ MAX_SMALL_PT, MAX_POLY_PT ):uniform:4000 }
  { [ MAX_POLY_PT, MAX_REDUCE_PT):uniform:4000 }  
  { [ MAX_REDUCE_PT, MAX_ASYM_PT):uniform:4000 }  
  { ( MAX_ASYM_PT, HALF_HUGE_PT):uniform:4000 }  
  { ( HALF_HUGE_PT, HUGE_PT):uniform:1000 }  
;

domain asinh_keypoint =
    lsb = 0.5;
    { -1.0 | der }
    { 0.0 | der }
;


test asinh_acc =
    type = accuracy 
        error = lsb;
        stats = max;
        points = 1024;
    ;

    function = asinh_func; 

    comparison_function = asinh_backup;

    domain = asinh_accuracy;

    build  = asinh_build;

    output =
        file = STR(PASTE(F_ENTRY_NAME , _acc.out));
    ;
; 


test asinh_key =
    type = key_point; 

    function = asinh_func;

    comparison_function = asinh_backup;

    domain = asinh_keypoint; 

    build  = asinh_build;
    output =
        file = STR(PASTE(F_ENTRY_NAME, _key.out));
        style = verbose;
    ;
;
/*  For testing acosh */
#else
    build acosh_build = STR(PASTE(F_ENTRY_NAME, .o)) "my_log.o" "my_logf.o"
"logp.o" "logpf.o" "dpml_globals.o" "dpml_exception.o" "dpml_sqrt_s_table.o" "dpml_sqrt_t_table.o";

    function acosh_func = F_CHAR F_ENTRY_NAME( F_CHAR.v.r );

    comparison_function acosh_backup = void mp_acosh(m.r.r, m.r.w);

domain acosh_accuracy =
  { [ 1.0, MAX_DIRECT_PT):uniform:4000 }  
  { [ MAX_DIRECT_PT, MAX_ASYM_PT):uniform:4000 }  
  { [ MAX_ASYM_PT, HALF_HUGE_PT):uniform:4000 }  
  { [ HALF_HUGE_PT, HUGE_PT):uniform:4000 }  
;

domain acosh_keypoint =
    lsb = 0.5;
    { 1.0 | der }
    { -1.0 | der }
;


test acosh_acc =
    type = accuracy 
        error = lsb;
        stats = max;
        points = 1024;
    ;

    function = acosh_func; 

    comparison_function = acosh_backup;

    domain = acosh_accuracy;

    build  = acosh_build;

    output =
        file = STR(PASTE(F_ENTRY_NAME, _acc.out));
    ;
; 


test acosh_key =
    type = key_point; 

    function = acosh_func;

    comparison_function = acosh_backup;

    domain = acosh_keypoint; 

    build  = acosh_build;
    output =
        file = STR(PASTE(F_ENTRY_NAME , _key.out));
        style = verbose;
    ;
;


#endif  /* MAKE_ASINH */
@end_divert

#endif  /* MTC */

