/* DO NOT EDIT: automatically built by dist/api_err.py. */

#include "wt_internal.h"

/*
 * wiredtiger_strerror --
 *	Return a string for any error value.
 */
const char *
wiredtiger_strerror(int error)
{
	static char errbuf[64];
	char *p;

	if (error == 0)
		return ("Successful return: 0");

	switch (error) {
	case WT_DEADLOCK:
		return ("WT_DEADLOCK: conflict with concurrent operation");
	case WT_ERROR:
		return ("WT_ERROR: non-specific WiredTiger error");
	case WT_NOTFOUND:
		return ("WT_NOTFOUND: item not found");
	case WT_PAGE_DELETED:
		return ("WT_PAGE_DELETED: requested page was deleted");
	case WT_READONLY:
		return ("WT_READONLY: attempt to modify a read-only value");
	case WT_RESTART:
		return ("WT_RESTART: restart the operation (internal)");
	case WT_TOOSMALL:
		return ("WT_TOOSMALL: buffer too small");
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
