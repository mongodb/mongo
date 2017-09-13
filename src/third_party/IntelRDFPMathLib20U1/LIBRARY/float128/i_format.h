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

#ifndef I_FORMAT_H
#define I_FORMAT_H


#ifndef I_TYPE


#	if defined(S8_INT)

#		define	I_TYPE	INT_8
#		define	I_CHAR	b
#		define	I_TYPE_SIGNED	1

#	elif defined(S16_INT)

#		define	I_TYPE	INT_16
#		define	I_CHAR	w
#		define	I_TYPE_SIGNED	1

#	elif defined(S32_INT)

#		define	I_TYPE	INT_32
#		define	I_CHAR	l
#		define	I_TYPE_SIGNED	1

#	elif defined(S64_INT)

#		define	I_TYPE	INT_64
#		define	I_CHAR	q
#		define	I_TYPE_SIGNED	1

#	elif defined(U8_INT)

#		define	I_TYPE	U_INT_8
#		define	I_CHAR	ub
#		undef	I_TYPE_SIGNED

#	elif defined(U16_INT)

#		define	I_TYPE	U_INT_16
#		define	I_CHAR	uw
#		undef	I_TYPE_SIGNED

#	elif defined(U32_INT)

#		define	I_TYPE	U_INT_32
#		define	I_CHAR	ul
#		undef	I_TYPE_SIGNED

#	elif defined(U64_INT)

#		define	I_TYPE	U_INT_64
#		define	I_CHAR	uq
#		undef	I_TYPE_SIGNED

#	else

#               if (DECC || __decc || dec_cc)
#	        	define	I_TYPE	INT_64
#               else
#		        define	I_TYPE	WORD
#               endif

#		define	I_CHAR	l
#		define	I_TYPE_SIGNED	1

#	endif


#endif  /* ndef I_TYPE */


#endif  /* I_FORMAT_H */

