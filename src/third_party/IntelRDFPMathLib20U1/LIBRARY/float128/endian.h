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

#include"architecture.h"

#ifndef ENDIAN_H
#define ENDIAN_H

#define big_endian	1
#define little_endian	0

#ifndef ENDIANESS
#   error  ENDIANESS is not defined
#else
#   if (ENDIANESS == big_endian)
#       define DATA_1x2(a,b)              b, a
#       define DATA_2x2(a,b,c,d)          b, a, d, c
#       define DATA_4(a,b,c,d)            b, a, d, c
#       define DATA_4R(a,b,c,d)           d, c, b, a
#       define DATA_3x2(a,b,c,d,e,f)      b, a, d, c, f, e
#       define DATA_4x2(a,b,c,d,e,f,g,h)  b, a, d, c, f, e, h, g
#   else
#       define DATA_1x2(a,b)              a, b
#       define DATA_2x2(a,b,c,d)          a, b, c, d
#       define DATA_4(a,b,c,d)            a, b, c, d
#       define DATA_4R(a,b,c,d)           a, b, c, d
#       define DATA_3x2(a,b,c,d,e,f)      a, b, c, d, e, f
#       define DATA_4x2(a,b,c,d,e,f,g,h)  a, b, c, d, e, f, g, h
#   endif
#endif

#endif  /* ENDIAN_H */

