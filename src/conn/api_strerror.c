/* DO NOT EDIT: automatically built by dist/api_err.py. */

#include "wt_internal.h"

/*
 * wiredtiger_strerror --
 *	Return a string for any error value in a static buffer.
 */
const char *
wiredtiger_strerror(int error)
{
	static char buf[128];

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

	switch (error) {
	case 0:
		p = "Successful return: 0";
		break;
	case WT_ROLLBACK:
		p = "WT_ROLLBACK: conflict between concurrent operations";
		break;
	case WT_DUPLICATE_KEY:
		p = "WT_DUPLICATE_KEY: attempt to insert an existing key";
		break;
	case WT_ERROR:
		p = "WT_ERROR: non-specific WiredTiger error";
		break;
	case WT_NOTFOUND:
		p = "WT_NOTFOUND: item not found";
		break;
	case WT_PANIC:
		p = "WT_PANIC: WiredTiger library panic";
		break;
	case WT_RESTART:
		p = "WT_RESTART: restart the operation (internal)";
		break;
	default:
		return (__wt_strerror_r(error, buf, buflen));
	}

	/*
	 * Return success if anything printed (we checked if the buffer had
	 * space for at least one character).
	 */
	return (snprintf(buf, buflen, "%s", p) > 0 ? 0 : ENOMEM);
}
