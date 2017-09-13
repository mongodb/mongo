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

#include "dpml_private.h"
#include "dpml_special_exp.h"


#ifndef BASE_NAME
#    define BASE_NAME   TGAMMA_BASE_NAME
#endif

#if !defined F_ENTRY_NAME
#   define F_ENTRY_NAME      F_TGAMMA_NAME
#endif

#if USE_BACKUP 
#   define LO_PART_DECL
#else
#   define LO_PART           pow2_low 
#   define LO_PART_DECL      , F_TYPE *LO_PART 
#endif 

extern F_TYPE F_RT_LGAMMA_NAME(F_TYPE , int *);
extern B_TYPE F_EXP_SPECIAL_ENTRY_NAME ( F_TYPE , WORD *  LO_PART_DECL );
extern F_TYPE F_LDEXP_NAME( F_TYPE, int );

static const U_INT_64 Inf = 0x7ff0000000000000;

#define INF	(((D_UNION *) &Inf)->f)


F_TYPE
F_ENTRY_NAME(F_TYPE x) {

    EXCEPTION_RECORD_DECLARATION

    F_TYPE y;
    F_TYPE mantissa_lo;
    WORD pow_of_two, bExp, j;
    int signgam = 0;
    F_UNION u;

    u.f = x;
    j = u.F_HI_WORD;
    if ( j & F_SIGN_BIT_MASK) {
        // In put is negative
        if ( (j & F_EXP_MASK) >= 
                (((WORD) (F_EXP_BIAS + F_PRECISION)) << F_EXP_POS)) {
            // Argument is a negitive integer
            return INF;
         }
    } else if ( x > 171.6243769563027208124443787857704267196259) {
        // Large positive value is garanteed to overflow
        return INF;
    } else if ( x != x ) {
        return (x);
    }

    // Normal argument, may or may not overflow
    u.f = F_RT_LGAMMA_NAME(x, &signgam);
    j = u.F_HI_WORD;
    bExp = j & F_EXP_MASK;
    if ( bExp == F_EXP_MASK ) {
        // Overflow or invalid
        return (signgam < 0) ? -u.f : u.f;
    } else if ( u.f < -750 ) {
        // Certain Underflow
        return (F_TYPE) 0.0;
    } else if ( u.f > 710 ) {
        // Certain overflow
        return signgam < 0 ? -INF : INF;
    } else {
         y = F_EXP_SPECIAL_ENTRY_NAME(u.f, &pow_of_two, &mantissa_lo );
         y += mantissa_lo;
         if ( signgam < 0 )
             y = -y;
         return F_LDEXP_NAME( y, pow_of_two >> POW2_K );
    }
}
