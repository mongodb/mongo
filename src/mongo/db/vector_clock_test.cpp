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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/vector_clock_gen.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/vector_clock_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

const NamespaceString kDummyNamespaceString("test", "foo");

using VectorClockTest = VectorClockTestFixture;

constexpr unsigned maxVal = std::numeric_limits<int32_t>::max();

LogicalTime buildLogicalTime(unsigned secs, unsigned inc) {
    return LogicalTime(Timestamp(secs, inc));
}

// Check that the initial time does not change during VectorClock creation.
TEST_F(VectorClockTest, roundtrip) {
    Timestamp tX(1);
    auto time = LogicalTime(tX);

    VectorClockMutable::get(getServiceContext())->tickClusterTimeTo(time);
    auto storedTime(getClusterTime());

    ASSERT_TRUE(storedTime == time);
}

// Verify the reserve ticks functionality.
TEST_F(VectorClockTest, reserveTicks) {
    // Set clock to a non-zero time, so we can verify wall clock synchronization.
    setMockClockSourceTime(Date_t::fromMillisSinceEpoch(10 * 1000));

    auto t1 = VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
    auto t2(getClusterTime());
    ASSERT_TRUE(t1 == t2);

    // Make sure we synchronized with the wall clock.
    ASSERT_TRUE(t2.asTimestamp().getSecs() == 10);

    auto t3 = VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
    t1.addTicks(1);
    ASSERT_TRUE(t3 == t1);

    t3 = VectorClockMutable::get(getServiceContext())->tickClusterTime(100);
    t1.addTicks(1);
    ASSERT_TRUE(t3 == t1);

    t3 = VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
    t1.addTicks(100);
    ASSERT_TRUE(t3 == t1);

    // Ensure overflow to a new second.
    auto initTimeSecs = getClusterTime().asTimestamp().getSecs();
    VectorClockMutable::get(getServiceContext())->tickClusterTime((1U << 31) - 1);
    auto newTimeSecs = getClusterTime().asTimestamp().getSecs();
    ASSERT_TRUE(newTimeSecs == initTimeSecs + 1);
}

// Verify the advanceClusterTime functionality.
TEST_F(VectorClockTest, advanceClusterTime) {
    auto t1 = VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
    t1.addTicks(100);
    advanceClusterTime(t1);
    ASSERT_TRUE(t1 == getClusterTime());
}

// Verify rate limiter rejects cluster times whose seconds values are too far ahead.
TEST_F(VectorClockTest, RateLimiterRejectsLogicalTimesTooFarAhead) {
    setMockClockSourceTime(Date_t::fromMillisSinceEpoch(10 * 1000));

    Timestamp tooFarAheadTimestamp(
        durationCount<Seconds>(getMockClockSourceTime().toDurationSinceEpoch()) +
            kMaxAcceptableLogicalClockDriftSecsDefault +
            10,  // Add 10 seconds to ensure limit is exceeded.
        1);
    LogicalTime t1(tooFarAheadTimestamp);

    ASSERT_THROWS_CODE(
        advanceClusterTime(t1), DBException, ErrorCodes::ClusterTimeFailsRateLimiter);
}

// Verify cluster time can be initialized to a very old time.
TEST_F(VectorClockTest, InitFromTrustedSourceCanAcceptVeryOldLogicalTime) {
    setMockClockSourceTime(Date_t::fromMillisSinceEpoch(
        durationCount<Seconds>(Seconds(kMaxAcceptableLogicalClockDriftSecsDefault)) * 10 * 1000));

    Timestamp veryOldTimestamp(
        durationCount<Seconds>(getMockClockSourceTime().toDurationSinceEpoch()) -
        (kMaxAcceptableLogicalClockDriftSecsDefault * 5));
    auto veryOldTime = LogicalTime(veryOldTimestamp);
    VectorClockMutable::get(getServiceContext())->tickClusterTimeTo(veryOldTime);

    ASSERT_TRUE(getClusterTime() == veryOldTime);
}

// Verify writes to the oplog advance cluster time.
TEST_F(VectorClockTest, WritesToOplogAdvanceClusterTime) {
    Timestamp tX(1, 0);
    auto initialTime = LogicalTime(tX);

    VectorClockMutable::get(getServiceContext())->tickClusterTimeTo(initialTime);
    ASSERT_TRUE(getClusterTime() == initialTime);

    getDBClient()->insert(kDummyNamespaceString.ns(), BSON("x" << 1));
    ASSERT_TRUE(getClusterTime() > initialTime);
    ASSERT_EQ(getClusterTime().asTimestamp(),
              replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp());
}

