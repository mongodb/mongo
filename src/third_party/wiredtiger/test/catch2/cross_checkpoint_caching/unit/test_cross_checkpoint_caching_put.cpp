/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/* Test the cross-checkpoint cache put function. [cross_checkpoint_caching_put] */

#include "../cross_checkpoint_caching_test_env.h"

using namespace utils;

TEST_CASE("cross_checkpoint_caching_put: inserting an item sets ref_count to 1",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_put]")
{
    cross_checkpoint_caching_test_env env(1);
    const uint8_t addr[] = {0x01, 0x02};

    WT_SHARED_DSK_ITEM *item = env.put(addr, sizeof(addr));

    REQUIRE(item != nullptr);
    REQUIRE(item->ref_count == 1);
    REQUIRE(item->fid == env.btree_id());
    REQUIRE(item->addr_size == sizeof(addr));
    REQUIRE(memcmp(item->addr, addr, sizeof(addr)) == 0);
    REQUIRE(env.bucket_size(0) == 1);
}

TEST_CASE("cross_checkpoint_caching_put: inserting an item makes it retrievable via get",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_put]")
{
    cross_checkpoint_caching_test_env env;
    const uint8_t addr[] = {0x01, 0x02};

    WT_SHARED_DSK_ITEM *put_item = env.put(addr, sizeof(addr));

    WT_SHARED_DSK_ITEM *got = nullptr;
    __wt_shared_dsk_cache_get(env.session(), addr, sizeof(addr), &got);

    REQUIRE(got == put_item);
    REQUIRE(env.stats()->cache_shared_dsk_hit == 1);
}

TEST_CASE(
  "cross_checkpoint_caching_put: putting the same addr twice increments ref_count and returns "
  "the existing item",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_put]")
{
    cross_checkpoint_caching_test_env env(1);
    const uint8_t addr[] = {0x01, 0x02};

    WT_SHARED_DSK_ITEM *first = env.put(addr, sizeof(addr));
    REQUIRE(first->ref_count == 1);
    REQUIRE(env.bucket_size(0) == 1);

    bool inserted;
    WT_SHARED_DSK_ITEM *second = env.put(addr, sizeof(addr), &inserted);

    /* Collision: the existing entry is returned with ref_count incremented, no new entry inserted.
     */
    REQUIRE(!inserted);
    REQUIRE(second == first);
    REQUIRE(first->ref_count == 2);
    REQUIRE(env.bucket_size(0) == 1);
}

TEST_CASE("cross_checkpoint_caching_put: two different addrs produce two distinct entries",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_put]")
{
    cross_checkpoint_caching_test_env env(1);
    const uint8_t addr_a[] = {0x10, 0x20};
    const uint8_t addr_b[] = {0x30, 0x40};

    WT_SHARED_DSK_ITEM *item_a = env.put(addr_a, sizeof(addr_a));
    WT_SHARED_DSK_ITEM *item_b = env.put(addr_b, sizeof(addr_b));

    REQUIRE(item_a != item_b);
    REQUIRE(item_a->ref_count == 1);
    REQUIRE(item_b->ref_count == 1);
    REQUIRE(env.bucket_size(0) == 2);
}

TEST_CASE(
  "cross_checkpoint_caching_put: same addr with different fileid produces two distinct entries",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_put]")
{
    /* Same addr hashes to the same bucket. Different fid means they are distinct entries. */
    cross_checkpoint_caching_test_env env(1);
    const uint8_t addr[] = {0x01, 0x02};
    const uint32_t fid_a = 100;
    const uint32_t fid_b = 200;

    S2BT(env.session())->id = fid_a;
    WT_SHARED_DSK_ITEM *item_a = env.put(addr, sizeof(addr));

    S2BT(env.session())->id = fid_b;
    WT_SHARED_DSK_ITEM *item_b = env.put(addr, sizeof(addr));

    REQUIRE(item_a != item_b);
    REQUIRE(item_a->fid == fid_a);
    REQUIRE(item_b->fid == fid_b);
    REQUIRE(item_a->ref_count == 1);
    REQUIRE(item_b->ref_count == 1);
    REQUIRE(env.bucket_size(0) == 2);
}

TEST_CASE(
  "cross_checkpoint_caching_put: collision on same addr and fid does not insert a new entry",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_put]")
{
    cross_checkpoint_caching_test_env env(1);
    const uint8_t addr[] = {0x01, 0x02};

    WT_SHARED_DSK_ITEM *item = env.put(addr, sizeof(addr));

    constexpr int ITERATIONS = 4;
    for (int i = 0; i < ITERATIONS; i++) {
        bool inserted;
        WT_SHARED_DSK_ITEM *collision = env.put(addr, sizeof(addr), &inserted);
        REQUIRE(!inserted);
        REQUIRE(collision == item);
        REQUIRE(item->ref_count == 2 + i);
        REQUIRE(env.bucket_size(0) == 1);
    }
}

TEST_CASE("cross_checkpoint_caching_put: a shared disk image is counted once in cache usage",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_put]")
{
    cross_checkpoint_caching_test_env env(1);
    WT_CACHE *cache = S2C(env.session())->cache;
    const uint8_t addr[] = {0x01, 0x02};
    const size_t image_size = CROSS_CHECKPOINT_CACHING_TEST_DATA_SIZE;

    uint64_t inmem_before = __wt_atomic_load_uint64_relaxed(&cache->bytes_inmem);
    uint64_t image_before = __wt_atomic_load_uint64_relaxed(&cache->bytes_image_leaf);

    /* Inserting the image accounts its bytes once in the cache totals. */
    WT_SHARED_DSK_ITEM *item = env.put(addr, sizeof(addr));
    REQUIRE(item->ref_count == 1);
    REQUIRE(__wt_atomic_load_uint64_relaxed(&cache->bytes_inmem) == inmem_before + image_size);
    REQUIRE(__wt_atomic_load_uint64_relaxed(&cache->bytes_image_leaf) == image_before + image_size);

    /* Sharing the image via a put collision bumps the reference count without recounting bytes. */
    env.put(addr, sizeof(addr));
    REQUIRE(item->ref_count == 2);
    REQUIRE(__wt_atomic_load_uint64_relaxed(&cache->bytes_inmem) == inmem_before + image_size);
    REQUIRE(__wt_atomic_load_uint64_relaxed(&cache->bytes_image_leaf) == image_before + image_size);

    /* Sharing it via get behaves the same way. */
    WT_SHARED_DSK_ITEM *got = nullptr;
    __wt_shared_dsk_cache_get(env.session(), addr, sizeof(addr), &got);
    REQUIRE(got == item);
    REQUIRE(item->ref_count == 3);
    REQUIRE(__wt_atomic_load_uint64_relaxed(&cache->bytes_inmem) == inmem_before + image_size);
    REQUIRE(__wt_atomic_load_uint64_relaxed(&cache->bytes_image_leaf) == image_before + image_size);
}
