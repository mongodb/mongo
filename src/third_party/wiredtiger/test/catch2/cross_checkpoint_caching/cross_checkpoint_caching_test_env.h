/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <catch2/catch.hpp>

#include "../wrappers/connection_wrapper.h"

extern "C" {
#include "wt_internal.h"
}

namespace utils {

constexpr u_int CROSS_CHECKPOINT_CACHING_TEST_HASH_SIZE = 16;
constexpr size_t CROSS_CHECKPOINT_CACHING_TEST_DATA_SIZE = WT_PAGE_HEADER_SIZE;

/*
 * Stands up the minimum state needed to exercise the cross-checkpoint cache against a real
 * connection: a real WT_CACHE (created by wiredtiger_open), an initialized shared dsk cache, a real
 * dhandle + btree obtained by creating a table and opening a cursor on it, and enough of
 * conn->disaggregated_storage to satisfy the disagg assertion in destroy.
 */
class cross_checkpoint_caching_test_env {
public:
    explicit cross_checkpoint_caching_test_env(
      u_int hash_size = CROSS_CHECKPOINT_CACHING_TEST_HASH_SIZE);
    ~cross_checkpoint_caching_test_env();

    cross_checkpoint_caching_test_env(const cross_checkpoint_caching_test_env &) = delete;
    cross_checkpoint_caching_test_env &operator=(
      const cross_checkpoint_caching_test_env &) = delete;

    WT_SESSION_IMPL *session();
    WT_CONNECTION_STATS *stats();
    uint32_t btree_id();

    /* Insert an item at the given addr; on collision returns the existing entry with ref_count
     * incremented rather than inserting a new one. */
    WT_SHARED_DSK_ITEM *put(const uint8_t *addr, size_t addr_size, bool *insertedp = nullptr);

    /* Count the entries currently chained in the given bucket. */
    int bucket_size(u_int bucket);

private:
    connection_wrapper _conn;
    WT_SESSION_IMPL *_session;
    WT_CURSOR *_cursor;
    int _disagg_sentinel;
};

} // namespace utils.
