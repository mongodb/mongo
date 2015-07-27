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

# ifndef DPML_EXCEPTION_H
# define DPML_EXCEPTION_H

/*									    */
/*  Standardize the definition of COMPATIBILITY_MODE, a macro used to aid   */
/*  the transition from the "old" style exception record interface to the   */
/*  exception dispatcher to the "new" style interface.  It is used on	    */
/*  platforms where both interfaces may co-exist.  It will not be needed    */
/*  when the old interface is no longer used.				    */
/*									    */
/*  If COMPATIBILITY_MODE is TRUE when compiling a DPML procedure, both the */
/*  old and new record interfaces are available to the procedure.  The	    */
/*  interface actually used is determined by the macros which the procedure */
/*  uses to invoke the exception dispatcher.  The GET_EXCEPTION_RESULT_n    */
/*  macros use the old interface while the RETURN_EXCEPTION_RESULT_n macros */
/*  use the new interface.  If COMPATIBILITY_MODE is FALSE, only the new    */
/*  interface is supported.						    */
/*									    */
/*  When compiling the exception dispatcher, if COMPATIBILITY_MODE is TRUE, */
/*  both the old and new record interfaces are supported by the dispatcher. */
/*  However, if COMPATIBILITY_MODE is FALSE, only the new interface is	    */
/*  supported.								    */
/*									    */
/*  Since all DPML procedures initially use the old interface, the default  */
/*  is that both interfaces are available.  Thus, if COMPATIBILITY_MODE is  */
/*  not defined or is TRUE, it is (re)defined to be TRUE.  However, if it   */
/*  is defined and is FALSE, it is redefined to be FALSE.		    */
/*									    */
/*  When most procedures have been modified to use the new interface, the   */
/*  default should be changed to cause only the new interface to be	    */
/*  provided.								    */
/*									    */
/*  Note that the use of COMPATIBILITY_MODE impacts makefiles.  DPML	    */
/*  procedures which use the new interface while the old is the default	    */
/*  must be compiled with COMPATIBILITY_MODE=0.  Conversely, when the new   */
/*  interface is the default, procedures continuing to use the old must be  */
/*  compiled with COMPATIBILITY_MODE=1.  As long as there are DPML	    */
/*  procedures using each style of interface, the exception dispatcher must */
/*  be compiled with COMPATIBILITY_MODE set to TRUE, either explicitly via  */
/*  COMPATIBILITY_MODE=1 or implicitly through the default value given to   */
/*  COMPATIBILITY_MODE.							    */
/*									    */

# if defined COMPATIBILITY_MODE
#    if COMPATIBILITY_MODE
#	undef COMPATIBILITY_MODE
#	define COMPATIBILITY_MODE 1
#    else
#	undef COMPATIBILITY_MODE
#	define COMPATIBILITY_MODE 0
#   endif
# else
#   define COMPATIBILITY_MODE 1
# endif

/*									    */
/*  Once the "old" style interface is no longer supported, all use of	    */
/*  ERROR_WORD_COMPATIBILITY_MODE_FLAG should be removed.		    */
/*									    */

# define ERROR_WORD_COMPATIBILITY_MODE_FLAG		\
    ( U_WORD )( ( U_WORD )1 << ( BITS_PER_WORD - 1 ) )

/*									    */
/*  If IEEE data types are used, default to IEEE behavior.		    */
/*									    */

# if !defined IEEE_EXCEPTION_BEHAVIOR 
#   if FLOAT_TYPES && IEEE_TYPES && !defined(MINIMAL_SILENT_MODE_EXCEPTION_HANDLER)
#      define IEEE_EXCEPTION_BEHAVIOR 1
#   else
#      define IEEE_EXCEPTION_BEHAVIOR 0
#      define DPML_UPDATE_STICKY_BITS( e )
#   endif
# else
#   undef  IEEE_EXCEPTION_BEHAVIOR
#   define IEEE_EXCEPTION_BEHAVIOR 1
# endif

# if COMPATIBILITY_MODE

/*									    */
/*  All of the DPML static exception behavior is encoded in an array of	    */
/*  structures.  The array is is indexed by error code (see		    */
/*  dpml_error_code.c) Each entry in the array contains an indication of    */
/*  which function generated the error code and a set of default responses  */
/*  to the error.  For discussion purposes, a response is an ordered pair   */
/*  of integers:  The first integer defines the DPML and the second defines */
/*  a return values (see dpml_error_code.c for details).  Currently, there  */
/*  are two responses associated with each error code: one for IEEE mode    */
/*  and one for fast mode.  While some platforms do not require all this    */
/*  information, currently it is always generated, regardless of platform   */
/*									    */
/*	NOTE: the function information is not currently used within the	    */
/*	DPML.  It is included for historic purposes and to allow other	    */
/*	users of the exception routines to generated function information   */
/*									    */

