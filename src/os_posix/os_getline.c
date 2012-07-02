/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_getline --
 *	Get a line from a stream.
 *
 * Implementation of the POSIX getline or BSD fgetln functions (finding the
 * function in a portable way is hard, it's simple enough to write it instead).
 *
 * Note: Unlike the standard getline calls, this function doesn't include the
 * trailing newline character in the returned buffer and discards empty lines
 * (so the caller's EOF marker is a returned line length of 0).
 */
int
__wt_getline(WT_SESSION_IMPL *session,
    char **bufp, size_t *buflenp, size_t *lenp, FILE *fp)
{
	size_t len;
	int c;

	if (*buflenp == 0)	/* A length of 0 implies buffer allocation */
		*bufp = NULL;

	for (len = 0; (c = fgetc(fp)) != EOF;) {
		if (len >= *buflenp)
			WT_RET(__wt_realloc(
			    session, buflenp, len + 1024, bufp));
		if (c == '\n') {
			if (len == 0)
				continue;
			break;
		}
		(*bufp)[len++] = (char)c;
	}
	if (c == EOF && ferror(fp))
		return (__wt_errno());

	(*bufp)[len] = '\0';
	*lenp = len;

	return (0);
}
