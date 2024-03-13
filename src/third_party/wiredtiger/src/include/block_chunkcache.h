/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *  All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/* WiredTiger's chunk cache. Locally caches chunks of remote objects. */

#define WT_CC_KEY_FORMAT WT_UNCHECKED_STRING(SLq)
#define WT_CC_VALUE_FORMAT WT_UNCHECKED_STRING(QQ)
#define WT_CC_APP_META_FORMAT \
    "app_metadata=\"version=1,capacity=%" PRIu64 ",buckets=%u,chunk_size=%" WT_SIZET_FMT "\""
#define WT_CC_META_CONFIG "key_format=" WT_CC_KEY_FORMAT ",value_format=" WT_CC_VALUE_FORMAT

/* The maximum number of metadata entries to write out per server wakeup. */
#define WT_CHUNKCACHE_METADATA_MAX_WORK 1000

/* Different types of chunk cache metadata operations. */
#define WT_CHUNKCACHE_METADATA_WORK_DEL 1
#define WT_CHUNKCACHE_METADATA_WORK_INS 2

struct __wt_chunkcache_metadata_work_unit {
    TAILQ_ENTRY(__wt_chunkcache_metadata_work_unit) q;
    uint8_t type;
    const char *name;
    uint32_t id;
    wt_off_t file_offset;
    uint64_t cache_offset;
    size_t data_sz;
};

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

/* The encapsulation of a cached chunk. */
struct __wt_chunkcache_chunk {
    TAILQ_ENTRY(__wt_chunkcache_chunk) next_chunk;
    TAILQ_ENTRY(__wt_chunkcache_chunk) next_lru_item;

    WT_CHUNKCACHE_HASHID hash_id;

#define WT_CHUNK_ACCESS_CAP_LIMIT 1000
    uint64_t access_count;
    uint64_t bucket_id; /* save hash bucket ID for quick removal */
    uint8_t *chunk_memory;
    wt_off_t chunk_offset;
    size_t chunk_size;
    wt_shared volatile bool valid; /* Availability to read data from the chunk marked. */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CHUNK_FROM_METADATA 0x1u
#define WT_CHUNK_PINNED 0x2u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;
};

struct __wt_chunkcache_bucket {
    /* This queue contains all chunks that mapped to this bucket. */
    TAILQ_HEAD(__wt_chunkchain_head, __wt_chunkcache_chunk) colliding_chunks;
    WT_SPINLOCK bucket_lock;
};

struct __wt_chunkcache_pinned_list {
    char **array;         /* list of objects we wish to pin in chunk cache */
    uint32_t entries;     /* count of pinned objects */
    WT_RWLOCK array_lock; /* Lock for pinned object array */
};

/*
 * WT_CHUNKCACHE --
 *     The chunk cache is a hash table of chunks. Each chunk list
 *     is uniquely identified by the file name, object id and offset.
 *     If more than one chunk maps to the same hash bucket, the colliding
 *     chunks are placed into a linked list. There is a per-bucket spinlock.
 */
#define WT_CHUNKCACHE_MAX_RETRIES 32 * 1024
#define WT_CHUNKCACHE_BITMAP_SIZE(capacity, chunk_size) \
    (WT_CEIL_POS((double)((capacity) / (chunk_size)) / 8.0))
struct __wt_chunkcache {
    /* Cache-wide. */
#define WT_CHUNKCACHE_FILE 1
#define WT_CHUNKCACHE_IN_VOLATILE_MEMORY 2
    uint8_t type;                  /* Location of the chunk cache (volatile memory or file) */
    wt_shared uint64_t bytes_used; /* Amount of data currently in cache */
    uint64_t capacity;             /* Maximum allowed capacity */

#define WT_CHUNKCACHE_DEFAULT_CHUNKSIZE 1024 * 1024
    size_t chunk_size;

    WT_CHUNKCACHE_BUCKET *hashtable;

#define WT_CHUNKCACHE_DEFAULT_HASHSIZE 32 * 1024
#define WT_CHUNKCACHE_MINHASHSIZE 64
#define WT_CHUNKCACHE_MAXHASHSIZE 1024 * 1024
    unsigned int hashtable_size; /* The number of buckets */

    /* Backing storage (or memory). */
    char *storage_path;   /* The storage path if we are on a file system or a block device */
    WT_FH *fh;            /* Only used when backed by a file */
    uint8_t *free_bitmap; /* Bitmap of free chunks in file */
    uint8_t *memory;      /* Memory location for the assigned chunk space */

    /* Content management. */
    wt_thread_t evict_thread_tid;
    unsigned int evict_trigger; /* When this percent of cache is full, we trigger eviction. */
    WT_CHUNKCACHE_PINNED_LIST pinned_objects;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CHUNKCACHE_CONFIGURED 0x1u
#define WT_CHUNK_CACHE_EXITING 0x2u
#define WT_CHUNK_CACHE_FLUSHED_DATA_INSERTION 0x4u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;
};
