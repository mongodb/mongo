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

#ifndef ARCHITECTURE_H
#define ARCHITECTURE_H

/*
** for historic reasons, map ia64 architecture to merced and ct architecture to amd64 
*/

#if (defined(ia64) || defined(__ia64) || defined(__ia64__))  && !defined(HPUX_OS)
#   undef  merced
#   define merced
#endif

#if defined(ct) || defined(efi2)
#   undef  _M_AMD64
#   define _M_AMD64
#endif

#if defined(HPUX_OS)
#define sparc
#endif

#if (defined(vax) || defined(VAX))

#	undef  vax
#	undef  mips
#	undef  hp_pa
#	undef  cray
#	undef  alpha
#	undef  ix86
#       undef  merced
#       undef  amd64
#	undef  sparc

#	define vax 1
#	define ARCHITECTURE vax

#	undef  LOCAL_DATA
#	undef  STATIC_ROUNDING_MODES
#	undef  DYNAMIC_ROUNDING_MODES
#	undef  DENORMS_EMULATED
#	undef  SEPARATE_FLOAT_REGS
#	undef  MULTIPLE_ISSUE
#	undef  UNSIGNED_TO_FLOAT
#	undef  UNSIGNED_MULTIPLY
#	define ENDIANESS little_endian
#	define SCALE_METHOD by_int
#	define CVT_TO_HI_LO_METHOD by_int

#	define BITS_PER_CHAR    8
#	define BITS_PER_SHORT  16
#	define BITS_PER_INT    32
#	define BITS_PER_LONG   32

#	define BITS_PER_FLOAT  32
#	define BITS_PER_DOUBLE 64
#	define BITS_PER_LONG_DOUBLE 128

#	define INT_8  signed char
#	define INT_16 signed short
#	define INT_32 signed int
#	undef  INT_64
#	undef  INT_128
#	define U_INT_8  unsigned char
#	define U_INT_16 unsigned short
#	define U_INT_32 unsigned int
#	undef  U_INT_64
#	undef  U_INT_128

#	define WORD      INT_32
#	define U_WORD  U_INT_32
#	define BITS_PER_WORD 32

#	define HALF_WORD      INT_16
#	define U_HALF_WORD  U_INT_16
#	define BITS_PER_HALF_WORD 16



#elif (defined(mips) || defined(MIPS))

#	undef  vax
#	undef  mips
#	undef  hp_pa
#	undef  cray
#	undef  alpha
#	undef  ix86
#       undef  merced
#       undef  amd64
#	undef  sparc

#	define mips 2
#	define ARCHITECTURE mips

#	define LOCAL_DATA 1
#	undef  STATIC_ROUNDING_MODES
#	define DYNAMIC_ROUNDING_MODES 1
#	define DENORMS_EMULATED 1
#	define SEPARATE_FLOAT_REGS 1
#	undef  MULTIPLE_ISSUE
#	undef  UNSIGNED_TO_FLOAT
#	define UNSIGNED_MULTIPLY 1
#	define ENDIANESS little_endian
#	define SCALE_METHOD by_int
#	define CVT_TO_HI_LO_METHOD by_flt

#	define BITS_PER_CHAR    8
#	define BITS_PER_SHORT  16
#	define BITS_PER_INT    32
#	define BITS_PER_LONG   32

#	define BITS_PER_FLOAT  32
#	define BITS_PER_DOUBLE 64
#	define BITS_PER_LONG_DOUBLE 64

#	define INT_8  signed char
#	define INT_16 signed short
#	define INT_32 signed int
#	undef  INT_64
#	undef  INT_128
#	define U_INT_8  unsigned char
#	define U_INT_16 unsigned short
#	define U_INT_32 unsigned int
#	undef  U_INT_64
#	undef  U_INT_128

#	define WORD      INT_32
#	define U_WORD  U_INT_32
#	define BITS_PER_WORD 32

#	define HALF_WORD      INT_16
#	define U_HALF_WORD  U_INT_16
#	define BITS_PER_HALF_WORD 16



