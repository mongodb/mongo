/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
