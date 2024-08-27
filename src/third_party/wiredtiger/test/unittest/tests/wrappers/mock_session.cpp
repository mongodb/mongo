/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *      All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <utility>

#include "mock_session.h"
#include "mock_connection.h"
#include "../utils.h"

MockSession::MockSession(WT_SESSION_IMPL *session, std::shared_ptr<MockConnection> mockConnection)
    : _sessionImpl(session), _mockConnection(std::move(mockConnection))
{
    // We could initialize this in the list but order is important and this seems easier.
    _handler_wrap.handler = {
      handleWiredTigerError, handleWiredTigerMessage, nullptr, nullptr, nullptr};
    _handler_wrap.mock_session = this;

    _sessionImpl->event_handler = &_handler_wrap.handler;
}

MockSession::~MockSession()
{
    if (_sessionImpl->block_manager != nullptr)
        __wt_free(nullptr, _sessionImpl->block_manager);
    __wt_free(nullptr, _sessionImpl);
}

std::shared_ptr<MockSession>
MockSession::buildTestMockSession()
{
    auto mockConnection = MockConnection::buildTestMockConnection();

    WT_SESSION_IMPL *sessionImpl = nullptr;
    utils::throwIfNonZero(__wt_calloc(nullptr, 1, sizeof(WT_SESSION_IMPL), &sessionImpl));
    sessionImpl->iface.connection = mockConnection->getWtConnection();

    // Construct an object that will now own the two pointers passed in.
    return std::shared_ptr<MockSession>(new MockSession(sessionImpl, mockConnection));
}

WT_BLOCK_MGR_SESSION *
MockSession::setupBlockManagerSession()
{
    // Initialize rnd state because block manager requires it.
    __wt_random_init(&_sessionImpl->rnd);
    utils::throwIfNonZero(
      __wt_calloc(nullptr, 1, sizeof(WT_BLOCK_MGR_SESSION), &_sessionImpl->block_manager));

    return static_cast<WT_BLOCK_MGR_SESSION *>(_sessionImpl->block_manager);
}

int
handleWiredTigerError(WT_EVENT_HANDLER *handler, WT_SESSION *session, int, const char *message)
{
    handleWiredTigerMessage(handler, session, message);
    return (0);
}

int
handleWiredTigerMessage(WT_EVENT_HANDLER *handler, WT_SESSION *, const char *message)
{
    reinterpret_cast<event_handler_wrap *>(handler)->mock_session->add_callback_message(message);
    return (0);
}
