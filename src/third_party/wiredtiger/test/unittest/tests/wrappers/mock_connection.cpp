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

MockConnection::MockConnection(WT_CONNECTION_IMPL *connectionImpl) : _connectionImpl(connectionImpl)
{
}

MockConnection::~MockConnection()
{
    if (_connectionImpl->blockhash != nullptr)
        __wt_free(nullptr, _connectionImpl->blockhash);
    if (_connectionImpl->fhhash != nullptr)
        __wt_free(nullptr, _connectionImpl->fhhash);
    if (_connectionImpl->block_lock.initialized == 1)
        __wt_spin_destroy(nullptr, &_connectionImpl->block_lock);
    __wt_free(nullptr, _connectionImpl->chunkcache.free_bitmap);
    __wt_free(nullptr, _connectionImpl);
}

std::shared_ptr<MockConnection>
MockConnection::buildTestMockConnection()
{
    WT_CONNECTION_IMPL *connectionImpl = nullptr;
    utils::throwIfNonZero(__wt_calloc(nullptr, 1, sizeof(WT_CONNECTION_IMPL), &connectionImpl));
    // Construct a Session object that will now own session.
    return std::shared_ptr<MockConnection>(new MockConnection(connectionImpl));
}

int
MockConnection::setupChunkCache(
  WT_SESSION_IMPL *session, uint64_t capacity, size_t chunk_size, WT_CHUNKCACHE *&chunkcache)
{
    chunkcache = &_connectionImpl->chunkcache;
    memset(chunkcache, 0, sizeof(WT_CHUNKCACHE));
    chunkcache->capacity = capacity;
    chunkcache->chunk_size = chunk_size;
    WT_RET(
      __wt_calloc(session, WT_CHUNKCACHE_BITMAP_SIZE(chunkcache->capacity, chunkcache->chunk_size),
        sizeof(uint8_t), &chunkcache->free_bitmap));

    return 0;
}

int
MockConnection::setupBlockManager(WT_SESSION_IMPL *session)
{
    // Check that there should be no connection flags set.
    WT_ASSERT(session, _connectionImpl->flags == 0);

    // Initialize block and file hashmap.
    _connectionImpl->hash_size = DEFAULT_HASH_SIZE;
    WT_RET(__wt_calloc_def(session, _connectionImpl->hash_size, &_connectionImpl->blockhash));
    WT_RET(__wt_calloc_def(session, _connectionImpl->hash_size, &_connectionImpl->fhhash));
    for (int i = 0; i < _connectionImpl->hash_size; ++i) {
        TAILQ_INIT(&_connectionImpl->blockhash[i]);
        TAILQ_INIT(&_connectionImpl->fhhash[i]);
    }

    // Initialize block and file handle queue.
    TAILQ_INIT(&_connectionImpl->blockqh);
    TAILQ_INIT(&_connectionImpl->fhqh);

    // Initialize the block manager and file list lock.
    WT_RET(__wt_spin_init(session, &_connectionImpl->fh_lock, "file list"));
    WT_RET(__wt_spin_init(session, &_connectionImpl->block_lock, "block manager"));

    // Initialize an in-memory file system layer used for testing purposes.
    F_SET(_connectionImpl, WT_CONN_IN_MEMORY);
    _connectionImpl->home = "";
    WT_RET(__wt_os_inmemory(session));
    return 0;
}
