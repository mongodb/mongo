/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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

	WT_COLLATOR *collator;		/* Custom collator */
	int collator_owned;		/* Collator is owned by this index */

	WT_EXTRACTOR *extractor;	/* Custom key extractor */
	int extractor_owned;		/* Extractor is owned by this index */

	const char *key_format;		/* Key format */
	const char *key_plan;		/* Key projection plan */
	const char *value_plan;		/* Value projection plan */

	const char *idxkey_format;	/* Index key format (hides primary) */
	const char *exkey_format;	/* Key format for custom extractors */
#define	WT_INDEX_IMMUTABLE	0x01
	uint32_t    flags;		/* Index configuration flags */
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
	uint64_t name_hash;		/* Hash of name */

	WT_CONFIG_ITEM cgconf, colconf;

	WT_COLGROUP **cgroups;
	WT_INDEX **indices;
	size_t idx_alloc;

	TAILQ_ENTRY(__wt_table) q;
	TAILQ_ENTRY(__wt_table) hashq;

	bool cg_complete, idx_complete, is_simple;
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
 * WT_WITH_LOCK_WAIT --
 *	Wait for a lock, perform an operation, drop the lock.
 */
#define	WT_WITH_LOCK_WAIT(session, lock, flag, op) do {			\
	if (F_ISSET(session, (flag))) {					\
		op;							\
	}  else {							\
		__wt_spin_lock(session, (lock));			\
		F_SET(session, (flag));					\
		op;							\
		F_CLR(session, (flag));					\
		__wt_spin_unlock(session, (lock));			\
	}								\
} while (0)

/*
 * WT_WITH_LOCK --
 *	Acquire a lock, perform an operation, drop the lock.
 */
#define	WT_WITH_LOCK(session, ret, lock, flag, op) do {			\
	ret = 0;							\
	if (!F_ISSET(session, (flag)) &&				\
	    F_ISSET(session, WT_SESSION_LOCK_NO_WAIT)) {		\
		if ((ret = __wt_spin_trylock(session, (lock))) == 0) {	\
			F_SET(session, (flag));				\
			op;						\
			F_CLR(session, (flag));				\
			__wt_spin_unlock(session, (lock));		\
		}							\
	} else								\
		WT_WITH_LOCK_WAIT(session, lock, flag, op);		\
} while (0)

/*
 * WT_WITH_CHECKPOINT_LOCK --
 *	Acquire the checkpoint lock, perform an operation, drop the lock.
 */
#define	WT_WITH_CHECKPOINT_LOCK(session, ret, op)			\
	WT_WITH_LOCK(session, ret,					\
	    &S2C(session)->checkpoint_lock, WT_SESSION_LOCKED_CHECKPOINT, op)

/*
 * WT_WITH_HANDLE_LIST_LOCK --
 *	Acquire the data handle list lock, perform an operation, drop the lock.
 *
 *	Note: always waits because some operations need the handle list lock to
 *	discard handles, and we only expect it to be held across short
 *	operations.
 */
#define	WT_WITH_HANDLE_LIST_LOCK(session, op)				\
	WT_WITH_LOCK_WAIT(session, 					\
	    &S2C(session)->dhandle_lock, WT_SESSION_LOCKED_HANDLE_LIST, op)

/*
 * WT_WITH_METADATA_LOCK --
 *	Acquire the metadata lock, perform an operation, drop the lock.
 */
#define	WT_WITH_METADATA_LOCK(session, ret, op)				\
	WT_WITH_LOCK(session, ret,					\
	    &S2C(session)->metadata_lock, WT_SESSION_LOCKED_METADATA, op)

/*
 * WT_WITH_SCHEMA_LOCK --
 *	Acquire the schema lock, perform an operation, drop the lock.
 *	Check that we are not already holding some other lock: the schema lock
 *	must be taken first.
 */
#define	WT_WITH_SCHEMA_LOCK(session, ret, op) do {			\
	WT_ASSERT(session,						\
	    F_ISSET(session, WT_SESSION_LOCKED_SCHEMA) ||		\
	    !F_ISSET(session, WT_SESSION_LOCKED_HANDLE_LIST |		\
	    WT_SESSION_NO_SCHEMA_LOCK | WT_SESSION_LOCKED_TABLE));	\
	WT_WITH_LOCK(session, ret,					\
	    &S2C(session)->schema_lock, WT_SESSION_LOCKED_SCHEMA, op);	\
} while (0)

/*
 * WT_WITH_TABLE_LOCK --
 *	Acquire the table lock, perform an operation, drop the lock.
 */
#define	WT_WITH_TABLE_LOCK(session, ret, op) do {			\
	WT_ASSERT(session,						\
	    F_ISSET(session, WT_SESSION_LOCKED_TABLE) ||		\
	    !F_ISSET(session, WT_SESSION_LOCKED_HANDLE_LIST));		\
	WT_WITH_LOCK(session, ret,					\
	    &S2C(session)->table_lock, WT_SESSION_LOCKED_TABLE, op);	\
} while (0)

/*
 * WT_WITHOUT_LOCKS --
 *	Drop the handle, table and/or schema locks, perform an operation,
 *	re-acquire the lock(s).
 */
#define	WT_WITHOUT_LOCKS(session, op) do {				\
	WT_CONNECTION_IMPL *__conn = S2C(session);			\
	bool __checkpoint_locked =					\
	    F_ISSET(session, WT_SESSION_LOCKED_CHECKPOINT);		\
	bool __handle_locked =						\
	    F_ISSET(session, WT_SESSION_LOCKED_HANDLE_LIST);		\
	bool __table_locked =						\
	    F_ISSET(session, WT_SESSION_LOCKED_TABLE);			\
	bool __schema_locked =						\
	    F_ISSET(session, WT_SESSION_LOCKED_SCHEMA);			\
	if (__handle_locked) {						\
		F_CLR(session, WT_SESSION_LOCKED_HANDLE_LIST);		\
		__wt_spin_unlock(session, &__conn->dhandle_lock);	\
	}								\
	if (__table_locked) {						\
		F_CLR(session, WT_SESSION_LOCKED_TABLE);		\
		__wt_spin_unlock(session, &__conn->table_lock);		\
	}								\
	if (__schema_locked) {						\
		F_CLR(session, WT_SESSION_LOCKED_SCHEMA);		\
		__wt_spin_unlock(session, &__conn->schema_lock);	\
	}								\
	if (__checkpoint_locked) {					\
		F_CLR(session, WT_SESSION_LOCKED_CHECKPOINT);		\
		__wt_spin_unlock(session, &__conn->checkpoint_lock);	\
	}								\
	op;								\
	if (__checkpoint_locked) {					\
		__wt_spin_lock(session, &__conn->checkpoint_lock);	\
		F_SET(session, WT_SESSION_LOCKED_CHECKPOINT);		\
	}								\
	if (__schema_locked) {						\
		__wt_spin_lock(session, &__conn->schema_lock);		\
		F_SET(session, WT_SESSION_LOCKED_SCHEMA);		\
	}								\
	if (__table_locked) {						\
		__wt_spin_lock(session, &__conn->table_lock);		\
		F_SET(session, WT_SESSION_LOCKED_TABLE);		\
	}								\
	if (__handle_locked) {						\
		__wt_spin_lock(session, &__conn->dhandle_lock);		\
		F_SET(session, WT_SESSION_LOCKED_HANDLE_LIST);		\
	}								\
} while (0)
