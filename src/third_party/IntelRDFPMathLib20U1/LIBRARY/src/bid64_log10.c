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

#include "bid_trans.h"

#define BID64_1 0x31c0000000000001ull

BID_F80_CONST_DEF( c_one,   3fff000000000000, 0000000000000000); // 1.0
BID_F80_CONST_DEF( c_half,  3ffe000000000000, 0000000000000000); // 0.5
BID_F80_CONST_DEF(c_ln_10,  400026bb1bbb5551, 582dd4adac5705a6); // ln(10)

BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT64, bid64_log10, BID_UINT64, x)

  BID_UINT64 res;
  BID_F80_TYPE xd, rd, e_bin, abs_e_bin, rt;
  int z;

// Check for NaN and just return the same NaN, quieted and canonized

  if ((x & NAN_MASK64) == NAN_MASK64)
   {
     #ifdef BID_SET_STATUS_FLAGS
     if ((x & SNAN_MASK64) == SNAN_MASK64)
        __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     res = x & 0xfc03ffffffffffffull;
     if ((res & 0x0003ffffffffffffull) > 999999999999999ull)
        res &= ~0x0003ffffffffffffull;
     BID_RETURN(res);
   }

  BIDECIMAL_CALL1_NORND_NOSTAT (bid64_isZero, z, x);
  if (z)
   { // -Infinite and Divide by Zero according C99
     res = 0xf800000000000000ull;
     #ifdef BID_SET_STATUS_FLAGS
     __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
     #endif
     BID_RETURN (res);
   }

  if (x & MASK_SIGN)
   { // QNaN Indefinite
     res = 0x7c00000000000000ull;
     #ifdef BID_SET_STATUS_FLAGS
     __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     BID_RETURN (res);
   }

  BIDECIMAL_CALL1 (bid64_to_binary80, xd, x);
  __bid_f80_log10( rd, xd );
  __bid_f80_sub( e_bin, xd, c_one.v);
  __bid_f80_fabs( abs_e_bin, e_bin);
  if (__bid_f80_lt(abs_e_bin, c_half.v ) )
   { BID_F80_TYPE tmp_e;
     BID_UINT64 e;
     BID_UINT64 b64 = (BID_UINT64) BID64_1;
     BIDECIMAL_CALL2 (bid64_sub, e, x, b64);
     BIDECIMAL_CALL1 (bid64_to_binary80, tmp_e, e);
     __bid_f80_mul( rt, c_ln_10.v, xd );
     __bid_f80_sub( tmp_e, e_bin, tmp_e);
     __bid_f80_div( tmp_e, tmp_e, rt);
     __bid_f80_sub( rd, rd, tmp_e);
   }
  BIDECIMAL_CALL1 (binary80_to_bid64, res, rd);
  BID_RETURN (res);
}
