/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *  All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WiredTiger's chunk cache. Locally caches chunks of remote objects.
 */

#define WT_CHUNKCACHE_DEFAULT_HASHSIZE 32 * 1024
#define WT_CHUNKCACHE_DEFAULT_CHUNKSIZE 1024 * 1024
#define WT_CHUNKCACHE_FILE 1
#define WT_CHUNKCACHE_IN_VOLATILE_MEMORY 0
#define WT_CHUNKCACHE_MINHASHSIZE 64
#define WT_CHUNKCACHE_MAXHASHSIZE 1024 * 1024
#define WT_CHUNKCACHE_MAX_RETRIES 32 * 1024
#define WT_CHUNKCACHE_UNCONFIGURED 0

struct __wt_chunkcache_hashid {
    const char *objectname;
    uint32_t objectid;
    wt_off_t offset;
};

/* Hold the values used while hashing object ID, name, and offset tuples. */
struct __wt_chunkcache_intermediate_hash {
    uint64_t name_hash;
    uint32_t objectid;
    wt_off_t offset;
};

/*
 * The encapsulation of a cached chunk.
 */
struct __wt_chunkcache_chunk {
    TAILQ_ENTRY(__wt_chunkcache_chunk) next_chunk;
    TAILQ_ENTRY(__wt_chunkcache_chunk) next_lru_item;

    WT_CHUNKCACHE_HASHID hash_id;
    uint64_t access_count;
    uint64_t bucket_id; /* save hash bucket ID for quick removal */
    uint8_t *chunk_memory;
    wt_off_t chunk_offset;
    size_t chunk_size;
    volatile uint32_t valid;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CHUNK_PINNED 0x1u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;
};

struct __wt_chunkcache_bucket {
    /* This queue contains all chunks that mapped to this bucket. */
    TAILQ_HEAD(__wt_chunkchain_head, __wt_chunkcache_chunk) colliding_chunks;
    WT_SPINLOCK bucket_lock;
};

/*
 * WT_CHUNKCACHE --
 *     The chunk cache is a hash table of chunks. Each chunk list
 *     is uniquely identified by the file name, object id and offset.
 *     If more than one chunk maps to the same hash bucket, the colliding
 *     chunks are placed into a linked list. There is a per-bucket spinlock.
 */
struct __wt_chunkcache {
    /* Cache-wide. */
    bool configured;     /* Whether the chunk cache should be used */
    int type;            /* Location of the chunk cache (volatile memory or file) */
    uint64_t bytes_used; /* Amount of data currently in cache */
    uint64_t capacity;   /* Maximum allowed capacity */
    size_t chunk_size;
    bool chunkcache_exiting;

    WT_CHUNKCACHE_BUCKET *hashtable;
    unsigned int hashtable_size; /* The number of buckets */

    /* Backing storage (or memory). */
    char *storage_path;   /* The storage path if we are on a file system or a block device */
    WT_FH *fh;            /* Only used when backed by a file */
    uint8_t *free_bitmap; /* Bitmap of free chunks in file */
    uint8_t *memory;      /* Memory location for the assigned chunk space */

    /* Content management. */
    unsigned int evict_trigger; /* When this percent of cache is full, we trigger eviction. */
    char **pinned_objects;      /* list of objects we wish to pin in chunk cache */
    uint32_t pinned_entries;    /* count of pinned objects */
};
