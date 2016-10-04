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

#define SIZE_MASK      0xffff8000
#define INVALID_RESULT 0x8000

BID_TO_SMALL_INT_CVT_FUNCTION (short, bid64_to_int16_rnint, BID_UINT64, x,
			       bid64_to_int32_rnint, int, SIZE_MASK,
			       INVALID_RESULT)

BID_TO_SMALL_INT_CVT_FUNCTION (short, bid64_to_int16_xrnint, BID_UINT64, x,
			       bid64_to_int32_xrnint, int, SIZE_MASK,
			       INVALID_RESULT)

BID_TO_SMALL_INT_CVT_FUNCTION (short, bid64_to_int16_rninta, BID_UINT64, x,
			       bid64_to_int32_rninta, int, SIZE_MASK,
			       INVALID_RESULT)

BID_TO_SMALL_INT_CVT_FUNCTION (short, bid64_to_int16_xrninta, BID_UINT64, x,
			       bid64_to_int32_xrninta, int, SIZE_MASK,
			       INVALID_RESULT)

BID_TO_SMALL_INT_CVT_FUNCTION (short, bid64_to_int16_int, BID_UINT64, x,
			       bid64_to_int32_int, int, SIZE_MASK,
			       INVALID_RESULT)

BID_TO_SMALL_INT_CVT_FUNCTION (short, bid64_to_int16_xint, BID_UINT64, x,
			       bid64_to_int32_xint, int, SIZE_MASK,
			       INVALID_RESULT)

BID_TO_SMALL_INT_CVT_FUNCTION (short, bid64_to_int16_floor, BID_UINT64, x,
			       bid64_to_int32_floor, int, SIZE_MASK,
			       INVALID_RESULT)

BID_TO_SMALL_INT_CVT_FUNCTION (short, bid64_to_int16_ceil, BID_UINT64, x,
			       bid64_to_int32_ceil, int, SIZE_MASK,
			       INVALID_RESULT)

BID_TO_SMALL_INT_CVT_FUNCTION (short, bid64_to_int16_xfloor, BID_UINT64, x,
			       bid64_to_int32_xfloor, int, SIZE_MASK,
			       INVALID_RESULT)

BID_TO_SMALL_INT_CVT_FUNCTION (short, bid64_to_int16_xceil, BID_UINT64, x,
			       bid64_to_int32_xceil, int, SIZE_MASK,
			       INVALID_RESULT)
