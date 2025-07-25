/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2020 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _RDMAP_H_
#define _RDMAP_H_

/**
 * @name Hash maps.
 *
 * Memory of key and value are allocated by the user but owned by the hash map
 * until elements are deleted or overwritten.
 *
 * The lower-case API provides a generic typeless (void *) hash map while
 * the upper-case API provides a strictly typed hash map implemented as macros
 * on top of the generic API.
 *
 * See rd_map_init(), et.al, for the generic API and RD_MAP_INITIALIZER()
 * for the typed API.
 *
 * @remark Not thread safe.
 */


/**
 * @struct Map element. This is the internal representation
 *         of the element and exposed to the user for iterating over the hash.
 */
typedef struct rd_map_elem_s {
        LIST_ENTRY(rd_map_elem_s) hlink; /**< Hash bucket link */
        LIST_ENTRY(rd_map_elem_s) link;  /**< Iterator link */
        unsigned int hash;               /**< Key hash value */
        const void *key;                 /**< Key (memory owned by map) */
        const void *value;               /**< Value (memory owned by map) */
} rd_map_elem_t;


/**
 * @struct Hash buckets (internal use).
 */
struct rd_map_buckets {
        LIST_HEAD(, rd_map_elem_s) * p; /**< Hash buckets array */
        int cnt;                        /**< Bucket count */
};


/**
 * @struct Hash map.
 */
typedef struct rd_map_s {
        struct rd_map_buckets rmap_buckets; /**< Hash buckets */
        int rmap_cnt;                       /**< Element count */

        LIST_HEAD(, rd_map_elem_s)
        rmap_iter; /**< Element list for iterating
                    *   over all elements. */

        int (*rmap_cmp)(const void *a, const void *b); /**< Key comparator */
        unsigned int (*rmap_hash)(const void *key);    /**< Key hash function */
        void (*rmap_destroy_key)(void *key);           /**< Optional key free */
        void (*rmap_destroy_value)(void *value); /**< Optional value free */

        void *rmap_opaque;
} rd_map_t;



/**
 * @brief Set/overwrite value in map.
 *
 * If an existing entry with the same key already exists its key and value
 * will be freed with the destroy_key and destroy_value functions
 * passed to rd_map_init().
 *
 * The map assumes memory ownership of both the \p key and \p value and will
 * use the destroy_key and destroy_value functions (if set) to free
 * the key and value memory when the map is destroyed or element removed.
 *
 * @returns the map element.
 */
rd_map_elem_t *rd_map_set(rd_map_t *rmap, void *key, void *value);


/**
 * @brief Look up \p key in the map and return its value, or NULL
 *        if \p key was not found.
 *
 * The returned memory is still owned by the map.
 */
void *rd_map_get(const rd_map_t *rmap, const void *key);


/**
 * @brief Delete \p key from the map, if it exists.
 *
 * The destroy_key and destroy_value functions (if set) will be used
 * to free the key and value memory.
 */
void rd_map_delete(rd_map_t *rmap, const void *key);


/** Key or Value Copy function signature. */
typedef void *(rd_map_copy_t)(const void *key_or_value);


/**
 * @brief Copy all elements from \p src to \p dst.
 *        \p dst must be initialized and compatible with \p src.
 *
 * @param dst Destination map to copy to.
 * @param src Source map to copy from.
 * @param key_copy Key copy callback. If NULL the \p dst key will just
 *                 reference the \p src key.
 * @param value_copy Value copy callback. If NULL the \p dst value will just
 *                   reference the \p src value.
 */
void rd_map_copy(rd_map_t *dst,
                 const rd_map_t *src,
                 rd_map_copy_t *key_copy,
                 rd_map_copy_t *value_copy);


/**
 * @returns the current number of elements in the map.
 */
size_t rd_map_cnt(const rd_map_t *rmap);

/**
 * @returns true if map is empty, else false.
 */
rd_bool_t rd_map_is_empty(const rd_map_t *rmap);


