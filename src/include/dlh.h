/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

struct __wt_dlh {
	TAILQ_ENTRY(__wt_dlh) q;		/* List of open libraries. */

	void	*handle;			/* Handle returned by dlopen. */
	char	*name;
};
