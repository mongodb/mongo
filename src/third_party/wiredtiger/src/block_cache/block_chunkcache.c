/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *  All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef HAVE_DIAGNOSTIC
#define WT_BLOCK_OVERLAPS_CHUNK(chunk_off, block_off, chunk_size, block_size) \
    (block_off < chunk_off + (wt_off_t)chunk_size) && (chunk_off < block_off + (wt_off_t)block_size)
#endif

#define WT_BUCKET_CHUNKS(chunkcache, bucket_id) &(chunkcache->hashtable[bucket_id].colliding_chunks)
#define WT_BUCKET_LOCK(chunkcache, bucket_id) &(chunkcache->hashtable[bucket_id].bucket_lock)

/* This rounds down to the chunk boundary. */
#define WT_CHUNK_OFFSET(chunkcache, offset) \
    (wt_off_t)(((size_t)offset / (chunkcache)->chunk_size) * (chunkcache)->chunk_size)

/*
 * __chunkcache_bitmap_find_free --
 *     Iterate through the bitmap to find a free chunk in the cache.
 */
static int
__chunkcache_bitmap_find_free(WT_SESSION_IMPL *session, size_t *bit_index)
{
    WT_CHUNKCACHE *chunkcache;
    size_t bitmap_size, bits_remainder, i, j;
    uint8_t map_byte;

    chunkcache = &S2C(session)->chunkcache;

    /* The bitmap size accounts for full bytes only, remainder bits are iterated separately. */
    bitmap_size = (chunkcache->capacity / chunkcache->chunk_size) / 8;

    /* Iterate through the bytes and bits of the bitmap to find free chunks. */
    for (i = 0; i < bitmap_size; i++) {
        map_byte = chunkcache->free_bitmap[i];
        if (map_byte != 0xff) {
            j = 0;
            while ((map_byte & 1) != 0) {
                j++;
                map_byte >>= 1;
            }
            *bit_index = ((i * 8) + j);
            return (0);
        }
    }

    /* If the number of chunks isn't divisible by 8, iterate through the remaining bits. */
    bits_remainder = (chunkcache->capacity / chunkcache->chunk_size) % 8;
    for (j = 0; j < bits_remainder; j++)
        if ((chunkcache->free_bitmap[bitmap_size] & (0x01 << j)) == 0) {
            *bit_index = ((bitmap_size * 8) + j);
            return (0);
        }
    return (ENOSPC);
}

/*
 * __chunkcache_bitmap_alloc --
 *     Find the bit index to allocate.
 */
static int
__chunkcache_bitmap_alloc(WT_SESSION_IMPL *session, size_t *bit_index)
{
    WT_CHUNKCACHE *chunkcache;
    uint8_t map_byte_expected, map_byte_mask;

    chunkcache = &S2C(session)->chunkcache;

retry:
    /* Use the bitmap to find a free slot for a chunk in the cache. */
    WT_RET(__chunkcache_bitmap_find_free(session, bit_index));

    /* Bit index should be less than the maximum number of chunks that can be allocated. */
    WT_ASSERT(session, *bit_index < (chunkcache->capacity / chunkcache->chunk_size));

    WT_READ_ONCE(map_byte_expected, chunkcache->free_bitmap[*bit_index / 8]);
    map_byte_mask = (uint8_t)(0x01 << (*bit_index % 8));
    if (((map_byte_expected & map_byte_mask) != 0) ||
      !__wt_atomic_cas8(&chunkcache->free_bitmap[*bit_index / 8], map_byte_expected,
        map_byte_expected | map_byte_mask))
        goto retry;

    return (0);
}

/*
 * __chunkcache_bitmap_free --
 *     Free the bit index.
 */
static void
__chunkcache_bitmap_free(WT_SESSION_IMPL *session, size_t index)
{
    WT_CHUNKCACHE *chunkcache;
    uint8_t map_byte_expected, map_byte_mask;

    chunkcache = &S2C(session)->chunkcache;

    do {
        map_byte_expected = chunkcache->free_bitmap[index / 8];
        map_byte_mask = (uint8_t)(0x01 << (index % 8));

        /* Assert to verify that the bit is allocated. */
        WT_ASSERT(session, (map_byte_expected & map_byte_mask) != 0);
    } while (!__wt_atomic_cas8(&chunkcache->free_bitmap[index / 8], map_byte_expected,
      map_byte_expected & (uint8_t) ~(map_byte_mask)));
}

/*
 * __name_in_pinned_list --
 *     Return true if the chunk belongs to the object in pinned object array.
 */
static inline bool
__name_in_pinned_list(WT_SESSION_IMPL *session, const char *name)
{
    WT_CHUNKCACHE *chunkcache;
    bool found;

    chunkcache = &S2C(session)->chunkcache;
    found = false;

    __wt_readlock(session, &chunkcache->pinned_objects.array_lock);
    WT_BINARY_SEARCH_STRING(
      name, chunkcache->pinned_objects.array, chunkcache->pinned_objects.entries, found);
    __wt_readunlock(session, &chunkcache->pinned_objects.array_lock);

    return (found);
}

/*
 * __chunkcache_alloc --
 *     Allocate memory for the chunk in the cache.
 */