// Tests the scenario where an admin incorrectly sets the wall clock more than
// maxAcceptableLogicalClockDriftSecs in the past at startup, and cluster time is initialized to
// the incorrect past time, then the admin resets the clock to the current time. In this case,
// cluster time can be advanced through metadata as long as the new time isn't
// maxAcceptableLogicalClockDriftSecs ahead of the correct current wall clock time, since the rate
// limiter compares new times to the wall clock, not the cluster time.
TEST_F(VectorClockTest, WallClockSetTooFarInPast) {
    auto oneDay = Seconds(24 * 60 * 60);

    // Current wall clock and cluster time.
    auto currentSecs = Seconds(kMaxAcceptableLogicalClockDriftSecsDefault) * 10;
    LogicalTime currentTime(Timestamp(currentSecs, 0));

    // Set wall clock more than maxAcceptableLogicalClockDriftSecs seconds in the past.
    auto pastSecs = currentSecs - Seconds(kMaxAcceptableLogicalClockDriftSecsDefault) - oneDay;
    setMockClockSourceTime(Date_t::fromDurationSinceEpoch(pastSecs));

    // If cluster time is either uninitialized or even farther in the past, a write would set
    // cluster time more than maxAcceptableLogicalClockDriftSecs in the past.
    getDBClient()->insert(kDummyNamespaceString.ns(), BSON("x" << 1));
    ASSERT_TRUE(getClusterTime() <
                LogicalTime(Timestamp(
                    currentSecs - Seconds(kMaxAcceptableLogicalClockDriftSecsDefault), 0)));

    // Set wall clock to the current time on the affected node.
    setMockClockSourceTime(Date_t::fromDurationSinceEpoch(currentSecs));

    // Verify that maxAcceptableLogicalClockDriftSecs parameter does not need to be increased to
    // advance cluster time through metadata back to the current time.
    advanceClusterTime(currentTime);
    ASSERT_TRUE(getClusterTime() == currentTime);
}

// Tests the scenario where an admin incorrectly sets the wall clock more than
// maxAcceptableLogicalClockDriftSecs in the future and a write is accepted, advancing cluster
// time, then the admin resets the clock to the current time. In this case, cluster time cannot be
// advanced through metadata unless the drift parameter is increased.
TEST_F(VectorClockTest, WallClockSetTooFarInFuture) {
    auto oneDay = Seconds(24 * 60 * 60);

    // Current wall clock and cluster time.
    auto currentSecs = Seconds(kMaxAcceptableLogicalClockDriftSecsDefault) * 10;
    LogicalTime currentTime(Timestamp(currentSecs, 0));

    // Set wall clock more than maxAcceptableLogicalClockDriftSecs seconds in the future.
    auto futureSecs = currentSecs + Seconds(kMaxAcceptableLogicalClockDriftSecsDefault) + oneDay;
    setMockClockSourceTime(Date_t::fromDurationSinceEpoch(futureSecs));

    // A write gets through and advances cluster time more than maxAcceptableLogicalClockDriftSecs
    // in the future.
    getDBClient()->insert(kDummyNamespaceString.ns(), BSON("x" << 1));
    ASSERT_TRUE(getClusterTime() >
                LogicalTime(Timestamp(
                    currentSecs + Seconds(kMaxAcceptableLogicalClockDriftSecsDefault), 0)));

    // Set wall clock to the current time on the affected node.
    setMockClockSourceTime(Date_t::fromDurationSinceEpoch(currentSecs));

    // Verify that maxAcceptableLogicalClockDriftSecs parameter has to be increased to advance
    // cluster time through metadata.
    auto nextTime = getClusterTime();
    nextTime.addTicks(1);  // The next lowest cluster time.

    ASSERT_THROWS_CODE(
        advanceClusterTime(nextTime), DBException, ErrorCodes::ClusterTimeFailsRateLimiter);

    // Set wall clock to the current time + 1 day to simulate increasing the
    // maxAcceptableLogicalClockDriftSecs parameter, which can only be set at startup, and verify
    // time can be advanced through metadata again.
    setMockClockSourceTime(Date_t::fromDurationSinceEpoch(currentSecs + oneDay));

    advanceClusterTime(nextTime);
    ASSERT_TRUE(getClusterTime() == nextTime);
}

