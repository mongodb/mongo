/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

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

#define	WT_ERR(env, name, error, fmt) {					\
	va_list __ap;							\
									\
	/* Application-specified callback function. */			\
	va_start(__ap, fmt);						\
	if ((env)->errcall != NULL)					\
		__wt_errcall(env, name, error, fmt, __ap);		\
	va_end(__ap);							\
									\
	/*								\
	 * If the application set an error callback function but not an	\
	 * error stream, we're done.  Otherwise, write an error	stream.	\
	 */								\
	if ((env)->errcall != NULL && !F_ISSET(env, WT_ENV_SET_ERRFILE))\
			return;						\
									\
	va_start(__ap, fmt);						\
	__wt_errfile(env, name, error, fmt, __ap);			\
	va_end(__ap);							\
}

/*
 * __wt_err --
 *	Standard error routine, with error number.
 */
void
__wt_err(IENV *ienv, int error, const char *fmt, ...)
{
	WT_ERR(ienv->env, NULL, error, fmt);
}

/*
 * __wt_errx --
 *	Standard error routine, without error number.
 */
void
__wt_errx(IENV *ienv, const char *fmt, ...)
{
	WT_ERR(ienv->env, NULL, 0, fmt);
}

/*
 * __wt_errcall --
 *	Pass an error message to a callback function.
 */
void
__wt_errcall(ENV *env, const char *name, int error, const char *fmt, va_list ap)
{
	size_t len;
	int separator;

	/*
	 * !!!
	 * SECURITY:
	 * Buffer placed at the end of the stack in case snprintf overflows.
	 */
	char s[2048];

	len = 0;
	separator = 0;
	if (env->errpfx != NULL) {
		len += snprintf(s + len, sizeof(s) - len, "%s", env->errpfx);
		separator = 1;
	}
	if (name != NULL && len < sizeof(s) - 1) {
		len += snprintf(s + len, sizeof(s) - len,
		    "%s%s", separator ? ": " : "", name);
		separator = 1;
	}
	if (separator && len < sizeof(s) - 1)
		len += snprintf(s + len, sizeof(s) - len,
		    "%s%s", separator ? ": " : "", name);
	if (len < sizeof(s) - 1)
		len += vsnprintf(s + len, sizeof(s) - len, fmt, ap);
	if (error != 0 && len < sizeof(s) - 1)
		snprintf(s + len, sizeof(s) - len,
		    "%s%s", separator ? ": " : "", wt_strerror(error));

	env->errcall(env, s);
}

/*
 * __wt_errfile --
 *	Write an error message to a FILE stream.
 */
void
__wt_errfile(ENV *env, const char *name, int error, const char *fmt, va_list ap)
{
	FILE *errfp;

	errfp = env->errfile == NULL ? stderr : env->errfile;

	if (env->errpfx != NULL) {
		(void)fprintf(errfp, "%s: ", env->errpfx);
	}
	(void)vfprintf(errfp, fmt, ap);
	if (error != 0)
		(void)fprintf(errfp, ": %s", wt_strerror(error));
	(void)fprintf(errfp, "\n");
	(void)fflush(errfp);
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
	__wt_errx(ienv,
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
	__wt_errx(ienv, "%s: illegal API flag specified", name);
	return (WT_ERROR);
}
