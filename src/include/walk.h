/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#if defined(__cplusplus)
extern "C" {
#endif

struct __wt_walk;		typedef struct __wt_walk WT_WALK;
struct __wt_walk_entry;		typedef struct __wt_walk_entry WT_WALK_ENTRY;

struct __wt_walk_entry {
	WT_REF	*ref;		/* Page reference */
	uint32_t indx;		/* Not-yet-visited slot on the page */
	int	 visited;	/* If page itself been visited */
};

struct __wt_walk {
	WT_WALK_ENTRY *tree;
	u_int	tree_len;	/* Tree stack in bytes */
	u_int	tree_slot;	/* Current tree stack slot */
};

#if defined(__cplusplus)
}
#endif