/**
 * @brief Iterate over all elements in the map.
 *
 * @warning The map MUST NOT be modified during the loop.
 *
 * @remark This is part of the untyped generic API.
 */
#define RD_MAP_FOREACH_ELEM(ELEM, RMAP)                                        \
        for (rd_map_iter_begin((RMAP), &(ELEM)); rd_map_iter(&(ELEM));         \
             rd_map_iter_next(&(ELEM)))


/**
 * @brief Begin iterating \p rmap, first element is set in \p *elem.
 */
void rd_map_iter_begin(const rd_map_t *rmap, const rd_map_elem_t **elem);

/**
 * @returns 1 if \p *elem is a valid iteration element, else 0.
 */
static RD_INLINE RD_UNUSED int rd_map_iter(const rd_map_elem_t **elem) {
        return *elem != NULL;
}

/**
 * @brief Advances the iteration to the next element.
 */
static RD_INLINE RD_UNUSED void rd_map_iter_next(const rd_map_elem_t **elem) {
        *elem = LIST_NEXT(*elem, link);
}


/**
 * @brief Initialize a map that is expected to hold \p expected_cnt elements.
 *
 * @param expected_cnt Expected number of elements in the map,
 *                     this is used to select a suitable bucket count.
 *                     Passing a value of 0 will set the bucket count
 *                     to a reasonable default.
 * @param cmp Key comparator that must return 0 if the two keys match.
 * @param hash Key hashing function that is used to map a key to a bucket.
 *             It must return an integer hash >= 0 of the key.
 * @param destroy_key (Optional) When an element is deleted or overwritten
 *                    this function will be used to free the key memory.
 * @param destroy_value (Optional) When an element is deleted or overwritten
 *                      this function will be used to free the value memory.
 *
 * Destroy the map with rd_map_destroy()
 *
 * @remarks The map is not thread-safe.
 */
void rd_map_init(rd_map_t *rmap,
                 size_t expected_cnt,
                 int (*cmp)(const void *a, const void *b),
                 unsigned int (*hash)(const void *key),
                 void (*destroy_key)(void *key),
                 void (*destroy_value)(void *value));


/**
 * @brief Internal use
 */
struct rd_map_buckets rd_map_alloc_buckets(size_t expected_cnt);


/**
 * @brief Empty the map and free all elements.
 */
void rd_map_clear(rd_map_t *rmap);


/**
 * @brief Free all elements in the map and free all memory associated
 *        with the map, but not the rd_map_t itself.
 *
 * The map is unusable after this call but can be re-initialized using
 * rd_map_init().
 *
 * @sa rd_map_clear()
 */
void rd_map_destroy(rd_map_t *rmap);


/**
 * @brief String comparator for (const char *) keys.
 */
int rd_map_str_cmp(const void *a, const void *b);


/**
 * @brief String hash function (djb2) for (const char *) keys.
 */
unsigned int rd_map_str_hash(const void *a);



/**
 * @name Typed hash maps.
 *
 * Typed hash maps provides a type-safe layer on top of the standard hash maps.
 */

/**
 * @brief Define a typed map type which can later be used with
 *        RD_MAP_INITIALIZER() and typed RD_MAP_*() API.
 */
#define RD_MAP_TYPE(KEY_TYPE, VALUE_TYPE)                                      \
        struct {                                                               \
                rd_map_t rmap;                                                 \
                KEY_TYPE key;                                                  \
                VALUE_TYPE value;                                              \
                const rd_map_elem_t *elem;                                     \
        }

/**
 * @brief Initialize a typed hash map. The left hand side variable must be
 *        a typed hash map defined by RD_MAP_TYPE().
 *
 * The typed hash map is a macro layer on top of the rd_map_t implementation
 * that provides type safety.
 * The methods are the same as the underlying implementation but in all caps
 * (to indicate their macro use), e.g., RD_MAP_SET() is the typed version
 * of rd_map_set().
 *
 * @param EXPECTED_CNT Expected number of elements in hash.
 * @param KEY_TYPE The type of the hash key.
 * @param VALUE_TYPE The type of the hash value.
 * @param CMP Comparator function for the key.
 * @param HASH Hash function for the key.
 * @param DESTROY_KEY Destructor for the key type.
 * @param DESTROY_VALUE Destructor for the value type.
 *
 * @sa rd_map_init()
 */
