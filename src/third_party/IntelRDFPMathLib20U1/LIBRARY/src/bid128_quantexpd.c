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

#define BID_128RES
#include "bid_internal.h"

/*****************************************************************************
 *  BID128_quantexpd
 ****************************************************************************/

/*
 Exceptions signaled: invalid
 */

BID128_FUNCTION_ARG1_NORND_CUSTOMRESTYPE(int, bid128_quantexp, x)

  int res; // quantum

  if ((x.w[1] & MASK_SPECIAL) == MASK_SPECIAL) {
    // set invalid flag 
    *pfpsf |= BID_INVALID_EXCEPTION;
    res = 0x80000000;
    BID_RETURN_VAL (res);
  }
  if ((x.w[1] & MASK_STEERING_BITS) == MASK_STEERING_BITS)
    res = (int)((x.w[1] >> 47) & 0x3fff) - 6176;
  else
    res = ((int)(x.w[1] >> 49) & 0x3fff) - 6176;
  BID_RETURN_VAL (res);
}