static int
__chunkcache_alloc(WT_SESSION_IMPL *session, WT_CHUNKCACHE_CHUNK *chunk)
{
    WT_CHUNKCACHE *chunkcache;
    size_t bit_index;

    chunkcache = &S2C(session)->chunkcache;
    bit_index = 0;

    if (chunkcache->type == WT_CHUNKCACHE_IN_VOLATILE_MEMORY)
        WT_RET(__wt_malloc(session, chunk->chunk_size, &chunk->chunk_memory));
    else {
        WT_RET(__chunkcache_bitmap_alloc(session, &bit_index));

        /* Allocate the free memory in the chunk cache. */
        chunk->chunk_memory = chunkcache->memory + chunkcache->chunk_size * bit_index;
    }

    /* Increment chunk's disk usage and update statistics. */
    __wt_atomic_add64(&chunkcache->bytes_used, chunk->chunk_size);
    WT_STAT_CONN_INCR(session, chunk_cache_chunks_inuse);
    WT_STAT_CONN_INCRV(session, chunk_cache_bytes_inuse, chunk->chunk_size);
    if (__name_in_pinned_list(session, chunk->hash_id.objectname)) {
        F_SET(chunk, WT_CHUNK_PINNED);
        WT_STAT_CONN_INCR(session, chunk_cache_chunks_pinned);
        WT_STAT_CONN_INCRV(session, chunk_cache_bytes_inuse_pinned, chunk->chunk_size);
    }

    return (0);
}

/*
 * __chunkcache_admit_size --
 *     Decide if we can admit the chunk given the limit on cache capacity.
 */
static size_t
__chunkcache_admit_size(WT_SESSION_IMPL *session)
{
    WT_CHUNKCACHE *chunkcache;

    chunkcache = &S2C(session)->chunkcache;

    if ((chunkcache->bytes_used + chunkcache->chunk_size) < chunkcache->capacity)
        return (chunkcache->chunk_size);

    WT_STAT_CONN_INCR(session, chunk_cache_exceeded_capacity);
    __wt_verbose(session, WT_VERB_CHUNKCACHE,
      "chunkcache exceeded capacity of %" PRIu64
      " bytes "
      "with %" PRIu64 " bytes in use and the chunk size of %" PRIu64 " bytes",
      chunkcache->capacity, chunkcache->bytes_used, (uint64_t)chunkcache->chunk_size);
    return (0);
}

/*
 * __chunkcache_alloc_chunk --
 *     Allocate the chunk and its metadata for a block at a given offset.
 */
static int
__chunkcache_alloc_chunk(WT_SESSION_IMPL *session, wt_off_t offset, wt_off_t size,
  WT_CHUNKCACHE_HASHID *hash_id, WT_CHUNKCACHE_CHUNK **newchunk)
{
    WT_CHUNKCACHE *chunkcache;
    WT_CHUNKCACHE_INTERMEDIATE_HASH intermediate;
    WT_DECL_RET;
    size_t chunk_size;
    uint64_t hash;

    *newchunk = NULL;
    chunkcache = &S2C(session)->chunkcache;
    WT_CLEAR(intermediate);

    WT_ASSERT(session, offset >= 0);

    /*
     * Calculate the size and the offset for the chunk. The chunk storage area is broken into
     * equally sized chunks of configured size. We calculate the offset of the chunk into which the
     * block's offset falls. Chunks are equally sized and are not necessarily a multiple of a block.
     * So a block may begin in one chunk and end in another. It may also span multiple chunks, if
     * the chunk size is configured much smaller than a block size (we hope that never happens). In
     * the allocation function we don't care about the block's size. If more than one chunk is
     * needed to cover the entire block, another function will take care of allocating multiple
     * chunks.
     */

    if ((chunk_size = __chunkcache_admit_size(session)) == 0)
        return (ENOSPC);
    WT_RET(__wt_calloc(session, 1, sizeof(WT_CHUNKCACHE_CHUNK), newchunk));

    /* Convert the block offset to the offset of the enclosing chunk. */
    (*newchunk)->chunk_offset = WT_CHUNK_OFFSET(chunkcache, offset);
    /* Chunk cannot be larger than the file. */
    (*newchunk)->chunk_size = WT_MIN(chunk_size, (size_t)(size - (*newchunk)->chunk_offset));

    /* Part of the hash ID was populated by the caller, but we must set the offset. */
    intermediate.name_hash = __wt_hash_city64(hash_id->objectname, strlen(hash_id->objectname));
    intermediate.objectid = hash_id->objectid;
    intermediate.offset = (*newchunk)->chunk_offset;

    (*newchunk)->hash_id.objectid = hash_id->objectid;
    (*newchunk)->hash_id.offset = (*newchunk)->chunk_offset;
    WT_RET(__wt_strdup(session, hash_id->objectname, &(*newchunk)->hash_id.objectname));
    hash = __wt_hash_city64(&intermediate, sizeof(WT_CHUNKCACHE_INTERMEDIATE_HASH));
    (*newchunk)->bucket_id = hash % chunkcache->hashtable_size;

    /* Initialize the access count, so the upper layer code doesn't need to remember to do that. */
    (*newchunk)->access_count++;

    WT_ASSERT_SPINLOCK_OWNED(session, WT_BUCKET_LOCK(chunkcache, (*newchunk)->bucket_id));

    if ((ret = __chunkcache_alloc(session, *newchunk)) != 0) {
        __wt_free(session, *newchunk);
        return (ret);
    }
    __wt_verbose(session, WT_VERB_CHUNKCACHE, "allocate: %s(%u), offset=%" PRIu64 ", size=%" PRIu64,
      (*newchunk)->hash_id.objectname, (*newchunk)->hash_id.objectid,
      (uint64_t)(*newchunk)->chunk_offset, (uint64_t)(*newchunk)->chunk_size);

    return (0);
}

/*
 * __chunkcache_free_chunk --
 *     Free the memory occupied by the chunk and the metadata.
 */
