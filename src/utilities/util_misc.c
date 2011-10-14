/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wiredtiger.h"
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
	static unsigned long long line = 0;
	uint32_t len;
	int ch;

	++line;
	*eofp = 0;

	for (len = 0;; ++len) {
		if ((ch = getchar()) == EOF) {
			if (len == 0) {
				if (eof_expected) {
					*eofp = 1;
					return (0);
				}
				return (util_err(0, 
				    "line %llu: unexpected end-of-file", line));
			}
			return (util_err(0,
			    "line %llu: no newline terminator", line));
		}
		if (ch == '\n')
			break;
		/*
		 * We nul-terminate the string so it's easier to convert the
		 * line into a record number, that means we always need one
		 * extra byte at the end.
		 */
		if (l->memsize == 0 || len >= l->memsize - 1) {
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
