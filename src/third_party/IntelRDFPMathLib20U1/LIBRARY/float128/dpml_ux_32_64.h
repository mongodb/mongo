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

#define AS_DIGIT(p,n)	(((UX_FRACTION_DIGIT_TYPE *)(p))[n])

#if BITS_PER_WORD == 32

/******************************************************************************/
/******************************************************************************/
/*                                                                            */
/*                              32-bit versions                               */
/*                                                                            */
/******************************************************************************/
/******************************************************************************/

#   define DIGIT_FORMAT		" %#8.4.16i,"
#   define ZERO_FORMAT		" 0x00000000,"

#   define COPY_TO_UX_FRACTION(p,q)					\
			(P_UX_FRACTION_DIGIT(q, 0, AS_DIGIT(p,0)),	\
			 P_UX_FRACTION_DIGIT(q, 1, AS_DIGIT(p,1)),	\
			 P_UX_FRACTION_DIGIT(q, 2, AS_DIGIT(p,2)),	\
			 P_UX_FRACTION_DIGIT(q, 3, AS_DIGIT(p,3)))

#   define _X_COPY(p,q)			(P_X_DIGIT(q, 0, G_X_DIGIT(p,0)), \
					 P_X_DIGIT(q, 1, G_X_DIGIT(p,1)), \
					 P_X_DIGIT(q, 2, G_X_DIGIT(p,2)), \
					 P_X_DIGIT(q, 3, G_X_DIGIT(p,3)))

#   define CLR_UX_FRACTION(p)		(P_UX_FRACTION_DIGIT(p, 0, 0),	\
					 P_UX_FRACTION_DIGIT(p, 1, 0),	\
					 P_UX_FRACTION_DIGIT(p, 2, 0),	\
					 P_UX_FRACTION_DIGIT(p, 3, 0))

#   define CLR_UX_LOW_FRACTION(p)	(P_UX_FRACTION_DIGIT(p, 1, 0),	\
					 P_UX_FRACTION_DIGIT(p, 2, 0),	\
					 P_UX_FRACTION_DIGIT(p, 3, 0))

#   define UX_OR_LOW_FRACTION_DIGITS(p)	(G_UX_FRACTION_DIGIT(p, 1) |	\
					 G_UX_FRACTION_DIGIT(p, 2) |	\
					 G_UX_FRACTION_DIGIT(p, 3) )

#   define SET_UX_FRACTION_TO_HALF(p)	(P_UX_FRACTION_DIGIT(p, 0, UX_MSB),\
					 P_UX_FRACTION_DIGIT(p, 1, 0),	\
					 P_UX_FRACTION_DIGIT(p, 2, 0),	\
					 P_UX_FRACTION_DIGIT(p, 3, 0))

#   define OTHER_DIGITS		, _F1, _F2

#   define G_UX_OTHER_DIGITS(p)	\
		( _F1  = G_UX_FRACTION_DIGIT(p,1), \
		  _F2  = G_UX_FRACTION_DIGIT(p,2) )

#   define DIGIT_SHIFT_FRACTION_RIGHT(l,m) \
		( (l) = _F2, \
		  _F2 = _F1, \
		  _F1 = (m), \
		  (m) = 0 ) 	

#   define DIGIT_SHIFT_FRACTION_LEFT(l,m) \
		( (m) = _F1, \
		  _F1 = _F2, \
		  _F2 = (l), \
		  (l) = 0 ) 	

#   define BIT_SHIFT_FRACTION_RIGHT(l,m,s,c)	\
		( (l) = ((l) >> (s)) | (_F1 << (c)), \
		  _F1 = (_F1 >> (s)) | (_F2 << (c)), \
		  _F2 = (_F2 >> (s)) | ((m) << (c)), \
		  (m) >>= (s))

#   define BIT_SHIFT_FRACTION_LEFT(l,m,s,c)	\
		( (m) = ((m) << (s)) | (_F2 >> (c)), \
		  _F2 = (_F2 << (s)) | (_F1 >> (c)), \
		  _F1 = (_F1 << (s)) | ((l) >> (c)), \
		  (l) << (s))

#   define P_UX_OTHER_DIGITS(p) \
		( P_UX_FRACTION_DIGIT(p,1,_F1), \
		  P_UX_FRACTION_DIGIT(p,2,_F2))

#elif BITS_PER_WORD == 64

/******************************************************************************/
/******************************************************************************/
/*                                                                            */
/*                              64-bit versions                               */
/*                                                                            */
/******************************************************************************/
/******************************************************************************/

#   define DIGIT_FORMAT		" %#16.4.16i,"
#   define ZERO_FORMAT		" 0x00000000, 0x00000000,"

#   define COPY_TO_UX_FRACTION(p,q)					\
			P_UX_FRACTION_DIGIT(q, 0, AS_DIGIT(p,0)),	\
			P_UX_FRACTION_DIGIT(q, 1, AS_DIGIT(p,1))

#   define _X_COPY(p,q)			(P_X_DIGIT(q, 0, G_X_DIGIT(p,0)), \
					 P_X_DIGIT(q, 1, G_X_DIGIT(p,1)))

#   define CLR_UX_FRACTION(p)		(P_UX_FRACTION_DIGIT(p, 0, 0),	\
					 P_UX_FRACTION_DIGIT(p, 1, 0))

#   define CLR_UX_LOW_FRACTION(p)	P_UX_FRACTION_DIGIT(p, 1, 0)

#   define UX_OR_LOW_FRACTION_DIGITS(p)	( G_UX_FRACTION_DIGIT(p, 1) )

#   define SET_UX_FRACTION_TO_HALF(p)	(P_UX_FRACTION_DIGIT(p, 0, UX_MSB),\
					 P_UX_FRACTION_DIGIT(p, 1, 0))

#   define OTHER_DIGITS
#   define G_UX_OTHER_DIGITS(p)	
#   define DIGIT_SHIFT_FRACTION_RIGHT(l,m)	( (l) = (m), (m) = 0 )
#   define DIGIT_SHIFT_FRACTION_LEFT(l,m)	( (m) = (l), (l) = 0 )
#   define BIT_SHIFT_FRACTION_RIGHT(l,m,s,c)	\
					( (l) = ((l) >> (s)) | ((m) << (c)), \
					  (m) >>= (s) )
#   define BIT_SHIFT_FRACTION_LEFT(l,m,s,c)	\
					( (m) = ((m) << (s)) | ((l) >> (c)), \
					  (l) <<= (s) )
#   define PROPAGATE_CARRY(c)		
#   define P_UX_OTHER_DIGITS(p)

#else

#    error "Unsupported WORD size"

#endif

