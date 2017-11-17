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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_clock_test_fixture.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const NamespaceString kDummyNamespaceString("test", "foo");

using LogicalClockTest = LogicalClockTestFixture;

LogicalTime buildLogicalTime(unsigned secs, unsigned inc) {
    return LogicalTime(Timestamp(secs, inc));
}

// Check that the initial time does not change during logicalClock creation.
TEST_F(LogicalClockTest, roundtrip) {
    Timestamp tX(1);
    auto time = LogicalTime(tX);

    getClock()->setClusterTimeFromTrustedSource(time);
    auto storedTime(getClock()->getClusterTime());

    ASSERT_TRUE(storedTime == time);
}

// Verify the reserve ticks functionality.
TEST_F(LogicalClockTest, reserveTicks) {
    // Set clock to a non-zero time, so we can verify wall clock synchronization.
    setMockClockSourceTime(Date_t::fromMillisSinceEpoch(10 * 1000));

    auto t1 = getClock()->reserveTicks(1);
    auto t2(getClock()->getClusterTime());
    ASSERT_TRUE(t1 == t2);

    // Make sure we synchronized with the wall clock.
    ASSERT_TRUE(t2.asTimestamp().getSecs() == 10);

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
    auto initTimeSecs = getClock()->getClusterTime().asTimestamp().getSecs();
    getClock()->reserveTicks((1U << 31) - 1);
    auto newTimeSecs = getClock()->getClusterTime().asTimestamp().getSecs();
    ASSERT_TRUE(newTimeSecs == initTimeSecs + 1);
}

// Verify the advanceClusterTime functionality.
TEST_F(LogicalClockTest, advanceClusterTime) {
    auto t1 = getClock()->reserveTicks(1);
    t1.addTicks(100);
    ASSERT_OK(getClock()->advanceClusterTime(t1));
    ASSERT_TRUE(t1 == getClock()->getClusterTime());
}

// Verify rate limiter rejects cluster times whose seconds values are too far ahead.
TEST_F(LogicalClockTest, RateLimiterRejectsLogicalTimesTooFarAhead) {
    setMockClockSourceTime(Date_t::fromMillisSinceEpoch(10 * 1000));

    Timestamp tooFarAheadTimestamp(
        durationCount<Seconds>(getMockClockSourceTime().toDurationSinceEpoch()) +
            durationCount<Seconds>(LogicalClock::kMaxAcceptableLogicalClockDriftSecs) +
            10,  // Add 10 seconds to ensure limit is exceeded.
        1);
    LogicalTime t1(tooFarAheadTimestamp);

    ASSERT_EQ(ErrorCodes::ClusterTimeFailsRateLimiter, getClock()->advanceClusterTime(t1));
}

// Verify cluster time can be initialized to a very old time.
TEST_F(LogicalClockTest, InitFromTrustedSourceCanAcceptVeryOldLogicalTime) {
    setMockClockSourceTime(Date_t::fromMillisSinceEpoch(
        durationCount<Seconds>(LogicalClock::kMaxAcceptableLogicalClockDriftSecs) * 10 * 1000));

    Timestamp veryOldTimestamp(
        durationCount<Seconds>(getMockClockSourceTime().toDurationSinceEpoch()) -
        (durationCount<Seconds>(LogicalClock::kMaxAcceptableLogicalClockDriftSecs) * 5));
    auto veryOldTime = LogicalTime(veryOldTimestamp);
    getClock()->setClusterTimeFromTrustedSource(veryOldTime);

    ASSERT_TRUE(getClock()->getClusterTime() == veryOldTime);
}

// Verify writes to the oplog advance cluster time.
TEST_F(LogicalClockTest, WritesToOplogAdvanceClusterTime) {
    Timestamp tX(1, 0);
    auto initialTime = LogicalTime(tX);

    getClock()->setClusterTimeFromTrustedSource(initialTime);
    ASSERT_TRUE(getClock()->getClusterTime() == initialTime);

    getDBClient()->insert(kDummyNamespaceString.ns(), BSON("x" << 1));
    ASSERT_TRUE(getClock()->getClusterTime() > initialTime);
    ASSERT_EQ(getClock()->getClusterTime().asTimestamp(),
              replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp());
}

