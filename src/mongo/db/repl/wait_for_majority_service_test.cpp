/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

// IWYU pragma: no_include "cxxabi.h"
#include <mutex>

#include "mongo/base/error_codes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {
using waitForOpTimeMockFunction =
    std::function<Status(mongo::OperationContext*, const repl::OpTime&)>;

class WaitForMajorityServiceForReadImplMock : public WaitForMajorityServiceForReadImpl {
public:
    void setWaitForOpTimeMockReturnValueFunction(waitForOpTimeMockFunction returnValueFunction) {
        _waitForOpTimeMockfunc = std::move(returnValueFunction);
    }

    SemiFuture<void> waitUntilMajority(const repl::OpTime& opTime,
                                       const CancellationToken& cancelToken) {
        return WaitForMajorityServiceForReadImpl::waitUntilMajority(opTime, cancelToken);
    }

private:
    Status _waitForOpTime(mongo::OperationContext* opCtx, const repl::OpTime& opTime) override {
        return _waitForOpTimeMockfunc(opCtx, opTime);
    }

    waitForOpTimeMockFunction _waitForOpTimeMockfunc;
};

class WaitForMajorityServiceMock : public WaitForMajorityService {
public:
    void setWaitForOpTimeMockReturnValueFunction(waitForOpTimeMockFunction returnValueFunction) {
        _readService.setWaitForOpTimeMockReturnValueFunction(returnValueFunction);
    }

    void startup(ServiceContext* ctx) {
        _readService.startup(ctx);
    }

    void shutDown() {
        _readService.shutDown();
    }

    SemiFuture<void> waitUntilMajorityForRead(const repl::OpTime& opTime,
                                              const CancellationToken& cancelToken) {
        return _readService.waitUntilMajority(opTime, cancelToken);
    }

private:
    WaitForMajorityServiceForReadImplMock _readService;
};

class ReplicationCoordinatorAwaitAsyncMock : public repl::ReplicationCoordinatorMock {
public:
    ReplicationCoordinatorAwaitAsyncMock(ServiceContext* service)
        : ReplicationCoordinatorMock(service){};
    SharedSemiFuture<void> awaitReplicationAsyncNoWTimeout(
        const repl::OpTime& opTime, const WriteConcernOptions& writeConcern) override {
        auto [promise, future] = makePromiseFuture<void>();
        _promise = std::move(promise);
        return std::move(future).share();
    }

private:
    Promise<void> _promise;
};

class WaitForMajorityServiceTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto service = getServiceContext();
        waitService()->startup(service);

        auto replCoord = std::make_unique<ReplicationCoordinatorAwaitAsyncMock>(service);
        replCoord->setAwaitReplicationReturnValueFunction([this](const repl::OpTime& opTime) {
            return repl::ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
        });

        repl::ReplicationCoordinator::set(service, std::move(replCoord));

        waitService()->setWaitForOpTimeMockReturnValueFunction(
            [this](mongo::OperationContext* opCtx, const repl::OpTime& opTime) {
                return waitForOpTimeMock(opCtx, opTime);
            });
    }

    void tearDown() override {
        waitService()->shutDown();
        ServiceContextMongoDTest::tearDown();
    }

    WaitForMajorityServiceMock* waitService() {
        return &_waitForMajorityService;
    }

    void finishWaitingOneOpTime() {
        // There is a safe race condition in WaitForMajorityService where
        // _periodicallyWaitForMajority can grab the mutex after the request has been marked as
        // processed, but before it is removed from the queue. In this case, _isTestReady will
        // flip without actually progressing through an OpTime, so we do the additional OpTime
        // check.
        auto opTimeBefore = _lastOpTimeWaited;

        do {
            stdx::unique_lock<Latch> lk(_mutex);
            _isTestReady = true;
            _isTestReadyCV.notify_one();

            while (_isTestReady) {
                _finishWaitingOneOpTimeCV.wait(lk);
            }
        } while (_lastOpTimeWaited == opTimeBefore);
    }

    Status waitForOpTimeMock(OperationContext* opCtx, const repl::OpTime& opTime) {
        stdx::unique_lock<Latch> lk(_mutex);

        _waitForMajorityCallCount++;
        _callCountChangedCV.notify_one();

        try {
            opCtx->waitForConditionOrInterrupt(_isTestReadyCV, lk, [&] { return _isTestReady; });
        } catch (const DBException& e) {
            _isTestReady = false;
            _finishWaitingOneOpTimeCV.notify_one();

            return e.toStatus();
        }

        _lastOpTimeWaited = opTime;
        _isTestReady = false;
        _finishWaitingOneOpTimeCV.notify_one();

        return Status::OK();
    }

    const repl::OpTime& getLastOpTimeWaited() {
        stdx::lock_guard<Latch> lk(_mutex);
        return _lastOpTimeWaited;
    }

    void waitForMajorityCallCountGreaterThan(int expectedCount) {
        stdx::unique_lock lk(_mutex);
        _callCountChangedCV.wait(lk, [&] { return _waitForMajorityCallCount > expectedCount; });
    }

    static inline const Status kCanceledStatus = {ErrorCodes::CallbackCanceled,
                                                  "waitForMajority canceled"};