static void
__chunkcache_free_chunk(WT_SESSION_IMPL *session, WT_CHUNKCACHE_CHUNK *chunk)
{
    WT_CHUNKCACHE *chunkcache;
    size_t index;

    chunkcache = &S2C(session)->chunkcache;

    /* Decrement chunk's disk usage and update statistics. */
    (void)__wt_atomic_sub64(&chunkcache->bytes_used, chunk->chunk_size);
    WT_STAT_CONN_DECR(session, chunk_cache_chunks_inuse);
    WT_STAT_CONN_DECRV(session, chunk_cache_bytes_inuse, chunk->chunk_size);
    /* We can fail when reading chunks, and some are pinned. Unset the pinned stats here. */
    if (F_ISSET(chunk, WT_CHUNK_PINNED)) {
        WT_STAT_CONN_DECR(session, chunk_cache_chunks_pinned);
        WT_STAT_CONN_DECRV(session, chunk_cache_bytes_inuse_pinned, chunk->chunk_size);
    }

    if (chunkcache->type == WT_CHUNKCACHE_IN_VOLATILE_MEMORY)
        __wt_free(session, chunk->chunk_memory);
    else {
        /* Update the bitmap, then free the chunk memory. */
        index = (size_t)(chunk->chunk_memory - chunkcache->memory) / chunkcache->chunk_size;
        __chunkcache_bitmap_free(session, index);
    }
    __wt_free(session, chunk);
}

/*
 * __chunkcache_tmp_hash --
 *     Populate the hash data structure, which uniquely identifies the chunk. The hash ID we
 *     populate will contain a pointer to the block name, thus the block name must outlive the hash
 *     ID.
 */
static inline uint64_t
__chunkcache_tmp_hash(WT_CHUNKCACHE *chunkcache, WT_CHUNKCACHE_HASHID *hash_id,
  const char *object_name, uint32_t objectid, wt_off_t offset)
{
    WT_CHUNKCACHE_INTERMEDIATE_HASH intermediate;
    uint64_t hash_final;

    WT_CLEAR(intermediate);
    intermediate.name_hash = __wt_hash_city64(object_name, strlen(object_name));
    intermediate.objectid = objectid;
    intermediate.offset = WT_CHUNK_OFFSET(chunkcache, offset);

    /*
     * The hashing situation is a little complex. We want to construct hashes as we iterate over the
     * chunks we add/remove, and these hashes consist of an object name, object ID, and offset. But
     * to hash these, the bytes need to be contiguous in memory. Having the object name as a
     * fixed-size character array would work, but it would need to be large, and that would waste a
     * lot of space most of the time. The alternative would be to allocate a new structure just for
     * hashing purposes, but then we're allocating/freeing on the hot path.
     *
     * Instead, we hash the object name separately, then bundle that hash into a temporary (stack
     * allocated) structure with the object ID and offset. Then, we hash that intermediate
     * structure.
     */
    WT_CLEAR(*hash_id);
    hash_id->objectid = objectid;
    hash_id->offset = WT_CHUNK_OFFSET(chunkcache, offset);
    hash_id->objectname = object_name;

    hash_final = __wt_hash_city64(&intermediate, sizeof(intermediate));

    /* Return the bucket ID. */
    return (hash_final % chunkcache->hashtable_size);
}

/*
 * __hash_id_eq --
 *     Compare two hash IDs and return whether they're equal.
 */
static inline bool
__hash_id_eq(WT_CHUNKCACHE_HASHID *a, WT_CHUNKCACHE_HASHID *b)
{
    return (a->objectid == b->objectid && a->offset == b->offset &&
      strcmp(a->objectname, b->objectname) == 0);
}

/*
 * __chunkcache_should_evict --
 *     Decide if we can evict this chunk.
 *
 * In the current algorithm we only evict the chunks with a zero access count. We always decrement
 *     the access count on the chunk that is given to us. The thread accessing the chunk increments
 *     the access count. As a result, we will only evict a chunk that has not been accessed for a
 *     time proportional to the number of accesses made to it.
 */
static inline bool
__chunkcache_should_evict(WT_CHUNKCACHE_CHUNK *chunk)
{
    bool valid;

    /*
     * Do not evict chunks that are in the process of being added to the cache. The ordered read,
     * and matching publish, are required since populating the chunk itself isn't protected by the
     * bucket lock. Ergo, we need to make sure that reads or writes to the valid field are not
     * reordered relative to reads or writes of other fields.
     */
    WT_ORDERED_READ(valid, chunk->valid);
    if (!valid)
        return (false);

    if (F_ISSET(chunk, WT_CHUNK_PINNED))
        return (false);

    if (chunk->access_count == 0)
        return (true);
    --chunk->access_count;

    return (false);
}

/*
 * __chunkcache_eviction_thread --
 *     Periodically sweep the cache and evict chunks with a zero access count.
 *
 * This strategy is similar to the clock eviction algorithm, which is an approximates LRU.
 */
