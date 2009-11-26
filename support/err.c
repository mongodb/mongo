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
 * __wt_msg_call --
 *	Pass a message to a callback function.
 */
void
__wt_msg_call(void *cb, void *handle,
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
	s[0] = '\0';
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

	/*lint -e611
	 *
	 * lint worries about this cast.
	 */
	((void (*)(void *, const char *))cb)(handle, s);
}

/*
 * __wt_msg_stream --
 *	Write a message to a FILE stream.
 */
void
__wt_msg_stream(FILE *fp,
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
__wt_assert(ENV *env, const char *check, const char *file_name, int line_number)
{
	__wt_api_env_errx(env,
	    "assertion failure: %s/%d: \"%s\"", file_name, line_number, check);

	__wt_abort();
	/* NOTREACHED */
}
#endif

/*
 * __wt_api_flags --
 *	Print a standard error message when an API function is passed illegal
 *	flags.
 */
int
__wt_api_flags(ENV *env, const char *name)
{
	__wt_api_env_errx(env, "%s: illegal API flag specified", name);
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
 * __wt_wt_toc_lockout --
 *	Standard WT_TOC handle lockout error message.
 */
int
__wt_wt_toc_lockout(WT_TOC *toc)
{
	return (__wt_env_lockout(toc->env));
}

/*
 * __wt_db_lockout --
 *	Standard DB handle lockout error message.
 */
int
__wt_db_lockout(DB *db)
{
	return (__wt_env_lockout(db->env));
}

/*
 * __wt_env_lockout --
 *	Standard ENV handle lockout error message.
 */
int
__wt_env_lockout(ENV *env)
{
	__wt_api_env_errx(env,
	    "This handle method is not available for some reason: for example, "
	    "handle methods are restricted after an error, or configuration "
	    "methods may be restricted after the database or environment have "
	    "been opened.");
	return (WT_ERROR);
}
