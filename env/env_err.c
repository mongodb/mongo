/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

void
wiredtiger_err_stream(FILE *stream)
{
	extern FILE *__wt_err_stream;

	__wt_err_stream = stream;
}

#define	WT_ENV_ERR(env, error, fmt) {					\
	extern FILE *__wt_err_stream;					\
	va_list __ap;							\
	/*								\
	 * Support error messages even when we don't yet have an ENV	\
	 * handle.							\
	 */								\
	if ((env) == NULL) {						\
		va_start(__ap, fmt);					\
		__wt_msg_stream(					\
		    __wt_err_stream, NULL, NULL, error, fmt, __ap);	\
		va_end(__ap);						\
		return;							\
	}								\
									\
	/* Application-specified callback function. */			\
	if (env->errcall != NULL) {					\
		va_start(__ap, fmt);					\
		__wt_msg_call(env->errcall,				\
		    env, env->errpfx, NULL, error, fmt, __ap);		\
		va_end(__ap);						\
	}								\
									\
	/*								\
	 * If the application set an error callback function but not an	\
	 * error stream, we're done.  Otherwise, write the stream.	\
	 */								\
	if (env->errcall != NULL && env->errfile == NULL)		\
			return;						\
									\
	va_start(__ap, fmt);						\
	__wt_msg_stream(env->errfile,					\
	    env->errpfx, NULL, error, fmt, __ap);			\
	va_end(__ap);							\
}

/*
 * __wt_api_env_err --
 *	Env.err method.
 */
void
__wt_api_env_err(ENV *env, int error, const char *fmt, ...)
{
	WT_ENV_ERR(env, error, fmt);
}

/*
 * __wt_api_env_errx --
 *	Env.errx method.
 */
void
__wt_api_env_errx(ENV *env, const char *fmt, ...)
{
	WT_ENV_ERR(env, 0, fmt);
}
