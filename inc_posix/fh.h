/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

struct __wt_fh {
	TAILQ_ENTRY(__wt_fh) q;			/* List of open handles */

	off_t	file_size;			/* File size */

	char	*name;				/* File name */
	int	fd;				/* POSIX file handle */

	u_int	refcnt;				/* Reference count */

	WT_STATS *stats;			/* Statistics */
};

#if defined(__cplusplus)
}
#endif
