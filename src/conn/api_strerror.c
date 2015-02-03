/* DO NOT EDIT: automatically built by dist/api_err.py. */

#include "wt_internal.h"

/*
 * Historically, there was only the wiredtiger_strerror call because the POSIX
 * port didn't need anything more complex; Windows requires memory allocation
 * of error strings, so we added the WT_SESSION.strerror method. Because we
 * want wiredtiger_strerror to continue to be as thread-safe as possible, errors
 * are split into three categories: WiredTiger constant strings, system constant
 * strings and Everything Else, and we check constant strings before Everything
 * Else.
 */

/*
 * __wiredtiger_error --
 *	Return a constant string for the WiredTiger errors.
 */
const char *
__wt_wiredtiger_error(int error)
{
	switch (error) {
	case WT_ROLLBACK:
		return ("WT_ROLLBACK: conflict between concurrent operations");
	case WT_DUPLICATE_KEY:
		return ("WT_DUPLICATE_KEY: attempt to insert an existing key");
	case WT_ERROR:
		return ("WT_ERROR: non-specific WiredTiger error");
	case WT_NOTFOUND:
		return ("WT_NOTFOUND: item not found");
	case WT_PANIC:
		return ("WT_PANIC: WiredTiger library panic");
	case WT_RESTART:
		return ("WT_RESTART: restart the operation (internal)");
	}
	return (NULL);
}

/*
 * wiredtiger_strerror --
 *	Return a string for any error value, non-thread-safe version.
 */
const char *
wiredtiger_strerror(int error)
{
	static char buf[128];
	const char *p;

	/* Check for a constant string. */
	if ((p = __wt_wiredtiger_error(error)) != NULL)
		return (p);
	if ((p = __wt_strerror(error)) != NULL)
		return (p);

	/* Else, fill in the non-thread-safe static buffer. */
	if (snprintf(buf, sizeof(buf), "error return: %d", error) > 0)
		return (buf);

	/* OK, we're done. */
	return ("Unable to return error string");
}
