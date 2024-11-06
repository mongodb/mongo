/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *      All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <memory>
#include <string>
#include <list>

#include "mock_connection.h"
#include "wt_internal.h"

int handle_wiredtiger_error(
  WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *message);
int handle_wiredtiger_message(WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *message);

/* Forward declare the class so we can include it in our wrapper. */
class mock_session;

/* This is a convenience type that lets us get back to our mock from the handler callback. */
struct event_handler_wrap {
    WT_EVENT_HANDLER handler;
    mock_session *ms;
};

class mock_session {
public:
    ~mock_session();

    WT_SESSION_IMPL *
    get_wt_session_impl()
    {
        return _session_impl;
    };

    std::shared_ptr<mock_connection>
    get_mock_connection()
    {
        return _mock_connection;
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

    static std::shared_ptr<mock_session> build_test_mock_session();

    WT_BLOCK_MGR_SESSION *setup_block_manager_session();

    // Allocate the necessary structures to perform write/read operations in block manager.
    void setup_block_manager_file_operations();

private:
    explicit mock_session(
      WT_SESSION_IMPL *session_impl, std::shared_ptr<mock_connection> mock_connection = nullptr);

    std::shared_ptr<mock_connection> _mock_connection;

    // This class is implemented such that it owns, and is responsible for freeing, this pointer
    WT_SESSION_IMPL *_session_impl;
    event_handler_wrap _handler_wrap;
    std::list<std::string> _messages;
};
