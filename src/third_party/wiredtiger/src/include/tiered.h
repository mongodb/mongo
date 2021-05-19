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

/* AUTOMATIC FLAG VALUE GENERATION START */
#define WT_TIERED_MANAGER_SHUTDOWN 0x1u /* Manager has shut down */
                                        /* AUTOMATIC FLAG VALUE GENERATION STOP */
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
/* AUTOMATIC FLAG VALUE GENERATION START */
#define WT_TIERED_NAME_LOCAL 0x1u
#define WT_TIERED_NAME_OBJECT 0x2u
#define WT_TIERED_NAME_PREFIX 0x4u
#define WT_TIERED_NAME_SHARED 0x8u
/* AUTOMATIC FLAG VALUE GENERATION STOP */

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
/* AUTOMATIC FLAG VALUE GENERATION START */
#define WT_TIERS_OP_FLUSH 0x1u
#define WT_TIERS_OP_READ 0x2u
#define WT_TIERS_OP_WRITE 0x4u
    /* AUTOMATIC FLAG VALUE GENERATION STOP */
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

    uint64_t current_id; /* Current object id number */
    uint64_t next_id;    /* Next object number */

    WT_COLLATOR *collator; /* TODO: handle custom collation */
    /* TODO: What about compression, encryption, etc? Do we need to worry about that here? */

/* AUTOMATIC FLAG VALUE GENERATION START */
#define WT_TIERED_FLAG_UNUSED 0x1u
    /* AUTOMATIC FLAG VALUE GENERATION STOP */
    uint32_t flags;
};

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

/* AUTOMATIC FLAG VALUE GENERATION START */
#define WT_TIERED_OBJ_LOCAL 0x1u /* Local resident also */
    /* AUTOMATIC FLAG VALUE GENERATION STOP */
    uint32_t flags;
};

/*
 * WT_TIERED_TREE --
 *     Definition of the shared tiered portion of a tree.
 */
struct __wt_tiered_tree {
    WT_DATA_HANDLE iface;
    const char *name, *config;
    const char *key_format, *value_format;

/* AUTOMATIC FLAG VALUE GENERATION START */
#define WT_TIERED_TREE_UNUSED 0x1u
    /* AUTOMATIC FLAG VALUE GENERATION STOP */
    uint32_t flags;
};
