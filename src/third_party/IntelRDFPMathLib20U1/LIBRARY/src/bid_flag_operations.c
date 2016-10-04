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

/*****************************************************************************
 *    Non-computational Operations on Flags:
 ****************************************************************************/

#include "bid_internal.h"

// Note the following definitions from bid_conf.h: if the status flags are
// global, they have a fixed name recognized by the library functions:
// _IDEC_glbflags; pfpsf, defined as &_IDEC_glbflags, can be used instead; no
// argument is passed for the status flags to the library functions; if the 
// status flags are local then they are passed as an arument, always by
// reference, to the library functions
//
// #if !DECIMAL_GLOBAL_EXCEPTION_FLAGS
//   #define _EXC_FLAGS_PARAM , _IDEC_flags *pfpsf
// #else
//   BID_EXTERN_C _IDEC_flags _IDEC_glbflags;
//   #define _EXC_FLAGS_PARAM
//   #define pfpsf &_IDEC_glbflags
// #endif

#if DECIMAL_CALL_BY_REFERENCE
void
bid_signalException (_IDEC_flags * pflagsmask _EXC_FLAGS_PARAM) {
  // *pflagsmask is the logical OR of the flags to be set, e.g.
  // *pflagsmask =BID_INVALID_EXCEPTION | BID_ZERO_DIVIDE_EXCEPTION | BID_OVERFLOW_EXCEPTION
  // BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION to set all five IEEE 754
  // exception flags
  *pfpsf = *pfpsf | (*pflagsmask & BID_IEEE_FLAGS);
}
#else
void
bid_signalException (_IDEC_flags flagsmask _EXC_FLAGS_PARAM) {
  // flagsmask is the logical OR of the flags to be set, e.g.
  // flagsmask = BID_INVALID_EXCEPTION | BID_ZERO_DIVIDE_EXCEPTION | BID_OVERFLOW_EXCEPTION
  // BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION to set all five IEEE 754
  // exception flags
  *pfpsf = *pfpsf | (flagsmask & BID_IEEE_FLAGS);
}
#endif

#if DECIMAL_CALL_BY_REFERENCE
void
bid_lowerFlags (_IDEC_flags * pflagsmask _EXC_FLAGS_PARAM) {
  // *pflagsmask is the logical OR of the flags to be cleared, e.g.
  // *pflagsmask =BID_INVALID_EXCEPTION | BID_ZERO_DIVIDE_EXCEPTION | BID_OVERFLOW_EXCEPTION
  // BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION to clear all five IEEE 754 
  // exception flags
  *pfpsf = *pfpsf & ~(*pflagsmask & BID_IEEE_FLAGS);
}
#else
void
bid_lowerFlags (_IDEC_flags flagsmask _EXC_FLAGS_PARAM) {
  // flagsmask is the logical OR of the flags to be cleared, e.g.
  // flagsmask = BID_INVALID_EXCEPTION | BID_ZERO_DIVIDE_EXCEPTION | BID_OVERFLOW_EXCEPTION 
  // BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION to clear all five IEEE 754    
  // exception flags
  *pfpsf = *pfpsf & ~(flagsmask & BID_IEEE_FLAGS);
}
#endif

#if DECIMAL_CALL_BY_REFERENCE
void
bid_testFlags (_IDEC_flags * praised,
	   _IDEC_flags * pflagsmask _EXC_FLAGS_PARAM) {
  // *praised is a pointer to the result, i.e. the logical OR of the flags 
  // selected by *pflagsmask that are set; e.g. if
  // *pflagsmask = BID_INVALID_EXCEPTION | BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION
  // and only the invalid and inexact flags are raised (set) then upon return 
  // *praised = BID_INVALID_EXCEPTION | BID_INEXACT_EXCEPTION
  *praised = *pfpsf & (*pflagsmask & BID_IEEE_FLAGS);
}
#else
_IDEC_flags
bid_testFlags (_IDEC_flags flagsmask _EXC_FLAGS_PARAM) {
  _IDEC_flags raised;
  // the raturn value raised is the logical OR of the flags  
  // selected by flagsmask, that are set; e.g. if
  // flagsmask = BID_INVALID_EXCEPTION | BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION and
  // only the invalid and inexact flags are raised (set) then the return value
  // is raised = BID_INVALID_EXCEPTION | BID_INEXACT_EXCEPTION
  raised = *pfpsf & (flagsmask & BID_IEEE_FLAGS);
  return (raised);
}
#endif