typedef struct {
    U_INT_32 func;
    U_INT_8 fast_err;
    U_INT_8 fast_val;
    U_INT_8 ieee_err;
    U_INT_8 ieee_val;
    } DPML_EXCEPTION_RESPONSE;
							    
#define	GET_IEEE_VALUE(n)	RESPONSE_TABLE[n].ieee_val
#define	GET_IEEE_ERROR(n)	RESPONSE_TABLE[n].ieee_err
#define	GET_FAST_VALUE(n)	RESPONSE_TABLE[n].fast_val
#define	GET_FAST_ERROR(n)	RESPONSE_TABLE[n].fast_err
#define GET_ERR_CODE_FUNC(c)	RESPONSE_TABLE[c].func

# endif

/*									    */
/*  There are five generic error codes that are processed by DPML exception */
/*  handler: underflow, overflow, singularity, invalid and lost		    */
/*  significance.  These corrospond roughly to the IEEE exceptions	    */
/*  underflow, overflow, divide by zero, invalid argument and inexact.  In  */
/*  addition there are two pseudo errors: NO_ERROR, which allows platform   */
/*  specific function returns and ENV_INFO, which allow DPML routines to    */
/*  determine what environment they are functioning in.			    */
/*									    */
/*  When the exception handler is invoked, it determines the environment it */
/*  is executing in.  The environment is encoded in a bit vector that	    */
/*  indicates: Which of the five DPML exceptions should cause signals to be */
/*  generated; Whether denormalized numbers should be flushed to zero and;  */
/*  Whether IEEE mode is enabled.					    */
/*									    */
/*  The following defines indicate the bit positions used to encode the	    */
/*  environment and the masks used to query the environment.  The ordinal   */
/*  values of the error codes are important only in they correspond to the  */
/*  logic in the exception handler and the macro ERRNO_VALUE maps the	    */
/*  UNDER/OVERFLOW and SINGULARITY/INVALID onto ERANGE and EDOM		    */
/*  repectively.							    */
/*									    */
/*  Many assumptions are made about the order (both relative and absolute)  */
/*  of these values.							    */
/*									    */

# define DPML_ENV_INFO	       -1
# define DPML_NO_ERROR		0
# define DPML_INVALID		1
# define DPML_SINGULARITY	2
# define DPML_OVERFLOW		3
# define DPML_UNDERFLOW		4
# define DPML_LOST_SIGNIFICANCE	5
# define DPML_FLUSH_TO_ZERO	6
# define DPML_IEEE_MODE		7

/*									    */
/*  It is required that the ENABLE_xxx symbols be equal to 1 << DPML_xxx.   */
/*  Unfortunately, that cannot be checked during compilation.		    */
/*									    */

# define ENABLE_NO_ERROR	  0x00
# define ENABLE_INVALID		  0x02
# define ENABLE_SINGULARITY	  0x04
# define ENABLE_OVERFLOW	  0x08
# define ENABLE_UNDERFLOW	  0x10
# define ENABLE_LOST_SIGNIFICANCE 0x20
# define ENABLE_FLUSH_TO_ZERO	  0x40
# define ENABLE_IEEE_MODE	  0x80

# define EXCEPTION_ENABLE_MASK ( ENABLE_INVALID		  |	\
				 ENABLE_SINGULARITY	  |	\
				 ENABLE_OVERFLOW	  |	\
				 ENABLE_UNDERFLOW	  |	\
				 ENABLE_LOST_SIGNIFICANCE )

# define STATUS_NO_ERROR	  ENABLE_NO_ERROR
# define STATUS_INVALID		  ENABLE_INVALID
# define STATUS_SINGULARITY	  ENABLE_SINGULARITY
# define STATUS_OVERFLOW	  ENABLE_OVERFLOW
# define STATUS_UNDERFLOW	  ENABLE_UNDERFLOW
# define STATUS_LOST_SIGNIFICANCE ENABLE_LOST_SIGNIFICANCE
# define STATUS_DENORM_PROCESSING ENABLE_FLUSH_TO_ZERO

# define EXCEPTION_STATUS_MASK ( STATUS_INVALID		  |	\
				 STATUS_SINGULARITY	  |	\
				 STATUS_OVERFLOW	  |	\
				 STATUS_UNDERFLOW	  |	\
				 STATUS_LOST_SIGNIFICANCE )

/*									    */
/*  These macros are used to determine the errno value associated with an   */
/*  error condition.  If the error is INVALID or SINGULARITY, errno is set  */
/*  to EDOM; if it is OVERFLOW or UNDERFLOW, it is set to ERANGE.  The	    */
/*  values DPML_EDOM and DPML_ERANGE are used internally in the DPML to	    */
/*  limit the need to include the system header file errno.h to the	    */
/*  exception dispatcher procedure itself.				    */
/*									    */