static WT_THREAD_RET
__chunkcache_eviction_thread(void *arg)
{
    WT_CHUNKCACHE *chunkcache;
    WT_CHUNKCACHE_CHUNK *chunk, *chunk_tmp;
    WT_SESSION_IMPL *session;
    int i;

    session = (WT_SESSION_IMPL *)arg;
    chunkcache = &S2C(session)->chunkcache;

    while (!F_ISSET(chunkcache, WT_CHUNK_CACHE_EXITING)) {
        /* Do not evict if we are not close to exceeding capacity. */
        if ((chunkcache->bytes_used + chunkcache->chunk_size) <
          chunkcache->evict_trigger * chunkcache->capacity / 100) {
            __wt_sleep(1, 0);
            continue;
        }
        for (i = 0; i < (int)chunkcache->hashtable_size; i++) {
            __wt_spin_lock(session, &chunkcache->hashtable[i].bucket_lock);
            TAILQ_FOREACH_SAFE(chunk, WT_BUCKET_CHUNKS(chunkcache, i), next_chunk, chunk_tmp)
            {
                if (__chunkcache_should_evict(chunk)) {
                    TAILQ_REMOVE(WT_BUCKET_CHUNKS(chunkcache, i), chunk, next_chunk);
                    __chunkcache_free_chunk(session, chunk);
                    WT_STAT_CONN_INCR(session, chunk_cache_chunks_evicted);
                    __wt_verbose(session, WT_VERB_CHUNKCACHE,
                      "evicted chunk: %s(%u), offset=%" PRId64 ", size=%" PRIu64,
                      chunk->hash_id.objectname, chunk->hash_id.objectid, chunk->chunk_offset,
                      (uint64_t)chunk->chunk_size);
                }
            }
            __wt_spin_unlock(session, &chunkcache->hashtable[i].bucket_lock);
            if (F_ISSET(chunkcache, WT_CHUNK_CACHE_EXITING))
                return (WT_THREAD_RET_VALUE);
        }
    }
    return (WT_THREAD_RET_VALUE);
}

/*
 * __chunkcache_str_cmp --
 *     Qsort function: sort string array.
 */
static int WT_CDECL
__chunkcache_str_cmp(const void *a, const void *b)
{
    return (strcmp(*(const char **)a, *(const char **)b));
}

/*
 * __chunkcache_arr_free --
 *     Free the array of strings.
 */
static void
__chunkcache_arr_free(WT_SESSION_IMPL *session, char ***arr)
{
    char **p;

    if ((p = (*arr)) != NULL) {
        for (; *p != NULL; ++p)
            __wt_free(session, *p);
        __wt_free(session, *arr);
    }
}

/*
 * __config_get_sorted_pinned_objects --
 *     Get sorted array of pinned objects from the config.
 */
static int
__config_get_sorted_pinned_objects(WT_SESSION_IMPL *session, const char *cfg[],
  char ***pinned_objects_list, unsigned int *pinned_entries)
{
    WT_CONFIG targetconf;
    WT_CONFIG_ITEM cval, k, v;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    char **pinned_objects;
    unsigned int cnt;

    pinned_objects = NULL;

    WT_RET(__wt_config_gets(session, cfg, "chunk_cache.pinned", &cval));
    __wt_config_subinit(session, &targetconf, &cval);
    for (cnt = 0; (ret = __wt_config_next(&targetconf, &k, &v)) == 0; ++cnt)
        ;
    *pinned_entries = cnt;
    WT_RET_NOTFOUND_OK(ret);

    if (cnt != 0) {
        WT_ERR(__wt_scr_alloc(session, 0, &tmp));
        WT_ERR(__wt_calloc_def(session, cnt + 1, &pinned_objects));
        __wt_config_subinit(session, &targetconf, &cval);
        for (cnt = 0; (ret = __wt_config_next(&targetconf, &k, &v)) == 0; ++cnt) {
            if (!WT_PREFIX_MATCH(k.str, "table:"))
                WT_ERR_MSG(session, EINVAL,
                  "chunk cache pinned configuration only supports objects of type \"table\"");

            if (v.len != 0)
                WT_ERR_MSG(session, EINVAL,
                  "invalid chunk cache pinned config %.*s: URIs may require quoting", (int)cval.len,
                  (char *)cval.str);

            WT_PREFIX_SKIP_REQUIRED(session, k.str, "table:");
            WT_ERR(__wt_buf_fmt(session, tmp, "%.*s", (int)(k.len - strlen("table:")), k.str));
            WT_ERR(__wt_strndup(session, tmp->data, tmp->size, &pinned_objects[cnt]));
        }
        WT_ERR_NOTFOUND_OK(ret, false);
        __wt_qsort(pinned_objects, cnt, sizeof(char *), __chunkcache_str_cmp);
        *pinned_objects_list = pinned_objects;
    }

err:
    __wt_scr_free(session, &tmp);
    if (ret != 0 && ret != WT_NOTFOUND) {
        __chunkcache_arr_free(session, &pinned_objects);
        return (ret);
    }

    return (0);
}

/*
 * __chunkcache_insert --
 *     Insert chunk into the chunkcache.
 */
static int
__chunkcache_insert(WT_SESSION_IMPL *session, wt_off_t offset, wt_off_t size,
  WT_CHUNKCACHE_HASHID *hash_id, uint64_t bucket_id, WT_CHUNKCACHE_CHUNK **new_chunk)
{
    WT_CHUNKCACHE *chunkcache;

    chunkcache = &S2C(session)->chunkcache;

    /*
     * !!! (Don't format the comment.)
     * Caller function should take a bucket lock before inserting the chunk.
     */
    WT_RET(__chunkcache_alloc_chunk(session, offset, size, hash_id, new_chunk));

    /*
     * Insert the invalid chunk into the bucket before releasing the lock and doing I/O. This way we
     * avoid two threads trying to cache the same chunk.
     */
    TAILQ_INSERT_HEAD(WT_BUCKET_CHUNKS(chunkcache, bucket_id), *new_chunk, next_chunk);

    return (0);
}

/*
 * __chunkcache_read_into_chunk --
 *     Read data into the chunk memory.
 */
