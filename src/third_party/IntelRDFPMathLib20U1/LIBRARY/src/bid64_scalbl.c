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

BID_TYPE0_FUNCTION_ARGTYPE1_OTHER_ARGTYPE2(BID_UINT64, bid64_scalbln, BID_UINT64, x, long int, n)
  BID_UINT64 res;

  int n1;
  n1 = (int)n;
  n1 = n1 < n ? (int)0x7fffffff : n1 > n ? (int)0x80000000 : n1; /* treat overflow/underflow */

#if DECIMAL_CALL_BY_REFERENCE
  bid64_scalbn (&res, &x, &n1
        _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
#else
  res = bid64_scalbn (x, n1 
        _RND_MODE_ARG _EXC_FLAGS_ARG _EXC_MASKS_ARG _EXC_INFO_ARG);
#endif
  BID_RETURN (res);

}
