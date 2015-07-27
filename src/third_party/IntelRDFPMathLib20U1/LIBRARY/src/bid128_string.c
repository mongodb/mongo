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

/*****************************************************************************
 *    BID128_to_string
 ****************************************************************************/

#define BID_128RES
#include <stdio.h>
#include "bid_internal.h"
#include "bid128_2_str.h"
#include "bid128_2_str_macros.h"

BID_EXTERN_C int bid128_bid_coeff_2_string (BID_UINT64 X_hi, BID_UINT64 X_lo,
				  char *char_ptr);

#if DECIMAL_CALL_BY_REFERENCE

void
bid128_to_string (char *str,
		  BID_UINT128 *
		  px _EXC_FLAGS_PARAM _EXC_MASKS_PARAM
		  _EXC_INFO_PARAM) {
  BID_UINT128 x;
#else

VOID_WRAPFN_OTHERTYPERES_DFP(bid128_to_string, char, 128)
void
bid128_to_string (char *str, BID_UINT128 x 
    _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT64 x_sign;
  BID_UINT64 x_exp;
  int exp; 	// unbiased exponent
  // Note: C1.w[1], C1.w[0] represent x_signif_hi, x_signif_lo (all are BID_UINT64)
  int ind;
  BID_UINT128 C1;
  unsigned int k = 0; // pointer in the string
  unsigned int d0, d123;
  BID_UINT64 HI_18Dig, LO_18Dig, Tmp;
  BID_UINT32 MiDi[12], *ptr;
  char *c_ptr_start, *c_ptr;
  int midi_ind, k_lcv, len;
  int save_fpsf;

#if DECIMAL_CALL_BY_REFERENCE
  x = *px;
#endif

  save_fpsf = *pfpsf; // dummy

  BID_SWAP128(x);
  // check for NaN or Infinity
  if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // x is special
    if ((x.w[1] & MASK_NAN) == MASK_NAN) { // x is NAN
      if ((x.w[1] & MASK_SNAN) == MASK_SNAN) { // x is SNAN
	// set invalid flag
    str[0] = ((BID_SINT64)x.w[1]<0)? '-':'+'; 
	str[1] = 'S';
	str[2] = 'N';
	str[3] = 'a';
	str[4] = 'N';
	str[5] = '\0';
      } else { // x is QNaN
    str[0] = ((BID_SINT64)x.w[1]<0)? '-':'+'; 
	str[1] = 'N';
	str[2] = 'a';
	str[3] = 'N';
	str[4] = '\0';
      }
    } else { // x is not a NaN, so it must be infinity
      if ((x.w[1] & MASK_SIGN) == 0x0ull) { // x is +inf
	str[0] = '+';
	str[1] = 'I';
	str[2] = 'n';
	str[3] = 'f';
	str[4] = '\0';
      } else { // x is -inf 
	str[0] = '-';
	str[1] = 'I';
	str[2] = 'n';
	str[3] = 'f';
	str[4] = '\0';
      }
    }
    return;
  } else if (((x.w[1] & MASK_COEFF) == 0x0ull) && (x.w[0] == 0x0ull)) {
    // x is 0
    len = 0;

    //determine if +/-
    if (x.w[1] & MASK_SIGN)
      str[len++] = '-';
    else
      str[len++] = '+';
    str[len++] = '0';
    str[len++] = 'E';

    // extract the exponent and print
    exp = (int) (((x.w[1] & MASK_EXP) >> 49) - 6176);
	if(exp > (((0x5ffe)>>1) - (6176))) {
		exp = (int) ((((x.w[1]<<2) & MASK_EXP) >> 49) - 6176);
	}
    if (exp >= 0) {
      str[len++] = '+';
      len += sprintf (str + len, "%u", exp);// should not use sprintf (should 
      // use sophisticated algorithm, since we know range of exp is limited)
      str[len++] = '\0';
    } else {
      len += sprintf (str + len, "%d", exp);// should not use sprintf (should 
      // use sophisticated algorithm, since we know range of exp is limited)
      str[len++] = '\0';
    }
    return;
  } else { // x is not special and is not zero
    // unpack x
    x_sign = x.w[1] & MASK_SIGN;// 0 for positive, MASK_SIGN for negative
    x_exp = x.w[1] & MASK_EXP;// biased and shifted left 49 bit positions
    if ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull)
       x_exp = (x.w[1]<<2) & MASK_EXP;// biased and shifted left 49 bit positions
    C1.w[1] = x.w[1] & MASK_COEFF;
    C1.w[0] = x.w[0];
    exp = (x_exp >> 49) - 6176;

    // determine sign's representation as a char
    if (x_sign)
      str[k++] = '-';// negative number
    else
      str[k++] = '+';// positive number

    // determine coefficient's representation as a decimal string

    // if zero or non-canonical, set coefficient to '0'
    if ((C1.w[1] > 0x0001ed09bead87c0ull) || 
        (C1.w[1] == 0x0001ed09bead87c0ull && 
        (C1.w[0] > 0x378d8e63ffffffffull)) || 
        ((x.w[1] & 0x6000000000000000ull) == 0x6000000000000000ull) || 
        ((C1.w[1] == 0) && (C1.w[0] == 0))) {
      str[k++] = '0';
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

      Tmp = C1.w[0] >> 59;
      LO_18Dig = (C1.w[0] << 5) >> 5;
      Tmp += (C1.w[1] << 5);
      HI_18Dig = 0;
      k_lcv = 0;
      // Tmp = {C1.w[1]{49:0}, C1.w[0]{63:59}}
      // Lo_18Dig = {C1.w[0]{58:0}}

      while (Tmp) {
	midi_ind = (int) (Tmp & 0x000000000000003FLL);
	midi_ind <<= 1;
	Tmp >>= 6;
	HI_18Dig += mod10_18_tbl[k_lcv][midi_ind++];
	LO_18Dig += mod10_18_tbl[k_lcv++][midi_ind];
	__L0_Normalize_10to18 (HI_18Dig, LO_18Dig);
      }
      ptr = MiDi;
      if (HI_18Dig == 0LL) {
	__L1_Split_MiDi_6_Lead (LO_18Dig, ptr);
      } else {
	__L1_Split_MiDi_6_Lead (HI_18Dig, ptr);
	__L1_Split_MiDi_6 (LO_18Dig, ptr);
      }
      len = ptr - MiDi;
      c_ptr_start = &(str[k]);
      c_ptr = c_ptr_start;

      /* now convert the MiDi into character strings */
      __L0_MiDi2Str_Lead (MiDi[0], c_ptr);
      for (k_lcv = 1; k_lcv < len; k_lcv++) {
	__L0_MiDi2Str (MiDi[k_lcv], c_ptr);
      }
      k = k + (c_ptr - c_ptr_start);
    }

    // print E and sign of exponent
    str[k++] = 'E';
    if (exp < 0) {
      exp = -exp;
      str[k++] = '-';
    } else {
      str[k++] = '+';
    }

    // determine exponent's representation as a decimal string
    // d0 = exp / 1000;
    // Use Property 1
    d0 = (exp * 0x418a) >> 24;// 0x418a * 2^-24 = (10^(-3))RP,15
    d123 = exp - 1000 * d0;

    if (d0) { // 1000 <= exp <= 6144 => 4 digits to return
      str[k++] = d0 + 0x30;// ASCII for decimal digit d0
      ind = 3 * d123;
      str[k++] = bid_char_table3[ind];
      str[k++] = bid_char_table3[ind + 1];
      str[k++] = bid_char_table3[ind + 2];
    } else { // 0 <= exp <= 999 => d0 = 0
      if (d123 < 10) { // 0 <= exp <= 9 => 1 digit to return
	str[k++] = d123 + 0x30;// ASCII
      } else if (d123 < 100) { // 10 <= exp <= 99 => 2 digits to return
	ind = 2 * (d123 - 10);
	str[k++] = bid_char_table2[ind];
	str[k++] = bid_char_table2[ind + 1];
      } else { // 100 <= exp <= 999 => 3 digits to return
	ind = 3 * d123;
	str[k++] = bid_char_table3[ind];
	str[k++] = bid_char_table3[ind + 1];
	str[k++] = bid_char_table3[ind + 2];
      }
    }
    str[k] = '\0';

  }
  return;

}