# define ERRNO_TEST( error_code ) ( ( error_code ) < DPML_OVERFLOW )
# define DPML_ERRNO_VALUE( error_code )				\
    ( ERRNO_TEST( error_code ) ? DPML_EDOM : DPML_ERANGE )
# define ERRNO_VALUE( error_code )				\
    ( ERRNO_TEST( error_code ) ? EDOM : ERANGE )

/*									    */
/*  In order to more easily encode the default return values for specific   */
/*  errors, the basic DPML errors are extended in some cases to include	    */
/*  positive and negative flavors.					    */
/*									    */

#define	POS_ERR(e)	((e) << 1)
#define	NEG_ERR(e)	(POS_ERR(e) + 1)
#define UNSIGNED_ERR(e) ((e) >> 1)

#define POS_UNDERFLOW_ERR	POS_ERR(DPML_UNDERFLOW)
#define NEG_UNDERFLOW_ERR	NEG_ERR(DPML_UNDERFLOW)
#define POS_OVERFLOW_ERR	POS_ERR(DPML_OVERFLOW)
#define NEG_OVERFLOW_ERR	NEG_ERR(DPML_OVERFLOW)
#define POS_SINGULARITY		POS_ERR(DPML_SINGULARITY)
#define NEG_SINGULARITY		NEG_ERR(DPML_SINGULARITY)
#define INVALID_ARGUMENT	POS_ERR(DPML_INVALID)			
#define LOSS_OF_SIGNIFICANCE	POS_ERR(DPML_LOST_SIGNIFICANCE)			

# if COMPATIBILITY_MODE

/*									    */
/*  The DPML exception handler is invoked with a type specific error code.  */
/*  The type information is given in the high five bits of the input.  The  */
/*  remaing bits are a type independent enumerated error code that is used  */
/*  to index into a table of default error responses.  The type		    */
/*  enumerations are defined in dpml_globals.c and the error codes in	    */
/*  dpml_error_codes.c.							    */
/*									    */

#define TYPE_WIDTH	5
#define TYPE_POS	(BITS_PER_INT - TYPE_WIDTH)

#define ADD_ERR_CODE_TYPE(e)            ((F_TYPE_ENUM << TYPE_POS) | (e))
#define GET_ERR_CODE_TYPE(c)            ((c) >> TYPE_POS)
#define GET_TYPELESS_ERR_CODE(c)        ((c) & ~MAKE_MASK(TYPE_WIDTH, TYPE_POS))

# endif

/*									    */
/*  Passing of data/value to subroutines and macros is done via a pointer   */
/*  to a structure rather than individual values.  The reason for this is   */
/*  that it simplifies macro definitions and subroutine interfaces.  The    */
/*  following structure definition is sufficiently general deal with all of */
/*  the currently supportted platforms.  Less general, platform specific    */
/*  data structures coulb be used.  However it does not appear the the	    */
/*  increase in efficency of platform specific structures justifies the	    */
/*  resulting complications.  If the platform we are dealing with requires  */
/*  reporting the actual arguments that cause an exception condition, use   */
/*  the follow union to pass them in.					    */
/*									    */

# if ARCHITECTURE == alpha

#   define EXCEPTION_INTERFACE_RECEIVE receive_exception_record
#   if defined MINIMAL_SILENT_MODE_EXCEPTION_HANDLER
#       define __PROCESS_DENORMS 0
#   endif

#   if OP_SYSTEM == osf
#	include "alpha_unix_exception.h"
#	if defined dec_cc
#	    define EXCEPTION_INTERFACE_SEND ( send_error_code     |	\
					      send_return_address |	\
					      send_return_value )
#	else
#	    define EXCEPTION_INTERFACE_SEND ( send_error_code   |	\
					      send_return_value )
#	endif

#   elif OP_SYSTEM == vms
#	include "alpha_vms_exception.h"
#	define EXCEPTION_INTERFACE_SEND ( send_error_code   |	\
					  send_return_value )
#	if VAX_FLOATING
#	    define __PROCESS_DENORMS 0
#	endif

#   elif OP_SYSTEM == wnt
#	include "alpha_nt_exception.h"
#	define EXCEPTION_INTERFACE_SEND ( send_error_code     |	\
					  send_function_name  |	\
					  send_return_address |	\
					  send_arguments )

#   elif OP_SYSTEM == linux
#	include "alpha_linux_exception.h"
#	define EXCEPTION_INTERFACE_SEND ( send_error_code   |	\
					  send_return_value )
#	define PLATFORM_SPECIFIC_HEADER_FILE "alpha_linux_exception.c"   

#   endif

# else

#   if ARCHITECTURE == mips
#	define PROCESS_DENORMS 1
#	define DPML_EXCEPTION_HANDLER DPML_EXCEPTION_NAME
#	define EXCEPTION_ARGUMENTS( error_code ) error_code
#	define PLATFORM_SPECIFIC_HEADER_FILE "mips_exception.c"