#elif (defined(hp_pa) || defined(HP_PA) || defined(__hppa) || defined(__HPPA))

#	undef  vax
#	undef  mips
#	undef  hp_pa
#	undef  cray
#	undef  alpha
#	undef  ix86
#       undef  merced
#       undef  amd64
#	undef  sparc

#	define hp_pa 3
#	define ARCHITECTURE hp_pa

#	define LOCAL_DATA 1
#	undef  STATIC_ROUNDING_MODES
#	define DYNAMIC_ROUNDING_MODES 1
#	define DENORMS_EMULATED 1
#	define SEPARATE_FLOAT_REGS 1
#	undef  MULTIPLE_ISSUE
#	undef  UNSIGNED_TO_FLOAT
#	define UNSIGNED_MULTIPLY 1
#	define ENDIANESS big_endian
#	define SCALE_METHOD by_int
#	define CVT_TO_HI_LO_METHOD by_flt

#	define BITS_PER_CHAR    8
#	define BITS_PER_SHORT  16
#	define BITS_PER_INT    32
#	define BITS_PER_LONG   32

#	define BITS_PER_FLOAT  32
#	define BITS_PER_DOUBLE 64
#	define BITS_PER_LONG_DOUBLE 128

#	define INT_8  signed char
#	define INT_16 signed short
#	define INT_32 signed int
#	undef  INT_64
#	undef  INT_128
#	define U_INT_8  unsigned char
#	define U_INT_16 unsigned short
#	define U_INT_32 unsigned int
#	undef  U_INT_64
#	undef  U_INT_128

#	define WORD      INT_32
#	define U_WORD  U_INT_32
#	define BITS_PER_WORD 32

#	define HALF_WORD      INT_16
#	define U_HALF_WORD  U_INT_16
#	define BITS_PER_HALF_WORD 16



#elif (defined(cray) || defined(CRAY))

#	undef  vax
#	undef  mips
#	undef  hp_pa
#	undef  cray
#	undef  alpha
#	undef  ix86
#       undef  merced
#       undef  amd64
#	undef  sparc

#	define cray 4
#	define ARCHITECTURE cray

#	undef  LOCAL_DATA 1
#	undef  STATIC_ROUNDING_MODES
#	undef  DYNAMIC_ROUNDING_MODES
#	define DENORMS_EMULATED ???
#	define SEPARATE_FLOAT_REGS 1
#	undef  MULTIPLE_ISSUE
#	undef  UNSIGNED_TO_FLOAT
#	define UNSIGNED_MULTIPLY 1
#	define ENDIANESS big_endian
#	define SCALE_METHOD by_int
#	define CVT_TO_HI_LO_METHOD by_int

#	define BITS_PER_CHAR    8
#	define BITS_PER_SHORT  16
#	define BITS_PER_INT    32
#	define BITS_PER_LONG   64

#	define BITS_PER_FLOAT  64
#	define BITS_PER_DOUBLE 64
#	define BITS_PER_LONG_DOUBLE 128

#	define INT_8  signed char
#	define INT_16 signed short
#	define INT_32 signed int
#	define INT_64 signed long
#	undef  INT_128
#	define U_INT_8  unsigned char
#	define U_INT_16 unsigned short
#	define U_INT_32 unsigned int
#	define U_INT_64 unsigned long
#	undef  U_INT_128

#	define WORD      INT_32
#	define U_WORD  U_INT_32
#	define BITS_PER_WORD 32

#	define HALF_WORD      INT_16
#	define U_HALF_WORD  U_INT_16
#	define BITS_PER_HALF_WORD 16



#elif ( defined(alpha) || defined(ALPHA) \
	|| defined(__alpha) || defined(__ALPHA) \
	|| defined(_ALPHA_) || defined(__Alpha_AXP) )

#	undef  vax
#	undef  mips
#	undef  hp_pa
#	undef  cray
#	undef  alpha
#	undef  ix86
#       undef  merced
#       undef  amd64
#	undef  sparc

#	define alpha 5
#	define ARCHITECTURE alpha