#define MAX_FORMAT_DIGITS_128   34
#define MAX_STRING_DIGITS_128   100
#define MAX_SEARCH              MAX_STRING_DIGITS_128-MAX_FORMAT_DIGITS_128-1



#if DECIMAL_CALL_BY_REFERENCE

void
bid128_from_string (BID_UINT128 * pres,
                    char *ps _RND_MODE_PARAM _EXC_FLAGS_PARAM
                    _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#else

DFP_WRAPFN_OTHERTYPE(128, bid128_from_string, char*)
BID_UINT128
bid128_from_string (char *ps _RND_MODE_PARAM _EXC_FLAGS_PARAM
                    _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  BID_UINT128 CX, res;
  BID_UINT64 sign_x, coeff_high, coeff_low, coeff2, coeff_l2, carry = 0x0ull,
    scale_high, right_radix_leading_zeros;
  int ndigits_before, ndigits_after, ndigits_total, dec_expon, sgn_exp,
    i, d2, rdx_pt_enc, set_inexact=0;
  char c, buffer[MAX_STRING_DIGITS_128];
  int save_rnd_mode;
  int save_fpsf;

#if DECIMAL_CALL_BY_REFERENCE
#if !DECIMAL_GLOBAL_ROUNDING
  _IDEC_round rnd_mode = *prnd_mode;
#endif
#endif

  save_rnd_mode = rnd_mode; // dummy
  save_fpsf = *pfpsf; // dummy

  right_radix_leading_zeros = rdx_pt_enc = 0;

  // if null string, return NaN
  if (!ps) {
    res.w[1] = 0x7c00000000000000ull;
    res.w[0] = 0;
    BID_RETURN (res);
  }
  // eliminate leading white space
  while ((*ps == ' ') || (*ps == '\t'))
    ps++;

  // c gets first character
  c = *ps;


  // if c is null or not equal to a (radix point, negative sign, 
  // positive sign, or number) it might be SNaN, sNaN, Infinity
  if (!c
      || (c != '.' && c != '-' && c != '+'
          && ((unsigned) (c - '0') > 9))) {
    res.w[0] = 0;
    // Infinity?
    if ((tolower_macro (ps[0]) == 'i' && tolower_macro (ps[1]) == 'n'
         && tolower_macro (ps[2]) == 'f')
        && (!ps[3]
            || (tolower_macro (ps[3]) == 'i'
                && tolower_macro (ps[4]) == 'n'
                && tolower_macro (ps[5]) == 'i'
                && tolower_macro (ps[6]) == 't'
                && tolower_macro (ps[7]) == 'y' && !ps[8])
        )) {
      res.w[1] = 0x7800000000000000ull;
      BID_RETURN (res);
    }
    // return sNaN
    if (tolower_macro (ps[0]) == 's' && tolower_macro (ps[1]) == 'n' && 
        tolower_macro (ps[2]) == 'a' && tolower_macro (ps[3]) == 'n') {        
        // case insensitive check for snan
      res.w[1] = 0x7e00000000000000ull;
      BID_RETURN (res);
    } else {
      // return qNaN
      res.w[1] = 0x7c00000000000000ull;
      BID_RETURN (res);
    }
  }
  // if +Inf, -Inf, +Infinity, or -Infinity (case insensitive check for inf)   
  if ((tolower_macro (ps[1]) == 'i' && tolower_macro (ps[2]) == 'n' && 
      tolower_macro (ps[3]) == 'f') && (!ps[4] || 
      (tolower_macro (ps[4]) == 'i' && tolower_macro (ps[5]) == 'n' && 
      tolower_macro (ps[6]) == 'i' && tolower_macro (ps[7]) == 't' && 
      tolower_macro (ps[8]) == 'y' && !ps[9]))) { // ci check for infinity
    res.w[0] = 0;

    if (c == '+')
      res.w[1] = 0x7800000000000000ull;
    else if (c == '-')
      res.w[1] = 0xf800000000000000ull;
    else
      res.w[1] = 0x7c00000000000000ull;

    BID_RETURN (res);
  }
  // if +sNaN, +SNaN, -sNaN, or -SNaN
  if (tolower_macro (ps[1]) == 's' && tolower_macro (ps[2]) == 'n'
      && tolower_macro (ps[3]) == 'a' && tolower_macro (ps[4]) == 'n') {
    res.w[0] = 0;
    if (c == '-')
      res.w[1] = 0xfe00000000000000ull;
    else
      res.w[1] = 0x7e00000000000000ull;
    BID_RETURN (res);
  }
  // set up sign_x to be OR'ed with the upper word later
  if (c == '-')
    sign_x = 0x8000000000000000ull;
  else
    sign_x = 0;

  // go to next character if leading sign
  if (c == '-' || c == '+')
    ps++;

  c = *ps;

  // if c isn't a decimal point or a decimal digit, return NaN
  if (c != '.' && ((unsigned) (c - '0') > 9)) {
    res.w[1] = 0x7c00000000000000ull | sign_x;
    res.w[0] = 0;
    BID_RETURN (res);
  }
  if(c=='.') {
	  rdx_pt_enc=1;
	  ps++;
  }

  // detect zero (and eliminate/ignore leading zeros)
  if (*(ps) == '0') {

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
            res.w[1] =
              (0x3040000000000000ull -
               (right_radix_leading_zeros << 49)) | sign_x;
            res.w[0] = 0;
            BID_RETURN (res);
          }
          ps = ps + 1;
        } else {
          // if 2 radix points, return NaN
          res.w[1] = 0x7c00000000000000ull | sign_x;
          res.w[0] = 0;
          BID_RETURN (res);
        }
      } else if (!*(ps)) {
		if(right_radix_leading_zeros>6176) right_radix_leading_zeros=6176;
        res.w[1] =
          (0x3040000000000000ull -
           (right_radix_leading_zeros << 49)) | sign_x;
        res.w[0] = 0;
        BID_RETURN (res);
      }
    }
  }

  c = *ps;
 
  // initialize local variables
  ndigits_before = ndigits_after = ndigits_total = 0;
  sgn_exp = 0;
  // pstart_coefficient = ps;

  if (!rdx_pt_enc) {
    // investigate string (before radix point)
    while ((unsigned) (c - '0') <= 9
           /*&& ndigits_before < MAX_STRING_DIGITS_128*/) {
      if(ndigits_before < MAX_FORMAT_DIGITS_128) buffer[ndigits_before] = c; 
      else if(ndigits_before < MAX_STRING_DIGITS_128) { buffer[ndigits_before] = c; if(c>'0') set_inexact=1; }
	  else if(c>'0') set_inexact=1;
      ps++;
      c = *ps;
      ndigits_before++;
    }

    ndigits_total = ndigits_before;
    if (c == '.') {
      ps++;
      if ((c = *ps)) {

        // investigate string (after radix point)
        while ((unsigned) (c - '0') <= 9
               /*&& ndigits_total < MAX_STRING_DIGITS_128*/) {
          if(ndigits_total < MAX_FORMAT_DIGITS_128) buffer[ndigits_total] = c;
          else if(ndigits_total < MAX_STRING_DIGITS_128) { buffer[ndigits_total] = c; if(c>'0') set_inexact=1; }
	      else if(c>'0') set_inexact=1;
          ps++;
          c = *ps;
          ndigits_total++;
        }
        ndigits_after = ndigits_total - ndigits_before;
      }
    }
  } else {
    // we encountered a radix point while detecting zeros
    //if (c = *ps){

    c = *ps;
    ndigits_total = 0;
    // investigate string (after radix point)
    while ((unsigned) (c - '0') <= 9
           /*&& ndigits_total < MAX_STRING_DIGITS_128*/) {
      if(ndigits_total < MAX_FORMAT_DIGITS_128)  buffer[ndigits_total] = c; 
	  else if(ndigits_total < MAX_STRING_DIGITS_128)  { buffer[ndigits_total] = c; if(c>'0') set_inexact=1; }
	  else if(c>'0') set_inexact=1;
      ps++;
      c = *ps;
      ndigits_total++;
    }
    ndigits_after = ndigits_total - ndigits_before;
  }

  // get exponent
  dec_expon = 0;
  /*if (ndigits_total < MAX_STRING_DIGITS_128)*/ {
    if (c) {
      if (c != 'e' && c != 'E') {
        // return NaN
        res.w[1] = 0x7c00000000000000ull;
        res.w[0] = 0;
        BID_RETURN (res);
      }
      ps++;
      c = *ps;

      if (((unsigned) (c - '0') > 9)
          && ((c != '+' && c != '-') || (unsigned) (ps[1] - '0') > 9)) {
        // return NaN
        res.w[1] = 0x7c00000000000000ull;
        res.w[0] = 0;
        BID_RETURN (res);
      }

      if (c == '-') {
        sgn_exp = -1;
        ps++;
        c = *ps;
      } else if (c == '+') {
        ps++;
        c = (*ps);
      }

      dec_expon = c - '0';
      i = 1;
      ps++;

	  if(!dec_expon) {
		  while((*ps)=='0') ps++;
	  }
      c = (*ps) - '0';

	  while (((unsigned) c) <= 9 && i < 7) {
        d2 = dec_expon + dec_expon;
        dec_expon = (d2 << 2) + d2 + c;
        ps++;
        c = *ps - '0';
        i++;
      }
    }

    dec_expon = (dec_expon + sgn_exp) ^ sgn_exp;
  }


  if (ndigits_total <= MAX_FORMAT_DIGITS_128) {
    dec_expon +=
      DECIMAL_EXPONENT_BIAS_128 - ndigits_after -
      right_radix_leading_zeros;
    if (dec_expon < 0) {
      res.w[1] = 0 | sign_x;
      res.w[0] = 0;
    }
    if (ndigits_total == 0) {
      CX.w[0] = 0;
      CX.w[1] = 0;
    } else if (ndigits_total <= 19) {
      coeff_high = buffer[0] - '0';
      for (i = 1; i < ndigits_total; i++) {
        coeff2 = coeff_high + coeff_high;
        coeff_high = (coeff2 << 2) + coeff2 + buffer[i] - '0';
      }
      CX.w[0] = coeff_high;
      CX.w[1] = 0;
    } else {
      coeff_high = buffer[0] - '0';
      for (i = 1; i < ndigits_total - 17; i++) {
        coeff2 = coeff_high + coeff_high;
        coeff_high = (coeff2 << 2) + coeff2 + buffer[i] - '0';
      }
      coeff_low = buffer[i] - '0';
      i++;
      for (; i < ndigits_total; i++) {
        coeff_l2 = coeff_low + coeff_low;
        coeff_low = (coeff_l2 << 2) + coeff_l2 + buffer[i] - '0';
      }
      // now form the coefficient as coeff_high*10^19+coeff_low+carry
      scale_high = 100000000000000000ull;
      __mul_64x64_to_128_fast (CX, coeff_high, scale_high);

      CX.w[0] += coeff_low;
      if (CX.w[0] < coeff_low)
        CX.w[1]++;
    }
    bid_get_BID128 (&res, sign_x, dec_expon, CX,&rnd_mode,pfpsf);
    BID_RETURN (res);
  } else {
    // simply round using the digits that were read

    dec_expon +=
      ndigits_before + DECIMAL_EXPONENT_BIAS_128 -
      MAX_FORMAT_DIGITS_128 - right_radix_leading_zeros;

    if (dec_expon < 0) {
      res.w[1] = 0 | sign_x;
      res.w[0] = 0;
    }

    coeff_high = buffer[0] - '0';
    for (i = 1; i < MAX_FORMAT_DIGITS_128 - 17; i++) {
      coeff2 = coeff_high + coeff_high;
      coeff_high = (coeff2 << 2) + coeff2 + buffer[i] - '0';
    }
    coeff_low = buffer[i] - '0';
    i++;
    for (; i < MAX_FORMAT_DIGITS_128; i++) {
      coeff_l2 = coeff_low + coeff_low;
      coeff_low = (coeff_l2 << 2) + coeff_l2 + buffer[i] - '0';
    }
	switch(rnd_mode) {
	case BID_ROUNDING_TO_NEAREST:
    carry = ((unsigned) ('4' - buffer[i])) >> 31;
    if ((buffer[i] == '5' && !(coeff_low & 1)) || dec_expon < 0) {
      if (dec_expon >= 0) {
        carry = 0;
        i++;
      }
      for (; i < ndigits_total; i++) {
        if (buffer[i] > '0') {
          carry = 1;
          break;
        }
      }
    }
	break;

	case BID_ROUNDING_DOWN:
		if(sign_x) 
      for (; i < ndigits_total; i++) {
        if (buffer[i] > '0') {
          carry = 1;
          break;
        }
      }
		break;
	case BID_ROUNDING_UP:
		if(!sign_x) 
      for (; i < ndigits_total; i++) {
        if (buffer[i] > '0') {
          carry = 1;
          break;
        }
      }
		break;
	case BID_ROUNDING_TO_ZERO:
		carry=0;
		break;
	case BID_ROUNDING_TIES_AWAY:
    carry = ((unsigned) ('4' - buffer[i])) >> 31;
    if (dec_expon < 0) {
      for (; i < ndigits_total; i++) {
        if (buffer[i] > '0') {
          carry = 1;
          break;
        }
      }
    }
		break;


	}
    // now form the coefficient as coeff_high*10^17+coeff_low+carry
    scale_high = 100000000000000000ull;
    if (dec_expon < 0) {
      if (dec_expon > -MAX_FORMAT_DIGITS_128) {
        scale_high = 1000000000000000000ull;
        coeff_low = (coeff_low << 3) + (coeff_low << 1);
        dec_expon--;
      }
      if (dec_expon == -MAX_FORMAT_DIGITS_128
          && coeff_high > 50000000000000000ull)
        carry = 0; 
    }

    __mul_64x64_to_128_fast (CX, coeff_high, scale_high);

    coeff_low += carry;
    CX.w[0] += coeff_low;
    if (CX.w[0] < coeff_low)
      CX.w[1]++;

#ifdef BID_SET_STATUS_FLAGS
	if(set_inexact)
      __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
#endif

    bid_get_BID128(&res, sign_x, dec_expon, CX, &rnd_mode, pfpsf);
    BID_RETURN (res);
  }
}