#define RD_MAP_INITIALIZER(EXPECTED_CNT, CMP, HASH, DESTROY_KEY,                \
                           DESTROY_VALUE)                                       \
        {                                                                       \
                .rmap = {                                                       \
                        .rmap_buckets     = rd_map_alloc_buckets(EXPECTED_CNT), \
                        .rmap_cmp         = CMP,                                \
                        .rmap_hash        = HASH,                               \
                        .rmap_destroy_key = DESTROY_KEY,                        \
                        .rmap_destroy_value = DESTROY_VALUE                     \
                }                                                               \
        }


/**
 * @brief Initialize a locally-defined typed hash map.
 *        This hash map can only be used in the current scope/function
 *        as its type is private to this initializement.
 *
 * @param RMAP Hash map variable name.
 *
 * For the other parameters, see RD_MAP_INITIALIZER().
 *
 * @sa RD_MAP_INITIALIZER()
 */
#define RD_MAP_LOCAL_INITIALIZER(RMAP, EXPECTED_CNT, KEY_TYPE, VALUE_TYPE,     \
                                 CMP, HASH, DESTROY_KEY, DESTROY_VALUE)        \
        struct {                                                               \
                rd_map_t rmap;                                                 \
                KEY_TYPE key;                                                  \
                VALUE_TYPE value;                                              \
                const rd_map_elem_t *elem;                                     \
        } RMAP = RD_MAP_INITIALIZER(EXPECTED_CNT, CMP, HASH, DESTROY_KEY,      \
                                    DESTROY_VALUE)


/**
 * @brief Initialize typed map \p RMAP.
 *
 * @sa rd_map_init()
 */
#define RD_MAP_INIT(RMAP, EXPECTED_CNT, CMP, HASH, DESTROY_KEY, DESTROY_VALUE) \
        rd_map_init(&(RMAP)->rmap, EXPECTED_CNT, CMP, HASH, DESTROY_KEY,       \
                    DESTROY_VALUE)


/**
 * @brief Allocate and initialize a typed map.
 */


/**
 * @brief Typed hash map: Set key/value in map.
 *
 * @sa rd_map_set()
 */
#define RD_MAP_SET(RMAP, KEY, VALUE)                                           \
        ((RMAP)->key = KEY, (RMAP)->value = VALUE,                             \
         rd_map_set(&(RMAP)->rmap, (void *)(RMAP)->key,                        \
                    (void *)(RMAP)->value))

/**
 * @brief Typed hash map: Get value for key.
 *
 * @sa rd_map_get()
 */
#define RD_MAP_GET(RMAP, KEY)                                                  \
        ((RMAP)->key   = (KEY),                                                \
         (RMAP)->value = rd_map_get(&(RMAP)->rmap, (RMAP)->key),               \
         (RMAP)->value)



/**
 * @brief Get value for key. If key does not exist in map a new
 *        entry is added using the DEFAULT_CODE.
 */
#define RD_MAP_GET_OR_SET(RMAP, KEY, DEFAULT_CODE)                             \
        (RD_MAP_GET(RMAP, KEY)                                                 \
             ? (RMAP)->value                                                   \
             : (RD_MAP_SET(RMAP, (RMAP)->key, DEFAULT_CODE), (RMAP)->value))


/**
 * @brief Typed hash map: Delete element by key.
 *
 * The destroy_key and destroy_value functions (if set) will be used
 * to free the key and value memory.
 *
 * @sa rd_map_delete()
 */
#define RD_MAP_DELETE(RMAP, KEY)                                               \
        ((RMAP)->key = (KEY), rd_map_delete(&(RMAP)->rmap, (RMAP)->key))