static int
__chunkcache_read_into_chunk(
  WT_SESSION_IMPL *session, uint64_t bucket_id, WT_FH *fh, WT_CHUNKCACHE_CHUNK *new_chunk)
{
    WT_CHUNKCACHE *chunkcache;
    WT_DECL_RET;

    chunkcache = &S2C(session)->chunkcache;

    /* Make sure the chunk is considered invalid when reading data into it. */
    WT_ASSERT(session, !new_chunk->valid);

    /* Read the new chunk. Only one thread would be caching the new chunk. */
    if ((ret = __wt_read(session, fh, new_chunk->chunk_offset, new_chunk->chunk_size,
           new_chunk->chunk_memory)) != 0) {
        __wt_spin_lock(session, WT_BUCKET_LOCK(chunkcache, bucket_id));
        TAILQ_REMOVE(WT_BUCKET_CHUNKS(chunkcache, bucket_id), new_chunk, next_chunk);
        __wt_spin_unlock(session, WT_BUCKET_LOCK(chunkcache, bucket_id));
        __chunkcache_free_chunk(session, new_chunk);
        WT_STAT_CONN_INCR(session, chunk_cache_io_failed);
        return (ret);
    }

    /*
     * Mark chunk as valid. The only thread that could be executing this code is the thread that won
     * the race and inserted this (invalid) chunk into the hash table. This thread has now read the
     * chunk, while any other threads that were looking for the same chunk would be spin-waiting for
     * this chunk to become valid. The current thread will mark the chunk as valid, and any waiters
     * will unblock and proceed reading it.
     */
    WT_PUBLISH(new_chunk->valid, true);

    return (0);
}

/*
 * __chunkcache_unpin_old_versions --
 *     Unpin the old versions of newly added chunks so eviction can remove them.
 */
static void
__chunkcache_unpin_old_versions(WT_SESSION_IMPL *session, const char *sp_obj_name)
{
    WT_CHUNKCACHE *chunkcache;
    WT_CHUNKCACHE_CHUNK *chunk, *chunk_tmp;
    size_t i;

    chunkcache = &S2C(session)->chunkcache;

    /* Optimization: check if the file contains objects in the pinned list, otherwise skip. */
    if (__name_in_pinned_list(session, sp_obj_name)) {
        /*
         * Loop through the entire chunkcache and search for matching objects from the file and
         * clear the pinned flag.
         */
        for (i = 0; i < chunkcache->hashtable_size; i++) {
            __wt_spin_lock(session, &chunkcache->hashtable[i].bucket_lock);
            TAILQ_FOREACH_SAFE(chunk, WT_BUCKET_CHUNKS(chunkcache, i), next_chunk, chunk_tmp)
            {
                if (strcmp(chunk->hash_id.objectname, sp_obj_name) == 0) {
                    if (F_ISSET(chunk, WT_CHUNK_PINNED)) {
                        /*
                         * Decrement the stat when a chunk that was initially pinned becomes
                         * unpinned.
                         */
                        WT_STAT_CONN_DECR(session, chunk_cache_chunks_pinned);
                        WT_STAT_CONN_DECRV(
                          session, chunk_cache_bytes_inuse_pinned, chunk->chunk_size);
                    }
                    F_CLR(chunk, WT_CHUNK_PINNED);
                }
            }
            __wt_spin_unlock(session, &chunkcache->hashtable[i].bucket_lock);
        }
    }
}

/*
 * __wt_chunkcache_get --
 *     Return the data to the caller if we have it. Otherwise read it from storage and cache it.
 *
 * During these operations we are holding one or more bucket locks. A bucket lock protects the
 *     linked list (i.e., the chain) or chunks hashing into the same bucket. We hold the bucket lock
 *     whenever we are looking for and are inserting a new chunk into that bucket. We must hold the
 *     lock throughout the entire operation: realizing that the chunk is not present, deciding to
 *     cache it, allocating the chunks metadata and inserting it into the chain. If we release the
 *     lock during this process, another thread might cache the same chunk; we do not want that. We
 *     insert the new chunk into the cache in the not valid state. Once we insert the chunk, we can
 *     release the lock. As long as the chunk is marked as invalid, no other thread will try to
 *     re-cache it or to read it. As a result, we can read data from the remote storage into this
 *     chunk without holding the lock: this is what the current code does. We can even allocate the
 *     space for that chunk outside the critical section: the current code does not do that. Once we
 *     read the data into the chunk, we atomically set the valid flag, so other threads can use it.
 */