#if DECIMAL_CALL_BY_REFERENCE
void
bid_testSavedFlags (_IDEC_flags * praised, _IDEC_flags * psavedflags,
		_IDEC_flags * pflagsmask) {
  // *praised is a pointer to the result, i.e. the logical OR of the flags
  // selected by *pflagsmask that are set in *psavedflags; e.g. if
  // *pflagsmask = BID_INVALID_EXCEPTION | BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION
  // and only the invalid and inexact flags are raised (set) in *psavedflags
  // then upon return *praised = BID_INVALID_EXCEPTION | BID_INEXACT_EXCEPTION
  // Note that the flags could be saved in a global variable, but this function
  // would still expect that value as an argument passed by reference
  *praised = *psavedflags & (*pflagsmask & BID_IEEE_FLAGS);
}
#else
_IDEC_flags
bid_testSavedFlags (_IDEC_flags savedflags, _IDEC_flags flagsmask) {
  _IDEC_flags raised;
  // the raturn value raised is the logical OR of the flags
  // selected by flagsmask, that are set in savedflags; e.g. if
  // flagsmask = BID_INVALID_EXCEPTION | BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION and
  // only the invalid and inexact flags are raised (set) in savedflags
  // then the return value is raised = BID_INVALID_EXCEPTION | BID_INEXACT_EXCEPTION
  // Note that the flags could be saved in a global variable, but this function
  // would still expect that value as an argument passed by value
  raised = savedflags & (flagsmask & BID_IEEE_FLAGS);
  return (raised);
}
#endif

#if DECIMAL_CALL_BY_REFERENCE
void
bid_restoreFlags (_IDEC_flags * pflagsvalues,
	      _IDEC_flags * pflagsmask _EXC_FLAGS_PARAM) {
  // restore the status flags selected by *pflagsmask to the values speciafied
  // (as a logical OR) in *pflagsvalues; e.g. if
  // *pflagsmask = BID_INVALID_EXCEPTION | BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION
  // and only the invalid and inexact flags are raised (set) in *pflagsvalues
  // then upon return the invalid status flag will be set, the underflow status
  // flag will be clear, and the inexact status flag will be set
  *pfpsf = *pfpsf & ~(*pflagsmask & BID_IEEE_FLAGS);
  // clear flags that have to be restored
  *pfpsf = *pfpsf | (*pflagsvalues & (*pflagsmask & BID_IEEE_FLAGS));
  // restore flags
}
#else
void
bid_restoreFlags (_IDEC_flags flagsvalues,
	      _IDEC_flags flagsmask _EXC_FLAGS_PARAM) {
  // restore the status flags selected by flagsmask to the values speciafied
  // (as a logical OR) in flagsvalues; e.g. if 
  // flagsmask = BID_INVALID_EXCEPTION | BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION
  // and only the invalid and inexact flags are raised (set) in flagsvalues 
  // then upon return the invalid status flag will be set, the underflow status
  // flag will be clear, and the inexact status flag will be set
  *pfpsf = *pfpsf & ~(flagsmask & BID_IEEE_FLAGS);
  // clear flags that have to be restored
  *pfpsf = *pfpsf | (flagsvalues & (flagsmask & BID_IEEE_FLAGS));
  // restore flags
}
#endif

