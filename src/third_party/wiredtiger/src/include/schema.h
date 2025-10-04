/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/* Character constants for projection plans */
#define WT_PROJ_KEY 'k'   /* Go to key in cursor <arg> */
#define WT_PROJ_NEXT 'n'  /* Process the next item (<arg> repeats) */
#define WT_PROJ_REUSE 'r' /* Reuse the previous item (<arg> repeats) */
#define WT_PROJ_SKIP 's'  /* Skip a column in the cursor (<arg> repeats) */
#define WT_PROJ_VALUE 'v' /* Go to the value in cursor <arg> */

struct __wt_colgroup {
    const char *name;   /* Logical name */
    const char *source; /* Underlying data source */
    const char *config; /* Configuration string */

    WT_CONFIG_ITEM colconf; /* List of columns from config */
};

struct __wt_index {
    const char *name;   /* Logical name */
    const char *source; /* Underlying data source */
    const char *config; /* Configuration string */

    WT_CONFIG_ITEM colconf; /* List of columns from config */

    WT_COLLATOR *collator; /* Custom collator */
    int collator_owned;    /* Collator is owned by this index */

    const char *key_format; /* Key format */
    const char *key_plan;   /* Key projection plan */
    const char *value_plan; /* Value projection plan */

    const char *idxkey_format; /* Index key format (hides primary) */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_INDEX_IMMUTABLE 0x1u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags; /* Index configuration flags */
};

/*
 * WT_TABLE --
 *	Handle for a logical table.  A table consists of one or more column
 *	groups, each of which holds some set of columns all sharing a primary
 *	key; and zero or more indices, each of which holds some set of columns
 *	in an index key that can be used to reconstruct the primary key.
 */
struct __wt_table {
    WT_DATA_HANDLE iface;

    const char *plan;
    const char *key_format, *value_format;

    WT_CONFIG_ITEM cgconf, colconf;

    WT_COLGROUP **cgroups;
    WT_INDEX **indices;
    size_t idx_alloc;

    bool cg_complete, idx_complete, is_simple, is_tiered_shared;
    u_int ncolgroups, nindices, nkey_columns;
};

/*
 * WT_LAYERED_TABLE --
 *	Handle for a layered table.
 */
struct __wt_layered_table {
    WT_DATA_HANDLE iface;

    uint32_t ingest_btree_id;

    WT_COLLATOR *collator; /* Custom collator */
    int collator_owned;

    /*
     * For ingest table garbage collection, the last checkpoint generation number that we know was
     * in use.
     */
    int64_t last_ckpt_inuse;

    const char *key_format, *value_format;
    const char *ingest_uri, *stable_uri;
};

/* Holds metadata entry name and the associated config string. */
struct __wt_import_entry {

    const char *uri;    /* metadata key */
    const char *config; /* metadata value */

/* Invalid file id for import operation. It is used to sort import entries by file id. */
#define WT_IMPORT_INVALID_FILE_ID -1

    /*
     * Actual value of file ID is uint_32. We use int64_t here to store invalid file id that is
     * defined above.
     */
    int64_t file_id; /* id config value */
};

/* Array of metadata entries used when importing from a metadata file. */
struct __wt_import_list {
    const char *uri;        /* entries in the list will be related to this uri */
    const char *uri_suffix; /* suffix of the URI */

    size_t entries_allocated; /* allocated */
    size_t entries_next;      /* next slot */
    WT_IMPORT_ENTRY *entries; /* import metadata entries */
};

/*
 * Tables without explicit column groups have a single default column group containing all of the
 * columns except tiered shared table as it contains two column groups to represent active and
 * shared tables.
 */
#define WT_COLGROUPS(t) WT_MAX((t)->ncolgroups, (u_int)((t)->is_tiered_shared ? 2 : 1))

/* Helpers for the locked state of the handle list and table locks. */
#define WT_SESSION_LOCKED_HANDLE_LIST \
    (WT_SESSION_LOCKED_HANDLE_LIST_READ | WT_SESSION_LOCKED_HANDLE_LIST_WRITE)
