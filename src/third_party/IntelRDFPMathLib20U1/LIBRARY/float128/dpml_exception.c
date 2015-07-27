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

#if !defined(DPML_DO_SIDE_EFFECTS_NAME)
#    define DPML_DO_SIDE_EFFECTS_NAME	__INTERNAL_NAME(do_side_effects)
#endif

#if !defined(DPML_GET_ENVIRONMENT_NAME)
#   define  DPML_GET_ENVIRONMENT_NAME	__INTERNAL_NAME(get_environment)
#endif

#if !defined(DPML_SIGNAL_NAME)
#   define  DPML_SIGNAL_NAME		__INTERNAL_NAME(signal)
#endif

#define GLOBAL_TABLE_VALUES
#include "dpml_private.h"
#include "dpml_error_codes.h"

/*
 * Include platform specific headers.  Anything not defined in them will
 * be defaulted below.
 */

#if defined(PLATFORM_SPECIFIC_HEADER_FILE)
#   include PLATFORM_SPECIFIC_HEADER_FILE
#endif

/*
 * The follow macros and code are used to to define the default exception
 * model.  If no exception definitions were made in the above files then
 * the default behavior will be determined by whether or not IEEE floating
 * point is defined.
 *
 * If IEEE floating point is defined, the exception handler assumes the
 * existance of a control register and a set of routines to read and write
 * it.  Otherwise the default behavior is defined to signal all exceptions
 * except underflow which is flushed to zero.  The default exception behaviors
 * do not allow for the mixing of IEEE behavior and non-IEEE.  I.e. by default
 * you get one or the other.
 *
 *
 *
 * If IEEE exception behavior make sure things that are supposed to be
 * defined, are defined
 */

#if IEEE_EXCEPTION_BEHAVIOR
#   if !defined(DPML_GET_FPCSR)
#       error "DPML_GET_FPCSR must be defined for IEEE exception behavior"
#   endif
#   if !defined(DPML_SET_FPCSR)
#       error "DPML_SET_FPCSR must be defined for IEEE exception behavior"
#   endif

    /*
     * The data type of the FPCSR needs to be known.  If not specified,
     * assume it has the same type as the basic integer type on the platform.
     */

#   if !defined(FP_CSR_TYPE)
#       define FP_CSR_TYPE	WORD
#   endif

    /*
     * We need to be able to map the five basic DPML exceptions onto the bit
     * positions of the sticky bits in the FPCSR.  If no mapping function is
     * provided assume that the DPML exception enumerations match the bit
     * positions of the sticky bits in the FPCSR
     */

#   if !defined(DPML_FPCSR_STICKY_BITS)
#       define DPML_FPCSR_STICKY_BITS(d)	SET_BIT(d)
#   endif

#endif


/*
 * If no user exception environment is defined, read the enviornment from
 * the exception enable in the FPCSR for the IEEE case and just set to
 * signal everything except underflow otherwise.
 */

#if !defined(DPML_GET_ENVIRONMENT)

#   if IEEE_EXCEPTION_BEHAVIOR
#       define DPML_GET_ENVIRONMENT(e) \
			P_EXCPT_REC_ENVIRONMENT(p, DPML_GET_FPCSR(e))
#   else
#       define DPML_GET_ENVIRONMENT(e) \
			P_EXCPT_REC_ENVIRONMENT(p, ( ENABLE_FLUSH_TO_ZERO \
						   | ENABLE_SINGULARITY \
						   | ENABLE_OVERFLOW \
						   | ENABLE_INVALID \
						   | ENABLE_LOST_SIGNIFICANCE ))
#   endif
#endif

/*
 * If no user supplied signal mechanism, use the ANSI C raise() to generate
 * signal
 */

#if !defined(DPML_SIGNAL) && !defined(MINIMAL_SILENT_MODE_EXCEPTION_HANDLER) && \
    !defined(wnt)

#   include <sys/signal.h>
#   define DPML_SIGNAL(p)	 raise(SIGFPE)

#else

#   define DPML_SIGNAL(p)

#endif


/*
 * If no side effects are specified then set errno, signal if the envirnment
 * indicates a signal and for the IEEE case update the sticky bits
 */

#if !defined(SET_ERRNO)

#   include  <errno.h>
#   define SET_ERRNO(p)	errno = ERRNO_VALUE(G_EXCPT_REC_DPML_ECODE(p))