#	define LOCAL_DATA 1
#	define STATIC_ROUNDING_MODES 1
#	undef  DYNAMIC_ROUNDING_MODES
#	define DENORMS_EMULATED 1
#	define SEPARATE_FLOAT_REGS 1
#	define MULTIPLE_ISSUE 1
#	undef  UNSIGNED_TO_FLOAT
#	define UNSIGNED_MULTIPLY 1
#	define ENDIANESS little_endian
#	define SCALE_METHOD by_int
#	define CVT_TO_HI_LO_METHOD by_flt

#	define BITS_PER_FLOAT  32
#	define BITS_PER_DOUBLE 64
#	define BITS_PER_LONG_DOUBLE 128

#	define BITS_PER_CHAR    8
#	define BITS_PER_SHORT  16
#	define BITS_PER_INT    32
#	if defined(__32BITS)
#		define BITS_PER_LONG   32
#	else
#		define BITS_PER_LONG   64
#	endif

#	define INT_8  signed char
#	define INT_16 signed short
#	define INT_32 signed int
#	undef  INT_128

#	define U_INT_8  unsigned char
#	define U_INT_16 unsigned short
#	define U_INT_32 unsigned int
#	undef  U_INT_128

#	if ((OP_SYSTEM == osf) || (OP_SYSTEM == linux)) 

#		define INT_64 signed long
#		define U_INT_64 unsigned long

#	else

#		define INT_64 signed __int64
#		define U_INT_64 unsigned __int64

#	endif

#	if defined(__32BITS)

#		define WORD      INT_32
#		define U_WORD  U_INT_32
#		define BITS_PER_WORD 32

#		define HALF_WORD      INT_16
#		define U_HALF_WORD  U_INT_16
#		define BITS_PER_HALF_WORD 16

#	else

#		define WORD      INT_64
#		define U_WORD  U_INT_64
#		define BITS_PER_WORD 64

#		define HALF_WORD      INT_32
#		define U_HALF_WORD  U_INT_32
#		define BITS_PER_HALF_WORD 32

#	endif



#elif (defined(_M_IX86) || defined(ix86) || defined(IX86) || defined(ia32) )

#	undef  vax
#	undef  mips
#	undef  hp_pa
#	undef  cray
#	undef  alpha
#	undef  ix86
#       undef  merced
#       undef  amd64
#	undef  sparc

#	define ix86 6
#	define ARCHITECTURE ix86

#	define LOCAL_DATA 1
#	undef  STATIC_ROUNDING_MODES
#	define DYNAMIC_ROUNDING_MODES 1
#	define DENORMS_EMULATED 1
#	define SEPARATE_FLOAT_REGS 1
#	undef  MULTIPLE_ISSUE
#	undef  UNSIGNED_TO_FLOAT
#	define UNSIGNED_MULTIPLY 1
#	define ENDIANESS little_endian
#	define SCALE_METHOD by_int
#	define CVT_TO_HI_LO_METHOD by_flt

#	define BITS_PER_CHAR    8
#	define BITS_PER_SHORT  16
#	define BITS_PER_INT    32
#	define BITS_PER_LONG   32

#	define BITS_PER_FLOAT  32
#	define BITS_PER_DOUBLE 64
#       if !defined(LONG_DOUBLE_128)
#	    define BITS_PER_LONG_DOUBLE 80
#       else
#	    define BITS_PER_LONG_DOUBLE 128
#           define LONG_DOUBLE_128_TYPE	_Quad
#       endif

#	define INT_8  signed char
#	define INT_16 signed short
#	define INT_32 signed int
#	define INT_64 long long
#	undef  INT_128
#	define U_INT_8  unsigned char
#	define U_INT_16 unsigned short
#	define U_INT_32 unsigned int
#	define U_INT_64 unsigned long long
#	undef  U_INT_128

#       if 0
#	    define WORD      INT_32
#	    define U_WORD  U_INT_32
#	    define BITS_PER_WORD 32
#       else
#	    define WORD      INT_64
#	    define U_WORD  U_INT_64
#	    define BITS_PER_WORD 64
#       endif