#define WT_SESSION_LOCKED_TABLE (WT_SESSION_LOCKED_TABLE_READ | WT_SESSION_LOCKED_TABLE_WRITE)
#define WT_SESSION_LOCKED_HOTBACKUP \
    (WT_SESSION_LOCKED_HOTBACKUP_READ | WT_SESSION_LOCKED_HOTBACKUP_WRITE)

/*
 * WT_WITH_LOCK_WAIT --
 *	Wait for a lock, perform an operation, drop the lock.
 *  This macro lets us treat spinlocks as re-entrant. The session is only accessible by its owning
 *  thread so the session's lock flags can be considered as thread-local tracking for whether we're
 *  already inside the lock.
 *  Coverity will complain that two threads might modify the lock flags concurrently, but this isn't
 *  possible so those warnings can be ignored.
 */
#define WT_WITH_LOCK_WAIT(session, lock, flag, op)    \
    do {                                              \
        if (FLD_ISSET(session->lock_flags, (flag))) { \
            op;                                       \
        } else {                                      \
            __wt_spin_lock_track(session, lock);      \
            FLD_SET(session->lock_flags, (flag));     \
            op;                                       \
            FLD_CLR(session->lock_flags, (flag));     \
            __wt_spin_unlock(session, lock);          \
        }                                             \
    } while (0)

/*
 * WT_WITH_LOCK_NOWAIT --
 *	Acquire a lock if available, perform an operation, drop the lock.
 *  This macro lets us treat spinlocks as re-entrant. The session is only accessible by its owning
 *  thread so the session's lock flags can be considered as thread-local tracking for whether we're
 *  already inside the lock.
 *  Coverity will complain that two threads might modify the lock flags concurrently, but this isn't
 *  possible so those warnings can be ignored.
 */
#define WT_WITH_LOCK_NOWAIT(session, ret, lock_ret, lock, flag, op)              \
    do {                                                                         \
        (ret) = 0;                                                               \
        (lock_ret) = 0;                                                          \
        if (FLD_ISSET(session->lock_flags, (flag))) {                            \
            op;                                                                  \
        } else if (((lock_ret) = __wt_spin_trylock_track(session, lock)) == 0) { \
            FLD_SET(session->lock_flags, (flag));                                \
            op;                                                                  \
            FLD_CLR(session->lock_flags, (flag));                                \
            __wt_spin_unlock(session, lock);                                     \
        }                                                                        \
    } while (0)

/*
 * WT_WITH_CHECKPOINT_LOCK, WT_WITH_CHECKPOINT_LOCK_NOWAIT --
 *	Acquire the checkpoint lock, perform an operation, drop the lock.
 */
#define WT_WITH_CHECKPOINT_LOCK(session, op) \
    WT_WITH_LOCK_WAIT(session, &S2C(session)->checkpoint_lock, WT_SESSION_LOCKED_CHECKPOINT, op)
#define WT_WITH_CHECKPOINT_LOCK_NOWAIT(session, ret, op)                                         \
    do {                                                                                         \
        int __checkpoint_lock_ret;                                                               \
        WT_WITH_LOCK_NOWAIT(session, ret, __checkpoint_lock_ret, &S2C(session)->checkpoint_lock, \
          WT_SESSION_LOCKED_CHECKPOINT, op);                                                     \
        if (__checkpoint_lock_ret != 0) {                                                        \
            if (__checkpoint_lock_ret == EBUSY)                                                  \
                __wt_session_set_last_error(session, EBUSY, WT_CONFLICT_CHECKPOINT_LOCK,         \
                  "another thread is currently holding the checkpoint lock");                    \
            else                                                                                 \
                __wt_session_set_last_error(session, __checkpoint_lock_ret, WT_NONE,             \
                  "failed to acquire the checkpoint lock");                                      \
            ret = __checkpoint_lock_ret;                                                         \
        }                                                                                        \
    } while (0)

