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

#include "bid_internal.h"

/*****************************************************************************
 *  BID32_round_integral_exact
 ****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT32, bid32_round_integral_exact, BID_UINT32, x)

  BID_UINT64 x64, res64;
  BID_UINT32 res;

  BIDECIMAL_CALL1_NORND (bid32_to_bid64, x64, x);
  BIDECIMAL_CALL1 (bid64_round_integral_exact, res64, x64);
  BIDECIMAL_CALL1 (bid64_to_bid32, res, res64);

  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32_round_integral_nearest_even
 ****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_NORND_DFP(BID_UINT32, bid32_round_integral_nearest_even, BID_UINT32, x)

  BID_UINT64 x64, res64;
  BID_UINT32 res;
#if !DECIMAL_GLOBAL_ROUNDING
  _IDEC_round rnd_mode = BID_ROUNDING_TO_NEAREST; // temporary
#endif

  BIDECIMAL_CALL1_NORND (bid32_to_bid64, x64, x);
  BIDECIMAL_CALL1_NORND (bid64_round_integral_nearest_even, res64, x64);
  BIDECIMAL_CALL1 (bid64_to_bid32, res, res64);
 
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32_round_integral_negative
 *****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_NORND_DFP(BID_UINT32, bid32_round_integral_negative, BID_UINT32, x)

  BID_UINT64 x64, res64;
  BID_UINT32 res;
#if !DECIMAL_GLOBAL_ROUNDING
  _IDEC_round rnd_mode = BID_ROUNDING_TO_NEAREST; // temporary
#endif
 
  BIDECIMAL_CALL1_NORND (bid32_to_bid64, x64, x);
  BIDECIMAL_CALL1_NORND (bid64_round_integral_negative, res64, x64);
  BIDECIMAL_CALL1 (bid64_to_bid32, res, res64);
 
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32_round_integral_positive
 ****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_NORND_DFP(BID_UINT32, bid32_round_integral_positive, BID_UINT32, x)

  BID_UINT64 x64, res64;
  BID_UINT32 res;
#if !DECIMAL_GLOBAL_ROUNDING
  _IDEC_round rnd_mode = BID_ROUNDING_TO_NEAREST; // temporary
#endif
 
  BIDECIMAL_CALL1_NORND (bid32_to_bid64, x64, x);
  BIDECIMAL_CALL1_NORND (bid64_round_integral_positive, res64, x64);
  BIDECIMAL_CALL1 (bid64_to_bid32, res, res64);
 
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32_round_integral_zero
 ****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_NORND_DFP(BID_UINT32, bid32_round_integral_zero, BID_UINT32, x)

  BID_UINT64 x64, res64;
  BID_UINT32 res;
#if !DECIMAL_GLOBAL_ROUNDING
  _IDEC_round rnd_mode = BID_ROUNDING_TO_NEAREST; // temporary
#endif
 
  BIDECIMAL_CALL1_NORND (bid32_to_bid64, x64, x);
  BIDECIMAL_CALL1_NORND (bid64_round_integral_zero, res64, x64);
  BIDECIMAL_CALL1 (bid64_to_bid32, res, res64);
 
  BID_RETURN (res);
}

/*****************************************************************************
 *  BID32_round_integral_nearest_away
 ****************************************************************************/

BID_TYPE0_FUNCTION_ARGTYPE1_NORND_DFP(BID_UINT32, bid32_round_integral_nearest_away, BID_UINT32, x)
  BID_UINT64 x64, res64;
  BID_UINT32 res;
#if !DECIMAL_GLOBAL_ROUNDING
  _IDEC_round rnd_mode = BID_ROUNDING_TO_NEAREST; // temporary
#endif

  BIDECIMAL_CALL1_NORND (bid32_to_bid64, x64, x);
  BIDECIMAL_CALL1_NORND (bid64_round_integral_nearest_away, res64, x64);
  BIDECIMAL_CALL1 (bid64_to_bid32, res, res64);
 
  BID_RETURN (res);
}
