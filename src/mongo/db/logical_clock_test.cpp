/**
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/db/logical_clock.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/signed_logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/platform/basic.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

/**
 * Setup LogicalClock with invalid initial time.
 */
class LogicalClockTestBase : public unittest::Test {
protected:
    void setUp() {
        _serviceContext = stdx::make_unique<ServiceContextNoop>();
        auto pTps = stdx::make_unique<TimeProofService>();
        _timeProofService = pTps.get();
        _clock = stdx::make_unique<LogicalClock>(_serviceContext.get(), std::move(pTps));
        _serviceContext->setFastClockSource(
            stdx::make_unique<SharedClockSourceAdapter>(_mockClockSource));
    }

    void tearDown() {
        _clock.reset();
        _serviceContext.reset();
    }

    LogicalClock* getClock() {
        return _clock.get();
    }

    void setMockClockSourceTime(Date_t time) {
        _mockClockSource->reset(time);
    }

    Date_t getMockClockSourceTime() {
        return _mockClockSource->now();
    }

    SignedLogicalTime makeSignedLogicalTime(LogicalTime logicalTime) {
        TimeProofService::Key key = {};
        return SignedLogicalTime(logicalTime, _timeProofService->getProof(logicalTime, key), 0);
    }

    const unsigned currentWallClockSecs() {
        return durationCount<Seconds>(
            _serviceContext->getFastClockSource()->now().toDurationSinceEpoch());
    }

private:
    TimeProofService* _timeProofService;
    std::unique_ptr<ServiceContextNoop> _serviceContext;

    std::shared_ptr<ClockSourceMock> _mockClockSource = std::make_shared<ClockSourceMock>();
    std::unique_ptr<LogicalClock> _clock;
};

// Check that the initial time does not change during logicalClock creation.
TEST_F(LogicalClockTestBase, roundtrip) {
    Timestamp tX(1);
    auto time = LogicalTime(tX);

    getClock()->initClusterTimeFromTrustedSource(time);
    auto storedTime(getClock()->getClusterTime());

    ASSERT_TRUE(storedTime.getTime() == time);
}

// Verify the reserve ticks functionality.
TEST_F(LogicalClockTestBase, reserveTicks) {
    // Set clock to a non-zero time, so we can verify wall clock synchronization.
    setMockClockSourceTime(Date_t::fromMillisSinceEpoch(10 * 1000));

    auto t1 = getClock()->reserveTicks(1);
    auto t2(getClock()->getClusterTime());
    ASSERT_TRUE(t1 == t2.getTime());

    // Make sure we synchronized with the wall clock.
    ASSERT_TRUE(t2.getTime().asTimestamp().getSecs() == 10);

    auto t3 = getClock()->reserveTicks(1);
    t1.addTicks(1);
    ASSERT_TRUE(t3 == t1);

    t3 = getClock()->reserveTicks(100);
    t1.addTicks(1);
    ASSERT_TRUE(t3 == t1);

    t3 = getClock()->reserveTicks(1);
    t1.addTicks(100);
    ASSERT_TRUE(t3 == t1);

    // Ensure overflow to a new second.
    auto initTimeSecs = getClock()->getClusterTime().getTime().asTimestamp().getSecs();
    getClock()->reserveTicks((1U << 31) - 1);
    auto newTimeSecs = getClock()->getClusterTime().getTime().asTimestamp().getSecs();
    ASSERT_TRUE(newTimeSecs == initTimeSecs + 1);
}

// Verify the advanceClusterTime functionality.
TEST_F(LogicalClockTestBase, advanceClusterTime) {
    auto t1 = getClock()->reserveTicks(1);
    t1.addTicks(100);
    SignedLogicalTime l1 = makeSignedLogicalTime(t1);
    ASSERT_OK(getClock()->advanceClusterTimeFromTrustedSource(l1));
    auto l2(getClock()->getClusterTime());
    ASSERT_TRUE(l1.getTime() == l2.getTime());
}

// Verify rate limiter rejects logical times whose seconds values are too far ahead.
TEST_F(LogicalClockTestBase, RateLimiterRejectsLogicalTimesTooFarAhead) {
    Timestamp tooFarAheadTimestamp(
        currentWallClockSecs() +
            durationCount<Seconds>(LogicalClock::kMaxAcceptableLogicalClockDrift) +
            10,  // Add 10 seconds to ensure limit is exceeded.
        1);
    SignedLogicalTime l1 = makeSignedLogicalTime(LogicalTime(tooFarAheadTimestamp));

    ASSERT_EQ(ErrorCodes::ClusterTimeFailsRateLimiter, getClock()->advanceClusterTime(l1));
    ASSERT_EQ(ErrorCodes::ClusterTimeFailsRateLimiter,
              getClock()->advanceClusterTimeFromTrustedSource(l1));
}

// Verify cluster time can be initialized to a very old time.
TEST_F(LogicalClockTestBase, InitFromTrustedSourceCanAcceptVeryOldLogicalTime) {
    Timestamp veryOldTimestamp(
        currentWallClockSecs() -
        (durationCount<Seconds>(LogicalClock::kMaxAcceptableLogicalClockDrift) * 5));
    auto veryOldTime = LogicalTime(veryOldTimestamp);
    getClock()->initClusterTimeFromTrustedSource(veryOldTime);

    ASSERT_TRUE(getClock()->getClusterTime().getTime() == veryOldTime);
}

}  // unnamed namespace
}  // namespace mongo
