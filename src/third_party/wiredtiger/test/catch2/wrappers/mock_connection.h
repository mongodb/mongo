/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *      All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <memory>

#include "wt_internal.h"

/*
 * Prefer a mock class over a "real" one when the operations you want to perform don't need a fully
 * fleshed-out connection (or session). There are large speed advantages here, since the real thing
 * will write a bunch of files to disk during the test, which also need to be removed.
 */
class mock_connection {
public:
    ~mock_connection();
    WT_CONNECTION_IMPL *
    get_wt_connection_impl()
    {
        return _connection_impl;
    };
    WT_CONNECTION *
    get_wt_connection()
    {
        return reinterpret_cast<WT_CONNECTION *>(_connection_impl);
    };

    static std::shared_ptr<mock_connection> build_test_mock_connection(WT_SESSION_IMPL *session);
    int setup_chunk_cache(WT_SESSION_IMPL *, uint64_t, size_t, WT_CHUNKCACHE *&);
    // Initialize the data structures, in-memory file system and variables used for file handles and
    // blocks. The block manager requires both to perform file type operations.
    int setup_block_manager(WT_SESSION_IMPL *);

private:
    explicit mock_connection(WT_CONNECTION_IMPL *connection_impl);
    int setup_stats(WT_SESSION_IMPL *session);

    // This class is implemented such that it owns, and is responsible for freeing,
    // this pointer
    WT_CONNECTION_IMPL *_connection_impl;
};
