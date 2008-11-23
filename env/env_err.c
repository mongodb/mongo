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

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_assert --
 *	Internal version of assert function.
 */
void
__wt_assert(
    IENV *ienv, const char *check, const char *file_name, int line_number)
{
	__wt_env_errx(ienv->env,
	    "assertion failure: %s/%d: \"%s\"", file_name, line_number, check);

	__wt_abort(ienv);
	/* NOTREACHED */
}
#endif

/*
 * __wt_api_flags --
 *	Print a standard error message when an API function is passed illegal
 *	flags.
 */
int
__wt_api_flags(IENV *ienv, const char *name)
{
	__wt_env_errx(ienv->env, "%s: illegal API flag specified", name);
	return (WT_ERROR);
}

/*
 * wt_strerror --
 *	Return a string for any error value.
 */
char *
wt_strerror(int error)
{
	static char errbuf[64];
	char *p;

	if (error == 0)
		return ("Successful return: 0");
	switch (error) {
	case WT_ERROR:
		return ("WT_ERROR: Non-specific error");
	default:
		if (error > 0 && (p = strerror(error)) != NULL)
			return (p);
		break;
	}

	/* 
	 * !!!
	 * Not thread-safe, but this is never supposed to happen.
	 */
	(void)snprintf(errbuf, sizeof(errbuf), "Unknown error: %d", error);
	return (errbuf);
}
