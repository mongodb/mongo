/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *      All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "mock_connection.h"
#include "../utils.h"

MockConnection::MockConnection(WT_CONNECTION_IMPL *connectionImpl) : _connectionImpl(connectionImpl)
{
}

MockConnection::~MockConnection()
{
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
