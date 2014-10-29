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
	case WT_ROLLBACK:
		return ("WT_ROLLBACK: conflict between concurrent operations");
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
