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
#include "bid_gcc_intrinsics.h"

#if DECIMAL_GLOBAL_ROUNDING
BID_THREAD _IDEC_round _IDEC_glbround = BID_ROUNDING_TO_NEAREST;

#if DECIMAL_GLOBAL_ROUNDING_ACCESS_FUNCTIONS
void
__dfp_set_round (int mode) {
  _IDEC_glbround = mode;
}

int
__dfp_get_round (void) {
  return _IDEC_glbround;
}
#endif
#endif

#if DECIMAL_GLOBAL_EXCEPTION_FLAGS
BID_THREAD _IDEC_flags _IDEC_glbflags = BID_EXACT_STATUS;

#if DECIMAL_GLOBAL_EXCEPTION_FLAGS_ACCESS_FUNCTIONS
#include <fenv.h>

void
__dfp_clear_except (void) {
  _IDEC_glbflags &= ~BID_FLAG_MASK;
}

int
__dfp_test_except (int mask) {
  int flags = 0;

  if ((_IDEC_glbflags & BID_INEXACT_EXCEPTION) != 0)
    flags |= mask & FE_INEXACT;
  if ((_IDEC_glbflags & BID_UNDERFLOW_EXCEPTION) != 0)
    flags |= mask & FE_UNDERFLOW;
  if ((_IDEC_glbflags & BID_OVERFLOW_EXCEPTION) != 0)
    flags |= mask & FE_OVERFLOW;
  if ((_IDEC_glbflags & BID_ZERO_DIVIDE_EXCEPTION) != 0)
    flags |= mask & FE_DIVBYZERO;
  if ((_IDEC_glbflags & BID_INVALID_EXCEPTION) != 0)
    flags |= mask & FE_INVALID;

  return flags;
}

void
__dfp_raise_except (int mask) {
  _IDEC_flags flags = 0;

  if ((mask & FE_INEXACT) != 0)
    flags |= BID_INEXACT_EXCEPTION;
  if ((mask & FE_UNDERFLOW) != 0)
    flags |= BID_UNDERFLOW_EXCEPTION;
  if ((mask & FE_OVERFLOW) != 0)
    flags |= BID_OVERFLOW_EXCEPTION;
  if ((mask & FE_DIVBYZERO) != 0)
    flags |= BID_ZERO_DIVIDE_EXCEPTION;
  if ((mask & FE_INVALID) != 0)
    flags |= BID_INVALID_EXCEPTION;

  _IDEC_glbflags |= flags;
}
#endif
#endif

#if DECIMAL_ALTERNATE_EXCEPTION_HANDLING
#if DECIMAL_GLOBAL_EXCEPTION_MASKS
BID_THREAD _IDEC_exceptionmasks _IDEC_glbexceptionmasks =
  _IDEC_allexcmasksset;
#endif
#if DECIMAL_GLOBAL_EXCEPTION_INFO
BID_THREAD _IDEC_excepthandling _IDEC_glbexcepthandling;
#endif
#endif