/*
 * WT_WITH_HANDLE_LIST_READ_LOCK --
 *	Acquire the data handle list lock in shared mode, perform an operation,
 *	drop the lock. The handle list lock is a read-write lock so the
 *	implementation is different to the other lock macros.
 *
 *	Note: always waits because some operations need the handle list lock to
 *	discard handles, and we only expect it to be held across short
 *	operations.
 */
#define WT_WITH_HANDLE_LIST_READ_LOCK(session, op)                            \
    do {                                                                      \
        if (FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST)) {  \
            op;                                                               \
        } else {                                                              \
            __wt_readlock(session, &S2C(session)->dhandle_lock);              \
            FLD_SET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST_READ); \
            op;                                                               \
            FLD_CLR(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST_READ); \
            __wt_readunlock(session, &S2C(session)->dhandle_lock);            \
        }                                                                     \
    } while (0)

/*
 * WT_WITH_HANDLE_LIST_WRITE_LOCK --
 *	Acquire the data handle list lock in exclusive mode, perform an
 *	operation, drop the lock. The handle list lock is a read-write lock so
 *	the implementation is different to the other lock macros.
 */
#define WT_WITH_HANDLE_LIST_WRITE_LOCK(session, op)                                          \
    do {                                                                                     \
        if (FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST_WRITE)) {           \
            op;                                                                              \
        } else {                                                                             \
            WT_ASSERT(                                                                       \
              session, !FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST_READ)); \
            __wt_writelock(session, &S2C(session)->dhandle_lock);                            \
            FLD_SET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST_WRITE);               \
            op;                                                                              \
            FLD_CLR(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST_WRITE);               \
            __wt_writeunlock(session, &S2C(session)->dhandle_lock);                          \
        }                                                                                    \
    } while (0)

/*
 * WT_WITH_METADATA_LOCK --
 *	Acquire the metadata lock, perform an operation, drop the lock.
 */
#define WT_WITH_METADATA_LOCK(session, op) \
    WT_WITH_LOCK_WAIT(session, &S2C(session)->metadata_lock, WT_SESSION_LOCKED_METADATA, op)

/*
 * WT_WITH_SCHEMA_LOCK, WT_WITH_SCHEMA_LOCK_NOWAIT --
 *	Acquire the schema lock, perform an operation, drop the lock.
 *	Check that we are not already holding some other lock: the schema lock
 *	must be taken first.
 */
#define WT_WITH_SCHEMA_LOCK(session, op)                                                      \
    do {                                                                                      \
        WT_ASSERT(session,                                                                    \
          FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SCHEMA) ||                         \
            !FLD_ISSET(session->lock_flags,                                                   \
              WT_SESSION_LOCKED_HANDLE_LIST | WT_SESSION_NO_SCHEMA_LOCK |                     \
                WT_SESSION_LOCKED_TABLE));                                                    \
        WT_WITH_LOCK_WAIT(session, &S2C(session)->schema_lock, WT_SESSION_LOCKED_SCHEMA, op); \
    } while (0)
#define WT_WITH_SCHEMA_LOCK_NOWAIT(session, ret, op)                                         \
    do {                                                                                     \
        WT_ASSERT(session,                                                                   \
          FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SCHEMA) ||                        \
            !FLD_ISSET(session->lock_flags,                                                  \
              WT_SESSION_LOCKED_HANDLE_LIST | WT_SESSION_NO_SCHEMA_LOCK |                    \
                WT_SESSION_LOCKED_TABLE));                                                   \
        int __schema_lock_ret;                                                               \
        WT_WITH_LOCK_NOWAIT(session, ret, __schema_lock_ret, &S2C(session)->schema_lock,     \
          WT_SESSION_LOCKED_SCHEMA, op);                                                     \
        if (__schema_lock_ret != 0) {                                                        \
            if (__schema_lock_ret == EBUSY)                                                  \
                __wt_session_set_last_error(session, EBUSY, WT_CONFLICT_SCHEMA_LOCK,         \
                  "another thread is currently holding the schema lock");                    \
            else                                                                             \
                __wt_session_set_last_error(                                                 \
                  session, __schema_lock_ret, WT_NONE, "failed to acquire the schema lock"); \
            ret = __schema_lock_ret;                                                         \
        }                                                                                    \
    } while (0)

