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

complementary_rnd_mode[MP_RM] = MP_RP;
complementary_rnd_mode[MP_RP] = MP_RM;
complementary_rnd_mode[MP_RZ] = MP_RZ;
complementary_rnd_mode[MP_RN] = MP_RN;


function as_int(value, num_bits, exp_width, exp_bias, rnd_mode)
    {
    auto t, sign_bit, scale, biased_exp_minus_1;

    sign_bit = 0;
    if (value < 0)
        {
        sign_bit = bldexp(1, exp_width);
        rnd_mode = complementary_rnd_mode[rnd_mode];
        value = -value;
        }

    shift = 1 - bexp(value);
    biased_exp_minus_1 = exp_bias - shift;

    if (biased_exp_minus_1 < 0 )
        { /* Arg was a denormalized value */
        biased_exp_minus_1 = 0;
        shift = 1 - exp_bias;
        }

    num_bits -= (exp_width + 1);
    t = bldexp(value, shift +  num_bits);

    if (rnd_mode == MP_RZ)
        t = trunc(t);
    else if (rnd_mode == MP_RM)
        t = floor(t);
    else if (rnd_mode == MP_RP)
        t = ceil(t);
    else
        t = rint(t);
            
    t = bldexp(sign_bit + biased_exp_minus_1, num_bits) + t;
    return t;
    }

/*
 * Most frequently, "as_int" is used on the current data type, F_TYPE.  So
 * the following macros can be used to eliminate the need to specify the
 * bias and exp width.  Note that the F_INDEX macro gets the exponent field
 * and the first n fraction bits as an integer.
 */

#define	MP_R_EXP_BIAS		(R_EXP_BIAS - R_NORM - 1)
#define	MP_F_EXP_BIAS		(F_EXP_BIAS - F_NORM - 1)
#define	MP_B_EXP_BIAS		(B_EXP_BIAS - B_NORM - 1)

#define R_INDEX_RND(v,n,r)	as_int(v, n + R_EXP_WIDTH + 1, R_EXP_WIDTH, \
				    MP_R_EXP_BIAS, r )
#define F_INDEX_RND(v,n,r)	as_int(v, n + F_EXP_WIDTH + 1, F_EXP_WIDTH, \
				    MP_F_EXP_BIAS, r )
#define B_INDEX_RND(v,n,r)	as_int(v, n + B_EXP_WIDTH + 1, B_EXP_WIDTH, \
				    MP_B_EXP_BIAS, r )

#define R_INDEX(v,n)		R_INDEX_RND(v,n,MP_RZ)
#define F_INDEX(v,n)		F_INDEX_RND(v,n,MP_RZ)
#define B_INDEX(v,n)		B_INDEX_RND(v,n,MP_RZ)

#define R_HI_BITS_RND(v,r)	R_INDEX_RND(v,R_EXP_POS,r)
#define F_HI_BITS_RND(v,r)	F_INDEX_RND(v,F_EXP_POS,r)
#define B_HI_BITS_RND(v,r)	B_INDEX_RND(v,B_EXP_POS,r)

#if BITS_PER_B_TYPE <= BITS_PER_WORD
#   define B_TOT_BITS	BITS_PER_B_TYPE
#else
#   define B_TOT_BITS	BITS_PER_WORD
#endif

#if BITS_PER_F_TYPE <= BITS_PER_WORD
#   define F_TOT_BITS	BITS_PER_F_TYPE
#else
#   define F_TOT_BITS	BITS_PER_WORD
#endif

#if BITS_PER_R_TYPE <= BITS_PER_WORD
#   define R_TOT_BITS	BITS_PER_R_TYPE
#else
#   define R_TOT_BITS	BITS_PER_WORD
#endif

#define R_AS_RND_WORD(v,r)	as_int(v, R_TOT_BITS, R_EXP_WIDTH, \
				    MP_R_EXP_BIAS, r)
#define F_AS_RND_WORD(v,r)	as_int(v, F_TOT_BITS, F_EXP_WIDTH, \
				    MP_F_EXP_BIAS, r)
#define B_AS_RND_WORD(v,r)	as_int(v, B_TOT_BITS, B_EXP_WIDTH, \
				    MP_B_EXP_BIAS, r)

#define R_AS_WORD(v)		R_AS_RND_WORD(v, MP_RN)
#define F_AS_WORD(v)		F_AS_RND_WORD(v, MP_RN)
#define B_AS_WORD(v)		B_AS_RND_WORD(v, MP_RN)


/*
 * "print_packed" takes a global array of values and prints them out k
 * at a time.  If the number of actual elements in the array is less than
 * a multiple of k, then the array is padded out by zeros to a multiple
 * of k.  Further, each group of k elements is printed enclosed in a macro
 * name, PACK - i.e
 *
 *		PACK(v1, v2, ... vk)
 *
 * This routine is generally used to print byte or word index tables for
 * doubly indexed table look-up algorithms
 *
 *		NOTE: the values to be printed are passed in the global
 *		array, tmp_val
 */

function print_packed_array(array_length, density, bit_offset, bit_size)
    {
    auto m, n, max_length;

    if (array_length == 0) return bit_offset;
    m_max = ceil(array_length/density)*density;

    while (array_length < m_max)
        /* pad to a full multiple of density */
        tmp_val[array_length++] = 0;

    printf("\n\t/* %3i */", BYTES(bit_offset));
    for (m = 0; m < array_length; m += density)
        {
        printf(" PACK(%i", tmp_val[m]);
        for (n = 1; n < density; n++)
            printf(", %i", tmp_val[m+n]);
        printf("),");
        }
    return (bit_offset + m_max*bit_size);
    }


/*
 * lsb, msb and find-first-set functions
 */

function lsb(x,p) { return (x == 0) ? 0 : bldexp(1,  bexp(x) - p ); }
function msb(x,p) { return (x == 0) ? 0 : bldexp(.5,  bexp(x)); }

#define F_LSB(x)     lsb(x, F_PRECISION)

function ffs(x)
    {
    auto i, n;

    if (trunc(x) != x)
        {
        printf("\tERROR: x not an integer in call to ffs\n");
        exit;
        }

    i = 0;
    while (1) {
        z = bldexp(x, i);
        if (z != trunc(z)) break;
        i--;
        }
    return 1-i;
    }
