/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

int
util_cerr(const char *uri, const char *op, int ret)
{
	return (util_err(ret, "%s: cursor.%s", uri, op));
}

/*
 * util_err --
 * 	Report an error.
 */
int
util_err(int e, const char *fmt, ...)
{
	va_list ap;

	(void)fprintf(stderr, "%s: ", progname);
	if (fmt != NULL) {
		va_start(ap, fmt);
		(void)vfprintf(stderr, fmt, ap);
		va_end(ap);
		if (e != 0)
			(void)fprintf(stderr, ": ");
	}
	if (e != 0)
		(void)fprintf(stderr, "%s", wiredtiger_strerror(e));
	(void)fprintf(stderr, "\n");
	return (1);
}

/*
 * util_read_line --
 *	Read a line from stdin into a ULINE.
 */
int
util_read_line(ULINE *l, int eof_expected, int *eofp)
{
	static uint64_t line = 0;
	size_t len;
	int ch;

	++line;
	*eofp = 0;

	if (l->memsize == 0) {
		if ((l->mem = realloc(l->mem, l->memsize + 1024)) == NULL)
			return (util_err(errno, NULL));
		l->memsize = 1024;
	}
	for (len = 0;; ++len) {
		if ((ch = getchar()) == EOF) {
			if (len == 0) {
				if (eof_expected) {
					*eofp = 1;
					return (0);
				}
				return (util_err(0,
				    "line %" PRIu64 ": unexpected end-of-file",
				    line));
			}
			return (util_err(0,
			    "line %" PRIu64 ": no newline terminator", line));
		}
		if (ch == '\n')
			break;
		/*
		 * We nul-terminate the string so it's easier to convert the
		 * line into a record number, that means we always need one
		 * extra byte at the end.
		 */
		if (len >= l->memsize - 1) {
			if ((l->mem =
			    realloc(l->mem, l->memsize + 1024)) == NULL)
				return (util_err(errno, NULL));
			l->memsize += 1024;
		}
		((uint8_t *)l->mem)[len] = (uint8_t)ch;
	}

	((uint8_t *)l->mem)[len] = '\0';		/* nul-terminate */

	return (0);
}

/*
 * util_str2recno --
 *	Convert a string to a record number.
 */
int
util_str2recno(const char *p, uint64_t *recnop)
{
	uint64_t recno;
	char *endptr;

	/*
	 * strtouq takes lots of things like hex values, signs and so on and so
	 * forth -- none of them are OK with us.  Check the string starts with
	 * digit, that turns off the special processing.
	 */
	if (!isdigit(p[0]))
		goto format;

	errno = 0;
	recno = __wt_strtouq(p, &endptr, 0);
	if (recno == ULLONG_MAX && errno == ERANGE)
		return (util_err(ERANGE, "%s: invalid record number", p));

	if (endptr[0] != '\0')
format:		return (util_err(EINVAL, "%s: invalid record number", p));

	*recnop = recno;
	return (0);
}

/*
 * util_flush --
 *	Flush the file successfully, or drop it.
 */
int
util_flush(WT_SESSION *session, const char *uri)
{
	WT_DECL_RET;
	size_t len;
	char *buf;

	len = strlen(uri) + 100;
	if ((buf = malloc(len)) == NULL)
		return (util_err(errno, NULL));

	(void)snprintf(buf, len, "target=(\"%s\")", uri);
	if ((ret = session->checkpoint(session, buf)) != 0) {
		ret = util_err(ret, "%s: session.checkpoint", uri);
		(void)session->drop(session, uri, NULL);
	}

	free(buf);
	return (ret);
}
