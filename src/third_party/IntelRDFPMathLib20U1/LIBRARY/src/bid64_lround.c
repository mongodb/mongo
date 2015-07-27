/******************************************************************************
  Copyright (c) 2011, Intel Corp.
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
 *  BID64_lroundd
 ****************************************************************************/

/*
  DESCRIPTION:
    The lround function rounds its argument to the nearest integer value of
    type long int, using rounding to nearest-away
  RETURN VALUE: 
    If the rounded value is outside the range of the return type or the 
    argument is infinity or NaN, the result is the largest negative value
    and the invalid exception is signaled
  EXCEPTIONS SIGNALED: 
    invalid
 */

BID_TYPE0_FUNCTION_ARGTYPE1_NORND(long int, bid64_lround, BID_UINT64, x)

#if BID_SIZE_LONG==4 
  BID_SINT32 res;
  BIDECIMAL_CALL1_NORND (bid64_to_int32_rninta, res, x);
#else
  // if BID_SIZE_LONG==8
  BID_SINT64 res;
  BIDECIMAL_CALL1_NORND (bid64_to_int64_rninta, res, x);
#endif
  BID_RETURN ((long int)res);
}