// Tests the scenario where an admin incorrectly sets the wall clock more than
// maxAcceptableLogicalClockDriftSecs in the past at startup, and cluster time is initialized to
// the incorrect past time, then the admin resets the clock to the current time. In this case,
// cluster time can be advanced through metadata as long as the new time isn't
// maxAcceptableLogicalClockDriftSecs ahead of the correct current wall clock time, since the rate
// limiter compares new times to the wall clock, not the cluster time.
TEST_F(LogicalClockTest, WallClockSetTooFarInPast) {
    auto oneDay = Seconds(24 * 60 * 60);

    // Current wall clock and cluster time.
    auto currentSecs = LogicalClock::kMaxAcceptableLogicalClockDriftSecs * 10;
    LogicalTime currentTime(Timestamp(currentSecs, 0));

    // Set wall clock more than maxAcceptableLogicalClockDriftSecs seconds in the past.
    auto pastSecs = currentSecs - LogicalClock::kMaxAcceptableLogicalClockDriftSecs - oneDay;
    setMockClockSourceTime(Date_t::fromDurationSinceEpoch(pastSecs));

    // If cluster time is either uninitialized or even farther in the past, a write would set
    // cluster time more than maxAcceptableLogicalClockDriftSecs in the past.
    getDBClient()->insert(kDummyNamespaceString.ns(), BSON("x" << 1));
    ASSERT_TRUE(
        getClock()->getClusterTime() <
        LogicalTime(Timestamp(currentSecs - LogicalClock::kMaxAcceptableLogicalClockDriftSecs, 0)));

    // Set wall clock to the current time on the affected node.
    setMockClockSourceTime(Date_t::fromDurationSinceEpoch(currentSecs));

    // Verify that maxAcceptableLogicalClockDriftSecs parameter does not need to be increased to
    // advance cluster time through metadata back to the current time.
    ASSERT_OK(getClock()->advanceClusterTime(currentTime));
    ASSERT_TRUE(getClock()->getClusterTime() == currentTime);
}

// Tests the scenario where an admin incorrectly sets the wall clock more than
// maxAcceptableLogicalClockDriftSecs in the future and a write is accepted, advancing cluster
// time, then the admin resets the clock to the current time. In this case, cluster time cannot be
// advanced through metadata unless the drift parameter is increased.
TEST_F(LogicalClockTest, WallClockSetTooFarInFuture) {
    auto oneDay = Seconds(24 * 60 * 60);

    // Current wall clock and cluster time.
    auto currentSecs = LogicalClock::kMaxAcceptableLogicalClockDriftSecs * 10;
    LogicalTime currentTime(Timestamp(currentSecs, 0));

    // Set wall clock more than maxAcceptableLogicalClockDriftSecs seconds in the future.
    auto futureSecs = currentSecs + LogicalClock::kMaxAcceptableLogicalClockDriftSecs + oneDay;
    setMockClockSourceTime(Date_t::fromDurationSinceEpoch(futureSecs));

    // A write gets through and advances cluster time more than maxAcceptableLogicalClockDriftSecs
    // in the future.
    getDBClient()->insert(kDummyNamespaceString.ns(), BSON("x" << 1));
    ASSERT_TRUE(
        getClock()->getClusterTime() >
        LogicalTime(Timestamp(currentSecs + LogicalClock::kMaxAcceptableLogicalClockDriftSecs, 0)));

    // Set wall clock to the current time on the affected node.
    setMockClockSourceTime(Date_t::fromDurationSinceEpoch(currentSecs));

    // Verify that maxAcceptableLogicalClockDriftSecs parameter has to be increased to advance
    // cluster time through metadata.
    auto nextTime = getClock()->getClusterTime();
    nextTime.addTicks(1);  // The next lowest cluster time.

    ASSERT_EQ(ErrorCodes::ClusterTimeFailsRateLimiter, getClock()->advanceClusterTime(nextTime));

    // Set wall clock to the current time + 1 day to simulate increasing the
    // maxAcceptableLogicalClockDriftSecs parameter, which can only be set at startup, and verify
    // time can be advanced through metadata again.
    setMockClockSourceTime(Date_t::fromDurationSinceEpoch(currentSecs + oneDay));

    ASSERT_OK(getClock()->advanceClusterTime(nextTime));
    ASSERT_TRUE(getClock()->getClusterTime() == nextTime);
}

