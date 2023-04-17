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
#define WT_CHUNKCACHE_NAMEMAX 50
#define WT_CHUNKCACHE_UNCONFIGURED 0

struct __wt_chunkcache_hashid {
    char objectname[WT_CHUNKCACHE_NAMEMAX];
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
    volatile bool being_evicted;
    char *chunk_memory;
    size_t chunk_size;
    uint64_t bucket_id; /* save hash bucket ID for quick removal */
    volatile uint32_t valid;
    wt_off_t chunk_offset;
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
    WT_CHUNKCACHE_BUCKET *hashtable;
    WT_SPINLOCK chunkcache_lru_lock; /* Locks the LRU queue. */
    TAILQ_HEAD(__wt_chunkcache_lru, __wt_chunkcache_chunk) chunkcache_lru_list;
#ifdef ENABLE_MEMKIND
    struct memkind *memkind; /* Lets us use jemalloc over a file. */
#endif
    uint64_t bytes_used; /* amount of data currently in cache */
    uint64_t capacity;   /* maximum allowed capacity */
    bool chunkcache_exiting;
    bool configured;
    size_t chunk_size;
    char *dev_path;             /* the storage path if we are on a file system or a block device */
    unsigned int evict_trigger; /* When this percent of cache is full, we trigger eviction. */
    unsigned int hashtable_size;
    int type; /* location of the chunk cache (volatile memory or file) */
};
