/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/* Character constants for projection plans */
#define	WT_PROJ_KEY	'k' /* Go to key in cursor <arg> */
#define	WT_PROJ_NEXT	'n' /* Process the next item (<arg> repeats) */
#define	WT_PROJ_REUSE	'r' /* Reuse the previous item (<arg> repeats) */
#define	WT_PROJ_SKIP	's' /* Skip a column in the cursor (<arg> repeats) */
#define	WT_PROJ_VALUE	'v' /* Go to the value in cursor <arg> */

struct __wt_colgroup {
	const char *name;		/* Logical name */
	const char *source;		/* Underlying data source */
	const char *config;		/* Configuration string */

	WT_CONFIG_ITEM colconf;		/* List of columns from config */
};

struct __wt_index {
	const char *name;		/* Logical name */
	const char *source;		/* Underlying data source */
	const char *config;		/* Configuration string */

	WT_CONFIG_ITEM colconf;		/* List of columns from config */

	WT_EXTRACTOR *extractor;	/* Custom key extractor */
	int extractor_owned;		/* Extractor is owned by this index */

	const char *key_format;		/* Key format */
	const char *key_plan;		/* Key projection plan */
	const char *value_plan;		/* Value projection plan */

	const char *idxkey_format;	/* Index key format (hides primary) */
	const char *exkey_format;	/* Key format for custom extractors */
};

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

	WT_COLGROUP **cgroups;
	WT_INDEX **indices;
	size_t idx_alloc;

	TAILQ_ENTRY(__wt_table) q;

	int cg_complete, idx_complete, is_simple;
	u_int ncolgroups, nindices, nkey_columns;

	uint32_t refcnt;	/* Number of open cursors */
	uint32_t schema_gen;	/* Cached schema generation number */
};

/*
 * Tables without explicit column groups have a single default column group
 * containing all of the columns.
 */
#define	WT_COLGROUPS(t)	WT_MAX((t)->ncolgroups, 1)

/*
 * WT_WITH_SCHEMA_LOCK --
 *	Acquire the schema lock, perform an operation, drop the lock.
 */
#define	WT_WITH_SCHEMA_LOCK(session, op) do {				\
	WT_ASSERT(session,						\
	    F_ISSET(session, WT_SESSION_SCHEMA_LOCKED) ||		\
	    !F_ISSET(session, WT_SESSION_NO_SCHEMA_LOCK));		\
	if (F_ISSET(session, WT_SESSION_SCHEMA_LOCKED)) {		\
		(op);							\
	} else {							\
		__wt_spin_lock(session, &S2C(session)->schema_lock);	\
		F_SET(session, WT_SESSION_SCHEMA_LOCKED);		\
		(op);							\
		__wt_spin_unlock(session, &S2C(session)->schema_lock);	\
		F_CLR(session, WT_SESSION_SCHEMA_LOCKED);		\
	}								\
} while (0)

/*
 * WT_WITHOUT_SCHEMA_LOCK --
 *	Drop the schema lock, perform an operation, re-acquire the lock.
 */
#define	WT_WITHOUT_SCHEMA_LOCK(session, op) do {			\
	if (F_ISSET(session, WT_SESSION_SCHEMA_LOCKED)) {		\
		__wt_spin_unlock(session, &S2C(session)->schema_lock);	\
		F_CLR(session, WT_SESSION_SCHEMA_LOCKED);		\
		(op);							\
		__wt_spin_lock(session, &S2C(session)->schema_lock);	\
		F_SET(session, WT_SESSION_SCHEMA_LOCKED);		\
	} else {							\
		(op);							\
	}								\
} while (0)
