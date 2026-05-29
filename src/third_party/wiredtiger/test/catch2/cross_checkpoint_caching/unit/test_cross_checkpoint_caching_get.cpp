/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

// Test the cross-checkpoint cache get function. [cross_checkpoint_caching_get]

#include "../cross_checkpoint_caching_test_env.h"

using namespace utils;

TEST_CASE("cross_checkpoint_caching_get: miss on empty cache returns NULL",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_get]")
{
    cross_checkpoint_caching_test_env env;
    const uint8_t addr[] = {0xde, 0xad, 0xbe, 0xef};

    WT_SHARED_DSK_ITEM *got = reinterpret_cast<WT_SHARED_DSK_ITEM *>(0x1);
    __wt_shared_dsk_cache_get(env.session(), addr, sizeof(addr), &got);

    REQUIRE(env.stats()->cache_shared_dsk_miss == 1);
    REQUIRE(env.stats()->cache_shared_dsk_hit == 0);
}

TEST_CASE("cross_checkpoint_caching_get: hit returns inserted item and increments ref_count",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_get]")
{
    cross_checkpoint_caching_test_env env;
    const uint8_t addr[] = {0x01, 0x02, 0x03, 0x04};

    WT_SHARED_DSK_ITEM *put_item = env.put(addr, sizeof(addr));
    REQUIRE(put_item->ref_count == 1);

    WT_SHARED_DSK_ITEM *got = nullptr;
    __wt_shared_dsk_cache_get(env.session(), addr, sizeof(addr), &got);

    REQUIRE(got == put_item);
    REQUIRE(got->data == put_item->data);
    REQUIRE(got->ref_count == 2);
    REQUIRE(got->fid == env.btree_id());
    REQUIRE(got->addr_size == sizeof(addr));
    REQUIRE(memcmp(got->addr, addr, sizeof(addr)) == 0);
    REQUIRE(env.stats()->cache_shared_dsk_hit == 1);
    REQUIRE(env.stats()->cache_shared_dsk_miss == 0);
}

TEST_CASE("cross_checkpoint_caching_get: different addr misses",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_get]")
{
    cross_checkpoint_caching_test_env env;
    const uint8_t addr_a[] = {0x01, 0x02, 0x03, 0x04};
    const uint8_t addr_b[] = {0x05, 0x06, 0x07, 0x08};

    WT_SHARED_DSK_ITEM *put_item = env.put(addr_a, sizeof(addr_a));

    WT_SHARED_DSK_ITEM *got = nullptr;
    __wt_shared_dsk_cache_get(env.session(), addr_b, sizeof(addr_b), &got);

    REQUIRE(got == nullptr);
    REQUIRE(put_item->ref_count == 1);
    REQUIRE(env.stats()->cache_shared_dsk_miss == 1);
}

TEST_CASE("cross_checkpoint_caching_get: different addr_size misses",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_get]")
{
    cross_checkpoint_caching_test_env env;
    const uint8_t addr_full[] = {0x01, 0x02, 0x03, 0x04};
    const uint8_t addr_short[] = {0x01, 0x02, 0x03};

    WT_SHARED_DSK_ITEM *put_item = env.put(addr_full, sizeof(addr_full));

    WT_SHARED_DSK_ITEM *got = nullptr;
    __wt_shared_dsk_cache_get(env.session(), addr_short, sizeof(addr_short), &got);

    REQUIRE(got == nullptr);
    REQUIRE(put_item->ref_count == 1);
    REQUIRE(env.stats()->cache_shared_dsk_miss == 1);
}

TEST_CASE("cross_checkpoint_caching_get: different file id misses even with identical addr",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_get]")
{
    cross_checkpoint_caching_test_env env;
    const uint8_t addr[] = {0x01, 0x02, 0x03, 0x04};

    WT_SHARED_DSK_ITEM *put_item = env.put(addr, sizeof(addr));

    /* Swap the btree id so the get call sees a different file id but the same hash bucket. */
    uint32_t original_id = S2BT(env.session())->id;
    S2BT(env.session())->id = original_id + 1;

    WT_SHARED_DSK_ITEM *got = nullptr;
    __wt_shared_dsk_cache_get(env.session(), addr, sizeof(addr), &got);

    REQUIRE(got == nullptr);
    REQUIRE(put_item->ref_count == 1);
    REQUIRE(env.stats()->cache_shared_dsk_miss == 1);

    S2BT(env.session())->id = original_id;
}

