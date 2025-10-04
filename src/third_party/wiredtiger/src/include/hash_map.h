/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * WT_HASH_MAP_ITEM --
 *     An item in the hash map.
 */
struct __wt_hash_map_item {
    TAILQ_ENTRY(__wt_hash_map_item) hashq;

    void *key;
    size_t key_size;

    void *data;
    size_t data_size;
};

/*
 * WT_HASH_MAP --
 *     A generic implementation of a hash map. The hash map is a simple array of linked lists, where
 *     each linked list is a bucket. The hash map owns the memory for the keys and values, so the
 *     caller must not free them.
 *
 *     Note that this hash map is not optimized for performance; it is designed for low-performance
 *     use cases and for prototyping.
 */
struct __wt_hash_map {

    TAILQ_HEAD(__wt_hash_map_hash, __wt_hash_map_item) * hash;
    WT_SPINLOCK *hash_locks;
    size_t hash_size;

    size_t value_size; /* Size of the value in bytes, if fixed-length. */
};