#   elif ARCHITECTURE == hp_pa
#	define PROCESS_DENORMS 1
#	define DPML_EXCEPTION_HANDLER DPML_EXCEPTION_NAME
#	define EXCEPTION_ARGUMENTS( error_code ) error_code
#	define PLATFORM_SPECIFIC_HEADER_FILE "hppa_exception.c"

#   elif ARCHITECTURE == ix86
#	define PROCESS_DENORMS 1
#	define DPML_EXCEPTION_HANDLER DPML_EXCEPTION_NAME
#	define EXCEPTION_ARGUMENTS( error_code ) error_code
//#	define PLATFORM_SPECIFIC_HEADER_FILE "intel_exception.c"

#   elif ARCHITECTURE == merced 

#     if OP_SYSTEM == linux
//#	include "linux_exception.h"
/*#	define EXCEPTION_INTERFACE_SEND ( send_error_code   |	\
					  send_return_value )*/
#	define PROCESS_DENORMS 1
#	define DPML_EXCEPTION_HANDLER DPML_EXCEPTION_NAME
//#	define PLATFORM_SPECIFIC_HEADER_FILE "linux_exception.c"   

#     elif OP_SYSTEM == vms
#	include "ia64_vms_exception.h"
#	define EXCEPTION_INTERFACE_SEND ( send_error_code   |	\
					  send_return_value )
#	if VAX_FLOATING
#	    define __PROCESS_DENORMS 0
#	endif

#     endif

#   else

#       if IEEE_FLOATING && !defined(MINIMAL_SILENT_MODE_EXCEPTION_HANDLER)
#          define PROCESS_DENORMS 1
#       else
#	   define PROCESS_DENORMS 0
#       endif
#       define DPML_EXCEPTION_HANDLER DPML_EXCEPTION_NAME 
#   endif

# endif

/*									    */
/*	Provide default definitions of an exception value and the exception */
/*	record.								    */
/*									    */

#if !defined(EXCEPTION_ARG_LIST)

    typedef union {
	WORD	    w ;
	float	    f ;
	double	    d ;
	long double ld ;
	} DPML_EXCEPTION_VALUE ;

    typedef struct {
	WORD		     func_error_code ;
	void*		     context ;
	WORD		     platform_specific_err_code ;
	WORD		     environment ;
	void*		     ret_val_ptr ;
	char*		     name ;
	char		     data_type ;
	char		     dpml_error ;
	char		     mode ;
	DPML_EXCEPTION_VALUE ret_val ;
	DPML_EXCEPTION_VALUE args[ 4 ] ;
	} DPML_EXCEPTION_RECORD ;

#   define EXCEPTION_ARG_LIST DPML_EXCEPTION_RECORD*

#endif


#define G_EXCPT_REC_FUNC_ECODE(p)	p->func_error_code
#define G_EXCPT_REC_PLTFRM_ECODE(p)	p->platform_specific_err_code
#define G_EXCPT_REC_ENVIRONMENT(p)	p->environment
#define G_EXCPT_REC_RET_VAL_PTR(p)	p->ret_val_ptr
#define G_EXCPT_REC_CONTEXT(p)		((CONTEXT *) p->context)
#define G_EXCPT_REC_NAME(p)		p->name
#define G_EXCPT_REC_DATA_TYPE(p)	p->data_type
#define G_EXCPT_REC_DPML_ECODE(p)	p->dpml_error
#define G_EXCPT_REC_MODE(p)		p->mode
#define G_EXCPT_REC_ARG(p,i,type)	p->PASTE_3(args[i], ., type)
#define G_EXCPT_REC_RET_VAL(p,type)	p->PASTE_3(ret_val, ., type)

#define P_EXCPT_REC_FUNC_ECODE(p,v)	p->func_error_code = (v)
#define P_EXCPT_REC_PLTFRM_ECODE(p,v)	p->platform_specific_err_code = (v)
#define P_EXCPT_REC_ENVIRONMENT(p,v)	p->environment = (v)
#define P_EXCPT_REC_RET_VAL_PTR(p,v)	p->ret_val_ptr = (void *)(v) 
#define P_EXCPT_REC_CONTEXT(p,v)	p->context = (void *)(v) 
#define P_EXCPT_REC_NAME(p,v)		p->name = (v) 
#define P_EXCPT_REC_DATA_TYPE(p,v)	p->data_type = (v) 
#define P_EXCPT_REC_DPML_ECODE(p,v)	p->dpml_error = (v)
#define P_EXCPT_REC_MODE(p,v)		p->mode = (v)
#define P_EXCPT_REC_ARG(p,i,type,v)	p->PASTE_3(args[i], ., type) = (v)
#define P_EXCPT_REC_RET_VAL(p,type,v)	p->PASTE_3(ret_val, ., type) = (v)

