/* DO NOT EDIT: automatically built by dist/api_err.py. */

#include "wt_internal.h"

/*
 * Historically, there was only the wiredtiger_strerror call because the POSIX
 * port didn't need anything more complex; Windows requires memory allocation
 * of error strings, so we added the wiredtiger_strerror_r call. Because we
 * want wiredtiger_strerror to continue to be as thread-safe as possible, errors
 * are split into three categories: WiredTiger constant strings, system constant
 * strings and Everything Else, and we check constant strings before Everything
 * Else.
 */

/*
 * __wiredtiger_error --
 *	Return a constant string for the WiredTiger errors.
 */
static const char *
__wiredtiger_error(int error)
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
	if ((p = __wiredtiger_error(error)) != NULL ||
	    (p = __wt_strerror(error)) != NULL)
		return (p);

	/* Else, fill in the non-thread-safe static buffer. */
	if (wiredtiger_strerror_r(error, buf, sizeof(buf)) != 0)
		(void)snprintf(buf, sizeof(buf), "error return: %d", error);

	return (buf);
}

/*
 * wiredtiger_strerror_r --
 *	Return a string for any error value, thread-safe version.
 */
int
wiredtiger_strerror_r(int error, char *buf, size_t buflen)
{
	const char *p;

	/* Require at least 2 bytes, printable character and trailing nul. */
	if (buflen < 2)
		return (ENOMEM);

	/* Check for a constant string. */
	if ((p = __wiredtiger_error(error)) != NULL ||
	    (p = __wt_strerror(error)) != NULL)
		return (snprintf(buf, buflen, "%s", p) > 0 ? 0 : ENOMEM);

	return (__wt_strerror_r(error, buf, buflen));
}
