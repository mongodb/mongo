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

#ifndef MTC_MACROS_H
#define MTC_MACROS_H



#if   (F_FORMAT == f_floating)

#	define MTC_NAN          f:00008000
#	define MTC_POS_ZERO     f:00000000
#	define MTC_NEG_ZERO     f:00000000
#	define MTC_POS_TINY     f:00000080
#	define MTC_NEG_TINY     f:00008080
#	define MTC_POS_HUGE     f:ffff7fff
#	define MTC_NEG_HUGE     f:ffffffff
#	define MTC_POS_INFINITY f:ffff7fff
#	define MTC_NEG_INFINITY f:ffffffff

#elif (F_FORMAT == d_floating)

#	define MTC_NAN          d:0000000000008000
#	define MTC_POS_ZERO     d:0000000000000000
#	define MTC_NEG_ZERO     d:0000000000000000
#	define MTC_POS_TINY     d:0000000000000080
#	define MTC_NEG_TINY     d:0000000000008080
#	define MTC_POS_HUGE     d:ffffffffffff7fff
#	define MTC_NEG_HUGE     d:ffffffffffffffff
#	define MTC_POS_INFINITY d:ffffffffffff7fff
#	define MTC_NEG_INFINITY d:ffffffffffffffff

#elif (F_FORMAT == g_floating)

#	define MTC_NAN          g:0000000000008000
#	define MTC_POS_ZERO     g:0000000000000000
#	define MTC_NEG_ZERO     g:0000000000000000
#	define MTC_POS_TINY     g:0000000000000010
#	define MTC_NEG_TINY     g:0000000000008010
#	define MTC_POS_HUGE     g:ffffffffffff7fff
#	define MTC_NEG_HUGE     g:ffffffffffffffff
#	define MTC_POS_INFINITY g:ffffffffffff7fff
#	define MTC_NEG_INFINITY g:ffffffffffffffff

#elif (F_FORMAT == h_floating)

#	error H_floating not supported.

#elif (F_FORMAT == s_floating)

#	define MTC_NAN          s:7fbfffff
#	define MTC_POS_ZERO     s:00000000
#	define MTC_NEG_ZERO     s:80000000
#	define MTC_POS_TINY     s:00000001
#	define MTC_NEG_TINY     s:80000001
#	define MTC_POS_HUGE     s:7f7fffff
#	define MTC_NEG_HUGE     s:ff7fffff
#	define MTC_POS_INFINITY s:7f800000
#	define MTC_NEG_INFINITY s:ff800000


#elif (F_FORMAT == t_floating)

#	define MTC_NAN          t:7ff7ffffffffffff
#	define MTC_POS_ZERO     t:0000000000000000
#	define MTC_NEG_ZERO     t:8000000000000000
#	define MTC_POS_TINY     t:0000000000000001
#	define MTC_NEG_TINY     t:8000000000000001
#	define MTC_POS_HUGE     t:7fefffffffffffff
#	define MTC_NEG_HUGE     t:ffefffffffffffff
#	define MTC_POS_INFINITY t:7ff0000000000000
#	define MTC_NEG_INFINITY t:fff0000000000000

#elif (F_FORMAT == x_floating)

#	define MTC_NAN          x:7fff7fffffffffffffffffffffffffff
#	define MTC_POS_ZERO     x:00000000000000000000000000000000
#	define MTC_NEG_ZERO     x:80000000000000000000000000000000
#	define MTC_POS_TINY     x:00000000000000000000000000000001
#	define MTC_NEG_TINY     x:80000000000000000000000000000001 
#	define MTC_POS_HUGE     x:7ffeffffffffffffffffffffffffffff
#	define MTC_NEG_HUGE     x:fffeffffffffffffffffffffffffffff
#	define MTC_POS_INFINITY x:7fff0000000000000000000000000000
#	define MTC_NEG_INFINITY x:ffff0000000000000000000000000000

#else

#	error Unsupported floating format.

#endif




#define MTC_POS_PI	 3.1415926535897932384626433832795028841972
#define MTC_NEG_PI	-3.1415926535897932384626433832795028841972

#define	MTC_POS_PI_OVER_2	 1.5707963267948966192313216916397514420986
#define	MTC_NEG_PI_OVER_2	-1.5707963267948966192313216916397514420986




#endif  /* MTC_MACROS_H */