TEST_CASE("cross_checkpoint_caching_get: repeated hits accumulate ref_count and stats",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_get]")
{
    cross_checkpoint_caching_test_env env;
    const uint8_t addr[] = {0xaa, 0xbb};

    WT_SHARED_DSK_ITEM *put_item = env.put(addr, sizeof(addr));

    constexpr int ITERATIONS = 5;
    for (int i = 0; i < ITERATIONS; i++) {
        WT_SHARED_DSK_ITEM *got = nullptr;
        __wt_shared_dsk_cache_get(env.session(), addr, sizeof(addr), &got);
        REQUIRE(got == put_item);
        REQUIRE(got->ref_count == 2 + i);
    }

    REQUIRE(put_item->ref_count == 1 + ITERATIONS);
    REQUIRE(env.stats()->cache_shared_dsk_hit == ITERATIONS);
    REQUIRE(env.stats()->cache_shared_dsk_miss == 0);
}

TEST_CASE("cross_checkpoint_caching_get: two entries in the same bucket are distinguished by addr",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_get]")
{
    // hash_size=1 to force every entry into bucket 0, so all puts collide.
    cross_checkpoint_caching_test_env env(1);
    const uint8_t addr_a[] = {0x10, 0x20};
    const uint8_t addr_b[] = {0x30, 0x40};

    WT_SHARED_DSK_ITEM *item_a = env.put(addr_a, sizeof(addr_a));
    WT_SHARED_DSK_ITEM *item_b = env.put(addr_b, sizeof(addr_b));
    REQUIRE(item_a != item_b);
    // Sanity check that both puts actually inserted distinct entries.
    REQUIRE(env.bucket_size(0) == 2);

    WT_SHARED_DSK_ITEM *got_a = nullptr;
    WT_SHARED_DSK_ITEM *got_b = nullptr;
    __wt_shared_dsk_cache_get(env.session(), addr_a, sizeof(addr_a), &got_a);
    __wt_shared_dsk_cache_get(env.session(), addr_b, sizeof(addr_b), &got_b);

    REQUIRE(got_a == item_a);
    REQUIRE(got_b == item_b);
    REQUIRE(item_a->ref_count == 2);
    REQUIRE(item_b->ref_count == 2);
}

TEST_CASE(
  "cross_checkpoint_caching_get: same addr with different fileid in same bucket distinguished by "
  "fileid",
  "[cross_checkpoint_caching],[cross_checkpoint_caching_get]")
{
    // Same addr means same bucket. Swap the session's btree id between puts so each entry stores
    // a different fileid. get should return the one whose fileid matches the session's current id.
    cross_checkpoint_caching_test_env env;
    const uint8_t addr[] = {0x01, 0x02, 0x03, 0x04};
    const uint32_t fid_a = 100;
    const uint32_t fid_b = 200;

    S2BT(env.session())->id = fid_a;
    WT_SHARED_DSK_ITEM *item_a = env.put(addr, sizeof(addr));

    S2BT(env.session())->id = fid_b;
    WT_SHARED_DSK_ITEM *item_b = env.put(addr, sizeof(addr));

    REQUIRE(item_a != item_b);
    REQUIRE(item_a->fid == fid_a);
    REQUIRE(item_b->fid == fid_b);

    // fid_b is set, get returns item_b.
    WT_SHARED_DSK_ITEM *got_b = nullptr;
    __wt_shared_dsk_cache_get(env.session(), addr, sizeof(addr), &got_b);
    REQUIRE(got_b == item_b);
    REQUIRE(got_b->data == item_b->data);

    // switch to fid_a, get returns item_a.
    S2BT(env.session())->id = fid_a;
    WT_SHARED_DSK_ITEM *got_a = nullptr;
    __wt_shared_dsk_cache_get(env.session(), addr, sizeof(addr), &got_a);
    REQUIRE(got_a == item_a);
    REQUIRE(got_a->data == item_a->data);
}
