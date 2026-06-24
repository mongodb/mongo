/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

// Test the cross-checkpoint cache release function. [cross_checkpoint_caching_release]

#include "../cross_checkpoint_caching_test_env.h"

using namespace utils;

TEST_CASE("cross_checkpoint_caching_release: releasing when ref_count > 1 keeps the item",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_release]")
{
    // hash_size=1 so we can inspect the single bucket directly.
    cross_checkpoint_caching_test_env env(1);
    const uint8_t addr[] = {0x01, 0x02};

    WT_SHARED_DSK_ITEM *put_item = env.put(addr, sizeof(addr));

    WT_SHARED_DSK_ITEM *got = nullptr;
    __wt_shared_dsk_cache_get(env.session(), addr, sizeof(addr), &got);
    REQUIRE(got == put_item);
    REQUIRE(put_item->ref_count == 2);

    __wt_shared_dsk_cache_release(env.session(), put_item);

    REQUIRE(put_item->ref_count == 1);
    REQUIRE(env.bucket_size(0) == 1);

    // Releasing when ref_count > 1 keeps the item in the cache.
    WT_SHARED_DSK_ITEM *got_again = nullptr;
    __wt_shared_dsk_cache_get(env.session(), addr, sizeof(addr), &got_again);
    REQUIRE(got_again == put_item);
    REQUIRE(put_item->ref_count == 2);
}

TEST_CASE("cross_checkpoint_caching_release: ref_count reaching zero removes the item",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_release]")
{
    cross_checkpoint_caching_test_env env(1);
    const uint8_t addr[] = {0x01, 0x02};

    WT_SHARED_DSK_ITEM *put_item = env.put(addr, sizeof(addr));
    REQUIRE(put_item->ref_count == 1);
    REQUIRE(env.bucket_size(0) == 1);

    // Drop the only reference: the item is removed and freed.
    __wt_shared_dsk_cache_release(env.session(), put_item);

    REQUIRE(env.bucket_size(0) == 0);

    // Setting got to a fake value to ensure that it is set to nullptr.
    WT_SHARED_DSK_ITEM *got = reinterpret_cast<WT_SHARED_DSK_ITEM *>(0x1);
    __wt_shared_dsk_cache_get(env.session(), addr, sizeof(addr), &got);
    REQUIRE(got == nullptr);
    REQUIRE(env.stats()->cache_shared_dsk_miss == 1);
    REQUIRE(env.stats()->cache_shared_dsk_hit == 0);
}

TEST_CASE(
  "cross_checkpoint_caching_release: only removes the released item, others in same bucket "
  "persist",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_release]")
{
    // hash_size=1 forces both entries into bucket 0.
    cross_checkpoint_caching_test_env env(1);
    const uint8_t addr_a[] = {0x10, 0x20};
    const uint8_t addr_b[] = {0x30, 0x40};

    WT_SHARED_DSK_ITEM *item_a = env.put(addr_a, sizeof(addr_a));
    WT_SHARED_DSK_ITEM *item_b = env.put(addr_b, sizeof(addr_b));
    REQUIRE(env.bucket_size(0) == 2);

    __wt_shared_dsk_cache_release(env.session(), item_a);

    REQUIRE(env.bucket_size(0) == 1);
    // item_b is untouched.
    REQUIRE(item_b->ref_count == 1);

    // Setting got_a to a fake value to ensure that it is set to nullptr.
    // item_b is still in the hash map; item_a is gone.
    WT_SHARED_DSK_ITEM *got_a = reinterpret_cast<WT_SHARED_DSK_ITEM *>(0x1);
    WT_SHARED_DSK_ITEM *got_b = nullptr;
    __wt_shared_dsk_cache_get(env.session(), addr_a, sizeof(addr_a), &got_a);
    __wt_shared_dsk_cache_get(env.session(), addr_b, sizeof(addr_b), &got_b);
    REQUIRE(got_a == nullptr);
    REQUIRE(got_b == item_b);
}

