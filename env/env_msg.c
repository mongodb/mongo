/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

#define	WT_MSG(env, fmt) {						\
	extern FILE *__wt_err_stream;					\
	va_list __ap;							\
	/*								\
	 * Support messages even when we don't yet have an ENV handle,	\
	 * using the error stream.
	 */								\
	if ((env) == NULL) {						\
		va_start(__ap, fmt);					\
		__wt_msg_stream(					\
		    __wt_err_stream, NULL, NULL, 0, fmt, __ap);		\
		va_end(__ap);						\
		return;							\
	}								\
									\
	/* Application-specified callback function. */			\
	if (env->msgcall != NULL) {					\
		va_start(__ap, fmt);					\
		__wt_msg_call(						\
		    env->msgcall, env, NULL, NULL, 0, fmt, __ap);	\
		va_end(__ap);						\
	}								\
									\
	/*								\
	 * If the application set an message callback function but not a\
	 * message stream, we're done.  Otherwise, write the stream.	\
	 */								\
	if (env->msgcall != NULL && env->msgfile == NULL)		\
			return;						\
									\
	va_start(__ap, fmt);						\
	__wt_msg_stream(env->msgfile, NULL, NULL, 0, fmt, __ap);	\
	va_end(__ap);							\
}

/*
 * __wt_msg --
 *	Write a message.
 */
void
__wt_msg(ENV *env, const char *fmt, ...)
{
	WT_MSG(env, fmt);
}