private:
    WaitForMajorityServiceMock _waitForMajorityService;

    Mutex _mutex = MONGO_MAKE_LATCH("WaitForMajorityServiceTest::_mutex");
    stdx::condition_variable _isTestReadyCV;
    stdx::condition_variable _finishWaitingOneOpTimeCV;
    stdx::condition_variable _callCountChangedCV;

    bool _isTestReady{false};
    repl::OpTime _lastOpTimeWaited;
    int _waitForMajorityCallCount{0};
};

class WaitForMajorityServiceNoStartupTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
    }

    void tearDown() override {
        waitService()->shutDown();
        ServiceContextMongoDTest::tearDown();
    }
    WaitForMajorityService* waitService() {
        return &_waitForMajorityService;
    }

private:
    WaitForMajorityService _waitForMajorityService;
};

TEST_F(WaitForMajorityServiceTest, ShutdownImmediatelyAfterStartupDoesNotCrashOrHang) {
    waitService()->shutDown();
}

TEST_F(WaitForMajorityServiceNoStartupTest, ShutdownBeforeStartupDoesNotCrashOrHang) {
    waitService()->shutDown();
}

TEST_F(WaitForMajorityServiceTest, WaitOneOpTime) {
    repl::OpTime t1(Timestamp(1, 0), 2);

    auto future = waitService()->waitUntilMajorityForRead(t1, CancellationToken::uncancelable());

    ASSERT_FALSE(future.isReady());

    finishWaitingOneOpTime();

    future.get();
    ASSERT_EQ(t1, getLastOpTimeWaited());
}

TEST_F(WaitForMajorityServiceTest, WaitWithSameOpTime) {
    repl::OpTime t1(Timestamp(1, 0), 2);

    auto future1 = waitService()->waitUntilMajorityForRead(t1, CancellationToken::uncancelable());
    auto future1b = waitService()->waitUntilMajorityForRead(t1, CancellationToken::uncancelable());

    ASSERT_FALSE(future1.isReady());
    ASSERT_FALSE(future1b.isReady());

    finishWaitingOneOpTime();

    future1.get();
    future1b.get();

    ASSERT_EQ(t1, getLastOpTimeWaited());
}

TEST_F(WaitForMajorityServiceTest, WaitWithOpTimeEarlierThanLowestQueued) {
    repl::OpTime laterOpTime(Timestamp(6, 0), 2);
    repl::OpTime earlierOpTime(Timestamp(1, 0), 2);

    auto laterFuture =
        waitService()->waitUntilMajorityForRead(laterOpTime, CancellationToken::uncancelable());

    // Wait until the background thread picks up the queued opTime.
    waitForMajorityCallCountGreaterThan(0);

    // The 2nd request has an earlier time, so it will interrupt 'laterOpTime' and skip the line.
    auto earlierFuture =
        waitService()->waitUntilMajorityForRead(earlierOpTime, CancellationToken::uncancelable());

    // Wait for background thread to finish transitioning from waiting on laterOpTime to
    // earlierOpTime.
    waitForMajorityCallCountGreaterThan(1);

    ASSERT_FALSE(laterFuture.isReady());
    ASSERT_FALSE(earlierFuture.isReady());

    finishWaitingOneOpTime();

    ASSERT_FALSE(laterFuture.isReady());

    earlierFuture.get();
    ASSERT_EQ(earlierOpTime, getLastOpTimeWaited());

    finishWaitingOneOpTime();
    laterFuture.get();

    ASSERT_EQ(laterOpTime, getLastOpTimeWaited());
}

