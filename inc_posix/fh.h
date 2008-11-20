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

struct __wt_fh_t {
	char	*name;				/* File name */
	int	 fd;				/* POSIX file handle */

	TAILQ_ENTRY(__wt_fh_t) q;		/* List of open handles */

	WT_STAT_DECL(read_count);		/* Statistics */
	WT_STAT_DECL(write_count);
};
	
#if defined(__cplusplus)
}
#endif