/*
 * WT_WITH_TABLE_READ_LOCK, WT_WITH_TABLE_WRITE_LOCK,
 * WT_WITH_TABLE_WRITE_LOCK_NOWAIT --
 *	Acquire the table lock, perform an operation, drop the lock.
 *	The table lock is a read-write lock so the implementation is different
 *	to most other lock macros.
 *
 *	Note: readlock always waits because some operations need the table lock
 *	to discard handles, and we only expect it to be held across short
 *	operations.
 */
#define WT_WITH_TABLE_READ_LOCK(session, op)                                                    \
    do {                                                                                        \
        if (FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TABLE)) {                          \
            op;                                                                                 \
        } else {                                                                                \
            WT_ASSERT(session, !FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST)); \
            __wt_readlock(session, &S2C(session)->table_lock);                                  \
            FLD_SET(session->lock_flags, WT_SESSION_LOCKED_TABLE_READ);                         \
            op;                                                                                 \
            FLD_CLR(session->lock_flags, WT_SESSION_LOCKED_TABLE_READ);                         \
            __wt_readunlock(session, &S2C(session)->table_lock);                                \
        }                                                                                       \
    } while (0)

#define WT_WITH_TABLE_WRITE_LOCK(session, op)                                   \
    do {                                                                        \
        if (FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TABLE_WRITE)) {    \
            op;                                                                 \
        } else {                                                                \
            WT_ASSERT(session,                                                  \
              !FLD_ISSET(session->lock_flags,                                   \
                WT_SESSION_LOCKED_TABLE_READ | WT_SESSION_LOCKED_HANDLE_LIST)); \
            __wt_writelock(session, &S2C(session)->table_lock);                 \
            FLD_SET(session->lock_flags, WT_SESSION_LOCKED_TABLE_WRITE);        \
            op;                                                                 \
            FLD_CLR(session->lock_flags, WT_SESSION_LOCKED_TABLE_WRITE);        \
            __wt_writeunlock(session, &S2C(session)->table_lock);               \
        }                                                                       \
    } while (0)
#define WT_WITH_TABLE_WRITE_LOCK_NOWAIT(session, ret, op)                                          \
    do {                                                                                           \
        WT_ASSERT(session,                                                                         \
          FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TABLE_WRITE) ||                         \
            !FLD_ISSET(                                                                            \
              session->lock_flags, WT_SESSION_LOCKED_TABLE_READ | WT_SESSION_LOCKED_HANDLE_LIST)); \
        int __table_lock_ret = 0;                                                                  \
        if (FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TABLE_WRITE)) {                       \
            op;                                                                                    \
        } else if ((__table_lock_ret = __wt_try_writelock(session, &S2C(session)->table_lock)) ==  \
          0) {                                                                                     \
            FLD_SET(session->lock_flags, WT_SESSION_LOCKED_TABLE_WRITE);                           \
            op;                                                                                    \
            FLD_CLR(session->lock_flags, WT_SESSION_LOCKED_TABLE_WRITE);                           \
            __wt_writeunlock(session, &S2C(session)->table_lock);                                  \
        }                                                                                          \
        if (__table_lock_ret != 0) {                                                               \
            WT_ASSERT(session, __table_lock_ret == EBUSY);                                         \
            __wt_session_set_last_error(session, EBUSY, WT_CONFLICT_TABLE_LOCK,                    \
              "another thread is currently holding the table lock");                               \
            ret = __table_lock_ret;                                                                \
        }                                                                                          \
    } while (0)

/*
 * WT_WITH_HOTBACKUP_READ_INT --
 *	Acquire the hot backup read lock and perform an operation provided that
 *	the backup state is in the correct state.  The skipp parameter can be used to
 *	check whether the operation got skipped or not.
 */
