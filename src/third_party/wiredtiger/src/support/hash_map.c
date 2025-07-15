/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_hash_map_init --
 *     Initialize a hash map.
 */
int
__wt_hash_map_init(WT_SESSION_IMPL *session, WT_HASH_MAP **hash_mapp, size_t hash_size)
{
    WT_DECL_RET;
    WT_HASH_MAP *hash_map;
    size_t i;

    hash_map = NULL;

    WT_RET(__wt_calloc_one(session, &hash_map));
    WT_ERR(__wt_calloc_def(session, hash_size, &hash_map->hash));
    WT_ERR(__wt_calloc_def(session, hash_size, &hash_map->hash_locks));

    for (i = 0; i < hash_size; i++) {
        TAILQ_INIT(&hash_map->hash[i]);
        WT_ERR(__wt_spin_init(session, &hash_map->hash_locks[i], "hash map bucket lock"));
    }

    hash_map->hash_size = hash_size;

    *hash_mapp = hash_map;
    return (0);

err:
    if (hash_map->hash_locks != NULL)
        for (i = 0; i < hash_size; i++)
            __wt_spin_destroy(session, &hash_map->hash_locks[i]);
    __wt_free(session, hash_map->hash_locks);
    __wt_free(session, hash_map->hash);
    __wt_free(session, hash_map);
    return (ret);
}

/*
 * __wt_hash_map_destroy --
 *     Destroy a hash map.
 */
void
__wt_hash_map_destroy(WT_SESSION_IMPL *session, WT_HASH_MAP **hash_mapp)
{
    WT_HASH_MAP *hash_map;
    WT_HASH_MAP_ITEM *item;
    size_t i;

    hash_map = *hash_mapp;
    if (hash_map == NULL)
        return;

    for (i = 0; i < hash_map->hash_size; i++) {
        while ((item = TAILQ_FIRST(&hash_map->hash[i])) != NULL) {
            TAILQ_REMOVE(&hash_map->hash[i], item, hashq);
            __wt_free(session, item->key);
            __wt_free(session, item->data);
            __wt_free(session, item);
        }
        __wt_spin_destroy(session, &hash_map->hash_locks[i]);
    }

    __wt_free(session, hash_map->hash_locks);
    __wt_free(session, hash_map->hash);
    __wt_free(session, hash_map);

    *hash_mapp = NULL;
}

/*
 * __hash_map_insert_new --
 *     Insert a new item into the hash map. The hash map must be already locked, and the entry must
 *     not exist.
 */
static int
__hash_map_insert_new(WT_SESSION_IMPL *session, WT_HASH_MAP *hash_map, size_t bucket,
  const void *key, size_t key_size, const void *data, size_t data_size, WT_HASH_MAP_ITEM **itemp)
{
    WT_DECL_RET;
    WT_HASH_MAP_ITEM *item;

    WT_RET(__wt_calloc_one(session, &item));

    WT_ERR(__wt_calloc(session, 1, key_size, &item->key));
    memcpy(item->key, key, key_size);
    item->key_size = key_size;

    WT_ERR(__wt_calloc(session, 1, data_size, &item->data));
    if (data != NULL)
        memcpy(item->data, data, data_size);
    item->data_size = data_size;

    TAILQ_INSERT_HEAD(&hash_map->hash[bucket], item, hashq);
    if (itemp != NULL)
        *itemp = item;
    return (0);

err:
    __wt_free(session, item->key);
    __wt_free(session, item->data);
    __wt_free(session, item);
    return (ret);
}

/*
 * __wt_hash_map_get --
 *     Get an item from a hash map. If the item is not found, optionally insert a zeroed item, but
 *     only if the hash map stores fixed-length items. The get function can keep the hash map locked
 *     if requested, but only if the operation is successful.
 */
int
__wt_hash_map_get(WT_SESSION_IMPL *session, WT_HASH_MAP *hash_map, const void *key, size_t key_size,
  void **datap, size_t *data_sizep, bool insert_if_not_found, bool keep_locked)
{
    WT_DECL_RET;
    WT_HASH_MAP_ITEM *item;
    size_t bucket, data_size;

    if (insert_if_not_found && hash_map->value_size == 0)
        return (EINVAL);

    bucket = __wt_hash_city64(key, key_size) % hash_map->hash_size;
    __wt_spin_lock(session, &hash_map->hash_locks[bucket]);

    /* Find the value if it exists. */
    TAILQ_FOREACH (item, &hash_map->hash[bucket], hashq) {
        if (item->key_size == key_size && memcmp(item->key, key, key_size) == 0) {
            *datap = item->data;
            if (data_sizep != NULL)
                *data_sizep = item->data_size;
            if (!keep_locked)
                __wt_spin_unlock(session, &hash_map->hash_locks[bucket]);
            return (ret);
        }
    }

    /* Insert a zeroed out item if requested. */
    if (insert_if_not_found) {
        data_size = hash_map->value_size;
        WT_ERR(
          __hash_map_insert_new(session, hash_map, bucket, key, key_size, NULL, data_size, &item));
        *datap = item->data;
        if (data_sizep != NULL)
            *data_sizep = data_size;
    } else
        ret = WT_NOTFOUND;

err:
    if (!keep_locked || ret != 0)
        __wt_spin_unlock(session, &hash_map->hash_locks[bucket]);
    return (ret);
}

/*
 * __wt_hash_map_unlock --
 *     Unlock the corresponding hash map bucket. This is used when the caller previously requested
 *     the get call to keep the bucket locked.
 */
void
__wt_hash_map_unlock(
  WT_SESSION_IMPL *session, WT_HASH_MAP *hash_map, const void *key, size_t key_size)
{
    size_t bucket;

    bucket = __wt_hash_city64(key, key_size) % hash_map->hash_size;
    WT_ASSERT(session, __wt_spin_owned(session, &hash_map->hash_locks[bucket]));
    __wt_spin_unlock(session, &hash_map->hash_locks[bucket]);
}