int
__wt_chunkcache_get(WT_SESSION_IMPL *session, WT_BLOCK *block, uint32_t objectid, wt_off_t offset,
  uint32_t size, void *dst, bool *cache_hit)
{
    WT_CHUNKCACHE *chunkcache;
    WT_CHUNKCACHE_CHUNK *chunk;
    WT_CHUNKCACHE_HASHID hash_id;
    WT_DECL_RET;
    size_t already_read, remains_to_read, readable_in_chunk, size_copied;
    uint64_t bucket_id, retries, sleep_usec;
    const char *object_name;
    bool chunk_cached, valid;

    chunkcache = &S2C(session)->chunkcache;
    already_read = 0;
    remains_to_read = size;
    retries = 0;
    sleep_usec = WT_THOUSAND;
    object_name = NULL;
    *cache_hit = false;

    if (!F_ISSET(chunkcache, WT_CHUNKCACHE_CONFIGURED))
        return (ENOTSUP);

    /* Only cache read-only tiered objects. */
    if (!block->readonly)
        return (0);

    __wt_verbose(session, WT_VERB_CHUNKCACHE, "get: %s(%u), offset=%" PRId64 ", size=%u",
      (char *)block->name, objectid, offset, size);
    WT_STAT_CONN_INCR(session, chunk_cache_lookups);

    WT_RET(
      __wt_tiered_name(session, session->dhandle, 0, WT_TIERED_NAME_SKIP_PREFIX, &object_name));

    /* A block may span two (or more) chunks. Loop until we have read all the data. */
    while (remains_to_read > 0) {
        /* Find the bucket for the chunk containing this offset. */
        bucket_id = __chunkcache_tmp_hash(
          chunkcache, &hash_id, object_name, objectid, offset + (wt_off_t)already_read);
retry:
        chunk_cached = false;
        __wt_spin_lock(session, WT_BUCKET_LOCK(chunkcache, bucket_id));
        TAILQ_FOREACH (chunk, WT_BUCKET_CHUNKS(chunkcache, bucket_id), next_chunk) {
            if (__hash_id_eq(&chunk->hash_id, &hash_id)) {
                /* If the chunk is there, but invalid, there is I/O in progress. Retry. */
                WT_ORDERED_READ(valid, chunk->valid);
                if (!valid) {
                    __wt_spin_unlock(session, WT_BUCKET_LOCK(chunkcache, bucket_id));
                    __wt_spin_backoff(&retries, &sleep_usec);
                    WT_STAT_CONN_INCR(session, chunk_cache_retries);
                    if (retries > WT_CHUNKCACHE_MAX_RETRIES)
                        WT_STAT_CONN_INCR(session, chunk_cache_toomany_retries);
                    goto retry;
                }
                /* Found the needed chunk. */
                chunk_cached = true;
                WT_ASSERT(session,
                  WT_BLOCK_OVERLAPS_CHUNK(chunk->chunk_offset, offset + (wt_off_t)already_read,
                    chunk->chunk_size, remains_to_read));

                /* We can't read beyond the chunk's boundary. */
                readable_in_chunk =
                  (size_t)chunk->chunk_offset + chunk->chunk_size - (size_t)offset;
                size_copied = WT_MIN(readable_in_chunk, remains_to_read);
                memcpy((void *)((uint64_t)dst + already_read),
                  chunk->chunk_memory + (offset + (wt_off_t)already_read - chunk->chunk_offset),
                  size_copied);

                /*
                 * Increment the access count for eviction. If we are accessing the new chunk, the
                 * access count would have been incremented on it when it was newly inserted to
                 * avoid eviction before the chunk is accessed. So we are giving two access counts
                 * to newly inserted chunks.
                 */
                chunk->access_count++;

                __wt_spin_unlock(session, WT_BUCKET_LOCK(chunkcache, bucket_id));

                if (already_read > 0)
                    WT_STAT_CONN_INCR(session, chunk_cache_spans_chunks_read);
                already_read += size_copied;
                remains_to_read -= size_copied;

                break;
            }
        }
        /* The chunk is not cached. Allocate space for it. Prepare for reading it from storage. */
        if (!chunk_cached) {
            WT_STAT_CONN_INCR(session, chunk_cache_misses);
            ret = __chunkcache_insert(
              session, offset + (wt_off_t)already_read, block->size, &hash_id, bucket_id, &chunk);
            __wt_spin_unlock(session, WT_BUCKET_LOCK(chunkcache, bucket_id));
            WT_RET(ret);

            WT_RET(__chunkcache_read_into_chunk(session, bucket_id, block->fh, chunk));

            __wt_verbose(session, WT_VERB_CHUNKCACHE,
              "insert: %s(%u), offset=%" PRId64 ", size=%lu", (char *)block->name, objectid,
              chunk->chunk_offset, chunk->chunk_size);
            goto retry;
        }
    }

    *cache_hit = true;
    return (0);
}

/*
 * __wt_chunkcache_ingest --
 *     Read all the contents from a file and insert it into the chunkcache.
 */
int
__wt_chunkcache_ingest(
  WT_SESSION_IMPL *session, const char *local_name, const char *sp_obj_name, uint32_t objectid)
{
    WT_CHUNKCACHE *chunkcache;
    WT_CHUNKCACHE_CHUNK *chunk;
    WT_CHUNKCACHE_HASHID hash_id;
    WT_DECL_RET;
    WT_FH *fh;
    wt_off_t already_read, size;
    uint64_t bucket_id;

    chunkcache = &S2C(session)->chunkcache;
    already_read = 0;

    if (!F_ISSET(chunkcache, WT_CHUNKCACHE_CONFIGURED) ||
      !F_ISSET(chunkcache, WT_CHUNK_CACHE_FLUSHED_DATA_INSERTION))
        return (0);

    /* Check and unpin any old versions of newly added objects. */
    __chunkcache_unpin_old_versions(session, sp_obj_name);

    WT_RET(__wt_open(session, local_name, WT_FS_OPEN_FILE_TYPE_DATA, WT_FS_OPEN_READONLY, &fh));
    WT_ERR(__wt_filesize(session, fh, &size));

    while (already_read < size) {
        bucket_id =
          __chunkcache_tmp_hash(chunkcache, &hash_id, sp_obj_name, objectid, already_read);

        __wt_spin_lock(session, WT_BUCKET_LOCK(chunkcache, bucket_id));
        ret = __chunkcache_insert(session, already_read, size, &hash_id, bucket_id, &chunk);
        __wt_spin_unlock(session, WT_BUCKET_LOCK(chunkcache, bucket_id));
        WT_ERR(ret);

        WT_ERR(__chunkcache_read_into_chunk(session, bucket_id, fh, chunk));

        WT_STAT_CONN_INCR(session, chunk_cache_newly_inserted);

        __wt_verbose(session, WT_VERB_CHUNKCACHE, "ingest: %s(%u), offset=%" PRId64 ", size=%lu",
          (char *)local_name, objectid, chunk->chunk_offset, chunk->chunk_size);

        already_read += (wt_off_t)chunk->chunk_size;
    }

err:
    WT_TRET(__wt_close(session, &fh));
    return (ret);
}