#define WT_WITH_HOTBACKUP_READ_INT(session, op, bk_off, skipp)                    \
    do {                                                                          \
        WT_CONNECTION_IMPL *__conn = S2C(session);                                \
        if ((skipp) != (bool *)NULL)                                              \
            *(bool *)(skipp) = true;                                              \
        if (FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HOTBACKUP)) {        \
            if ((__wt_atomic_load64(&__conn->hot_backup_start) == 0) == bk_off) { \
                if ((skipp) != (bool *)NULL)                                      \
                    *(bool *)(skipp) = false;                                     \
                op;                                                               \
            }                                                                     \
        } else {                                                                  \
            __wt_readlock(session, &__conn->hot_backup_lock);                     \
            FLD_SET(session->lock_flags, WT_SESSION_LOCKED_HOTBACKUP_READ);       \
            if ((__wt_atomic_load64(&__conn->hot_backup_start) == 0) == bk_off) { \
                if ((skipp) != (bool *)NULL)                                      \
                    *(bool *)(skipp) = false;                                     \
                op;                                                               \
            }                                                                     \
            FLD_CLR(session->lock_flags, WT_SESSION_LOCKED_HOTBACKUP_READ);       \
            __wt_readunlock(session, &__conn->hot_backup_lock);                   \
        }                                                                         \
    } while (0)

/*
 * WT_WITH_HOTBACKUP_READ_LOCK --
 *	Acquire the hot backup read lock and perform an operation provided that
 *	there is no hot backup in progress.
 */
#define WT_WITH_HOTBACKUP_READ_LOCK(session, op, skipp) \
    WT_WITH_HOTBACKUP_READ_INT(session, op, true, skipp);

/*
 * WT_WITH_HOTBACKUP_READ_LOCK_BACKUP --
 *	Acquire the hot backup read lock and perform an operation provided that
 *	there is a hot backup in progress.
 */
#define WT_WITH_HOTBACKUP_READ_LOCK_BACKUP(session, op, skipp) \
    WT_WITH_HOTBACKUP_READ_INT(session, op, false, skipp);

/*
 * WT_WITH_HOTBACKUP_READ_LOCK_UNCOND --
 *	Acquire the hot backup read lock and perform an operation
 *	unconditionally.  This is a specialized macro for a few isolated cases.
 *	Code that wishes to acquire the read lock should default to using
 *	WT_WITH_HOTBACKUP_READ_LOCK which checks that there is no hot backup in
 *	progress.
 */
#define WT_WITH_HOTBACKUP_READ_LOCK_UNCOND(session, op)                     \
    do {                                                                    \
        WT_CONNECTION_IMPL *__conn = S2C(session);                          \
        if (FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HOTBACKUP)) {  \
            op;                                                             \
        } else {                                                            \
            __wt_readlock(session, &__conn->hot_backup_lock);               \
            FLD_SET(session->lock_flags, WT_SESSION_LOCKED_HOTBACKUP_READ); \
            op;                                                             \
            FLD_CLR(session->lock_flags, WT_SESSION_LOCKED_HOTBACKUP_READ); \
            __wt_readunlock(session, &__conn->hot_backup_lock);             \
        }                                                                   \
    } while (0)

/*
 * WT_WITH_HOTBACKUP_WRITE_LOCK --
 *	Acquire the hot backup write lock and perform an operation.
 */
#define WT_WITH_HOTBACKUP_WRITE_LOCK(session, op)                                                  \
    do {                                                                                           \
        WT_CONNECTION_IMPL *__conn = S2C(session);                                                 \
        if (FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HOTBACKUP_WRITE)) {                   \
            op;                                                                                    \
        } else {                                                                                   \
            WT_ASSERT(session, !FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HOTBACKUP_READ)); \
            __wt_writelock(session, &__conn->hot_backup_lock);                                     \
            FLD_SET(session->lock_flags, WT_SESSION_LOCKED_HOTBACKUP_WRITE);                       \
            op;                                                                                    \
            FLD_CLR(session->lock_flags, WT_SESSION_LOCKED_HOTBACKUP_WRITE);                       \
            __wt_writeunlock(session, &__conn->hot_backup_lock);                                   \
        }                                                                                          \
    } while (0)
