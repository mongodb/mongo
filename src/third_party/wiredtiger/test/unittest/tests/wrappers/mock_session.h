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
#include <string>
#include <list>

#include "wt_internal.h"
#include "mock_connection.h"

int handleWiredTigerError(
  WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *message);
int handleWiredTigerMessage(WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *message);

/* Forward declare the class so we can include it in our wrapper. */
class MockSession;

/* This is a convenience type that lets us get back to our mock from the handler callback. */
struct event_handler_wrap {
    WT_EVENT_HANDLER handler;
    MockSession *mock_session;
};

class MockSession {
public:
    ~MockSession();
    WT_SESSION_IMPL *
    getWtSessionImpl()
    {
        return _sessionImpl;
    };
    std::shared_ptr<MockConnection>
    getMockConnection()
    {
        return _mockConnection;
    };

    void
    add_callback_message(const char *message)
    {
        _messages.push_back(std::string(message));
    }

    const std::string &
    get_last_message()
    {
        return _messages.back();
    }
    static std::shared_ptr<MockSession> buildTestMockSession();
    WT_BLOCK_MGR_SESSION *setupBlockManagerSession();

private:
    explicit MockSession(
      WT_SESSION_IMPL *sessionImpl, std::shared_ptr<MockConnection> mockConnection = nullptr);

    std::shared_ptr<MockConnection> _mockConnection;

    // This class is implemented such that it owns, and is responsible for freeing, this pointer
    WT_SESSION_IMPL *_sessionImpl;
    event_handler_wrap _handler_wrap;
    std::list<std::string> _messages;
};

#endif // WT_MOCK_SESSION_H