TEST_CASE(
  "cross_checkpoint_caching_release: N releases undo N gets, and one more removes the entry",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_release]")
{
    cross_checkpoint_caching_test_env env(1);
    const uint8_t addr[] = {0x01, 0x02};

    WT_SHARED_DSK_ITEM *put_item = env.put(addr, sizeof(addr));

    constexpr int ITERATIONS = 5;
    for (int i = 0; i < ITERATIONS; i++) {
        WT_SHARED_DSK_ITEM *got = nullptr;
        __wt_shared_dsk_cache_get(env.session(), addr, sizeof(addr), &got);
        REQUIRE(got == put_item);
    }
    REQUIRE(put_item->ref_count == 1 + ITERATIONS);

    for (int i = 0; i < ITERATIONS; i++) {
        __wt_shared_dsk_cache_release(env.session(), put_item);
        REQUIRE(put_item->ref_count == ITERATIONS - i);
        REQUIRE(env.bucket_size(0) == 1);
    }

    // One final release drops ref_count to zero and removes the item.
    __wt_shared_dsk_cache_release(env.session(), put_item);
    REQUIRE(env.bucket_size(0) == 0);
}

TEST_CASE(
  "cross_checkpoint_caching_release: same addr with different fileid in same bucket only "
  "removes the matching entry",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_release]")
{
    // Same addr means same bucket. Swap the session's btree id between puts so each entry stores
    // a different fileid. Releasing one entry must not disturb the other.
    cross_checkpoint_caching_test_env env(1);
    const uint8_t addr[] = {0x01, 0x02};
    const uint32_t fid_a = 100;
    const uint32_t fid_b = 200;

    S2BT(env.session())->id = fid_a;
    WT_SHARED_DSK_ITEM *item_a = env.put(addr, sizeof(addr));

    S2BT(env.session())->id = fid_b;
    WT_SHARED_DSK_ITEM *item_b = env.put(addr, sizeof(addr));

    REQUIRE(env.bucket_size(0) == 2);

    __wt_shared_dsk_cache_release(env.session(), item_a);

    REQUIRE(env.bucket_size(0) == 1);
    REQUIRE(item_b->ref_count == 1);

    // fid_b is set, get returns item_b.
    WT_SHARED_DSK_ITEM *got_b = nullptr;
    __wt_shared_dsk_cache_get(env.session(), addr, sizeof(addr), &got_b);
    REQUIRE(got_b == item_b);

    // Switch to fid_a, the entry is gone.
    // Setting got to a fake value to ensure that it is set to nullptr.
    S2BT(env.session())->id = fid_a;
    WT_SHARED_DSK_ITEM *got_a = reinterpret_cast<WT_SHARED_DSK_ITEM *>(0x1);
    __wt_shared_dsk_cache_get(env.session(), addr, sizeof(addr), &got_a);
    REQUIRE(got_a == nullptr);
}

TEST_CASE(
  "cross_checkpoint_caching_release: shared image bytes are drained only on the last release",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_release]")
{
    cross_checkpoint_caching_test_env env(1);
    WT_CACHE *cache = S2C(env.session())->cache;
    const uint8_t addr[] = {0x01, 0x02};
    const size_t image_size = CROSS_CHECKPOINT_CACHING_TEST_DATA_SIZE;

    uint64_t inmem_before = __wt_atomic_load_uint64_relaxed(&cache->bytes_inmem);
    uint64_t image_before = __wt_atomic_load_uint64_relaxed(&cache->bytes_image_leaf);

    /* Two pages share one disk image: counted once, reference count two. */
    WT_SHARED_DSK_ITEM *item = env.put(addr, sizeof(addr));
    env.put(addr, sizeof(addr));
    REQUIRE(item->ref_count == 2);
    REQUIRE(__wt_atomic_load_uint64_relaxed(&cache->bytes_inmem) == inmem_before + image_size);
    REQUIRE(__wt_atomic_load_uint64_relaxed(&cache->bytes_image_leaf) == image_before + image_size);

    /* Releasing one of two references keeps the image, so its bytes stay counted. */
    __wt_shared_dsk_cache_release(env.session(), item);
    REQUIRE(item->ref_count == 1);
    REQUIRE(__wt_atomic_load_uint64_relaxed(&cache->bytes_inmem) == inmem_before + image_size);
    REQUIRE(__wt_atomic_load_uint64_relaxed(&cache->bytes_image_leaf) == image_before + image_size);

    /* The last release removes the image and drains its bytes once, back to the baseline. */
    __wt_shared_dsk_cache_release(env.session(), item);
    REQUIRE(__wt_atomic_load_uint64_relaxed(&cache->bytes_inmem) == inmem_before);
    REQUIRE(__wt_atomic_load_uint64_relaxed(&cache->bytes_image_leaf) == image_before);
}