#define P_EXCPTN_VALUE_w(x,v)    x.w = (v)
#define P_EXCPTN_VALUE_f(x,v)    x.f = (v)
#define P_EXCPTN_VALUE_g(x,v)    x.d = (v)
#define P_EXCPTN_VALUE_s(x,v)    x.f = (v)
#define P_EXCPTN_VALUE_t(x,v)    x.d = (v)
#define P_EXCPTN_VALUE_x(x,v)    x.ld = (v)
#define P_EXCPTN_VALUE_F	PASTE_3(P_EXCPTN_VALUE, _, F_CHAR)

#define G_EXCPTN_VALUE_w(x)      x.w
#define G_EXCPTN_VALUE_f(x)      x.f
#define G_EXCPTN_VALUE_g(x)      x.d
#define G_EXCPTN_VALUE_s(x)      x.f
#define G_EXCPTN_VALUE_t(x)      x.d
#define G_EXCPTN_VALUE_x(x)      x.ld
#define G_EXCPTN_VALUE_F	PASTE_3(G_EXCPTN_VALUE, _, F_CHAR)

/*									    */
/*  Define platform specific execption information that is required for	    */
/*  compilation of individual DPML routines.  At the same time define the   */
/*  name of the header files that determine the exception behavior for the  */
/*  platform.								    */
/*									    */
/*  One of the symbols that must be available for individual DPML	    */
/*  compilations is PROCESS_DENORMS.  On some of the alpha platforms,	    */
/*  PROCESS_DENORMS is an informational call to the exception handler.	    */
/*  However, on other platforms if defaults to TRUE for IEEE types and	    */
/*  FALSE otherwise.							    */
/*									    */
/*  Finally, define the calling conventions between dpml routines and the   */
/*  exception handler.  Since on some platforms, dpml routines call a	    */
/*  "capture context" routine which in turn calls dpml_exception, the	    */
/*  calling interface to "the exception handler" and the actual interface   */
/*  to dpml_exception may be different.  Consquently, we use two macros to  */
/*  define hook-up between DPML routines and dpml_exception		    */
/*									    */
/*  The information passed to the exception handler is defined by the	    */
/*  symbol EXCEPTION_INTERFACE_SEND.  Currently, there are 3 disjoint sets  */
/*  of data that can be passed: the error code, the function name and the   */
/*  arguments.								    */
/*									    */
/*  We assume that the error code will always be passed, so that is the.    */
/*  If more than information than the error code is passed, then it is	    */
/*  assumed that the information is passed in an exception record.	    */
/*  Otherwise it is simply passed as an integer.			    */
/*									    */
/*  The information received by dpml_exception is defined by the macro	    */
/*  EXCEPTION_INTERFACE_RECEIVE.  Currently, there are 2 methods of	    */
/*  receiving data in dpml_exception: a error code only or a pointer to an  */
/*  exception record.  The default is error code only.			    */
/*									    */
/*  Provide names for the actual exception dispatcher procedure and a	    */
/*  possible "capture context" procedure unless they already have been	    */
/*  given names.  Typically, one of these names will be used as the	    */
/*  definition of DPML_EXCEPTION_HANDLER.				    */
/*									    */

# if !defined( DPML_EXCEPTION_NAME )
#   define DPML_EXCEPTION_NAME __INTERNAL_NAME( exception )
# endif

# if !defined( DPML_CAPTURE_CONTEXT_NAME )
#   define DPML_CAPTURE_CONTEXT_NAME __INTERNAL_NAME( capture_context )
# endif

# if !defined EXCEPTION_INTERFACE_SEND
#   define EXCEPTION_INTERFACE_SEND	send_error_code
# endif

# if !defined EXCEPTION_INTERFACE_RECEIVE
#   define EXCEPTION_INTERFACE_RECEIVE	receive_error_code
# endif

# define receive_error_code	  1
# define receive_exception_record 2
# define send_error_code	  1
# define send_function_name	  2
# define send_arguments		  4
# define send_exception_record    8
# define send_return_address     16
# define send_return_value       32

# if EXCEPTION_INTERFACE_SEND & send_function_name
#   define INIT_NAME P_EXCPT_REC_NAME( ( &tmp_rec ), STR( F_ENTRY_NAME ) )
# else
#   define INIT_NAME
# endif

# if EXCEPTION_INTERFACE_SEND & send_arguments
#   define P_F_ARG_VALUE( n, x ) P_EXCPTN_VALUE_F( tmp_rec.args[ n ], x )
#   define P_W_ARG_VALUE( n, x ) P_EXCPTN_VALUE_w( tmp_rec.args[ n ], x )
# else
#   define P_F_ARG_VALUE( n, x )
#   define P_W_ARG_VALUE( n, x )
# endif

# if EXCEPTION_INTERFACE_SEND & send_return_address
    /* NOTE THE TRAILING COMMA						    */
#   define INIT_RETURN_ADDRESS \
	P_EXCPT_REC_RET_VAL_PTR( ( &tmp_rec ), RET_ADDR ),