#	define HALF_WORD      INT_16
#	define U_HALF_WORD  U_INT_16
#	define BITS_PER_HALF_WORD 16

#elif ( defined(merced) || defined(MERCED))

#       undef  vax
#       undef  mips
#       undef  hp_pa
#       undef  cray
#       undef  alpha
#       undef  ix86
#       undef  merced
#       undef  amd64
#	undef  sparc

#       define merced 7
#       define ARCHITECTURE merced

#       define LOCAL_DATA 1

#       define STATIC_ROUNDING_MODES 1

#       undef  DYNAMIC_ROUNDING_MODES
#       define DENORMS_EMULATED 1
#       define SEPARATE_FLOAT_REGS 1
#       define MULTIPLE_ISSUE 1
#       undef  UNSIGNED_TO_FLOAT
#       define UNSIGNED_MULTIPLY 1
#       define ENDIANESS little_endian
#       define SCALE_METHOD by_int
#       define CVT_TO_HI_LO_METHOD by_flt

#       define BITS_PER_FLOAT  32
#       define BITS_PER_DOUBLE 64
#       define BITS_PER_LONG_DOUBLE 128
#       define LONG_DOUBLE_128_TYPE	_Quad

#       define BITS_PER_CHAR    8
#       define BITS_PER_SHORT  16
#       define BITS_PER_INT    32
#       define BITS_PER_LONG   64

#       define INT_8  signed char
#       define INT_16 signed short
#       define INT_32 signed int
#       undef  INT_128

#       define U_INT_8  unsigned char
#       define U_INT_16 unsigned short
#       define U_INT_32 unsigned int
#       undef  U_INT_128

#	if ( COMPILER == gnu_cc ) 
#           define   INT_64   signed long
#           define U_INT_64 unsigned long
#       else
#           define   INT_64   signed __int64 
#           define U_INT_64 unsigned __int64
#       endif

#       define WORD      INT_64
#       define U_WORD  U_INT_64
#       define BITS_PER_WORD 64
#       define HALF_WORD      INT_32
#       define U_HALF_WORD  U_INT_32
#       define BITS_PER_HALF_WORD 32

#elif (defined(__sparc) || defined(sparc))

#	undef  vax
#	undef  mips
#	undef  hp_pa
#	undef  cray
#	undef  alpha
#	undef  ix86
#       undef  merced
#       undef  amd64
#	undef  sparc

#	define sparc 9
#	define ARCHITECTURE sparc

#	define LOCAL_DATA 1
#	undef  STATIC_ROUNDING_MODES
#	define DYNAMIC_ROUNDING_MODES 1
#	define DENORMS_EMULATED 1
#	define SEPARATE_FLOAT_REGS 1
#	undef  MULTIPLE_ISSUE
#	undef  UNSIGNED_TO_FLOAT
#	define UNSIGNED_MULTIPLY 1
#	define ENDIANESS big_endian
#	define SCALE_METHOD by_int
#	define CVT_TO_HI_LO_METHOD by_flt

#	define BITS_PER_CHAR    8
#	define BITS_PER_SHORT  16
#	define BITS_PER_INT    32
#    	    define BITS_PER_LONG   32

#       define BITS_PER_ADDRESS 32

#	define BITS_PER_FLOAT  32
#	define BITS_PER_DOUBLE 64
#	define BITS_PER_LONG_DOUBLE 128
#       define LONG_DOUBLE_128_TYPE	_Quad

#	if ( COMPILER == gnu_cc ) 
#	    define __INT_64 long long
#       else
#	    define __INT_64 __int64
#       endif

#	define INT_8  signed char
#	define INT_16 signed short
#	define INT_32 signed int
#	define INT_64 signed __INT_64
#	undef  INT_128
#	define U_INT_8  unsigned char
#	define U_INT_16 unsigned short
#	define U_INT_32 unsigned int
#	define U_INT_64 unsigned __INT_64
#	undef  U_INT_128

#       if 0
            /* Setup for 32-bits */
