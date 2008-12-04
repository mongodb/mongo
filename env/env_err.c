/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

#define	WT_ERR(env, error, fmt) {					\
	va_list __ap;							\
									\
	/* Application-specified callback function. */			\
	va_start(__ap, fmt);						\
	if ((env)->errcall != NULL)					\
		__wt_errcall((env)->errcall, (env),			\
		    (env)->errpfx, NULL, error, fmt, __ap);		\
	va_end(__ap);							\
									\
	/*								\
	 * If the application set an error callback function but not an	\
	 * error stream, we're done.  Otherwise, write an error	stream.	\
	 */								\
	if ((env)->errcall != NULL && (env)->errfile == NULL)		\
			return;						\
									\
	va_start(__ap, fmt);						\
	__wt_errfile((env)->errfile,					\
	    (env)->errpfx, NULL, error, fmt, __ap);			\
	va_end(__ap);							\
}

/*
 * __wt_env_err --
 *	Env.err method.
 */
void
__wt_env_err(ENV *env, int error, const char *fmt, ...)
{
	WT_ERR(env, error, fmt);
}

/*
 * __wt_env_errx --
 *	Env.errx method.
 */
void
__wt_env_errx(ENV *env, const char *fmt, ...)
{
	WT_ERR(env, 0, fmt);
}