// Verify the behavior of advancing cluster time around the max allowed values.
TEST_F(VectorClockTest, ReserveTicksBehaviorAroundMaxTime) {
    // Verify clock can be advanced near the max values.

    // Can always advance to the max value for the inc field.
    resetClock()->tickClusterTimeTo(buildLogicalTime(maxVal - 1, maxVal - 1));
    VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(maxVal - 1, maxVal));

    resetClock()->tickClusterTimeTo(buildLogicalTime(maxVal - 1, maxVal - 5));
    VectorClockMutable::get(getServiceContext())->tickClusterTime(5);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(maxVal - 1, maxVal));

    resetClock()->tickClusterTimeTo(buildLogicalTime(0, maxVal - 1));
    VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(0, maxVal));

    // Can overflow inc into seconds to reach max seconds value.
    resetClock()->tickClusterTimeTo(buildLogicalTime(maxVal - 1, maxVal));
    VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(maxVal, 1));

    resetClock()->tickClusterTimeTo(buildLogicalTime(maxVal - 1, maxVal - 5));
    VectorClockMutable::get(getServiceContext())->tickClusterTime(10);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(maxVal, 10));

    resetClock()->tickClusterTimeTo(buildLogicalTime(maxVal - 1, 1));
    VectorClockMutable::get(getServiceContext())->tickClusterTime(maxVal);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(maxVal, maxVal));

    // Can advance inc field when seconds field is at the max value.
    resetClock()->tickClusterTimeTo(buildLogicalTime(maxVal, 1));
    VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(maxVal, 2));

    resetClock()->tickClusterTimeTo(buildLogicalTime(maxVal, 1));
    VectorClockMutable::get(getServiceContext())->tickClusterTime(100);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(maxVal, 101));

    // Can advance to the max value for both the inc and seconds fields.
    resetClock()->tickClusterTimeTo(buildLogicalTime(maxVal, maxVal - 1));
    VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(maxVal, maxVal));

    resetClock()->tickClusterTimeTo(buildLogicalTime(maxVal, maxVal - 5));
    VectorClockMutable::get(getServiceContext())->tickClusterTime(5);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(maxVal, maxVal));

    // Verify scenarios where the clock cannot be advanced.

    // Can't overflow inc into seconds when seconds field is at the max value.
    resetClock()->tickClusterTimeTo(buildLogicalTime(maxVal, maxVal));
    ASSERT_THROWS(VectorClockMutable::get(getServiceContext())->tickClusterTime(1), DBException);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(maxVal, maxVal));

    resetClock()->tickClusterTimeTo(buildLogicalTime(maxVal, maxVal));
    ASSERT_THROWS(VectorClockMutable::get(getServiceContext())->tickClusterTime(5), DBException);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(maxVal, maxVal));

    resetClock()->tickClusterTimeTo(buildLogicalTime(maxVal, maxVal - 1));
    ASSERT_THROWS(VectorClockMutable::get(getServiceContext())->tickClusterTime(2), DBException);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(maxVal, maxVal - 1));

    resetClock()->tickClusterTimeTo(buildLogicalTime(maxVal, maxVal - 11));
    ASSERT_THROWS(VectorClockMutable::get(getServiceContext())->tickClusterTime(12), DBException);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(maxVal, maxVal - 11));
}

// Verify behavior of advancing cluster time when the wall clock is near the max allowed value.
TEST_F(VectorClockTest, ReserveTicksBehaviorWhenWallClockNearMaxTime) {
    // Can be set to the max possible time by catching up to the wall clock.
    setMockClockSourceTime(Date_t::fromDurationSinceEpoch(Seconds(maxVal)));

    resetClock()->tickClusterTimeTo(buildLogicalTime(1, 1));
    VectorClockMutable::get(getServiceContext())->tickClusterTime(1);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(maxVal, 1));

    // Should fail when wall clock would advance cluster time beyond the max allowed time.
    setMockClockSourceTime(Date_t::max());

    resetClock()->tickClusterTimeTo(buildLogicalTime(1, 1));
    ASSERT_THROWS(VectorClockMutable::get(getServiceContext())->tickClusterTime(1), DBException);
    ASSERT_EQ(getClusterTime(), buildLogicalTime(1, 1));
}

// Verify the clock rejects cluster times greater than the max allowed time.
TEST_F(VectorClockTest, RejectsLogicalTimesGreaterThanMaxTime) {
    // A cluster time can be greater than the maximum value allowed because the signed integer
    // maximum is used for legacy compatibility, but these fields are stored as unsigned integers.
    auto beyondMaxTime = buildLogicalTime(maxVal + 1, maxVal + 1);

    // The clock can't be initialized to a time greater than the max possible.
    resetClock();
    ASSERT_THROWS(VectorClockMutable::get(getServiceContext())->tickClusterTimeTo(beyondMaxTime),
                  DBException);
    ASSERT_TRUE(getClusterTime() == VectorClock::kInitialComponentTime);

    // The time can't be advanced through metadata to a time greater than the max possible.
    // Advance the wall clock close enough to the new value so the rate check is passed.
    auto almostMaxSecs =
        Seconds(maxVal) - Seconds(kMaxAcceptableLogicalClockDriftSecsDefault) + Seconds(10);
    setMockClockSourceTime(Date_t::fromDurationSinceEpoch(almostMaxSecs));
    ASSERT_THROWS(advanceClusterTime(beyondMaxTime), DBException);
    ASSERT_TRUE(getClusterTime() == VectorClock::kInitialComponentTime);
}

}  // unnamed namespace
}  // namespace mongo