// Verify the behavior of advancing cluster time around the max allowed values.
TEST_F(LogicalClockTest, ReserveTicksBehaviorAroundMaxTime) {
    unsigned maxVal = LogicalClock::kMaxSignedInt;

    // Verify clock can be advanced near the max values.

    // Can always advance to the max value for the inc field.
    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(maxVal - 1, maxVal - 1));
    getClock()->reserveTicks(1);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(maxVal - 1, maxVal));

    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(maxVal - 1, maxVal - 5));
    getClock()->reserveTicks(5);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(maxVal - 1, maxVal));

    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(0, maxVal - 1));
    getClock()->reserveTicks(1);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(0, maxVal));

    // Can overflow inc into seconds to reach max seconds value.
    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(maxVal - 1, maxVal));
    getClock()->reserveTicks(1);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(maxVal, 1));

    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(maxVal - 1, maxVal - 5));
    getClock()->reserveTicks(10);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(maxVal, 10));

    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(maxVal - 1, 1));
    getClock()->reserveTicks(maxVal);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(maxVal, maxVal));

    // Can advance inc field when seconds field is at the max value.
    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(maxVal, 1));
    getClock()->reserveTicks(1);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(maxVal, 2));

    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(maxVal, 1));
    getClock()->reserveTicks(100);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(maxVal, 101));

    // Can advance to the max value for both the inc and seconds fields.
    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(maxVal, maxVal - 1));
    getClock()->reserveTicks(1);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(maxVal, maxVal));

    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(maxVal, maxVal - 5));
    getClock()->reserveTicks(5);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(maxVal, maxVal));

    // Verify scenarios where the clock cannot be advanced.

    // Can't overflow inc into seconds when seconds field is at the max value.
    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(maxVal, maxVal));
    ASSERT_THROWS(getClock()->reserveTicks(1), std::exception);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(maxVal, maxVal));

    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(maxVal, maxVal));
    ASSERT_THROWS(getClock()->reserveTicks(5), std::exception);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(maxVal, maxVal));

    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(maxVal, maxVal - 1));
    ASSERT_THROWS(getClock()->reserveTicks(2), std::exception);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(maxVal, maxVal - 1));

    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(maxVal, maxVal - 11));
    ASSERT_THROWS(getClock()->reserveTicks(12), std::exception);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(maxVal, maxVal - 11));
}

// Verify behavior of advancing cluster time when the wall clock is near the max allowed value.
TEST_F(LogicalClockTest, ReserveTicksBehaviorWhenWallClockNearMaxTime) {
    unsigned maxVal = LogicalClock::kMaxSignedInt;

    // Can be set to the max possible time by catching up to the wall clock.
    setMockClockSourceTime(Date_t::fromDurationSinceEpoch(Seconds(maxVal)));

    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(1, 1));
    getClock()->reserveTicks(1);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(maxVal, 1));

    // Should fail when wall clock would advance cluster time beyond the max allowed time.
    setMockClockSourceTime(Date_t::max());

    resetClock()->setClusterTimeFromTrustedSource(buildLogicalTime(1, 1));
    ASSERT_THROWS(getClock()->reserveTicks(1), std::exception);
    ASSERT_EQ(getClock()->getClusterTime(), buildLogicalTime(1, 1));
}

// Verify the clock rejects cluster times greater than the max allowed time.
TEST_F(LogicalClockTest, RejectsLogicalTimesGreaterThanMaxTime) {
    unsigned maxVal = LogicalClock::kMaxSignedInt;

    // A cluster time can be greater than the maximum value allowed because the signed integer
    // maximum is used for legacy compatibility, but these fields are stored as unsigned integers.
    auto beyondMaxTime = buildLogicalTime(maxVal + 1, maxVal + 1);

    // The clock can't be initialized to a time greater than the max possible.
    resetClock();
    ASSERT_THROWS(getClock()->setClusterTimeFromTrustedSource(beyondMaxTime), std::exception);
    ASSERT_TRUE(getClock()->getClusterTime() == LogicalTime());

    // The time can't be advanced through metadata to a time greater than the max possible.
    // Advance the wall clock close enough to the new value so the rate check is passed.
    auto almostMaxSecs =
        Seconds(maxVal) - LogicalClock::kMaxAcceptableLogicalClockDriftSecs + Seconds(10);
    setMockClockSourceTime(Date_t::fromDurationSinceEpoch(almostMaxSecs));
    ASSERT_THROWS(getClock()->advanceClusterTime(beyondMaxTime).transitional_ignore(),
                  std::exception);
    ASSERT_TRUE(getClock()->getClusterTime() == LogicalTime());
}

}  // unnamed namespace
}  // namespace mongo