TEST_F(WaitForMajorityServiceTest, WaitWithDifferentOpTime) {
    repl::OpTime t1(Timestamp(1, 0), 2);
    repl::OpTime t2(Timestamp(14, 0), 2);

    auto future1 = waitService()->waitUntilMajorityForRead(t1, CancellationToken::uncancelable());
    auto future2 = waitService()->waitUntilMajorityForRead(t2, CancellationToken::uncancelable());

    ASSERT_FALSE(future1.isReady());
    ASSERT_FALSE(future2.isReady());

    finishWaitingOneOpTime();

    ASSERT_FALSE(future2.isReady());

    future1.get();
    ASSERT_EQ(t1, getLastOpTimeWaited());

    finishWaitingOneOpTime();

    future2.get();
    ASSERT_EQ(t2, getLastOpTimeWaited());
}

TEST_F(WaitForMajorityServiceTest, WaitWithOpTimeEarlierThanOpTimeAlreadyWaited) {
    repl::OpTime t1(Timestamp(5, 0), 2);
    repl::OpTime t2(Timestamp(14, 0), 2);

    auto future1 = waitService()->waitUntilMajorityForRead(t1, CancellationToken::uncancelable());
    auto future2 = waitService()->waitUntilMajorityForRead(t2, CancellationToken::uncancelable());

    ASSERT_FALSE(future1.isReady());
    ASSERT_FALSE(future2.isReady());

    finishWaitingOneOpTime();

    ASSERT_FALSE(future2.isReady());

    future1.get();
    ASSERT_EQ(t1, getLastOpTimeWaited());

    repl::OpTime oldTs(Timestamp(4, 0), 2);
    auto oldFuture =
        waitService()->waitUntilMajorityForRead(oldTs, CancellationToken::uncancelable());
    auto alreadyWaitedFuture =
        waitService()->waitUntilMajorityForRead(t1, CancellationToken::uncancelable());

    ASSERT_FALSE(future2.isReady());

    oldFuture.get();
    alreadyWaitedFuture.get();
    ASSERT_EQ(t1, getLastOpTimeWaited());

    finishWaitingOneOpTime();

    future2.get();
    ASSERT_EQ(t2, getLastOpTimeWaited());
}

TEST_F(WaitForMajorityServiceTest, ShutdownShouldCancelQueuedRequests) {
    repl::OpTime t1(Timestamp(5, 0), 2);
    repl::OpTime t2(Timestamp(14, 0), 2);

    auto future1 = waitService()->waitUntilMajorityForRead(t1, CancellationToken::uncancelable());
    auto future2 = waitService()->waitUntilMajorityForRead(t2, CancellationToken::uncancelable());

    ASSERT_FALSE(future1.isReady());
    ASSERT_FALSE(future2.isReady());

    waitService()->shutDown();

    ASSERT_THROWS_CODE(future1.get(), AssertionException, ErrorCodes::InterruptedAtShutdown);
    ASSERT_THROWS_CODE(future2.get(), AssertionException, ErrorCodes::InterruptedAtShutdown);
}

TEST_F(WaitForMajorityServiceTest, CanCancelWaitOnOneOptime) {
    repl::OpTime t(Timestamp(1, 2), 4);
    CancellationSource source;
    auto future = waitService()->waitUntilMajorityForRead(t, source.token());
    ASSERT_FALSE(future.isReady());
    source.cancel();
    // The future should now become ready without having to wait for any opTime.
    ASSERT_EQ(future.getNoThrow(), kCanceledStatus);
}

TEST_F(WaitForMajorityServiceTest, CancelingEarlierOpTimeRequestDoesNotAffectLaterOpTimeRequests) {
    repl::OpTime earlier(Timestamp(1, 2), 4);
    repl::OpTime later(Timestamp(5, 2), 5);
    CancellationSource source;
    auto cancelFuture = waitService()->waitUntilMajorityForRead(earlier, source.token());
    auto uncancelableFuture =
        waitService()->waitUntilMajorityForRead(later, CancellationToken::uncancelable());
    ASSERT_FALSE(cancelFuture.isReady());
    ASSERT_FALSE(uncancelableFuture.isReady());
    // Wait until the background thread picks up the initial request. Otherwise, there is a race
    // between the cancellation callback removing the initial request and the background thread
    // waiting on it.
    waitForMajorityCallCountGreaterThan(0);
    source.cancel();
    // The future should now become ready without having to wait for any opTime.
    ASSERT_EQ(cancelFuture.getNoThrow(), kCanceledStatus);
    ASSERT_FALSE(uncancelableFuture.isReady());
    finishWaitingOneOpTime();
    ASSERT_FALSE(uncancelableFuture.isReady());
    finishWaitingOneOpTime();
    uncancelableFuture.wait();
    ASSERT_EQ(later, getLastOpTimeWaited());
}

