/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/* The fixed name of the schema file. */
#define	WT_SCHEMA_FILENAME	"__schema.wt"

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
	const char *name, *config;
	const char *key_format, *value_format;
	const char *plan;

	WT_CONFIG_ITEM cgconf, colconf;

	int is_complete, is_simple;
	int ncolgroups, nindices, nkey_columns;

	WT_BTREE **colgroup;
	WT_BTREE **index;

	TAILQ_ENTRY(__wt_table) q;
};

