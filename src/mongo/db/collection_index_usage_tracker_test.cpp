/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class CollectionIndexUsageTrackerTest : public unittest::Test {
protected:
    CollectionIndexUsageTrackerTest() : _tracker(&_clockSource) {}

    /**
     * Returns an unowned pointer to the tracker owned by this test fixture.
     */
    CollectionIndexUsageTracker* getTracker() {
        return &_tracker;
    }

    /**
     * Returns an unowned pointer to the mock clock source owned by this test fixture.
     */
    ClockSourceMock* getClockSource() {
        return &_clockSource;
    }

private:
    ClockSourceMock _clockSource;
    CollectionIndexUsageTracker _tracker;
};

// Test that a newly contructed tracker has an empty map.
TEST_F(CollectionIndexUsageTrackerTest, Empty) {
    ASSERT(getTracker()->getUsageStats().empty());
}

// Test that recording of a single index hit is reflected in returned stats map.
TEST_F(CollectionIndexUsageTrackerTest, SingleHit) {
    getTracker()->registerIndex("foo", BSON("foo" << 1));
    getTracker()->recordIndexAccess("foo");
    CollectionIndexUsageMap statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap.find("foo") != statsMap.end());
    ASSERT_EQUALS(1, statsMap["foo"].accesses.loadRelaxed());
}

// Test that recording of multiple index hits are reflected in stats map.
TEST_F(CollectionIndexUsageTrackerTest, MultipleHit) {
    getTracker()->registerIndex("foo", BSON("foo" << 1));
    getTracker()->recordIndexAccess("foo");
    getTracker()->recordIndexAccess("foo");
    CollectionIndexUsageMap statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap.find("foo") != statsMap.end());
    ASSERT_EQUALS(2, statsMap["foo"].accesses.loadRelaxed());
}

TEST_F(CollectionIndexUsageTrackerTest, IndexKey) {
    getTracker()->registerIndex("foo", BSON("foo" << 1));
    CollectionIndexUsageMap statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap.find("foo") != statsMap.end());
    ASSERT_EQUALS(BSON("foo" << 1), statsMap["foo"].indexKey);
}

// Test that index registration generates an entry in the stats map.
TEST_F(CollectionIndexUsageTrackerTest, Register) {
    getTracker()->registerIndex("foo", BSON("foo" << 1));
    ASSERT_EQUALS(1U, getTracker()->getUsageStats().size());
    getTracker()->registerIndex("bar", BSON("bar" << 1));
    ASSERT_EQUALS(2U, getTracker()->getUsageStats().size());
}

// Test that index deregistration results in removal of an entry from the stats map.
TEST_F(CollectionIndexUsageTrackerTest, Deregister) {
    getTracker()->registerIndex("foo", BSON("foo" << 1));
    getTracker()->registerIndex("bar", BSON("bar" << 1));
    ASSERT_EQUALS(2U, getTracker()->getUsageStats().size());
    getTracker()->unregisterIndex("foo");
    ASSERT_EQUALS(1U, getTracker()->getUsageStats().size());
    getTracker()->unregisterIndex("bar");
    ASSERT_EQUALS(0U, getTracker()->getUsageStats().size());
}

// Test that index deregistration results in reset of the usage counter.
TEST_F(CollectionIndexUsageTrackerTest, HitAfterDeregister) {
    getTracker()->registerIndex("foo", BSON("foo" << 1));
    getTracker()->recordIndexAccess("foo");
    getTracker()->recordIndexAccess("foo");
    CollectionIndexUsageMap statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap.find("foo") != statsMap.end());
    ASSERT_EQUALS(2, statsMap["foo"].accesses.loadRelaxed());

    getTracker()->unregisterIndex("foo");
    statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap.find("foo") == statsMap.end());

    getTracker()->registerIndex("foo", BSON("foo" << 1));
    getTracker()->recordIndexAccess("foo");
    statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap.find("foo") != statsMap.end());
    ASSERT_EQUALS(1, statsMap["foo"].accesses.loadRelaxed());
}

// Test that index tracker start date/time is reset on index deregistration/registration.
TEST_F(CollectionIndexUsageTrackerTest, DateTimeAfterDeregister) {
    getTracker()->registerIndex("foo", BSON("foo" << 1));
    CollectionIndexUsageMap statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap.find("foo") != statsMap.end());
    ASSERT_EQUALS(statsMap["foo"].trackerStartTime, getClockSource()->now());

    getTracker()->unregisterIndex("foo");
    statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap.find("foo") == statsMap.end());

    // Increment clock source so that a new index registration has different start time.
    getClockSource()->advance(Milliseconds(1));

    getTracker()->registerIndex("foo", BSON("foo" << 1));
    statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap.find("foo") != statsMap.end());
    ASSERT_EQUALS(statsMap["foo"].trackerStartTime, getClockSource()->now());
}
}  // namespace
}  // namespace mongo
