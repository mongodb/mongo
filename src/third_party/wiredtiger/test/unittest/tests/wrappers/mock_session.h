/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *      All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifndef WT_MOCK_SESSION_H
#define WT_MOCK_SESSION_H

#include <memory>
#include "wt_internal.h"
#include "mock_connection.h"

class MockSession {
    public:
    ~MockSession();
    WT_SESSION_IMPL *
    getWtSessionImpl()
    {
        return _sessionImpl;
    };

    static std::shared_ptr<MockSession> buildTestMockSession();

    private:
    explicit MockSession(
      WT_SESSION_IMPL *sessionImpl, std::shared_ptr<MockConnection> mockConnection = nullptr);

    std::shared_ptr<MockConnection> _mockConnection;

    // This class is implemented such that it owns, and is responsible for freeing, this pointer
    WT_SESSION_IMPL *_sessionImpl;
};

#endif // WT_MOCK_SESSION_H