/*
 * __wt_chunkcache_reconfig --
 *     Re-configure the chunk cache.
 */
int
__wt_chunkcache_reconfig(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CHUNKCACHE *chunkcache;
    WT_CHUNKCACHE_CHUNK *chunk, *chunk_tmp;
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    char **old_pinned_list, **pinned_objects;
    unsigned int cnt, i;

    chunkcache = &S2C(session)->chunkcache;
    old_pinned_list = chunkcache->pinned_objects.array;
    pinned_objects = NULL;
    cnt = 0;

    /* When reconfiguring, check if there are any modifications that we care about. */
    if ((ret = __wt_config_gets(session, cfg + 1, "chunk_cache", &cval)) == WT_NOTFOUND)
        return (0);

    if (!F_ISSET(chunkcache, WT_CHUNKCACHE_CONFIGURED))
        WT_RET_MSG(
          session, EINVAL, "chunk cache reconfigure requested, but cache has not been configured");

    WT_RET(__config_get_sorted_pinned_objects(session, cfg, &pinned_objects, &cnt));

    /*
     * Acquire the pinned array lock to avoid racing with threads reading the pinned array, and then
     * update the array.
     */
    __wt_writelock(session, &chunkcache->pinned_objects.array_lock);
    chunkcache->pinned_objects.array = pinned_objects;
    chunkcache->pinned_objects.entries = cnt;
    __wt_writeunlock(session, &chunkcache->pinned_objects.array_lock);

    /* Release the memory allocated to the old array. */
    __chunkcache_arr_free(session, &old_pinned_list);

    /* Iterate through all the chunks and mark them as pinned if necessary. */
    for (i = 0; i < chunkcache->hashtable_size; i++) {
        __wt_spin_lock(session, &chunkcache->hashtable[i].bucket_lock);
        TAILQ_FOREACH_SAFE(chunk, WT_BUCKET_CHUNKS(chunkcache, i), next_chunk, chunk_tmp)
        {
            if (__name_in_pinned_list(session, chunk->hash_id.objectname)) {
                /* Increment the stat when a chunk that was initially unpinned becomes pinned. */
                if (!F_ISSET(chunk, WT_CHUNK_PINNED)) {
                    WT_STAT_CONN_INCR(session, chunk_cache_chunks_pinned);
                    WT_STAT_CONN_INCRV(session, chunk_cache_bytes_inuse_pinned, chunk->chunk_size);
                }
                F_SET(chunk, WT_CHUNK_PINNED);
            } else {
                /* Decrement the stat when a chunk that was initially pinned becomes unpinned. */
                if (F_ISSET(chunk, WT_CHUNK_PINNED)) {
                    WT_STAT_CONN_DECR(session, chunk_cache_chunks_pinned);
                    WT_STAT_CONN_DECRV(session, chunk_cache_bytes_inuse_pinned, chunk->chunk_size);
                }
                F_CLR(chunk, WT_CHUNK_PINNED);
            }
        }
        __wt_spin_unlock(session, &chunkcache->hashtable[i].bucket_lock);
    }

    return (0);
}

/*
 * __wt_chunkcache_setup --
 *     Set up the chunk cache.
 */