TEST_F(WaitForMajorityServiceTest, CancelingOneRequestOnOpTimeDoesNotAffectOthersOnSameOpTime) {
    repl::OpTime t1(Timestamp(1, 2), 4);
    repl::OpTime t1Dupe(Timestamp(1, 2), 4);
    CancellationSource source;
    auto cancelFuture = waitService()->waitUntilMajorityForRead(t1, source.token());
    auto uncancelableFuture =
        waitService()->waitUntilMajorityForRead(t1Dupe, CancellationToken::uncancelable());
    ASSERT_FALSE(cancelFuture.isReady());
    ASSERT_FALSE(uncancelableFuture.isReady());
    source.cancel();
    // The future should now become ready without having to wait for any opTime.
    ASSERT_EQ(cancelFuture.getNoThrow(), kCanceledStatus);
    ASSERT_FALSE(uncancelableFuture.isReady());
    finishWaitingOneOpTime();
    uncancelableFuture.wait();
    ASSERT_EQ(t1, getLastOpTimeWaited());
}

TEST_F(WaitForMajorityServiceTest, CancelingLaterOpTimeRequestDoesNotAffectEarlierOpTimeRequests) {
    repl::OpTime t1(Timestamp(1, 2), 4);
    repl::OpTime smallerOpTime(Timestamp(1, 2), 1);
    CancellationSource source;
    auto cancelFuture = waitService()->waitUntilMajorityForRead(t1, source.token());
    // Wait until the background thread picks up the queued opTime.
    waitForMajorityCallCountGreaterThan(0);
    auto earlierFuture =
        waitService()->waitUntilMajorityForRead(smallerOpTime, CancellationToken::uncancelable());
    // Wait for background thread to finish transitioning from waiting on t1 to smallerOpTime.
    waitForMajorityCallCountGreaterThan(1);
    ASSERT_FALSE(cancelFuture.isReady());
    ASSERT_FALSE(earlierFuture.isReady());
    source.cancel();
    // The future should now become ready without having to wait for any opTime.
    ASSERT_EQ(cancelFuture.getNoThrow(), kCanceledStatus);
    ASSERT_FALSE(earlierFuture.isReady());
    finishWaitingOneOpTime();
    earlierFuture.wait();
    ASSERT_EQ(smallerOpTime, getLastOpTimeWaited());
}

TEST_F(WaitForMajorityServiceTest, SafeToCallCancelOnRequestAlreadyCompletedByShutdown) {
    repl::OpTime t(Timestamp(1, 2), 4);
    CancellationSource source;
    auto deadFuture = waitService()->waitUntilMajorityForRead(t, source.token());
    ASSERT_FALSE(deadFuture.isReady());
    waitService()->shutDown();
    ASSERT(deadFuture.isReady());
    ASSERT_THROWS_CODE(deadFuture.get(), AssertionException, ErrorCodes::InterruptedAtShutdown);
    source.cancel();
}

TEST_F(WaitForMajorityServiceTest, SafeToCallCancelOnRequestAlreadyCompletedByWaiting) {
    repl::OpTime t(Timestamp(1, 2), 4);
    CancellationSource source;
    auto future = waitService()->waitUntilMajorityForRead(t, source.token());
    ASSERT_FALSE(future.isReady());
    waitForMajorityCallCountGreaterThan(0);
    finishWaitingOneOpTime();
    future.get();
    ASSERT_EQ(t, getLastOpTimeWaited());
    source.cancel();
}

TEST_F(WaitForMajorityServiceTest, PassingAlreadyCanceledTokenCompletesFutureWithNoWaiting) {
    repl::OpTime t(Timestamp(1, 2), 4);
    CancellationSource source;
    source.cancel();
    auto future = waitService()->waitUntilMajorityForRead(t, source.token());
    ASSERT_EQ(future.getNoThrow(), kCanceledStatus);
}

TEST_F(WaitForMajorityServiceTest, CanCancelWaitOnOneOptimeForWrite) {
    repl::OpTime t(Timestamp(1, 2), 4);
    CancellationSource source;
    auto future = waitService()->waitUntilMajorityForWrite(getServiceContext(), t, source.token());
    ASSERT_FALSE(future.isReady());
    source.cancel();
    // The future should now become ready without having to wait for any opTime.
    ASSERT_EQ(future.getNoThrow(), kCanceledStatus);
}

}  // namespace
}  // namespace mongo
