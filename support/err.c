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
 * __wt_errcall --
 *	Pass an error message to a callback function.
 */
void
__wt_errcall(void *cb, void *handle,
    const char *pfx1, const char *pfx2,
    int error, const char *fmt, va_list ap)
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
	if (pfx1 != NULL) {
		len += (size_t)snprintf(s + len, sizeof(s) - len, "%s", pfx1);
		separator = 1;
	}
	if (pfx2 != NULL && len < sizeof(s) - 1) {
		len += (size_t)snprintf(s + len, sizeof(s) - len,
		    "%s%s", separator ? ": " : "", pfx2);
		separator = 1;
	}
	if (separator && len < sizeof(s) - 1)
		len += (size_t)snprintf(s + len, sizeof(s) - len, ": ");
	if (len < sizeof(s) - 1)
		len += (size_t)vsnprintf(s + len, sizeof(s) - len, fmt, ap);
	if (error != 0 && len < sizeof(s) - 1)
		(void)snprintf(
		    s + len, sizeof(s) - len, ": %s", wt_strerror(error));

	((void (*)(void *, const char *))cb)(handle, s);
}

/*
 * __wt_errfile --
 *	Write an error message to a FILE stream.
 */
void
__wt_errfile(FILE *fp,
    const char *pfx1, const char *pfx2, int error, const char *fmt, va_list ap)
{
	if (fp == NULL)
		fp = stderr;

	if (pfx1 != NULL)
		(void)fprintf(fp, "%s: ", pfx1);
	if (pfx2 != NULL)
		(void)fprintf(fp, "%s: ", pfx2);
	(void)vfprintf(fp, fmt, ap);
	if (error != 0)
		(void)fprintf(fp, ": %s", wt_strerror(error));
	(void)fprintf(fp, "\n");
	(void)fflush(fp);
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
 * __wt_database_format --
 *	Print a standard error message when a database format error is
 *	suddenly discovered.
 */
int
__wt_database_format(DB *db)
{
	__wt_db_errx(db, "the database is corrupted; use the Db.salvage"
	    " method or the db_salvage utility to repair the database");
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
