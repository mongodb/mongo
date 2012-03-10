/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

struct __wt_cursor_lsm {
	WT_CURSOR iface;

	WT_LSM_TREE *lsmtree;
	WT_CURSOR **cursors;
};

struct __wt_lsm_tree {
	const char *name;
	const char *key_format, *value_format;

	WT_RWLOCK *rwlock;
	uint64_t dsk_gen;

	TAILQ_ENTRY(__wt_lsm_tree) q;

	int chunks;		/* Number of active chunks. */
	const char **chunk;	/* Array of chunk URIs. */
};

struct __wt_lsm_data_source {
	WT_DATA_SOURCE iface;

	WT_RWLOCK *rwlock;

	TAILQ_HEAD(__trees, __wt_lsm_tree) trees;
};
