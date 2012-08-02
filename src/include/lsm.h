/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

struct __wt_cursor_lsm {
	WT_CURSOR iface;

	WT_LSM_TREE *lsmtree;
	WT_CURSOR *current;     /* The current cursor for iteration. */
	WT_CURSOR **cursors;

#define	WT_CLSM_MULTIPLE        0x01    /* Multiple cursors have values for the
					   current key. */
#define	WT_CLSM_ITERATE_NEXT    0x02    /* Forward iteration. */
#define	WT_CLSM_ITERATE_PREV    0x04    /* Backward iteration. */
	uint32_t flags;
};

struct __wt_lsm_tree {
	const char *name, *filename;
	const char *key_format, *value_format;

	WT_COLLATOR *collator;

	WT_RWLOCK *rwlock;
	uint64_t dsk_gen;

	TAILQ_ENTRY(__wt_lsm_tree) q;

	int nchunks;		/* Number of active chunks. */
	const char **chunk;	/* Array of chunk URIs. */
};

struct __wt_lsm_data_source {
	WT_DATA_SOURCE iface;

	WT_RWLOCK *rwlock;

	TAILQ_HEAD(__trees, __wt_lsm_tree) trees;
};
