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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/wait_for_majority_service.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class WaitForMajorityServiceTest : public ServiceContextTest {
public:
    void setUp() override {
        auto service = getServiceContext();
        waitService()->setUp(service);

        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);

        replCoord->setAwaitReplicationReturnValueFunction(
            [this](OperationContext* opCtx, const repl::OpTime& opTime) {
                auto status = waitForWriteConcernStub(opCtx, opTime);
                return repl::ReplicationCoordinator::StatusAndDuration(status, Milliseconds(0));
            });

        repl::ReplicationCoordinator::set(service, std::move(replCoord));
    }

    void tearDown() override {
        waitService()->shutDown();
    }

    WaitForMajorityService* waitService() {
        return &_waitForMajorityService;
    }

    void finishWaitingOneOpTime() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _isTestReady = true;
        _isTestReadyCV.notify_one();

        while (_isTestReady) {
            _finishWaitingOneOpTimeCV.wait(lk);
        }
    }

    Status waitForWriteConcernStub(OperationContext* opCtx, const repl::OpTime& opTime) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        while (!_isTestReady) {
            auto status = opCtx->waitForConditionOrInterruptNoAssert(_isTestReadyCV, lk);
            if (!status.isOK()) {
                _isTestReady = false;
                _finishWaitingOneOpTimeCV.notify_one();

                return status;
            }
        }

        _lastOpTimeWaited = opTime;
        _isTestReady = false;
        _finishWaitingOneOpTimeCV.notify_one();

        return Status::OK();
    }

    const repl::OpTime& getLastOpTimeWaited() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _lastOpTimeWaited;
    }

private:
    WaitForMajorityService _waitForMajorityService;

    stdx::mutex _mutex;
    stdx::condition_variable _isTestReadyCV;
    stdx::condition_variable _finishWaitingOneOpTimeCV;

    bool _isTestReady{false};
    repl::OpTime _lastOpTimeWaited;
};

TEST_F(WaitForMajorityServiceTest, WaitOneOpTime) {
    repl::OpTime t1(Timestamp(1, 0), 2);

    auto future = waitService()->waitUntilMajority(t1);

    ASSERT_FALSE(future.isReady());

    finishWaitingOneOpTime();

    future.get();
    ASSERT_EQ(t1, getLastOpTimeWaited());
}

TEST_F(WaitForMajorityServiceTest, WaitWithSameOpTime) {
    repl::OpTime t1(Timestamp(1, 0), 2);

    auto future1 = waitService()->waitUntilMajority(t1);
    auto future1b = waitService()->waitUntilMajority(t1);

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

    auto laterFuture = waitService()->waitUntilMajority(laterOpTime);
    auto earlierFuture = waitService()->waitUntilMajority(earlierOpTime);

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

    auto future1 = waitService()->waitUntilMajority(t1);
    auto future2 = waitService()->waitUntilMajority(t2);

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

    auto future1 = waitService()->waitUntilMajority(t1);
    auto future2 = waitService()->waitUntilMajority(t2);

    ASSERT_FALSE(future1.isReady());
    ASSERT_FALSE(future2.isReady());

    finishWaitingOneOpTime();

    ASSERT_FALSE(future2.isReady());

    future1.get();
    ASSERT_EQ(t1, getLastOpTimeWaited());

    repl::OpTime oldTs(Timestamp(4, 0), 2);
    auto oldFuture = waitService()->waitUntilMajority(oldTs);
    auto alreadyWaitedFuture = waitService()->waitUntilMajority(t1);

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

    auto future1 = waitService()->waitUntilMajority(t1);
    auto future2 = waitService()->waitUntilMajority(t2);

    ASSERT_FALSE(future1.isReady());
    ASSERT_FALSE(future2.isReady());

    waitService()->shutDown();

    ASSERT_THROWS_CODE(future1.get(), AssertionException, ErrorCodes::InterruptedAtShutdown);
    ASSERT_THROWS_CODE(future2.get(), AssertionException, ErrorCodes::InterruptedAtShutdown);
}

TEST_F(WaitForMajorityServiceTest, WriteConcernErrorGetsPropagatedCorrectly) {
    repl::OpTime t(Timestamp(5, 0), 2);

    auto replCoord = dynamic_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(getServiceContext()));
    replCoord->setAwaitReplicationReturnValueFunction(
        [this](OperationContext* opCtx, const repl::OpTime& opTime) {
            return repl::ReplicationCoordinator::StatusAndDuration(
                {ErrorCodes::PrimarySteppedDown, "test stepdown"}, Milliseconds(0));
        });

    auto future = waitService()->waitUntilMajority(t);
    ASSERT_THROWS_CODE(future.get(), AssertionException, ErrorCodes::PrimarySteppedDown);
}

}  // namespace
}  // namespace mongo
