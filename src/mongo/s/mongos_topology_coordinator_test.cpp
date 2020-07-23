/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/logv2/log.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/platform/basic.h"
#include "mongo/s/mongos_topology_coordinator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

using std::unique_ptr;

namespace mongo {
namespace {

class MongosTopoCoordTest : public ServiceContextTest {
public:
    virtual void setUp() {
        _topo = std::make_unique<MongosTopologyCoordinator>();

        // The fast clock is used by OperationContext::hasDeadlineExpired.
        getServiceContext()->setFastClockSource(
            std::make_unique<SharedClockSourceAdapter>(_clkSource));
        // The precise clock is used by waitForConditionOrInterruptNoAssertUntil.
        getServiceContext()->setPreciseClockSource(
            std::make_unique<SharedClockSourceAdapter>(_clkSource));
    }

    virtual void tearDown() {}

protected:
    /**
     * Gets the MongosTopologCoordinator.
     */
    MongosTopologyCoordinator& getTopoCoord() {
        return *_topo;
    }

    /**
     * Advance the time by millis on both clock source mocks.
     */
    void advanceTime(Milliseconds millis) {
        _clkSource->advance(millis);
    }

    /**
     * Assumes that the times on both clock source mocks is the same.
     */
    Date_t now() {
        return _clkSource->now();
    }

private:
    std::unique_ptr<MongosTopologyCoordinator> _topo;
    std::shared_ptr<ClockSourceMock> _clkSource = std::make_shared<ClockSourceMock>();
};

TEST_F(MongosTopoCoordTest, MongosTopologyVersionCounterInitializedAtStartup) {
    ASSERT_EQ(0, getTopoCoord().getTopologyVersion().getCounter());
}

TEST_F(MongosTopoCoordTest, AwaitIsMasterReturnsCorrectFieldTypes) {
    std::string mongosString = "isdbgrid";
    auto opCtx = makeOperationContext();
    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();

    // Simple isMaster request with no topologyVersion or deadline. We just want to test a code path
    // that calls _makeIsMasterResponse.
    auto response = getTopoCoord().awaitIsMasterResponse(opCtx.get(), boost::none, boost::none);

    // Validate isMaster response field types.
    ASSERT_EQUALS(response->getTopologyVersion().getProcessId(),
                  currentTopologyVersion.getProcessId());
    ASSERT_EQUALS(response->getTopologyVersion().getCounter(), currentTopologyVersion.getCounter());
    // Mongos isMaster responses will always contain ismaster: true and msg: "isdbgrid"
    ASSERT_TRUE(response->getIsMaster());
    ASSERT_EQUALS(response->getMsg(), mongosString);
}

TEST_F(MongosTopoCoordTest, AwaitIsMasterResponseReturnsCurrentMongosTopologyVersionOnTimeOut) {
    auto opCtx = makeOperationContext();
    auto maxAwaitTime = Milliseconds(5000);
    auto halfwayToMaxAwaitTime = maxAwaitTime / 2;
    auto deadline = now() + maxAwaitTime;

    // isMaster request with the current TopologyVersion should attempt to wait for maxAwaitTimeMS.
    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();

    bool isMasterReturned = false;
    stdx::thread getIsMasterThread([&] {
        Client::setCurrent(getServiceContext()->makeClient("getIsMasterThread"));
        auto threadOpCtx = cc().makeOperationContext();
        const auto response = getTopoCoord().awaitIsMasterResponse(
            threadOpCtx.get(), currentTopologyVersion, deadline);
        isMasterReturned = true;
        auto topologyVersion = response->getTopologyVersion();
        // Assert that on timeout, the returned IsMasterResponse contains the same TopologyVersion.
        ASSERT_EQUALS(topologyVersion.getCounter(), currentTopologyVersion.getCounter());
        ASSERT_EQUALS(topologyVersion.getProcessId(), currentTopologyVersion.getProcessId());
    });

    // Advance the clocks halfway and make sure awaitIsMasterResponse did not return yet.
    advanceTime(halfwayToMaxAwaitTime);
    ASSERT_FALSE(isMasterReturned);

    // Advance the clocks the rest of the way so that awaitIsMasterResponse times out.
    advanceTime(halfwayToMaxAwaitTime);
    getIsMasterThread.join();
    ASSERT_TRUE(isMasterReturned);
}

TEST_F(MongosTopoCoordTest, AwaitIsMasterErrorsWithHigherCounterAndSameProcessID) {
    auto opCtx = makeOperationContext();
    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = now() + maxAwaitTime;

    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();

    // Higher counter with same process ID should return an error.
    StringBuilder sb;
    sb << "Received a topology version with counter: 1 which is greater than the mongos topology "
          "version counter: 0";
    auto higherTopologyVersionWithSameProcessId = TopologyVersion(
        currentTopologyVersion.getProcessId(), currentTopologyVersion.getCounter() + 1);
    ASSERT_THROWS_WHAT(getTopoCoord().awaitIsMasterResponse(
                           opCtx.get(), higherTopologyVersionWithSameProcessId, deadline),
                       AssertionException,
                       sb.str());
}

TEST_F(MongosTopoCoordTest, AwaitIsMasterReturnsImmediatelyWithHigherCounterAndDifferentProcessID) {
    auto opCtx = makeOperationContext();
    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = now() + maxAwaitTime;

    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();

    // Higher counter with different process ID should return immediately.
    auto differentPid = OID::gen();
    ASSERT_NOT_EQUALS(differentPid, currentTopologyVersion.getProcessId());
    auto higherTopologyVersionWithDifferentProcessId =
        TopologyVersion(differentPid, currentTopologyVersion.getCounter() + 1);

    auto response = getTopoCoord().awaitIsMasterResponse(
        opCtx.get(), higherTopologyVersionWithDifferentProcessId, deadline);
    ASSERT_EQUALS(response->getTopologyVersion().getProcessId(),
                  currentTopologyVersion.getProcessId());
    ASSERT_EQUALS(response->getTopologyVersion().getCounter(), currentTopologyVersion.getCounter());
}

TEST_F(MongosTopoCoordTest,
       AwaitIsMasterReturnsImmediatelyWithCurrentCounterAndDifferentProcessID) {
    auto opCtx = makeOperationContext();
    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = now() + maxAwaitTime;

    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();

    // Current counter with different process ID should return immediately.
    auto differentPid = OID::gen();
    ASSERT_NOT_EQUALS(differentPid, currentTopologyVersion.getProcessId());

    // Different process ID should return immediately with the current TopologyVersion.
    auto topologyVersionWithDifferentProcessId =
        TopologyVersion(differentPid, currentTopologyVersion.getCounter());
    auto response = getTopoCoord().awaitIsMasterResponse(
        opCtx.get(), topologyVersionWithDifferentProcessId, deadline);
    ASSERT_EQUALS(response->getTopologyVersion().getProcessId(),
                  currentTopologyVersion.getProcessId());
    ASSERT_EQUALS(response->getTopologyVersion().getCounter(), currentTopologyVersion.getCounter());
}

TEST_F(MongosTopoCoordTest, AwaitIsMasterReturnsImmediatelyWithNoTopologyVersion) {
    auto opCtx = makeOperationContext();
    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();

    // No topology version should return immediately with the current TopologyVersion. Note that we
    // do not specify deadline when there is no topology version.
    auto response = getTopoCoord().awaitIsMasterResponse(opCtx.get(), boost::none, boost::none);
    ASSERT_EQUALS(response->getTopologyVersion().getProcessId(),
                  currentTopologyVersion.getProcessId());
    ASSERT_EQUALS(response->getTopologyVersion().getCounter(), currentTopologyVersion.getCounter());
}

TEST_F(MongosTopoCoordTest, IsMasterReturnsErrorInQuiesceMode) {
    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();
    auto opCtx = makeOperationContext();
    auto maxAwaitTime = Milliseconds(5000);
    auto deadline = now() + maxAwaitTime;
    // Use 0 ms for quiesce time so that we can immediately return from enterQuiesceModeAndWait.
    auto quiesceTime = Milliseconds(0);

    getTopoCoord().enterQuiesceModeAndWait(opCtx.get(), quiesceTime);

    ASSERT_EQUALS(currentTopologyVersion.getCounter() + 1,
                  getTopoCoord().getTopologyVersion().getCounter());

    // The following isMaster requests should fail immediately with ShutdownInProgress errors
    // instead of following the usual error precedence.

    // Stale topology version
    ASSERT_THROWS_CODE(
        getTopoCoord().awaitIsMasterResponse(opCtx.get(), currentTopologyVersion, deadline),
        AssertionException,
        ErrorCodes::ShutdownInProgress);

    // Current topology version
    currentTopologyVersion = getTopoCoord().getTopologyVersion();
    ASSERT_THROWS_CODE(
        getTopoCoord().awaitIsMasterResponse(opCtx.get(), currentTopologyVersion, deadline),
        AssertionException,
        ErrorCodes::ShutdownInProgress);

    // Different process ID
    auto differentPid = OID::gen();
    ASSERT_NOT_EQUALS(differentPid, currentTopologyVersion.getProcessId());
    auto topologyVersionWithDifferentProcessId =
        TopologyVersion(differentPid, currentTopologyVersion.getCounter());
    ASSERT_THROWS_CODE(getTopoCoord().awaitIsMasterResponse(
                           opCtx.get(), topologyVersionWithDifferentProcessId, deadline),
                       AssertionException,
                       ErrorCodes::ShutdownInProgress);

    // No topology version
    ASSERT_THROWS_CODE(getTopoCoord().awaitIsMasterResponse(opCtx.get(), boost::none, boost::none),
                       AssertionException,
                       ErrorCodes::ShutdownInProgress);
}

TEST_F(MongosTopoCoordTest, IsMasterReturnsErrorOnEnteringQuiesceMode) {
    auto opCtx = makeOperationContext();
    auto currentTopologyVersion = getTopoCoord().getTopologyVersion();
    // Use 0 ms for quiesce time so that we can immediately return from enterQuiesceModeAndWait.
    auto quiesceTime = Milliseconds(0);

    // This will cause the isMaster request to hang.
    auto waitForIsMasterFailPoint = globalFailPointRegistry().find("waitForIsMasterResponse");
    auto timesEnteredFailPoint = waitForIsMasterFailPoint->setMode(FailPoint::alwaysOn);
    ON_BLOCK_EXIT([&] { waitForIsMasterFailPoint->setMode(FailPoint::off, 0); });
    stdx::thread getIsMasterThread([&] {
        Client::setCurrent(getServiceContext()->makeClient("getIsMasterThread"));
        auto threadOpCtx = cc().makeOperationContext();
        auto maxAwaitTime = Milliseconds(5000);
        auto deadline = now() + maxAwaitTime;
        ASSERT_THROWS_CODE(getTopoCoord().awaitIsMasterResponse(
                               threadOpCtx.get(), currentTopologyVersion, deadline),
                           AssertionException,
                           ErrorCodes::ShutdownInProgress);
    });

    // Ensure that awaitIsMasterResponse() is called before entering quiesce mode.
    waitForIsMasterFailPoint->waitForTimesEntered(timesEnteredFailPoint + 1);
    getTopoCoord().enterQuiesceModeAndWait(opCtx.get(), quiesceTime);
    ASSERT_EQUALS(currentTopologyVersion.getCounter() + 1,
                  getTopoCoord().getTopologyVersion().getCounter());
    waitForIsMasterFailPoint->setMode(FailPoint::off);
    getIsMasterThread.join();
}

}  // namespace
}  // namespace mongo
