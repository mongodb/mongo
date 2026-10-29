// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/mock_async_client_factory.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"

namespace mongo::executor {
namespace {

const HostAndPort kTarget("localhost", 12345);

class MockAsyncClientFactoryTest : public unittest::Test {
protected:
    MockAsyncClientFactory& factory() {
        return _factory;
    }

private:
    MockAsyncClientFactory _factory;
};

using MockAsyncClientFactoryDeathTest = MockAsyncClientFactoryTest;

TEST_F(MockAsyncClientFactoryTest, GetAndLeaseAreRecordedInCallOrder) {
    CancellationSource source;
    auto getFut = factory().get(kTarget, transport::kEnableSSL, Milliseconds(100), source.token());
    auto leaseFut =
        factory().lease(HostAndPort("otherhost", 1), transport::kDisableSSL, Seconds(2));

    ASSERT_EQ(factory().requestCount(), 2);

    auto get = factory().request(0);
    ASSERT_EQ(get.target, kTarget);
    ASSERT_EQ(get.sslMode, transport::kEnableSSL);
    ASSERT_EQ(get.timeout, Milliseconds(100));
    ASSERT_TRUE(get.token.isCancelable());
    ASSERT_FALSE(get.isLease);
    ASSERT_FALSE(get.settled());
    ASSERT_FALSE(getFut.isReady());

    auto lease = factory().request(1);
    ASSERT_EQ(lease.target, HostAndPort("otherhost", 1));
    ASSERT_EQ(lease.sslMode, transport::kDisableSSL);
    ASSERT_EQ(lease.timeout, Seconds(2));
    ASSERT_FALSE(lease.token.isCancelable());
    ASSERT_TRUE(lease.isLease);
    ASSERT_FALSE(lease.settled());
    ASSERT_FALSE(leaseFut.isReady());
}

TEST_F(MockAsyncClientFactoryTest, FailSettlesWithGivenStatus) {
    auto fut = factory().get(kTarget, transport::kGlobalSSLMode, Milliseconds(100));

    factory().request(0).fail(Status(ErrorCodes::HostUnreachable, "test"));

    ASSERT_TRUE(factory().request(0).settled());
    ASSERT_EQ(fut.getNoThrow().getStatus(), ErrorCodes::HostUnreachable);
}

TEST_F(MockAsyncClientFactoryTest, FirstFailWins) {
    auto fut = factory().get(kTarget, transport::kGlobalSSLMode, Milliseconds(100));

    factory().request(0).fail(Status(ErrorCodes::HostUnreachable, "first"));
    factory().request(0).fail(Status(ErrorCodes::NetworkTimeout, "second"));

    ASSERT_EQ(fut.getNoThrow().getStatus(), ErrorCodes::HostUnreachable);
}

TEST_F(MockAsyncClientFactoryTest, CopiesShareSettlementState) {
    auto fut = factory().get(kTarget, transport::kGlobalSSLMode, Milliseconds(100));
    auto copy = factory().request(0);

    copy.fail(Status(ErrorCodes::HostUnreachable, "test"));

    ASSERT_TRUE(factory().request(0).settled());
    ASSERT_TRUE(fut.isReady());
}

TEST_F(MockAsyncClientFactoryTest, RequestsSettleIndependently) {
    auto fut0 = factory().get(kTarget, transport::kGlobalSSLMode, Milliseconds(100));
    auto fut1 = factory().get(kTarget, transport::kGlobalSSLMode, Milliseconds(100));

    factory().request(1).fail(Status(ErrorCodes::HostUnreachable, "test"));

    ASSERT_FALSE(factory().request(0).settled());
    ASSERT_FALSE(fut0.isReady());
    ASSERT_TRUE(factory().request(1).settled());
    ASSERT_EQ(fut1.getNoThrow().getStatus(), ErrorCodes::HostUnreachable);
}

TEST_F(MockAsyncClientFactoryTest, CancellationFailsPendingRequest) {
    CancellationSource source;
    auto fut = factory().get(kTarget, transport::kGlobalSSLMode, Milliseconds(100), source.token());
    ASSERT_FALSE(fut.isReady());

    source.cancel();

    ASSERT_TRUE(factory().request(0).settled());
    ASSERT_EQ(fut.getNoThrow().getStatus(), ErrorCodes::CallbackCanceled);
}

TEST_F(MockAsyncClientFactoryTest, CancellationFailsPendingLease) {
    CancellationSource source;
    auto fut =
        factory().lease(kTarget, transport::kGlobalSSLMode, Milliseconds(100), source.token());

    source.cancel();

    ASSERT_EQ(fut.getNoThrow().getStatus(), ErrorCodes::CallbackCanceled);
}

TEST_F(MockAsyncClientFactoryTest, AlreadyCancelledTokenFailsImmediately) {
    CancellationSource source;
    source.cancel();

    auto fut = factory().get(kTarget, transport::kGlobalSSLMode, Milliseconds(100), source.token());

    ASSERT_EQ(factory().requestCount(), 1);
    ASSERT_TRUE(factory().request(0).settled());
    ASSERT_EQ(fut.getNoThrow().getStatus(), ErrorCodes::CallbackCanceled);
}

TEST_F(MockAsyncClientFactoryTest, CancellationAfterFailDoesNotOverride) {
    CancellationSource source;
    auto fut = factory().get(kTarget, transport::kGlobalSSLMode, Milliseconds(100), source.token());

    factory().request(0).fail(Status(ErrorCodes::HostUnreachable, "test"));
    source.cancel();

    ASSERT_EQ(fut.getNoThrow().getStatus(), ErrorCodes::HostUnreachable);
}

TEST_F(MockAsyncClientFactoryTest, FailAfterCancellationDoesNotOverride) {
    CancellationSource source;
    auto fut = factory().get(kTarget, transport::kGlobalSSLMode, Milliseconds(100), source.token());

    source.cancel();
    factory().request(0).fail(Status(ErrorCodes::HostUnreachable, "test"));

    ASSERT_EQ(fut.getNoThrow().getStatus(), ErrorCodes::CallbackCanceled);
}

TEST_F(MockAsyncClientFactoryTest, ShutdownFailsOnlyPendingRequests) {
    auto failedFut = factory().get(kTarget, transport::kGlobalSSLMode, Milliseconds(100));
    auto pendingGetFut = factory().get(kTarget, transport::kGlobalSSLMode, Milliseconds(100));
    auto pendingLeaseFut = factory().lease(kTarget, transport::kGlobalSSLMode, Milliseconds(100));
    factory().request(0).fail(Status(ErrorCodes::HostUnreachable, "test"));

    factory().shutdown();

    ASSERT_EQ(failedFut.getNoThrow().getStatus(), ErrorCodes::HostUnreachable);
    ASSERT_EQ(pendingGetFut.getNoThrow().getStatus(), ErrorCodes::ShutdownInProgress);
    ASSERT_EQ(pendingLeaseFut.getNoThrow().getStatus(), ErrorCodes::ShutdownInProgress);
}

TEST_F(MockAsyncClientFactoryTest, CancellationAfterShutdownDoesNotOverride) {
    CancellationSource source;
    auto fut = factory().get(kTarget, transport::kGlobalSSLMode, Milliseconds(100), source.token());

    factory().shutdown();
    source.cancel();

    ASSERT_EQ(fut.getNoThrow().getStatus(), ErrorCodes::ShutdownInProgress);
}

DEATH_TEST_F(MockAsyncClientFactoryDeathTest, FailWithOKStatus, "invariant") {
    auto fut = factory().get(kTarget, transport::kGlobalSSLMode, Milliseconds(100));
    factory().request(0).fail(Status::OK());
}

DEATH_TEST_F(MockAsyncClientFactoryDeathTest, RequestOutOfRange, "invariant") {
    factory().request(0);
}

}  // namespace
}  // namespace mongo::executor
