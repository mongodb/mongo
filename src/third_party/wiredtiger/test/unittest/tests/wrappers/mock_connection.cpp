/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *      All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "mock_connection.h"
#include "../utils.h"

const int DEFAULT_HASH_SIZE = 512;

mock_connection::mock_connection(WT_CONNECTION_IMPL *connection_impl)
    : _connection_impl(connection_impl)
{
}

mock_connection::~mock_connection()
{
    if (_connection_impl->blockhash != nullptr)
        __wt_free(nullptr, _connection_impl->blockhash);
    if (_connection_impl->fhhash != nullptr)
        __wt_free(nullptr, _connection_impl->fhhash);
    if (_connection_impl->block_lock.initialized == 1)
        __wt_spin_destroy(nullptr, &_connection_impl->block_lock);
    __wt_free(nullptr, _connection_impl->chunkcache.free_bitmap);
    __wt_free(nullptr, _connection_impl);
}

std::shared_ptr<mock_connection>
mock_connection::build_test_mock_connection()
{
    WT_CONNECTION_IMPL *connection_impl = nullptr;
    utils::throw_if_non_zero(__wt_calloc(nullptr, 1, sizeof(WT_CONNECTION_IMPL), &connection_impl));
    // Construct a Session object that will now own session.
    return std::shared_ptr<mock_connection>(new mock_connection(connection_impl));
}

int
mock_connection::setup_chunk_cache(
  WT_SESSION_IMPL *session, uint64_t capacity, size_t chunk_size, WT_CHUNKCACHE *&chunkcache)
{
    chunkcache = &_connection_impl->chunkcache;
    memset(chunkcache, 0, sizeof(WT_CHUNKCACHE));
    chunkcache->capacity = capacity;
    chunkcache->chunk_size = chunk_size;
    WT_RET(
      __wt_calloc(session, WT_CHUNKCACHE_BITMAP_SIZE(chunkcache->capacity, chunkcache->chunk_size),
        sizeof(uint8_t), &chunkcache->free_bitmap));

    return 0;
}

int
mock_connection::setup_block_manager(WT_SESSION_IMPL *session)
{
    // Check that there should be no connection flags set.
    WT_ASSERT(session, _connection_impl->flags == 0);

    // Initialize block and file hashmap.
    _connection_impl->hash_size = DEFAULT_HASH_SIZE;
    WT_RET(__wt_calloc_def(session, _connection_impl->hash_size, &_connection_impl->blockhash));
    WT_RET(__wt_calloc_def(session, _connection_impl->hash_size, &_connection_impl->fhhash));
    for (int i = 0; i < _connection_impl->hash_size; ++i) {
        TAILQ_INIT(&_connection_impl->blockhash[i]);
        TAILQ_INIT(&_connection_impl->fhhash[i]);
    }

    // Initialize block and file handle queue.
    TAILQ_INIT(&_connection_impl->blockqh);
    TAILQ_INIT(&_connection_impl->fhqh);

    // Initialize the block manager and file list lock.
    WT_RET(__wt_spin_init(session, &_connection_impl->fh_lock, "file list"));
    WT_RET(__wt_spin_init(session, &_connection_impl->block_lock, "block manager"));

    // Initialize an in-memory file system layer used for testing purposes.
    F_SET(_connection_impl, WT_CONN_IN_MEMORY);
    _connection_impl->home = "";
    WT_RET(__wt_os_inmemory(session));
    return 0;
}
