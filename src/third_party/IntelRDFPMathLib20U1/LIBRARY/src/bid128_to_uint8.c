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

#define SIZE_MASK      0xffffff00
#define INVALID_RESULT 0x80

BID_TO_SMALL_BID_UINT_CVT_FUNCTION (unsigned char, bid128_to_uint8_rnint,
				BID_UINT128, x, bid128_to_uint32_rnint,
				unsigned int, SIZE_MASK, INVALID_RESULT)

BID_TO_SMALL_BID_UINT_CVT_FUNCTION (unsigned char, bid128_to_uint8_xrnint,
				BID_UINT128, x, bid128_to_uint32_xrnint,
				unsigned int, SIZE_MASK, INVALID_RESULT)

BID_TO_SMALL_BID_UINT_CVT_FUNCTION (unsigned char, bid128_to_uint8_rninta,
				BID_UINT128, x, bid128_to_uint32_rninta,
				unsigned int, SIZE_MASK, INVALID_RESULT)

BID_TO_SMALL_BID_UINT_CVT_FUNCTION (unsigned char, bid128_to_uint8_xrninta,
				BID_UINT128, x, bid128_to_uint32_xrninta,
				unsigned int, SIZE_MASK, INVALID_RESULT)

BID_TO_SMALL_BID_UINT_CVT_FUNCTION (unsigned char, bid128_to_uint8_int,
				BID_UINT128, x, bid128_to_uint32_int,
				unsigned int, SIZE_MASK, INVALID_RESULT)

BID_TO_SMALL_BID_UINT_CVT_FUNCTION (unsigned char, bid128_to_uint8_xint,
				BID_UINT128, x, bid128_to_uint32_xint,
				unsigned int, SIZE_MASK, INVALID_RESULT)

BID_TO_SMALL_BID_UINT_CVT_FUNCTION (unsigned char, bid128_to_uint8_floor,
				BID_UINT128, x, bid128_to_uint32_floor,
				unsigned int, SIZE_MASK, INVALID_RESULT)

BID_TO_SMALL_BID_UINT_CVT_FUNCTION (unsigned char, bid128_to_uint8_ceil,
				BID_UINT128, x, bid128_to_uint32_ceil,
				unsigned int, SIZE_MASK, INVALID_RESULT)

BID_TO_SMALL_BID_UINT_CVT_FUNCTION (unsigned char, bid128_to_uint8_xfloor,
				BID_UINT128, x, bid128_to_uint32_xfloor,
				unsigned int, SIZE_MASK, INVALID_RESULT)

BID_TO_SMALL_BID_UINT_CVT_FUNCTION (unsigned char, bid128_to_uint8_xceil,
				BID_UINT128, x, bid128_to_uint32_xceil,
				unsigned int, SIZE_MASK, INVALID_RESULT)
