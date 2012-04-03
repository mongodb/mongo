/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_SCHEMA_FILENAME	"WiredTiger.wt"		/* Schema file name */
#define	WT_SCHEMA_URI		"file:WiredTiger.wt"	/* Schema file URI */

#define	WT_SCHEMA_TURTLE	"WiredTiger.turtle"	/* Schema information */
#define	WT_SCHEMA_TURTLE_SET	"WiredTiger.turtle.set"	/* Schema root temp */
#define	WT_SCHEMA_TURTLE_MSG	"The turtle."

#define	WT_SCHEMA_VERSION	"WiredTiger version"	/* Version keys */
#define	WT_SCHEMA_VERSION_STR	"WiredTiger version string"

/* Character constants for projection plans. */
#define	WT_PROJ_KEY	'k' /* Go to key in cursor <arg>. */
#define	WT_PROJ_NEXT	'n' /* Process the next item (<arg> repeats). */
#define	WT_PROJ_REUSE	'r' /* Reuse the previous item (<arg> repeats). */
#define	WT_PROJ_SKIP	's' /* Skip a column in the cursor (<arg> repeats). */
#define	WT_PROJ_VALUE	'v' /* Go to the value in cursor <arg>. */

/*
 * WT_TABLE --
 *	Handle for a logical table.  A table consists of one or more column
 *	groups, each of which holds some set of columns all sharing a primary
 *	key; and zero or more indices, each of which holds some set of columns
 *	in an index key that can be used to reconstruct the primary key.
 */
struct __wt_table {
	const char *name, *config, *plan;
	const char *key_format, *value_format;

	WT_CONFIG_ITEM cgconf, colconf;

	WT_BTREE **colgroup;
	WT_BTREE **index;
	size_t index_alloc;

	TAILQ_ENTRY(__wt_table) q;

	int cg_complete, idx_complete, is_simple;
	int ncolgroups, nindices, nkey_columns;
};

/*
 * Tables without explicit column groups have a single default column group
 * containing all of the columns.
 */
#define	WT_COLGROUPS(t)	WT_MAX((t)->ncolgroups, 1)
