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

#include <ctype.h>
#include "bid_internal.h"
#include "bid128_2_str.h"
#include "bid128_2_str_macros.h"

#define MAX_FORMAT_DIGITS     16
#define DECIMAL_EXPONENT_BIAS 398
#define MAX_DECIMAL_EXPONENT  767

#if DECIMAL_CALL_BY_REFERENCE

void
bid64_to_string (char *ps, BID_UINT64 * px
		 _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT64 x;
#else

VOID_WRAPFN_OTHERTYPERES_DFP(bid64_to_string, char, 64)
void
bid64_to_string (char *ps, BID_UINT64 x
		 _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
// the destination string (pointed to by ps) must be pre-allocated
  BID_UINT64 sign_x, coefficient_x, D, ER10;
  int istart, exponent_x, j, digits_x, bin_expon_cx;
  int_float tempx;
  BID_UINT32 MiDi[12], *ptr;
  BID_UINT64 HI_18Dig, LO_18Dig, Tmp;
  char *c_ptr_start, *c_ptr;
  int midi_ind, k_lcv, len;
  unsigned int save_fpsf;

#if DECIMAL_CALL_BY_REFERENCE
  x = *px;
#endif

  save_fpsf = *pfpsf; // place holder only
  // unpack arguments, check for NaN or Infinity
  if (!unpack_BID64 (&sign_x, &exponent_x, &coefficient_x, x)) {
    // x is Inf. or NaN or 0

    // Inf or NaN?
    if ((x & 0x7800000000000000ull) == 0x7800000000000000ull) {
      if ((x & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
    ps[0] = (sign_x) ? '-' : '+';
    ps[1] = 'S';
		j = ((x & MASK_SNAN) == MASK_SNAN)? 2: 1;
	ps[j++] = 'N';
	ps[j++] = 'a';
	ps[j++] = 'N';
	ps[j++] = 0;
	return;
      }
      // x is Inf
      ps[0] = (sign_x) ? '-' : '+';
      ps[1] = 'I';
      ps[2] = 'n';
      ps[3] = 'f';
      ps[4] = 0;
      return;
    }
    // 0
    istart = 1;
	ps[0] = (sign_x)? '-': '+';

    ps[istart++] = '0';
    ps[istart++] = 'E';

    exponent_x -= 398;
    if (exponent_x < 0) {
      ps[istart++] = '-';
      exponent_x = -exponent_x;
    } else
      ps[istart++] = '+';

    if (exponent_x) {
      // get decimal digits in coefficient_x
      tempx.d = (float) exponent_x;
      bin_expon_cx = ((tempx.i >> 23) & 0xff) - 0x7f;
      digits_x = bid_estimate_decimal_digits[bin_expon_cx];
      if ((BID_UINT64)exponent_x >= bid_power10_table_128[digits_x].w[0])
	digits_x++;

      j = istart + digits_x - 1;
      istart = j + 1;

      // 2^32/10
      ER10 = 0x1999999a;

      while (exponent_x > 9) {
	D = (BID_UINT64) exponent_x *ER10;
	D >>= 32;
	exponent_x = exponent_x - (D << 1) - (D << 3);

	ps[j--] = '0' + (char) exponent_x;
	exponent_x = D;
      }
      ps[j] = '0' + (char) exponent_x;
    } else {
      ps[istart++] = '0';
    }

    ps[istart] = 0;

    return;
  }
  // convert expon, coeff to ASCII
  exponent_x -= DECIMAL_EXPONENT_BIAS;

  ER10 = 0x1999999a;

    istart = 1;
	ps[0] = (sign_x)? '-': '+';

  // if zero or non-canonical, set coefficient to '0'
  if ((coefficient_x > 9999999999999999ull) ||	// non-canonical
      ((coefficient_x == 0))	// significand is zero
    ) {
    ps[istart++] = '0';
  } else {
    /* ****************************************************
       This takes a bid coefficient in C1.w[1],C1.w[0] 
       and put the converted character sequence at location 
       starting at &(str[k]). The function returns the number
       of MiDi returned. Note that the character sequence 
       does not have leading zeros EXCEPT when the input is of
       zero value. It will then output 1 character '0'
       The algorithm essentailly tries first to get a sequence of
       Millenial Digits "MiDi" and then uses table lookup to get the
       character strings of these MiDis.
       **************************************************** */
    /* Algorithm first decompose possibly 34 digits in hi and lo
       18 digits. (The high can have at most 16 digits). It then
       uses macro that handle 18 digit portions.
       The first step is to get hi and lo such that
       2^(64) C1.w[1] + C1.w[0] = hi * 10^18  + lo,   0 <= lo < 10^18.
       We use a table lookup method to obtain the hi and lo 18 digits.
       [C1.w[1],C1.w[0]] = c_8 2^(107) + c_7 2^(101) + ... + c_0 2^(59) + d
       where 0 <= d < 2^59 and each c_j has 6 bits. Because d fits in
       18 digits,  we set hi = 0, and lo = d to begin with.
       We then retrieve from a table, for j = 0, 1, ..., 8
       that gives us A and B where c_j 2^(59+6j) = A * 10^18 + B.
       hi += A ; lo += B; After each accumulation into lo, we normalize 
       immediately. So at the end, we have the decomposition as we need. */

    Tmp = coefficient_x >> 59;
    LO_18Dig = (coefficient_x << 5) >> 5;
    HI_18Dig = 0;
    k_lcv = 0;

    while (Tmp) {
      midi_ind = (int) (Tmp & 0x000000000000003FLL);
      midi_ind <<= 1;
      Tmp >>= 6;
      HI_18Dig += mod10_18_tbl[k_lcv][midi_ind++];
      LO_18Dig += mod10_18_tbl[k_lcv++][midi_ind];
      __L0_Normalize_10to18 (HI_18Dig, LO_18Dig);
    }

    ptr = MiDi;
    __L1_Split_MiDi_6_Lead (LO_18Dig, ptr);
    len = ptr - MiDi;
    c_ptr_start = &(ps[istart]);
    c_ptr = c_ptr_start;

    /* now convert the MiDi into character strings */
    __L0_MiDi2Str_Lead (MiDi[0], c_ptr);
    for (k_lcv = 1; k_lcv < len; k_lcv++) {
      __L0_MiDi2Str (MiDi[k_lcv], c_ptr);
    }
    istart = istart + (c_ptr - c_ptr_start);
  }

  ps[istart++] = 'E';

  if (exponent_x < 0) {
    ps[istart++] = '-';
    exponent_x = -exponent_x;
  } else
    ps[istart++] = '+';

  if (exponent_x) {
    // get decimal digits in coefficient_x
    tempx.d = (float) exponent_x;
    bin_expon_cx = ((tempx.i >> 23) & 0xff) - 0x7f;
    digits_x = bid_estimate_decimal_digits[bin_expon_cx];
    if ((BID_UINT64)exponent_x >= bid_power10_table_128[digits_x].w[0])
      digits_x++;

    j = istart + digits_x - 1;
    istart = j + 1;

    // 2^32/10
    ER10 = 0x1999999a;

    while (exponent_x > 9) {
      D = (BID_UINT64) exponent_x *ER10;
      D >>= 32;
      exponent_x = exponent_x - (D << 1) - (D << 3);

      ps[j--] = '0' + (char) exponent_x;
      exponent_x = D;
    }
    ps[j] = '0' + (char) exponent_x;
  } else {
    ps[istart++] = '0';
  }

  ps[istart] = 0;

  return;

}


#if DECIMAL_CALL_BY_REFERENCE
void
bid64_from_string (BID_UINT64 * pres, char *ps
		   _RND_MODE_PARAM _EXC_FLAGS_PARAM 
                   _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#else
DFP_WRAPFN_OTHERTYPE(64, bid64_from_string, char*)
BID_UINT64
bid64_from_string (char *ps
		   _RND_MODE_PARAM _EXC_FLAGS_PARAM 
                   _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT64 sign_x, coefficient_x = 0, rounded = 0, res;
  int expon_x = 0, sgn_expon, ndigits, add_expon = 0, midpoint =
    0, rounded_up = 0;
  int dec_expon_scale = 0, right_radix_leading_zeros = 0, rdx_pt_enc =
    0;
  char c;
  unsigned int save_fpsf;

#if DECIMAL_CALL_BY_REFERENCE
#if !DECIMAL_GLOBAL_ROUNDING
  _IDEC_round rnd_mode = *prnd_mode;
#endif
#endif

  save_fpsf = *pfpsf; // place holder only
  // eliminate leading whitespace
  while (((*ps == ' ') || (*ps == '\t')) && (*ps))
    ps++;

  // get first non-whitespace character
  c = *ps;

  // detect special cases (INF or NaN)
  if (!c || (c != '.' && c != '-' && c != '+' && (c < '0' || c > '9'))) {
    // Infinity?
    if ((tolower_macro (ps[0]) == 'i' && tolower_macro (ps[1]) == 'n' && 
        tolower_macro (ps[2]) == 'f') && (!ps[3] || 
        (tolower_macro (ps[3]) == 'i' && 
        tolower_macro (ps[4]) == 'n' && tolower_macro (ps[5]) == 'i' && 
        tolower_macro (ps[6]) == 't' && tolower_macro (ps[7]) == 'y' && 
        !ps[8]))) {
      res = 0x7800000000000000ull;
      BID_RETURN (res);
    }
    // return sNaN
    if (tolower_macro (ps[0]) == 's' && tolower_macro (ps[1]) == 'n' && 
        tolower_macro (ps[2]) == 'a' && tolower_macro (ps[3]) == 'n') { 
        // case insensitive check for snan
      res = 0x7e00000000000000ull;
      BID_RETURN (res);
    } else {
      // return qNaN
      res = 0x7c00000000000000ull;
      BID_RETURN (res);
    }
  }
  // detect +INF or -INF
  if ((tolower_macro (ps[1]) == 'i' && tolower_macro (ps[2]) == 'n' && 
      tolower_macro (ps[3]) == 'f') && (!ps[4] || 
      (tolower_macro (ps[4]) == 'i' && tolower_macro (ps[5]) == 'n' && 
      tolower_macro (ps[6]) == 'i' && tolower_macro (ps[7]) == 't' && 
      tolower_macro (ps[8]) == 'y' && !ps[9]))) {
    if (c == '+')
      res = 0x7800000000000000ull;
    else if (c == '-')
      res = 0xf800000000000000ull;
    else
      res = 0x7c00000000000000ull;
    BID_RETURN (res);
  }
  // if +sNaN, +SNaN, -sNaN, or -SNaN
  if (tolower_macro (ps[1]) == 's' && tolower_macro (ps[2]) == 'n'
      && tolower_macro (ps[3]) == 'a' && tolower_macro (ps[4]) == 'n') {
    if (c == '-')
      res = 0xfe00000000000000ull;
    else
      res = 0x7e00000000000000ull;
    BID_RETURN (res);
  }
  // determine sign
  if (c == '-')
    sign_x = 0x8000000000000000ull;
  else
    sign_x = 0;

  // get next character if leading +/- sign
  if (c == '-' || c == '+') {
    ps++;
    c = *ps;
  }
  // if c isn't a decimal point or a decimal digit, return NaN
  if (c != '.' && (c < '0' || c > '9')) {
    // return NaN
    res = 0x7c00000000000000ull | sign_x;
    BID_RETURN (res);
  }

  rdx_pt_enc = 0;

  // detect zero (and eliminate/ignore leading zeros)
  if (*(ps) == '0' || *(ps) == '.') {

    if (*(ps) == '.') {
      rdx_pt_enc = 1;
      ps++;
    }
    // if all numbers are zeros (with possibly 1 radix point, the number is zero
    // should catch cases such as: 000.0
    while (*ps == '0') {
      ps++;
      // for numbers such as 0.0000000000000000000000000000000000001001, 
      // we want to count the leading zeros
      if (rdx_pt_enc) {
	right_radix_leading_zeros++;
      }
      // if this character is a radix point, make sure we haven't already 
      // encountered one
      if (*(ps) == '.') {
	if (rdx_pt_enc == 0) {
	  rdx_pt_enc = 1;
	  // if this is the first radix point, and the next character is NULL, 
          // we have a zero
	  if (!*(ps + 1)) {
	    res =
	      ((BID_UINT64) (398 - right_radix_leading_zeros) << 53) |
	      sign_x;
	    BID_RETURN (res);
	  }
	  ps = ps + 1;
	} else {
	  // if 2 radix points, return NaN
	  res = 0x7c00000000000000ull | sign_x;
	  BID_RETURN (res);
	}
      } else if (!*(ps)) {
	//pres->w[1] = 0x3040000000000000ull | sign_x;
	res =
	  ((BID_UINT64) (398 - right_radix_leading_zeros) << 53) | sign_x;
	BID_RETURN (res);
      }
    }
  }

  c = *ps;

  ndigits = 0;
  while ((c >= '0' && c <= '9') || c == '.') {
    if (c == '.') {
      if (rdx_pt_enc) {
	// return NaN
	res = 0x7c00000000000000ull | sign_x;
	BID_RETURN (res);
      }
      rdx_pt_enc = 1;
      ps++;
      c = *ps;
      continue;
    }
    dec_expon_scale += rdx_pt_enc;

    ndigits++;
    if (ndigits <= 16) {
      coefficient_x = (coefficient_x << 1) + (coefficient_x << 3);
      coefficient_x += (BID_UINT64) (c - '0');
    } else if (ndigits == 17) {
      // coefficient rounding
		switch(rnd_mode){
	case BID_ROUNDING_TO_NEAREST:
      midpoint = (c == '5' && !(coefficient_x & 1)) ? 1 : 0; 
          // if coefficient is even and c is 5, prepare to round up if 
          // subsequent digit is nonzero
      // if str[MAXDIG+1] > 5, we MUST round up
      // if str[MAXDIG+1] == 5 and coefficient is ODD, ROUND UP!
      if (c > '5' || (c == '5' && (coefficient_x & 1))) {
	coefficient_x++;
	rounded_up = 1;
	break;

	case BID_ROUNDING_DOWN:
		if(sign_x) { coefficient_x++; rounded_up=1; }
		break;
	case BID_ROUNDING_UP:
		if(!sign_x) { coefficient_x++; rounded_up=1; }
		break;
	case BID_ROUNDING_TIES_AWAY:
		if(c>='5') { coefficient_x++; rounded_up=1; }
		break;
	  }
	if (coefficient_x == 10000000000000000ull) {
	  coefficient_x = 1000000000000000ull;
	  add_expon = 1;
	}
      }
      if (c > '0')
	rounded = 1;
      add_expon += 1;
    } else { // ndigits > 17
      add_expon++;
      if (midpoint && c > '0') {
	coefficient_x++;
	midpoint = 0;
	rounded_up = 1;
      }
      if (c > '0')
	rounded = 1;
    }
    ps++;
    c = *ps;
  }

  add_expon -= (dec_expon_scale + right_radix_leading_zeros);

  if (!c) {
 #ifdef BID_SET_STATUS_FLAGS
	if(rounded)
      __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
#endif
	res =
      fast_get_BID64_check_OF (sign_x,
			       add_expon + DECIMAL_EXPONENT_BIAS,
			       coefficient_x, 0, pfpsf);
    BID_RETURN (res);
  }

  if (c != 'E' && c != 'e') {
    // return NaN
    res = 0x7c00000000000000ull | sign_x;
    BID_RETURN (res);
  }
  ps++;
  c = *ps;
  sgn_expon = (c == '-') ? 1 : 0;
  if (c == '-' || c == '+') {
    ps++;
    c = *ps;
  }
  if (!c || c < '0' || c > '9') {
    // return NaN
    res = 0x7c00000000000000ull | sign_x;
    BID_RETURN (res);
  }

  while ((c >= '0') && (c <= '9')) {
   if(expon_x<(1<<20)) {
    expon_x = (expon_x << 1) + (expon_x << 3);
	expon_x += (int) (c - '0'); }

    ps++;
    c = *ps;
  }

  if (c) {
    // return NaN
    res = 0x7c00000000000000ull | sign_x;
    BID_RETURN (res);
  }

#ifdef BID_SET_STATUS_FLAGS
	if(rounded)
      __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
#endif

  if (sgn_expon)
    expon_x = -expon_x;

  expon_x += add_expon + DECIMAL_EXPONENT_BIAS;

  if (expon_x < 0) {
    if (rounded_up)
      coefficient_x--;
    rnd_mode = 0;
    res =
      get_BID64_UF (sign_x, expon_x, coefficient_x, rounded, rnd_mode,
		    pfpsf);
    BID_RETURN (res);
  }
  res = get_BID64 (sign_x, expon_x, coefficient_x, rnd_mode, pfpsf);
  BID_RETURN (res);
}
