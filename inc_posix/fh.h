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

#define	WT_OPEN_CREATE	0x01			/* Create the file */

struct __wt_fh {
	TAILQ_ENTRY(__wt_fh) q;			/* List of open handles */

	char	*name;				/* File name */
	int	 fd;				/* POSIX file handle */

	WT_STATS *stats;			/* Statistics */
};
	
#if defined(__cplusplus)
}
#endif
