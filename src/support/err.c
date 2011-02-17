/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
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
		(void)snprintf(s + len,
		    sizeof(s) - len, ": %s", wiredtiger_strerror(error));

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
		(void)fprintf(fp, ": %s", wiredtiger_strerror(error));
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

	__wt_abort(env);
	/* NOTREACHED */
}
#endif

/*
 * __wt_api_args --
 *	Print a standard error message when an API function is passed illegal
 *	arguments.
 */
int
__wt_api_args(ENV *env, const char *name)
{
	__wt_api_env_errx(env,
	    "%s: illegal API arguments or flag values specified", name);
	return (WT_ERROR);
}

/*
 * __wt_api_arg_min --
 *	Print a standard error message when an API function is passed a
 *	too-small argument.
 */
int
__wt_api_arg_min(ENV *env,
    const char *name, const char *arg_name, uint32_t v, uint32_t min)
{
	if (v >= min)
		return (0);

	__wt_api_env_errx(env,
	    "%s: %s argument less than minimum value of %lu",
	    name, arg_name, (u_long)min);
	return (WT_ERROR);
}

/*
 * __wt_api_arg_max --
 *	Print a standard error message when an API function is passed a
 *	too-large argument.
 */
int
__wt_api_arg_max(ENV *env,
    const char *name, const char *arg_name, uint32_t v, uint32_t max)
{
	if (v <= max)
		return (0);

	__wt_api_env_errx(env,
	    "%s: %s argument larger than maximum value of %lu",
	    name, arg_name, (u_long)max);
	return (WT_ERROR);
}

/*
 * __wt_file_method_type --
 *	Print a standard error message on attempts to call methods inappropriate
 *	for a file type.
 */
int
__wt_file_method_type(DB *db, const char *name, int column_err)
{
	__wt_api_db_errx(db,
	    "%s: this method is not supported for a %s file",
	    name, column_err ? "column-store" : "row-store");
	return (WT_ERROR);
}

/*
 * __wt_file_wrong_fixed_size --
 *	Print a standard error message on attempts to put the wrong size element
 *	into a fixed-size file.
 */
int
__wt_file_wrong_fixed_size(WT_TOC *toc, uint32_t len)
{
	DB *db;

	db = toc->db;

	__wt_api_db_errx(db,
	    "%s: length of %lu does not match fixed-length file configuration "
	    "of %lu",
	     toc->name, (u_long)len, (u_long)db->fixed_len);
	return (WT_ERROR);
}

/*
 * __wt_file_readonly --
 *	Print a standard error message on attempts to modify a read-only file.
 */
int
__wt_file_readonly(DB *db, const char *name)
{
	__wt_api_db_errx(db,
	    "%s: the file was opened read-only and may not be modified", name);
	return (WT_READONLY);
}

/*
 * __wt_file_format --
 *	Print a standard error message when a file format error is suddenly
 *	discovered.
 */
int
__wt_file_format(DB *db)
{
	__wt_api_db_errx(db, "the file is corrupted; use the Db.salvage"
	    " method or the db_salvage utility to repair the file");
	return (WT_ERROR);
}

/*
 * __wt_file_item_too_big --
 *	Print a standard error message when an element is too large to store.
 */
int
__wt_file_item_too_big(DB *db)
{
	__wt_api_db_errx(db, "the item is too large for the file to store");
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
	    "An unavailable handle method was called; the handle method is "
	    "not available for some reason, for example, handle methods are "
	    "restricted after an error, or configuration methods may be "
	    "restricted after the file or environment have been opened, "
	    "or operational methods may be restricted until the file or "
	    "environment has been opened.");
	return (WT_ERROR);
}
