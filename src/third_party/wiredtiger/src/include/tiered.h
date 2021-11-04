/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_TIERED_MANAGER --
 *	A structure that holds resources used to manage any tiered storage
 *	for the whole database.
 */
struct __wt_tiered_manager {
    uint64_t wait_usecs; /* Wait time period */
    uint32_t workers;    /* Current number of workers */
    uint32_t workers_max;
    uint32_t workers_min;

#define WT_TIERED_MAX_WORKERS 20
#define WT_TIERED_MIN_WORKERS 1

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TIERED_MANAGER_SHUTDOWN 0x1u /* Manager has shut down */
                                        /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/*
 * Define the maximum number of tiers for convenience. We expect at most two initially. This can
 * change if more are needed. It is easier to have the array statically allocated initially than
 * worrying about the memory management. For now also assign types to slots. Local files in slot 0.
 * Shared tier top level in slot 1.
 */
#define WT_TIERED_INDEX_INVALID (uint32_t) - 1
#define WT_TIERED_INDEX_LOCAL 0
#define WT_TIERED_INDEX_SHARED 1

#define WT_TIERED_MAX_TIERS 4

/* Object name types */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TIERED_NAME_LOCAL 0x1u
#define WT_TIERED_NAME_OBJECT 0x2u
#define WT_TIERED_NAME_PREFIX 0x4u
#define WT_TIERED_NAME_SHARED 0x8u
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

/* Flush tier flags */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_FLUSH_TIER_FORCE 0x1u
#define WT_FLUSH_TIER_OFF 0x2u
#define WT_FLUSH_TIER_ON 0x4u
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

/*
 * The flush state is a simple counter we manipulate atomically.
 */
#define WT_FLUSH_STATE_DONE(state) ((state) == 0)

/*
 * Different types of work units for tiered trees.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TIERED_WORK_DROP_LOCAL 0x1u  /* Drop object from local storage. */
#define WT_TIERED_WORK_DROP_SHARED 0x2u /* Drop object from tier. */
#define WT_TIERED_WORK_FLUSH 0x4u       /* Flush object to tier. */
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

/*
 * WT_TIERED_WORK_UNIT --
 *	A definition of maintenance that a tiered tree needs done.
 */
struct __wt_tiered_work_unit {
    TAILQ_ENTRY(__wt_tiered_work_unit) q; /* Worker unit queue */
    uint32_t type;                        /* Type of operation */
    uint64_t op_val;                      /* A value for the operation */
    WT_TIERED *tiered;                    /* Tiered tree */
    uint32_t id;                          /* Id of the object */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TIERED_WORK_FORCE 0x1u /* Force operation */
#define WT_TIERED_WORK_FREE 0x2u  /* Free data after operation */
                                  /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;               /* Flags for operation */
};

/*
 * WT_TIERED_TIERS --
 *	Information we need to keep about each tier such as its data handle and name.
 *	We define operations that each tier can accept. The local tier should be able to accept
 *	reads and writes. The shared tier can do reads and flushes. Other ideas for future tiers
 *	may include a merge tier that is read only or an archival tier that is flush only.
 */
struct __wt_tiered_tiers {
    WT_DATA_HANDLE *tier; /* Data handle for this tier */
    const char *name;     /* Tier's metadata name */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TIERS_OP_FLUSH 0x1u
#define WT_TIERS_OP_READ 0x2u
#define WT_TIERS_OP_WRITE 0x4u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags; /* Flags including operations */
};

/*
 * WT_TIERED --
 *	Handle for a tiered data source. This data structure is used as the basis for metadata
 *	as the top level definition of a tiered table. This structure tells us where to find the
 *	parts of the tree and in what order we should look at the tiers. Prior to the first call
 *	to flush_tier after the creation of this table the only tier that exists will be the local
 *	disk represented by a file: URI. Then a second (or more) set of tiers will be where the
 *	tiered data lives. The non-local tier will point to a tier: URI and that is described by a
 *	WT_TIERED_TREE data structure that will encapsulate what the current state of the
 *	individual objects is.
 */
struct __wt_tiered {
    WT_DATA_HANDLE iface;

    const char *obj_config; /* Config to use for each object */
    const char *key_format, *value_format;

    WT_BUCKET_STORAGE *bstorage;

    WT_TIERED_TIERS tiers[WT_TIERED_MAX_TIERS]; /* Tiers array */

    WT_BLOCK_FILE_OPENER opener;

    uint32_t current_id; /* Current object id number */
    uint32_t next_id;    /* Next object number */
    uint32_t oldest_id;  /* Oldest object id number */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TIERED_FLAG_UNUSED 0x1u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/* FIXME: Currently the WT_TIERED_OBJECT data structure is not used. */
/*
 * WT_TIERED_OBJECT --
 *     Definition of a tiered object. This is a single object in a tiered tree.
 *     This is the lowest level data structure and item that makes
 *     up a tiered table. This structure contains the information needed to construct the name of
 *     this object and how to access it.
 */
struct __wt_tiered_object {
    const char *uri;      /* Data source for this object */
    WT_TIERED_TREE *tree; /* Pointer to tree this object is part of */
    uint64_t count;       /* Approximate count of records */
    uint64_t size;        /* Final size of object */
    uint64_t switch_txn;  /* Largest txn that can write to this object */
    uint64_t switch_ts;   /* Timestamp for switching */
    uint32_t id;          /* This object's id */
    uint32_t generation;  /* Do we need this?? */
    uint32_t refcnt;      /* Number of references */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TIERED_OBJ_LOCAL 0x1u /* Local resident also */
                                 /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/* FIXME: Currently the WT_TIERED_TREE data structure is not used. */
/*
 * WT_TIERED_TREE --
 *     Definition of the shared tiered portion of a tree.
 */
struct __wt_tiered_tree {
    WT_DATA_HANDLE iface;
    const char *name, *config;
    const char *key_format, *value_format;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TIERED_TREE_UNUSED 0x1u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};