/**
 * @brief Copy all elements from \p SRC to \p DST.
 *        \p DST must be initialized and compatible with \p SRC.
 *
 * @param DST Destination map to copy to.
 * @param SRC Source map to copy from.
 * @param KEY_COPY Key copy callback. If NULL the \p DST key will just
 *                 reference the \p SRC key.
 * @param VALUE_COPY Value copy callback. If NULL the \p DST value will just
 *                   reference the \p SRC value.
 */
#define RD_MAP_COPY(DST, SRC, KEY_COPY, VALUE_COPY)                            \
        do {                                                                   \
                if ((DST) != (SRC)) /*implicit type-check*/                    \
                        rd_map_copy(&(DST)->rmap, &(SRC)->rmap, KEY_COPY,      \
                                    VALUE_COPY);                               \
        } while (0)


/**
 * @brief Empty the map and free all elements.
 *
 * @sa rd_map_clear()
 */
#define RD_MAP_CLEAR(RMAP) rd_map_clear(&(RMAP)->rmap)


/**
 * @brief Typed hash map: Destroy hash map.
 *
 * @sa rd_map_destroy()
 */
#define RD_MAP_DESTROY(RMAP) rd_map_destroy(&(RMAP)->rmap)


/**
 * @brief Typed hash map: Destroy and free the hash map.
 *
 * @sa rd_map_destroy()
 */
#define RD_MAP_DESTROY_AND_FREE(RMAP)                                          \
        do {                                                                   \
                rd_map_destroy(&(RMAP)->rmap);                                 \
                rd_free(RMAP);                                                 \
        } while (0)


/**
 * @brief Typed hash map: Iterate over all elements in the map.
 *
 * @warning The current or previous elements may be removed, but the next
 *          element after the current one MUST NOT be modified during the loop.
 *
 * @warning RD_MAP_FOREACH() only supports one simultaneous invocation,
 *          that is, special care must be taken not to call FOREACH() from
 *          within a FOREACH() or FOREACH_KEY() loop on the same map.
 *          This is due to how RMAP->elem is used as the iterator.
 *          This restriction is unfortunately not enforced at build or run time.
 *
 * @remark The \p RMAP may not be const.
 */
#define RD_MAP_FOREACH(K, V, RMAP)                                             \
        for (rd_map_iter_begin(&(RMAP)->rmap, &(RMAP)->elem), (K) = NULL,      \
                                                              (V) = NULL;      \
             rd_map_iter(&(RMAP)->elem) &&                                     \
             ((RMAP)->key = (void *)(RMAP)->elem->key, (K) = (RMAP)->key,      \
             (RMAP)->value = (void *)(RMAP)->elem->value, (V) = (RMAP)->value, \
             rd_map_iter_next(&(RMAP)->elem), rd_true);)


/**
 * @brief Typed hash map: Iterate over all keys in the map.
 *
 * @warning The current or previous elements may be removed, but the next
 *          element after the current one MUST NOT be modified during the loop.
 *
 * @warning RD_MAP_FOREACH_KEY() only supports one simultaneous invocation,
 *          that is, special care must be taken not to call FOREACH_KEY() from
 *          within a FOREACH() or FOREACH_KEY() loop on the same map.
 *          This is due to how RMAP->elem is used as the iterator.
 *          This restriction is unfortunately not enforced at build or run time.
 *
 * @remark The \p RMAP may not be const.
 */
#define RD_MAP_FOREACH_KEY(K, RMAP)                                            \
        for (rd_map_iter_begin(&(RMAP)->rmap, &(RMAP)->elem), (K) = NULL;      \
             rd_map_iter(&(RMAP)->elem) &&                                     \
             ((RMAP)->key = (void *)(RMAP)->elem->key, (K) = (RMAP)->key,      \
             rd_map_iter_next(&(RMAP)->elem), rd_true);)


/**
 * @returns the number of elements in the map.
 */
#define RD_MAP_CNT(RMAP) rd_map_cnt(&(RMAP)->rmap)

/**
 * @returns true if map is empty, else false.
 */
#define RD_MAP_IS_EMPTY(RMAP) rd_map_is_empty(&(RMAP)->rmap)

#endif /* _RDMAP_H_ */
