// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "src/mongo/db/query/util/stop_token.h"

#include "mongo/unittest/unittest.h"


TEST(StopToken, CallbackInvokedImmediately) {
    mongo::stop_source ss;
    auto token = ss.get_token();
    // Token stop requested before callback constructed.
    ss.request_stop();

    bool called = false;

    mongo::stop_callback cb(token, [&]() { called = true; });

    ASSERT_TRUE(called);
}

TEST(StopToken, CallbackInvokedOnStop) {
    mongo::stop_source ss;
    auto token = ss.get_token();

    bool called = false;

    mongo::stop_callback cb(token, [&]() { called = true; });

    ASSERT_FALSE(called);
    ss.request_stop();
    ASSERT_TRUE(called);
}

TEST(StopToken, CallbackInvokedOnce) {
    int called = 0;
    {
        mongo::stop_source ss;
        auto token = ss.get_token();

        mongo::stop_callback cb(token, [&]() { ++called; });

        ASSERT_EQ(called, 0);
        ss.request_stop();
        ASSERT_EQ(called, 1);
    }
    ASSERT_EQ(called, 1);
}

TEST(StopToken, CallbackNotInvoked) {
    int called = 0;
    {
        mongo::stop_source ss;
        auto token = ss.get_token();

        mongo::stop_callback cb(token, [&]() { ++called; });

        ASSERT_EQ(called, 0);
        // ss not stopped!
    }
    ASSERT_EQ(called, 0);
}

TEST(StopToken, CallbackNotInvokedWhenNotPossible) {
    int called = 0;
    {
        mongo::stop_token token;
        ASSERT_FALSE(token.stop_possible());

        mongo::stop_callback cb(token, [&]() { ++called; });

        ASSERT_EQ(called, 0);
    }
    ASSERT_EQ(called, 0);
}