#if DECIMAL_CALL_BY_REFERENCE
void
bid_saveFlags (_IDEC_flags * pflagsvalues,
	   _IDEC_flags * pflagsmask _EXC_FLAGS_PARAM) {
  // return in *pflagsvalues the status flags specified (as a logical OR) in
  // *pflagsmask; e.g. if
  // *pflagsmask = BID_INVALID_EXCEPTION | BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION
  // and only the invalid and inexact flags are raised (set) in the status word,
  // then upon return the value in *pflagsvalues will have the invalid status 
  // flag set, the underflow status flag clear, and the inexact status flag set
  *pflagsvalues = *pfpsf & (*pflagsmask & BID_IEEE_FLAGS);
}
#else
_IDEC_flags
bid_saveFlags (_IDEC_flags flagsmask _EXC_FLAGS_PARAM) {
  _IDEC_flags flagsvalues;
  // return the status flags specified (as a logical OR) in flagsmask; e.g. if 
  // flagsmask = BID_INVALID_EXCEPTION | BID_UNDERFLOW_EXCEPTION | BID_INEXACT_EXCEPTION
  // and only the invalid and inexact flags are raised (set) in the status word,
  // then the return value will have the invalid status  flag set, the 
  // underflow status flag clear, and the inexact status flag set 
  flagsvalues = *pfpsf & (flagsmask & BID_IEEE_FLAGS);
  return (flagsvalues);
}
#endif

// Note the following definitions from bid_conf.h (rearranged): if the rounding
// mode is global, it has a fixed name recognized by the library functions:
// _IDEC_glbround; rnd_mode, defined as &_IDEC_glbround, can be used instead; no
// argument is passed for the rounding mode to the library functions; if the
// rounding mode is local then it is passed as an arument, by reference or by
// value, to the library functions
//
// #if DECIMAL_CALL_BY_REFERENCE
//   #if !DECIMAL_GLOBAL_ROUNDING
//     #define _RND_MODE_PARAM , _IDEC_round *prnd_mode
//   #else
//     #define _RND_MODE_PARAM
//     #define rnd_mode _IDEC_glbround
//   #endif
// #else
//   #if !DECIMAL_GLOBAL_ROUNDING
//     #define _RND_MODE_PARAM , _IDEC_round rnd_mode
//   #else
//     #define _RND_MODE_PARAM
//     #define rnd_mode _IDEC_glbround
//   #endif
// #endif

#if DECIMAL_CALL_BY_REFERENCE
#if !DECIMAL_GLOBAL_ROUNDING
    // #define _RND_MODE_PARAM , _IDEC_round *prnd_mode
void
bid_getDecimalRoundingDirection (_IDEC_round * rounding_mode
			     _RND_MODE_PARAM) {
  // returns the current rounding mode
  *rounding_mode = *prnd_mode;
}
#else
    // #define _RND_MODE_PARAM
    // #define rnd_mode _IDEC_glbround
void
bid_getDecimalRoundingDirection (_IDEC_round * rounding_mode
			     _RND_MODE_PARAM) {
  // returns the current rounding mode
  *rounding_mode = rnd_mode;
}
#endif
#else
#if !DECIMAL_GLOBAL_ROUNDING
    // #define _RND_MODE_PARAM , _IDEC_round rnd_mode
_IDEC_round
bid_getDecimalRoundingDirection (_IDEC_round rnd_mode) {
  // returns the current rounding mode
  return (rnd_mode);
}
#else
    // #define _RND_MODE_PARAM
    // #define rnd_mode _IDEC_glbround
_IDEC_round
bid_getDecimalRoundingDirection (void) {
  // returns the current rounding mode
  return (rnd_mode);
}
#endif
#endif

#if DECIMAL_CALL_BY_REFERENCE
#if !DECIMAL_GLOBAL_ROUNDING
    // #define _RND_MODE_PARAM , _IDEC_round *prnd_mode
void
bid_setDecimalRoundingDirection (_IDEC_round * rounding_mode
			     _RND_MODE_PARAM) {
  // sets the current rounding mode to the value in *rounding_mode, if valid
  if (*rounding_mode == BID_ROUNDING_TO_NEAREST ||
      *rounding_mode == BID_ROUNDING_DOWN ||
      *rounding_mode == BID_ROUNDING_UP ||
      *rounding_mode == BID_ROUNDING_TO_ZERO ||
      *rounding_mode == BID_ROUNDING_TIES_AWAY) {
    *prnd_mode = *rounding_mode;
  }
}
#else
    // #define _RND_MODE_PARAM
    // #define rnd_mode _IDEC_glbround