#           define WORD                 INT_32
#           define U_WORD               U_INT_32
#           define BITS_PER_WORD        32
#           define HALF_WORD            INT_16
#           define U_HALF_WORD          U_INT_16
#           define BITS_PER_HALF_WORD   16
#       else

            /* Setup for 64 bits */
#           define WORD                 INT_64
#           define U_WORD               U_INT_64
#           define BITS_PER_WORD        64
#           define HALF_WORD            INT_32
#           define U_HALF_WORD          U_INT_32
#           define BITS_PER_HALF_WORD   32
#       endif






#elif (defined(_M_AMD64))

#	undef  vax
#	undef  mips
#	undef  hp_pa
#	undef  cray
#	undef  alpha
#	undef  ix86
#       undef  merced
#       undef  amd64
#	undef  sparc

#	define amd64 8
#	define ARCHITECTURE amd64

#	define LOCAL_DATA 1
#	undef  STATIC_ROUNDING_MODES
#	define DYNAMIC_ROUNDING_MODES 1
#	define DENORMS_EMULATED 1
#	define SEPARATE_FLOAT_REGS 1
#	undef  MULTIPLE_ISSUE
#	undef  UNSIGNED_TO_FLOAT
#	define UNSIGNED_MULTIPLY 1
#	define ENDIANESS little_endian
#	define SCALE_METHOD by_int
#	define CVT_TO_HI_LO_METHOD by_flt

#	define BITS_PER_CHAR    8
#	define BITS_PER_SHORT  16
#	define BITS_PER_INT    32
#       if (OP_SYSTEM == linux)
#    	    define BITS_PER_LONG   64
#       else
#    	    define BITS_PER_LONG   32
#       endif

#       define BITS_PER_ADDRESS 64

#	define BITS_PER_FLOAT  32
#	define BITS_PER_DOUBLE 64
#	define BITS_PER_LONG_DOUBLE 128
#       define LONG_DOUBLE_128_TYPE	_Quad

#	if ( COMPILER == gnu_cc ) 
#	    define __INT_64 long long
#       else
#	    define __INT_64 __int64
#       endif

#	define INT_8  signed char
#	define INT_16 signed short
#	define INT_32 signed int
#	define INT_64 signed __INT_64
#	undef  INT_128
#	define U_INT_8  unsigned char
#	define U_INT_16 unsigned short
#	define U_INT_32 unsigned int
#	define U_INT_64 unsigned __INT_64
#	undef  U_INT_128

#       if 0
            /* Setup for 32-bits */
#           define WORD                 INT_32
#           define U_WORD               U_INT_32
#           define BITS_PER_WORD        32
#           define HALF_WORD            INT_16
#           define U_HALF_WORD          U_INT_16
#           define BITS_PER_HALF_WORD   16
#       else

            /* Setup for 64 bits */
#           define WORD                 INT_64
#           define U_WORD               U_INT_64
#           define BITS_PER_WORD        64
#           define HALF_WORD            INT_32
#           define U_HALF_WORD          U_INT_32
#           define BITS_PER_HALF_WORD   32
#       endif


#else

#	error Architecture must be specified.

#endif

#if !defined(BITS_PER_ADDRESS)
#   define BITS_PER_ADDRESS BITS_PER_LONG
#endif

#if !defined(ADDRESS)
#   define ADDRESS          PASTE(U_INT_, BITS_PER_ADDRESS)
#endif

#undef little_endian
#undef big_endian

#define little_endian 0
#define big_endian 1

#undef  by_int
#undef  by_flt

#define by_int 0
#define by_flt 1


#if (ARCHITECTURE == vax)
#	define FLOAT_TYPES	VAX_TYPES
#elif ((ARCHITECTURE == alpha) || (ARCHITECTURE == merced)) && (OP_SYSTEM == vms)
#	define FLOAT_TYPES	(VAX_TYPES + IEEE_TYPES)
#else
#	define FLOAT_TYPES	IEEE_TYPES
#endif


#endif  /* ARCHITECTURE_H */

