/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "cross_checkpoint_caching_test_env.h"

#include <cstring>

#include "../utils.h"

namespace utils {

static constexpr const char *CROSS_CHECKPOINT_CACHING_TEST_URI =
  "table:cross_checkpoint_caching_test";

cross_checkpoint_caching_test_env::cross_checkpoint_caching_test_env(u_int hash_size)
    : _conn(DB_HOME, "create,statistics=(fast)"), _session(nullptr), _cursor(nullptr),
      _disagg_sentinel(0)
{
    _session = _conn.create_session();
    WT_SESSION *session = &_session->iface;

    /*
     * Create a real table and open a cursor on it so the btree has a WT-assigned id. We borrow the
     * cursor's dhandle for _session->dhandle so the btree id resolves against a real btree.
     */
    REQUIRE(session->create(
              session, CROSS_CHECKPOINT_CACHING_TEST_URI, "key_format=S,value_format=S") == 0);
    REQUIRE(session->open_cursor(
              session, CROSS_CHECKPOINT_CACHING_TEST_URI, nullptr, nullptr, &_cursor) == 0);
    _session->dhandle = ((WT_CURSOR_BTREE *)_cursor)->dhandle;

    WT_CONNECTION_IMPL *conn = S2C(_session);

    conn->disaggregated_storage.page_log_meta =
      reinterpret_cast<WT_PAGE_LOG_HANDLE *>(&_disagg_sentinel);

    REQUIRE(__wti_shared_dsk_cache_init(_session, hash_size) == 0);
    conn->cache->shared_dsk_cache.enabled = true;
}

cross_checkpoint_caching_test_env::~cross_checkpoint_caching_test_env()
{
    WT_CONNECTION_IMPL *conn = S2C(_session);

    __wti_shared_dsk_cache_destroy(_session);
    /* Prevent the connection-close cache destroy from running again. */
    conn->cache->shared_dsk_cache.enabled = false;

    conn->disaggregated_storage.page_log_meta = nullptr;

    if (_cursor != nullptr)
        _cursor->close(_cursor);
    _session->dhandle = nullptr;

    /* Drop the table so it doesn't linger in DB_HOME across tests. */
    WT_SESSION *session = &_session->iface;
    (void)session->drop(session, CROSS_CHECKPOINT_CACHING_TEST_URI, "force=true");
}

WT_SESSION_IMPL *
cross_checkpoint_caching_test_env::session()
{
    return _session;
}

WT_CONNECTION_STATS *
cross_checkpoint_caching_test_env::stats()
{
    return S2C(_session)->stats[_session->stat_conn_bucket];
}

uint32_t
cross_checkpoint_caching_test_env::btree_id()
{
    return S2BT(_session)->id;
}

WT_SHARED_DSK_ITEM *
cross_checkpoint_caching_test_env::put(const uint8_t *addr, size_t addr_size)
{
    void *data = nullptr;
    REQUIRE(__wt_calloc(_session, 1, CROSS_CHECKPOINT_CACHING_TEST_DATA_SIZE, &data) == 0);

    WT_PAGE_BLOCK_META block_meta;
    memset(&block_meta, 0, sizeof(block_meta));

    WT_SHARED_DSK_ITEM *item = nullptr;
    bool inserted = false;
    REQUIRE(__wt_shared_dsk_cache_put(_session, data, CROSS_CHECKPOINT_CACHING_TEST_DATA_SIZE, addr,
              addr_size, &block_meta, &item, &inserted) == 0);
    REQUIRE(inserted);
    REQUIRE(item != nullptr);
    return item;
}

int
cross_checkpoint_caching_test_env::bucket_size(u_int bucket)
{
    WT_SHARED_DSK_CACHE *cache = &S2C(_session)->cache->shared_dsk_cache;
    REQUIRE(bucket < cache->hash_size);

    int count = 0;
    WT_SHARED_DSK_ITEM *item;
    TAILQ_FOREACH (item, &cache->hash[bucket], hashq)
        count++;
    return count;
}

} // namespace utils.