# else
#   define INIT_RETURN_ADDRESS
# endif

# if EXCEPTION_INTERFACE_SEND & send_return_value
#   define INIT_RETURN_VALUE( v ) P_EXCPTN_VALUE_F( tmp_rec.ret_val, v )
# else
#   define INIT_RETURN_VALUE( v )
# endif

# if EXCEPTION_INTERFACE_SEND > send_error_code
#   undef EXCEPTION_INTERFACE_SEND
#   define EXCEPTION_INTERFACE_SEND send_exception_record
# endif

# if EXCEPTION_INTERFACE_SEND == send_exception_record

#   define EXCEPTION_RECORD_DECLARATION	DPML_EXCEPTION_RECORD tmp_rec ;

#   if !defined EXCEPTION_ARG
#	define EXCEPTION_ARG( e ) \
	    ( P_EXCPT_REC_FUNC_ECODE( (&tmp_rec), ADD_ERR_CODE_TYPE(e)), \
				       INIT_RETURN_ADDRESS \
				       (&tmp_rec) \
				       )
#   endif

# else

#   define EXCEPTION_ARG_TYPE WORD
#   define EXCEPTION_RECORD_DECLARATION
#   define EXCEPTION_ARG( e ) ADD_ERR_CODE_TYPE( e )

# endif

# define SIGNAL_INTOVF 1
# define SIGNAL_INTDIV 2

# define DENORM_SCREEN  0
# define DENORM_UNSCALE 1

# if !defined SIGNAL_LOGZERNEG
#   define SIGNAL_LOGZERNEG 0
# endif
# if !defined SIGNAL_UNDEXP
#   define SIGNAL_UNDEXP 0
# endif
# if !defined SIGNAL_SQUROONEG
#   define SIGNAL_SQUROONEG 0
# endif

# if COMPATIBILITY_MODE

    /*									    */
    /*	Define the "old" interface to the exception dispatcher.		    */
    /*									    */

#   if !defined GET_EXCEPTION_RESULT
#	define GET_EXCEPTION_RESULT INIT_NAME
#   endif


#   define GET_EXCEPTION_RESULT_1( error_code,				\
				   argument,				\
				   result ) {				\
	GET_EXCEPTION_RESULT ;						\
	P_F_ARG_VALUE( 0, argument ) ;					\
	result = *( F_TYPE* )						\
	    DPML_EXCEPTION_HANDLER( EXCEPTION_ARG( error_code ) ) ;	\
	}

#   define GET_EXCEPTION_RESULT_2( error_code,				\
				   argument_0,				\
				   argument_1,				\
				   result ) {				\
	GET_EXCEPTION_RESULT ;						\
	P_F_ARG_VALUE( 0, argument_0 ) ;				\
	P_F_ARG_VALUE( 1, argument_1 ) ;				\
	result = *( F_TYPE* )						\
	    DPML_EXCEPTION_HANDLER( EXCEPTION_ARG( error_code ) ) ;	\
	}

#   define GET_EXCEPTION_RESULT_4( error_code,				\
				   argument_0,				\
				   argument_1,				\
				   argument_2,				\
				   argument_3,				\
				   result ) {				\
	GET_EXCEPTION_RESULT ;						\
	P_F_ARG_VALUE( 0, argument_0 ) ;				\
	P_F_ARG_VALUE( 1, argument_1 ) ;				\
	P_F_ARG_VALUE( 2, argument_2 ) ;				\
	P_F_ARG_VALUE( 3, argument_3 ) ;				\
	result = *( F_TYPE* )						\
	    DPML_EXCEPTION_HANDLER( EXCEPTION_ARG( error_code ) ) ;	\
	}

# else

    /*									    */
    /*	Define the "new" style interface to the exception dispatcher.	    */
    /*									    */

#   define RETURN_EXCEPTION_RESULT_1( error_word,	\
				      argument,		\
				      signature,	\
				      operation ) {	\
	P_F_ARG_VALUE( 0, argument ) ;			\
	RETURN_EXCEPTION_RESULT( error_word,		\
				 signature,		\
				 operation )		\
	}

#   define RETURN_EXCEPTION_RESULT_2( error_word,	\
				      argument_0,	\
				      argument_1,	\
				      signature,	\
				      operation ) {	\
	P_F_ARG_VALUE( 0, argument_0 ) ;		\
	P_F_ARG_VALUE( 1, argument_1 ) ;		\
	RETURN_EXCEPTION_RESULT( error_word,		\
				 signature,		\
				 operation )		\
	}