int
__wt_chunkcache_setup(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CHUNKCACHE *chunkcache;
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    unsigned int cnt, i;
    char **pinned_objects;
    size_t mapped_size;

    chunkcache = &S2C(session)->chunkcache;
    pinned_objects = NULL;
    cnt = 0;

    if (F_ISSET(chunkcache, WT_CHUNKCACHE_CONFIGURED))
        WT_RET_MSG(session, EINVAL, "chunk cache setup requested, but cache is already configured");

    WT_RET(__wt_config_gets(session, cfg, "chunk_cache.enabled", &cval));
    if (cval.val == 0)
        return (0);

    WT_RET(__wt_config_gets(session, cfg, "chunk_cache.capacity", &cval));
    if ((chunkcache->capacity = (uint64_t)cval.val) <= 0)
        WT_RET_MSG(session, EINVAL, "chunk cache capacity must be greater than zero");

    WT_RET(__wt_config_gets(session, cfg, "chunk_cache.chunk_cache_evict_trigger", &cval));
    if (((chunkcache->evict_trigger = (u_int)cval.val) == 0) || (chunkcache->evict_trigger > 100))
        WT_RET_MSG(session, EINVAL, "evict trigger must be between 0 and 100");

    WT_RET(__wt_config_gets(session, cfg, "chunk_cache.chunk_size", &cval));
    if ((chunkcache->chunk_size = (uint64_t)cval.val) <= 0)
        chunkcache->chunk_size = WT_CHUNKCACHE_DEFAULT_CHUNKSIZE;

    WT_RET(__wt_config_gets(session, cfg, "chunk_cache.hashsize", &cval));
    if ((chunkcache->hashtable_size = (u_int)cval.val) == 0)
        chunkcache->hashtable_size = WT_CHUNKCACHE_DEFAULT_HASHSIZE;
    else if (chunkcache->hashtable_size < WT_CHUNKCACHE_MINHASHSIZE ||
      chunkcache->hashtable_size > WT_CHUNKCACHE_MAXHASHSIZE)
        WT_RET_MSG(session, EINVAL,
          "chunk cache hashtable size must be between %d and %d entries and we have %u",
          WT_CHUNKCACHE_MINHASHSIZE, WT_CHUNKCACHE_MAXHASHSIZE, chunkcache->hashtable_size);

    WT_RET(__wt_config_gets(session, cfg, "chunk_cache.type", &cval));
    if (cval.len == 0 || WT_STRING_MATCH("dram", cval.str, cval.len) ||
      WT_STRING_MATCH("DRAM", cval.str, cval.len))
        chunkcache->type = WT_CHUNKCACHE_IN_VOLATILE_MEMORY;
    else if (WT_STRING_MATCH("file", cval.str, cval.len) ||
      WT_STRING_MATCH("FILE", cval.str, cval.len)) {
        chunkcache->type = WT_CHUNKCACHE_FILE;
        WT_RET(__wt_config_gets(session, cfg, "chunk_cache.storage_path", &cval));
        if (cval.len == 0)
            WT_RET_MSG(session, EINVAL, "chunk cache storage path not provided in the config.");

        WT_RET(__wt_strndup(session, cval.str, cval.len, &chunkcache->storage_path));
        WT_RET(__wt_open(session, chunkcache->storage_path, WT_FS_OPEN_FILE_TYPE_DATA,
          WT_FS_OPEN_CREATE | WT_FS_OPEN_FORCE_MMAP, &chunkcache->fh));

        WT_RET(__wt_ftruncate(session, chunkcache->fh, (wt_off_t)chunkcache->capacity));

        if (chunkcache->fh->handle->fh_map == NULL) {
            WT_IGNORE_RET(__wt_close(session, &chunkcache->fh));
            WT_RET_MSG(session, EINVAL, "Not on a supported platform for memory-mapping files");
        }
        WT_RET(chunkcache->fh->handle->fh_map(chunkcache->fh->handle, &session->iface,
          (void **)&chunkcache->memory, &mapped_size, NULL));
        if (mapped_size != chunkcache->capacity)
            WT_RET_MSG(session, EINVAL,
              "Storage size mapping %lu does not equal capacity of chunk cache %" PRIu64,
              mapped_size, chunkcache->capacity);

        WT_RET(__wt_calloc(session,
          WT_CHUNKCACHE_BITMAP_SIZE(chunkcache->capacity, chunkcache->chunk_size), sizeof(uint8_t),
          &chunkcache->free_bitmap));
    }

    WT_RET(__wt_config_gets(session, cfg, "chunk_cache.flushed_data_cache_insertion", &cval));
    if (cval.val != 0)
        F_SET(chunkcache, WT_CHUNK_CACHE_FLUSHED_DATA_INSERTION);

    WT_ERR(__wt_rwlock_init(session, &chunkcache->pinned_objects.array_lock));
    WT_ERR(__config_get_sorted_pinned_objects(session, cfg, &pinned_objects, &cnt));
    chunkcache->pinned_objects.array = pinned_objects;
    chunkcache->pinned_objects.entries = cnt;

    WT_ERR(__wt_calloc_def(session, chunkcache->hashtable_size, &chunkcache->hashtable));

    for (i = 0; i < chunkcache->hashtable_size; i++) {
        TAILQ_INIT(&(chunkcache->hashtable[i].colliding_chunks));
        WT_ERR(__wt_spin_init(
          session, &chunkcache->hashtable[i].bucket_lock, "chunk cache bucket lock"));
    }

    WT_ERR(__wt_thread_create(
      session, &chunkcache->evict_thread_tid, __chunkcache_eviction_thread, (void *)session));

    F_SET(chunkcache, WT_CHUNKCACHE_CONFIGURED);
    __wt_verbose(session, WT_VERB_CHUNKCACHE, "configured cache in %s, with capacity %" PRIu64 "",
      (chunkcache->type == WT_CHUNKCACHE_IN_VOLATILE_MEMORY) ? "volatile memory" : "file system",
      chunkcache->capacity);

    return (0);
err:
    __wt_rwlock_destroy(session, &chunkcache->pinned_objects.array_lock);
    return (ret);
}

/*
 * __wt_chunkcache_teardown --
 *     Tear down the chunk cache.
 */
int
__wt_chunkcache_teardown(WT_SESSION_IMPL *session)
{
    WT_CHUNKCACHE *chunkcache;
    WT_DECL_RET;

    chunkcache = &S2C(session)->chunkcache;

    if (!F_ISSET(chunkcache, WT_CHUNKCACHE_CONFIGURED))
        return (0);

    F_SET(chunkcache, WT_CHUNK_CACHE_EXITING);
    WT_TRET(__wt_thread_join(session, &chunkcache->evict_thread_tid));

    __chunkcache_arr_free(session, &chunkcache->pinned_objects.array);
    __wt_rwlock_destroy(session, &chunkcache->pinned_objects.array_lock);

    if (chunkcache->type != WT_CHUNKCACHE_IN_VOLATILE_MEMORY) {
        WT_TRET(__wt_close(session, &chunkcache->fh));
        __wt_free(session, chunkcache->storage_path);
        __wt_free(session, chunkcache->free_bitmap);
    }

    return (ret);
}

#ifdef HAVE_UNITTEST

int
__ut_chunkcache_bitmap_alloc(WT_SESSION_IMPL *session, size_t *bit_index)
{
    return (__chunkcache_bitmap_alloc(session, bit_index));
}

void
__ut_chunkcache_bitmap_free(WT_SESSION_IMPL *session, size_t bit_index)
{
    __chunkcache_bitmap_free(session, bit_index);
}

#endif