void
bid_setDecimalRoundingDirection (_IDEC_round * rounding_mode
			     ) {
  // sets the global rounding mode to the value in *rounding_mode, if valid
  if (*rounding_mode == BID_ROUNDING_TO_NEAREST ||
      *rounding_mode == BID_ROUNDING_DOWN ||
      *rounding_mode == BID_ROUNDING_UP ||
      *rounding_mode == BID_ROUNDING_TO_ZERO ||
      *rounding_mode == BID_ROUNDING_TIES_AWAY) {
    rnd_mode = *rounding_mode;
  }
}
#endif
#else
#if !DECIMAL_GLOBAL_ROUNDING
    // #define _RND_MODE_PARAM , _IDEC_round rnd_mode
_IDEC_round
bid_setDecimalRoundingDirection (_IDEC_round rounding_mode _RND_MODE_PARAM) {
  // sets the current rounding mode to the value in rounding_mode;
  // however, when arguments are passed by value and the rounding mode
  // is a local variable, this is not of any use
  if (rounding_mode == BID_ROUNDING_TO_NEAREST ||
      rounding_mode == BID_ROUNDING_DOWN ||
      rounding_mode == BID_ROUNDING_UP ||
      rounding_mode == BID_ROUNDING_TO_ZERO ||
      rounding_mode == BID_ROUNDING_TIES_AWAY) {
    return (rounding_mode);
  }
  return (rnd_mode);
}
#else
    // #define _RND_MODE_PARAM
    // #define rnd_mode _IDEC_glbround
void
bid_setDecimalRoundingDirection (_IDEC_round rounding_mode) {
  // sets the current rounding mode to the value in rounding_mode, if valid;
  if (rounding_mode == BID_ROUNDING_TO_NEAREST ||
      rounding_mode == BID_ROUNDING_DOWN ||
      rounding_mode == BID_ROUNDING_UP ||
      rounding_mode == BID_ROUNDING_TO_ZERO ||
      rounding_mode == BID_ROUNDING_TIES_AWAY) {
    rnd_mode = rounding_mode;
  }
}
#endif
#endif

#if DECIMAL_CALL_BY_REFERENCE
void
bid_is754 (int *retval) {
  *retval = 0;
}
#else
int
bid_is754 (void) {
  return 0;
}
#endif

#if DECIMAL_CALL_BY_REFERENCE
void
bid_is754R (int *retval) {
  *retval = 1;
}
#else
int
bid_is754R (void) {
  return 1;
}
#endif


#ifdef BID_MS_FLAGS

#include <float.h>

extern unsigned int __bid_flag_mask;

unsigned int __bid_ms_restore_flags(unsigned int* pflags)
{
unsigned int crt_flags, n=0;
int_float tmp;


    crt_flags = _statusfp();

	if(crt_flags != *pflags)
	{
          _clearfp();

		  if(crt_flags & _SW_INEXACT)
		  {
			  tmp.i = 0x3f800001;
			  tmp.d *= tmp.d;
			  n |= tmp.i;
		  }
		  if(crt_flags & _SW_UNDERFLOW)
		  {
			  tmp.i = 0x00800001;
			  tmp.d *= tmp.d;
			  n |= tmp.i;
		  }
		  if(crt_flags & _SW_OVERFLOW)
		  {
			  tmp.i = 0x7f000001;
			  tmp.d *= tmp.d;
			  n |= tmp.i;
		  }
		  if(crt_flags & _SW_ZERODIVIDE)
		  {
			  tmp.i = 0x80000000;
			  tmp.d = 1.0/tmp.d;
			  n |= tmp.i;
		  }
		  if(crt_flags & _SW_INVALID)
		  {
			  tmp.i = 0x80000000;
			  tmp.d /= tmp.d;
			  n |= tmp.i;
		  }

		  if(crt_flags & _SW_DENORMAL)
		  {
			  tmp.i = 0x80000001;
			  tmp.d = 1.0+tmp.d;
			  n |= tmp.i;
		  }
	}

    n &= __bid_flag_mask;

	return n;
}

#endif