#   define RETURN_EXCEPTION_RESULT_4( error_word,	\
				      argument_0,	\
				      argument_1,	\
				      argument_2,	\
				      argument_3,	\
				      signature,	\
				      operation ) {	\
	P_F_ARG_VALUE( 0, argument_0 ) ;		\
	P_F_ARG_VALUE( 1, argument_1 ) ;		\
	P_F_ARG_VALUE( 2, argument_2 ) ;		\
	P_F_ARG_VALUE( 3, argument_3 ) ;		\
	RETURN_EXCEPTION_RESULT( error_word,		\
				 signature,		\
				 operation )		\
	}

# endif

/*									    */
/*  DPML_GET_ENVIRONMENT(p) is a macro that fills the envrionment field of  */
/*  the exception record pointed to by p with a bit vector that describes   */
/*  the enviroment the exception handler is operating in.  The specific bit */
/*  interpretations are defined by the ENABLE_<error> macros defined above. */
/*  If __DPML_EXCPT_ENVIRONMENT is defined at this point, then it is	    */
/*  assumed that the exception behavior of the library is determined at	    */
/*  compile time as indicated by the value of __DPML_EXCPT_ENVIRONMENT.	    */
/*  Otherwise, the exception behavior is assumed to be determined at	    */
/*  runtime.  In the latter case, the macro DPML_GET_ENVIRONMENT must	    */
/*  eventually be defined to be some code sequence that fills in the	    */
/*  environment field.							    */
/*									    */
/*  In order to facilitate efficient code generation, if the exception	    */
/*  behavior is static, then PROCESS_DENORMS is  defined to be a compile    */
/*  time constant.  If the exception behavior is dynamic, and		    */
/*  PROCESS_DENORMS is not already defined, set it up to probe the	    */
/*  environment via the exception handler				    */
/*									    */

#if defined(__DPML_EXCPT_ENVIRONMENT)
#   define DPML_GET_ENVIRONMENT(p) \
		 P_EXCPT_REC_ENVIRONMENT(p, __DPML_EXCPT_ENVIRONMENT)
#else
#   define __DPML_EXCPT_ENVIRONMENT \
		((WORD) DPML_EXCEPTION_HANDLER(EXCEPTION_ARG(DPML_ENV_INFO)))
#endif

#if !defined(__PROCESS_DENORMS)
#   define __PROCESS_DENORMS	\
			((__DPML_EXCPT_ENVIRONMENT & ENABLE_FLUSH_TO_ZERO) == 0)
#endif

# if ARCHITECTURE == alpha
#   if OP_SYSTEM == osf
#   elif OP_SYSTEM == vms
#   elif OP_SYSTEM == wnt
#   elif OP_SYSTEM == linux
#   else
	typedef void* EXCEPTION_RETURN_TYPE ;
#   endif
# elif ARCHITECTURE == merced
#   if OP_SYSTEM == vms
#   else
	typedef void* EXCEPTION_RETURN_TYPE ;
#   endif
# else	/*  The hardware architecture is not Alpha or Merced		    */
    typedef void* EXCEPTION_RETURN_TYPE ;
# endif

extern EXCEPTION_RETURN_TYPE DPML_EXCEPTION_HANDLER( EXCEPTION_ARG_LIST ) ;

/*									    */
/*  After including the operating-system header files, use default	    */
/*  definitions for those required macros which are not defined.	    */
/*									    */

# if !defined GET_CONTEXT_INFO
#   define GET_CONTEXT_INFO
# endif

# if !defined GET_FUNCTION_INFO
#   define GET_FUNCTION_INFO( signature, operation )
# endif

# if !defined INIT_NAME
#   define INIT_NAME
# endif

# if COMPATIBILITY_MODE

#   if !defined PROCESS_DENORMS
#	define PROCESS_DENORMS (						\
	    ( ( U_WORD )DPML_EXCEPTION_HANDLER(					\
		EXCEPTION_ARGUMENTS( ( U_WORD )( WORD )DPML_ENV_INFO ) )	\
	    & ( ENABLE_FLUSH_TO_ZERO ) ) == 0					\
	    )
#   endif
# endif

