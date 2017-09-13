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

#if DECIMAL_CALL_BY_REFERENCE

void
bid32_to_string (char *ps, BID_UINT32 * px
		 _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT32 x;
#else
VOID_WRAPFN_OTHERTYPERES_DFP(bid32_to_string, char, 32)
void
bid32_to_string (char *ps, BID_UINT32 x
		 _EXC_FLAGS_PARAM _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif
  // the destination string (pointed to by ps) must be pre-allocated
  BID_UINT64 CT;
  int d, j, istart, istart0;
  BID_UINT32 sign_x, coefficient_x;
  int  exponent_x;
  unsigned int save_fpsf;

#if DECIMAL_CALL_BY_REFERENCE
  x = *px;
#endif

  save_fpsf = *pfpsf; // place holder only
  // unpack arguments, check for NaN or Infinity
  if (!unpack_BID32 (&sign_x, &exponent_x, &coefficient_x, x)) {
	      ps[0] = (sign_x) ? '-' : '+';
    // x is Inf. or NaN or 0
    if((x&NAN_MASK32)==NAN_MASK32)  {
      ps[1] = 'S';
		j = ((x & SNAN_MASK32) == SNAN_MASK32)? 2: 1;
	  ps[j++] = 'N';
	  ps[j++] = 'a';
	  ps[j++] = 'N';
	  ps[j++] = 0;
	  return;
      }
	if((x&INFINITY_MASK32)==INFINITY_MASK32) {
      ps[1] = 'I';
      ps[2] = 'n';
      ps[3] = 'f';
      ps[4] = 0;
      return;
    }
    istart = 1;
    ps[istart++] = '0';

  }
  else // x is not special
  {
  ps[0] = sign_x? '-': '+';
  istart = 1;
  if(coefficient_x>=1000000)
  {
	  CT = (BID_UINT64)coefficient_x * 0x431BDE83ull;
	  CT >>= 32;
	  d = CT >> (50-32);
	  ps[istart++] = d + '0';

	  coefficient_x -= d*1000000;

	  // get lower 6 digits
	  CT = (BID_UINT64)coefficient_x * 0x20C49BA6ull;
	  CT >>= 32;
	  d = CT >> (39-32);
	  ps[istart++] = bid_midi_tbl[d][0];
	  ps[istart++] = bid_midi_tbl[d][1];
	  ps[istart++] = bid_midi_tbl[d][2];

	  d = coefficient_x - d*1000;

	  ps[istart++] = bid_midi_tbl[d][0];
	  ps[istart++] = bid_midi_tbl[d][1];
	  ps[istart++] = bid_midi_tbl[d][2];
	  //ps[istart] = 0;
  }
  else if(coefficient_x>=1000) {
	  CT = (BID_UINT64)coefficient_x * 0x20C49BA6ull;
	  CT >>= 32;
	  d = CT >> (39-32);

	  istart0=istart;
	  ps[istart] = bid_midi_tbl[d][0];  if(ps[istart]!='0') istart++;
	  ps[istart] = bid_midi_tbl[d][1];
	  if((ps[istart]!='0') || (istart!=istart0)) istart++;
	  ps[istart++] = bid_midi_tbl[d][2];

	  d = coefficient_x - d*1000;

	  ps[istart++] = bid_midi_tbl[d][0];
	  ps[istart++] = bid_midi_tbl[d][1];
	  ps[istart++] = bid_midi_tbl[d][2];
	  //ps[istart] = 0;
  } else {
	  d = coefficient_x;

  	  istart0=istart;
	  ps[istart] = bid_midi_tbl[d][0];  if(ps[istart]!='0') istart++;
	  ps[istart] = bid_midi_tbl[d][1];
	  if((ps[istart]!='0') || (istart!=istart0)) istart++;
	  ps[istart++] = bid_midi_tbl[d][2];
  }
  }

    ps[istart++] = 'E';

    exponent_x -= DECIMAL_EXPONENT_BIAS_32;
    if (exponent_x < 0) {
      ps[istart++] = '-';
      exponent_x = -exponent_x;
    } else
      ps[istart++] = '+';

	istart0 = istart;
	ps[istart]=bid_midi_tbl[exponent_x][0];   if(ps[istart]!='0') istart++;
	ps[istart]=bid_midi_tbl[exponent_x][1]; 
	if((ps[istart]!='0') || (istart!=istart0)) istart++;
	ps[istart++]=bid_midi_tbl[exponent_x][2];
	ps[istart]=0;
	return;

}
 

#if DECIMAL_CALL_BY_REFERENCE
void
bid32_from_string (BID_UINT32 * pres, char *ps
		   _RND_MODE_PARAM _EXC_FLAGS_PARAM 
                   _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#else
DFP_WRAPFN_OTHERTYPE(32, bid32_from_string, char*)
BID_UINT32
bid32_from_string (char *ps
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
      res = 0x78000000ull;
      BID_RETURN (res);
    }
    // return sNaN
    if (tolower_macro (ps[0]) == 's' && tolower_macro (ps[1]) == 'n' && 
        tolower_macro (ps[2]) == 'a' && tolower_macro (ps[3]) == 'n') { 
        // case insensitive check for snan
      res = 0x7e000000ul;
      BID_RETURN (res);
    } else {
      // return qNaN
      res = 0x7c000000ul;
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
      res = 0x78000000ul;
    else if (c == '-')
      res = 0xf8000000ul;
    else
      res = 0x7c000000ul;
    BID_RETURN (res);
  }
  // if +sNaN, +SNaN, -sNaN, or -SNaN
  if (tolower_macro (ps[1]) == 's' && tolower_macro (ps[2]) == 'n'
      && tolower_macro (ps[3]) == 'a' && tolower_macro (ps[4]) == 'n') {
    if (c == '-')
      res = 0xfe000000ul;
    else
      res = 0x7e000000ul;
    BID_RETURN (res);
  }
  // determine sign
  if (c == '-')
    sign_x = 0x80000000ul;
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
    res = 0x7c000000ul | sign_x;
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
		  right_radix_leading_zeros = DECIMAL_EXPONENT_BIAS_32 - right_radix_leading_zeros;
		  if(right_radix_leading_zeros<0)
			  right_radix_leading_zeros=0;
	    res =
	      ((BID_UINT64) (right_radix_leading_zeros) << 23) |
	      sign_x;
	    BID_RETURN (res);
	  }
	  ps = ps + 1;
	} else {
	  // if 2 radix points, return NaN
	  res = 0x7c000000ul | sign_x;
	  BID_RETURN (res);
	}
      } else if (!*(ps)) {
		  right_radix_leading_zeros = DECIMAL_EXPONENT_BIAS_32 - right_radix_leading_zeros;
		  if(right_radix_leading_zeros<0)
			  right_radix_leading_zeros=0;
	    res =
	      ((BID_UINT64) (right_radix_leading_zeros) << 23) |
	      sign_x;
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
	res = 0x7c000000ul | sign_x;
	BID_RETURN (res);
      }
      rdx_pt_enc = 1;
      ps++;
      c = *ps;
      continue;
    }
    dec_expon_scale += rdx_pt_enc;

    ndigits++;
    if (ndigits <= 7) {
      coefficient_x = (coefficient_x << 1) + (coefficient_x << 3);
      coefficient_x += (BID_UINT64) (c - '0');
    } else if (ndigits == 8) {
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
	if (coefficient_x == 10000000ul) {
	  coefficient_x = 1000000ul;
	  add_expon = 1;
	}
      }
      if (c > '0')
	rounded = 1;
      add_expon += 1;
    } else { // ndigits > 8
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
      get_BID32 (sign_x,
			       add_expon + DECIMAL_EXPONENT_BIAS_32,
			       coefficient_x, 0, pfpsf);
    BID_RETURN (res);
  }

  if (c != 'E' && c != 'e') {
    // return NaN
    res = 0x7c000000ul | sign_x;
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
    res = 0x7c000000ul | sign_x;
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
    res = 0x7c000000ul | sign_x;
    BID_RETURN (res);
  }

#ifdef BID_SET_STATUS_FLAGS
	if(rounded)
      __set_status_flags (pfpsf, BID_INEXACT_EXCEPTION);
#endif

  if (sgn_expon)
    expon_x = -expon_x;

  expon_x += add_expon + DECIMAL_EXPONENT_BIAS_32;

  if (expon_x < 0) {
    if (rounded_up)
      coefficient_x--;
    rnd_mode = 0; 
    res =
      get_BID32_UF (sign_x, expon_x, coefficient_x, rounded, rnd_mode,
		    pfpsf);
    BID_RETURN (res);
  }
  res = get_BID32 (sign_x, expon_x, coefficient_x, rnd_mode, pfpsf);
  BID_RETURN (res);


}
