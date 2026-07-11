// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_name.h"

#include <memory>

namespace mongo {
namespace {

class ThreadClientTest : public unittest::Test, public ScopedGlobalServiceContextForTest {};

TEST_F(ThreadClientTest, TestNoAssignment) {
    ASSERT_FALSE(haveClient());
    {
        ThreadClient tc(getThreadName(), getGlobalServiceContext()->getService());
    }
    ASSERT_FALSE(haveClient());
}

TEST_F(ThreadClientTest, TestAssignment) {
    ASSERT_FALSE(haveClient());
    ThreadClient threadClient(getThreadName(), getGlobalServiceContext()->getService());
    ASSERT_TRUE(haveClient());
}

TEST_F(ThreadClientTest, TestDifferentArgs) {
    {
        ASSERT_FALSE(haveClient());
        ThreadClient tc(getGlobalServiceContext()->getService());
        ASSERT_TRUE(haveClient());
    }
    {
        ASSERT_FALSE(haveClient());
        ThreadClient tc("Test", getGlobalServiceContext()->getService());
        ASSERT_TRUE(haveClient());
    }
    {
        ASSERT_FALSE(haveClient());
        transport::TransportLayerMock mock;
        std::shared_ptr<transport::Session> handle = mock.createSession();
        ThreadClient tc(getThreadName(), getGlobalServiceContext()->getService(), handle);
        ASSERT_TRUE(haveClient());
    }
    {
        ASSERT_FALSE(haveClient());
        auto sc = ServiceContext::make();
        ThreadClient tc("Test", sc.get()->getService(), nullptr);
        ASSERT_TRUE(haveClient());
    }
}

TEST_F(ThreadClientTest, TestAlternativeClientRegion) {
    ASSERT_FALSE(haveClient());
    ThreadClient threadClient(getThreadName(), getGlobalServiceContext()->getService());

    ServiceContext::UniqueClient swapClient =
        getGlobalServiceContext()->getService()->makeClient("swapClient");
    {
        AlternativeClientRegion altRegion(swapClient);
    }

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
        ThreadClient threadClient("MyThreadClient", getGlobalServiceContext()->getService());
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