/*									    */
/*  The structure of ERROR_WORD if only IEEE data types are used is:	    */
/*									    */
/*     7     6	   5	 4     3     2	   1	 0			    */
/*  +-----+-----+-----+-----+-----+-----+-----+-----+			    */
/*  | MBZ |	    exception_cause	      |errno|			    */
/*  +-----+-----+-----+-----+-----+-----+-----+-----+			    */
/*									    */
/*     17    16	   15	 14    13    12	   11	 10    9     8		    */
/*  +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+	    */
/*  |	    IEEE_value	    |	    fast_value	    | data_type |	    */
/*  +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+	    */
/*									    */
/*     63    62    61    60    59             22    21    20    19    18    */
/*  +-----+-----+-----+-----+-----+--/////--+-----+-----+-----+-----+-----+ */
/*  | flag|                   exception_extension                         | */
/*  +-----+-----+-----+-----+-----+--/////--+-----+-----+-----+-----+-----+ */
/*									    */
/*									    */
/*  A possible record structure for ERROR_WORD is			    */
/*									    */
/*  struct {								    */
/*	unsigned int errno : 1 ;					    */
/*	unsigned int exception_cause : 6 ;				    */
/*	unsigned int : 1 ;						    */
/*	unsigned int data_type : 2 ;					    */
/*	unsigned int fast_value : 4 ;					    */
/*	unsigned int IEEE_value : 4 ;					    */
/*	unsigned int exception_extension : 45 ;				    */
/*	unsigned int flag : 1 ;						    */
/*	} ERROR_WORD ;							    */
/*									    */
/*									    */
/*  However, if both IEEE and VAX data types are supported, the format of   */
/*  ERROR_WORD is							    */
/*									    */
/*     7     6	   5	 4     3     2	   1	 0			    */
/*  +-----+-----+-----+-----+-----+-----+-----+-----+			    */
/*  | MBZ |	    exception_cause	      |errno|			    */
/*  +-----+-----+-----+-----+-----+-----+-----+-----+			    */
/*									    */
/*     18    17    16	 15    14    13    12	 11    10    9     8	    */
/*  +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+	    */
/*  |	    IEEE_value	    |	    fast_value	    |    data_type    |	    */
/*  +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+	    */
/*									    */
/*     63    62    61    60    59             23    22    21    20    19    */
/*  +-----+-----+-----+-----+-----+--/////--+-----+-----+-----+-----+-----+ */
/*  | flag|                   exception_extension                         | */
/*  +-----+-----+-----+-----+-----+--/////--+-----+-----+-----+-----+-----+ */
/*									    */
/*									    */
/*  A possible record structure for ERROR_WORD is then			    */
/*									    */
/*  struct {								    */
/*	unsigned int errno : 1 ;					    */
/*	unsigned int exception_cause : 6 ;				    */
/*	unsigned int : 1 ;						    */
/*	unsigned int data_type : 3 ;					    */
/*	unsigned int fast_value : 4 ;					    */
/*	unsigned int IEEE_value : 4 ;					    */
/*	unsigned int exception_extension : 44 ;				    */
/*	unsigned int flag : 1 ;						    */
/*	} ERROR_WORD ;							    */
/*									    */

# define DPML_EDOM 0
# define DPML_ERANGE 1
# define ERROR_WORD_ERRNO_FLAG( errno )		\
    ( ( ( errno ) == DPML_EDOM ) ? 0 : 1 )
# if OP_SYSTEM == vms
#   define ERROR_WORD_DATA_TYPE_SIZE 3
# else
#   define ERROR_WORD_DATA_TYPE_SIZE 2
# endif
# define ERROR_WORD_VALUE_SIZE 4
# define ERROR_EXTENSION_SIZE (					\
    ( 8 * sizeof( U_WORD ) - 1 ) -				\
    ( ERROR_WORD_DATA_TYPE_POS + ERROR_WORD_DATA_TYPE_SIZE +	\
    2 * ERROR_WORD_VALUE_SIZE )					\
    )

# define ERROR_WORD_DATA_TYPE_POS 8
# define ERROR_WORD_FAST_VALUE_POS				\
    ( ERROR_WORD_DATA_TYPE_POS + ERROR_WORD_DATA_TYPE_SIZE )
# define ERROR_WORD_IEEE_VALUE_POS				\
    ( ERROR_WORD_FAST_VALUE_POS + ERROR_WORD_VALUE_SIZE )
# define ERROR_WORD_EXTRA_INFO_POS				\
    ( ERROR_WORD_IEEE_VALUE_POS + ERROR_WORD_VALUE_SIZE )

# define ERROR_WORD( exception_cause,				\
		     fast_value,				\
		     IEEE_value,				\
		     data_type,					\
		     errno,					\
		     exception_extension )			\
    ( ERROR_WORD_ERRNO_FLAG( errno ) |				\
    ( exception_cause ) |					\
    ( ( data_type ) << ERROR_WORD_DATA_TYPE_POS ) |		\
    ( ( fast_value ) << ERROR_WORD_FAST_VALUE_POS ) |		\
    ( ( ( IEEE_value ) ^ ( fast_value ) )			\
	<< ERROR_WORD_IEEE_VALUE_POS ) |			\
    ( ( exception_extension ) << ERROR_WORD_EXTRA_INFO_POS ) |	\
    ( ERROR_WORD_COMPATIBILITY_MODE_FLAG ) )

/*									    */
/*  Some of the definitions in this file are used in generating		    */
/*  dpml_globals.h and dpml_error_codes_enum.h.  If this is the case, don't */
/*  include those files now.						    */
/*									    */

# ifndef MAKE_DPML_ERROR_CODES_ENUM
#   include "dpml_globals.h"	      /*  Include type specifiers and type  */
#   include "dpml_error_codes_enum.h" /*  independent error codes.	    */
# endif

# endif	/*  ifndef DPML_EXCEPTION_H					    */
