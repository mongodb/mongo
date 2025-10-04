/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wiredtiger.h"
#include "../wrappers/mock_session.h"

/*
 * [mock_session]:
 * Test basic functionalities of the mock session class.
 */

TEST_CASE("Directly test the error handler on the mock session", "[mock_session]")
{
    // Build Mock session, this will automatically create a mock connection.
    std::shared_ptr<mock_session> session_mock = mock_session::build_test_mock_session();

    // Push a new message onto the queue.
    session_mock->add_callback_message("Message 1!");

    // Check that we get it back.
    REQUIRE(session_mock->get_last_message() == "Message 1!");

    // Get back to our newly created handler, this wouldn't be the typical usage path.
    WT_EVENT_HANDLER *handler = session_mock->get_wt_session_impl()->event_handler;

    // Call the two valid function pointers on the handler, validate the rest are NULL.
    REQUIRE(handler->handle_error != nullptr);
    REQUIRE(handler->handle_error(handler, nullptr, 0, "Message 2!") == 0);
    REQUIRE(session_mock->get_last_message() == "Message 2!");

    REQUIRE(handler->handle_message != nullptr);
    REQUIRE(handler->handle_message(handler, nullptr, "Message 3!") == 0);
    REQUIRE(session_mock->get_last_message() == "Message 3!");

    REQUIRE((handler->handle_close == nullptr && handler->handle_general == nullptr &&
      handler->handle_progress == nullptr));
}
