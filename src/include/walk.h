/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#if defined(__cplusplus)
extern "C" {
#endif

struct __wt_walk_entry {
	WT_PAGE	*page;		/* Page being traversed */
	uint32_t indx;		/* Not-yet-visited slot on the page */
	int	 visited;	/* If page itself been visited */

	WT_PAGE *hazard;	/* Last page returned -- has hazard reference */
};

struct __wt_walk {
	WT_WALK_ENTRY *tree;

	uint32_t tree_len;	/* Tree stack in bytes */
	u_int	 tree_slot;	/* Current tree stack slot */

	uint32_t flags;		/* Flags specified for the walk */
};

#if defined(__cplusplus)
}
#endif