#endif

#if defined(IEEE_EXCEPTION_BEHAVIOR)
#   if  !defined(DPML_UPDATE_STICKY_BITS)

#       define DPML_UPDATE_STICKY_BITS(p)	\
		    { \
		    FP_CSR_TYPE fpcsr; \
		    DPML_GET_FPCSR(fpcsr); \
		    fpcsr |= FPCSR_STICKY_BITS(G_EXCPT_REC_DPML_ECODE(p)); \
		    DPML_SET_FPCSR(fpcsr); \
		    }
#   endif
#else
#    define DPML_UPDATE_STICKY_BITS(p)
#endif

#if !defined(DPML_DO_SIDE_EFFECTS)

#   define DPML_DO_SIDE_EFFECTS(p)	DPML_DO_SIDE_EFFECTS_NAME(p)

    static void
    DPML_DO_SIDE_EFFECTS_NAME(DPML_EXCEPTION_RECORD *p)
        {
        SET_ERRNO(p);

#     if !defined (MINIMAL_SILENT_MODE_EXCEPTION_HANDLER)

        /* set ieee sticky bit before signaling exception */
        DPML_UPDATE_STICKY_BITS(p);

        if (G_EXCPT_REC_ENVIRONMENT(p) & SET_BIT(G_EXCPT_REC_DPML_ECODE(p)))
            DPML_SIGNAL(p);
#     endif

        }

#endif /* !defined(DPML_DO_SIDE_EFFECTS) */

#define RET_VAL(type,val)      GLOBAL_ADDR(type,val)


#if !defined(GET_DPML_EXCEPTION_AND_VALUE)

	/*
	 * NOTE: This should be fixed.  The response table should
	 * have only one set of (err,value) pairs if only one
	 * behavior is supportted.
	 */

#   if IEEE_EXCEPTION_BEHAVIOR

#       define	GET_DPML_EXCEPTION_AND_VALUE(p) \
			{ \
			WORD e, v, t; \
			e = G_EXCPT_REC_FUNC_ECODE(p); \
			P_EXCPT_REC_DPML_ECODE(p, GET_IEEE_ERROR(e)); \
			v = GET_IEEE_VALUE(e); \
			t = G_EXCPT_REC_DATA_TYPE(p); \
			P_EXCPT_REC_RET_VAL_PTR(p, RET_VAL(t,v)); \
			}
#   else

#       define	GET_DPML_EXCEPTION_AND_VALUE(p) \
			{ \
			WORD e, v, t; \
			e = G_EXCPT_REC_FUNC_ECODE(p); \
			P_EXCPT_REC_DPML_ECODE(p, GET_FAST_ERROR(e)); \
			v = GET_FAST_VALUE(e); \
			t = G_EXCPT_REC_DATA_TYPE(p); \
			P_EXCPT_REC_RET_VAL_PTR(p, RET_VAL(t,v)); \
			}
#   endif

#endif

#if (EXCEPTION_INTERFACE_RECEIVE == receive_exception_record)
#   define EXCPTN_ARG	DPML_EXCEPTION_RECORD *p
#   define DECLARATIONS	WORD err = G_EXCPT_REC_FUNC_ECODE(p)
#else
#   define EXCPTN_ARG	WORD err
#   define DECLARATIONS	DPML_EXCEPTION_RECORD __tmp, *p = &__tmp;
#endif

#if !defined(DPML_EXCEPTION)

    void * DPML_EXCEPTION_NAME(EXCPTN_ARG)
        {
        DECLARATIONS;

        /* Split input error code into type, and base error */
        P_EXCPT_REC_DATA_TYPE(p, GET_ERR_CODE_TYPE(err));
        P_EXCPT_REC_FUNC_ECODE(p, GET_TYPELESS_ERR_CODE(err));

        DPML_GET_ENVIRONMENT(p);
        if (err < 0)
            /* Just a request for info */
            return (void *) G_EXCPT_REC_ENVIRONMENT(p);

        GET_DPML_EXCEPTION_AND_VALUE(p);

        if (G_EXCPT_REC_DPML_ECODE(p) != DPML_NO_ERROR)
            DPML_DO_SIDE_EFFECTS(p);

        return G_EXCPT_REC_RET_VAL_PTR(p);
        }

#endif
