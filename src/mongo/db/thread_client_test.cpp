/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/concurrency/thread_name.h"

namespace mongo {
namespace {

class ThreadClientTest : public unittest::Test, public ScopedGlobalServiceContextForTest {};

TEST_F(ThreadClientTest, TestNoAssignment) {
    ASSERT_FALSE(haveClient());
    { ThreadClient tc(getThreadName(), getGlobalServiceContext()); }
    ASSERT_FALSE(haveClient());
}

TEST_F(ThreadClientTest, TestAssignment) {
    ASSERT_FALSE(haveClient());
    ThreadClient threadClient(getThreadName(), getGlobalServiceContext());
    ASSERT_TRUE(haveClient());
}

TEST_F(ThreadClientTest, TestDifferentArgs) {
    {
        ASSERT_FALSE(haveClient());
        ThreadClient tc(getGlobalServiceContext());
        ASSERT_TRUE(haveClient());
    }
    {
        ASSERT_FALSE(haveClient());
        ThreadClient tc("Test", getGlobalServiceContext());
        ASSERT_TRUE(haveClient());
    }
    {
        ASSERT_FALSE(haveClient());
        transport::TransportLayerMock mock;
        std::shared_ptr<transport::Session> handle = mock.createSession();
        ThreadClient tc(getThreadName(), getGlobalServiceContext(), handle);
        ASSERT_TRUE(haveClient());
    }
    {
        ASSERT_FALSE(haveClient());
        auto sc = ServiceContext::make();
        ThreadClient tc("Test", sc.get(), nullptr);
        ASSERT_TRUE(haveClient());
    }
}

TEST_F(ThreadClientTest, TestAlternativeClientRegion) {
    ASSERT_FALSE(haveClient());
    ThreadClient threadClient(getThreadName(), getGlobalServiceContext());

    ServiceContext::UniqueClient swapClient = getGlobalServiceContext()->makeClient("swapClient");
    { AlternativeClientRegion altRegion(swapClient); }

    ASSERT_TRUE(haveClient());
}

/**
 * This test asserts that original thread names are restored after a ThreadClient object
 * goes out of scope.
 */
TEST_F(ThreadClientTest, TestThreadName) {
    ASSERT_FALSE(haveClient());
    const auto originalThreadName = getThreadName();

    {
        ThreadClient threadClient("MyThreadClient", getGlobalServiceContext());
        ASSERT_TRUE(haveClient());
        // The instatiation of ThreadClient should have changed this thread name
        ASSERT_NE(originalThreadName, getThreadName());
    }

    ASSERT_FALSE(haveClient());
    // The original name for this thread should have been restored.
    ASSERT_EQ(originalThreadName, getThreadName());
}
}  // namespace
}  // namespace mongo
