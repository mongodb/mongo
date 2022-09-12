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

#include "mongo/platform/basic.h"

#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class CollectionIndexUsageTrackerTest : public unittest::Test {
protected:
    CollectionIndexUsageTrackerTest() : _tracker(&_aggregatedIndexUsage, &_clockSource) {}

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

    AggregatedIndexUsageTracker* getAggregatedIndexUsage() {
        return &_aggregatedIndexUsage;
    }

private:
    AggregatedIndexUsageTracker _aggregatedIndexUsage;
    ClockSourceMock _clockSource;
    CollectionIndexUsageTracker _tracker;
};

// Test that a newly contructed tracker has an empty map.
TEST_F(CollectionIndexUsageTrackerTest, Empty) {
    ASSERT(getTracker()->getUsageStats()->empty());
}

// Test that recording of a single index hit is reflected in returned stats map.
TEST_F(CollectionIndexUsageTrackerTest, SingleHit) {
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    getTracker()->recordIndexAccess("foo");
    auto statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap->find("foo") != statsMap->end());
    ASSERT_EQUALS(1, statsMap->at("foo")->accesses.loadRelaxed());
}

// Test that recording of multiple index hits are reflected in stats map.
TEST_F(CollectionIndexUsageTrackerTest, MultipleHit) {
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    getTracker()->recordIndexAccess("foo");
    getTracker()->recordIndexAccess("foo");
    auto statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap->find("foo") != statsMap->end());
    ASSERT_EQUALS(2, statsMap->at("foo")->accesses.loadRelaxed());
}

// Test that an index is registered correctly with indexKey.
TEST_F(CollectionIndexUsageTrackerTest, IndexKey) {
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    auto statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap->find("foo") != statsMap->end());
    ASSERT_BSONOBJ_EQ(BSON("foo" << 1), statsMap->at("foo")->indexKey);
}

// Test that index registration generates an entry in the stats map.
TEST_F(CollectionIndexUsageTrackerTest, Register) {
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    ASSERT_EQUALS(1U, getTracker()->getUsageStats()->size());
    getTracker()->registerIndex("bar", BSON("bar" << 1), {});
    ASSERT_EQUALS(2U, getTracker()->getUsageStats()->size());
}

// Test that index deregistration results in removal of an entry from the stats map.
TEST_F(CollectionIndexUsageTrackerTest, Deregister) {
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    getTracker()->registerIndex("bar", BSON("bar" << 1), {});
    ASSERT_EQUALS(2U, getTracker()->getUsageStats()->size());
    getTracker()->unregisterIndex("foo");
    ASSERT_EQUALS(1U, getTracker()->getUsageStats()->size());
    getTracker()->unregisterIndex("bar");
    ASSERT_EQUALS(0U, getTracker()->getUsageStats()->size());
}

// Test that index deregistration results in reset of the usage counter.
TEST_F(CollectionIndexUsageTrackerTest, HitAfterDeregister) {
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    getTracker()->recordIndexAccess("foo");
    getTracker()->recordIndexAccess("foo");
    auto statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap->find("foo") != statsMap->end());
    ASSERT_EQUALS(2, statsMap->at("foo")->accesses.loadRelaxed());

    getTracker()->unregisterIndex("foo");
    statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap->find("foo") == statsMap->end());

    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    getTracker()->recordIndexAccess("foo");
    statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap->find("foo") != statsMap->end());
    ASSERT_EQUALS(1, statsMap->at("foo")->accesses.loadRelaxed());
}

// Test that index tracker start date/time is reset on index deregistration/registration.
TEST_F(CollectionIndexUsageTrackerTest, DateTimeAfterDeregister) {
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    auto statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap->find("foo") != statsMap->end());
    ASSERT_EQUALS(statsMap->at("foo")->trackerStartTime, getClockSource()->now());

    getTracker()->unregisterIndex("foo");
    statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap->find("foo") == statsMap->end());

    // Increment clock source so that a new index registration has different start time.
    getClockSource()->advance(Milliseconds(1));

    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap->find("foo") != statsMap->end());
    ASSERT_EQUALS(statsMap->at("foo")->trackerStartTime, getClockSource()->now());
}

// Test that the fetched stats map retains an index after the entry is concurrently removed.
TEST_F(CollectionIndexUsageTrackerTest, UsageStatsMapRemainsValidAfterIndexErasure) {
    // Set up an index in the tracker.
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    getTracker()->recordIndexAccess("foo");
    getTracker()->recordIndexAccess("foo");
    auto statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap->find("foo") != statsMap->end());
    ASSERT_EQUALS(2, statsMap->at("foo")->accesses.loadRelaxed());

    // Fetch the map with the index and then erase the index from the map. The previously fetched
    // map should still safely retain the index.
    auto staleStatsMap = getTracker()->getUsageStats();

    getTracker()->unregisterIndex("foo");
    statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap->find("foo") == statsMap->end());

    ASSERT(staleStatsMap->find("foo") != staleStatsMap->end());
    ASSERT_EQUALS(2, staleStatsMap->at("foo")->accesses.loadRelaxed());
}

// Test that a stale stats map copy's index remains unmodified after the up-to-date map removes and
// recreates the index.
TEST_F(CollectionIndexUsageTrackerTest, StaleUsageStatsMapEntryIsNotUpdatedAfterErasure) {
    // Set up an index in the tracker.
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    getTracker()->recordIndexAccess("foo");
    auto statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap->find("foo") != statsMap->end());
    ASSERT_EQUALS(1, statsMap->at("foo")->accesses.loadRelaxed());

    // Fetch a copy of the map with the index and then erase and recreate the index in the map with
    // a different usage count. The previously fetched copy of the map should still safely retain
    // the prior index version of the index with a different usage count.
    auto staleStatsMap = getTracker()->getUsageStats();

    getTracker()->unregisterIndex("foo");
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    getTracker()->recordIndexAccess("foo");
    getTracker()->recordIndexAccess("foo");
    statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap->find("foo") != statsMap->end());
    ASSERT_EQUALS(2, statsMap->at("foo")->accesses.loadRelaxed());

    ASSERT(staleStatsMap->find("foo") != staleStatsMap->end());
    ASSERT_EQUALS(1, staleStatsMap->at("foo")->accesses.loadRelaxed());
}

namespace {
int getFeatureUseCount(AggregatedIndexUsageTracker* aggregatedIndexUsage,
                       std::string featureSearch) {
    int count = 0;
    aggregatedIndexUsage->forEachFeature([&](auto feature, auto& stats) {
        if (featureSearch == feature) {
            count += stats.count.load();
        }
    });
    return count;
}

int getFeatureAccessCount(AggregatedIndexUsageTracker* aggregatedIndexUsage,
                          std::string featureSearch) {
    int accesses = 0;
    aggregatedIndexUsage->forEachFeature([&](auto feature, auto& stats) {
        if (featureSearch == feature) {
            accesses += stats.accesses.load();
        }
    });
    return accesses;
}
}  // namespace

TEST_F(CollectionIndexUsageTrackerTest, GlobalFeatureUsageBasic) {
    auto idSpec = BSON("key" << BSON("_id" << 1) << "unique" << true << "v" << 2);
    auto idDesc = IndexDescriptor("", idSpec);
    getTracker()->registerIndex("_id_", idSpec, IndexFeatures::make(&idDesc, false /* internal */));
    getTracker()->recordIndexAccess("_id_");

    ASSERT_EQ(1, getAggregatedIndexUsage()->getCount());
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "id"));
    ASSERT_EQ(0, getFeatureUseCount(getAggregatedIndexUsage(), "normal"));
    ASSERT_EQ(0, getFeatureUseCount(getAggregatedIndexUsage(), "single"));
    ASSERT_EQ(0, getFeatureUseCount(getAggregatedIndexUsage(), "sparse"));
    ASSERT_EQ(0, getFeatureUseCount(getAggregatedIndexUsage(), "unique"));

    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "id"));
    ASSERT_EQ(0, getFeatureAccessCount(getAggregatedIndexUsage(), "normal"));
    ASSERT_EQ(0, getFeatureAccessCount(getAggregatedIndexUsage(), "single"));
    ASSERT_EQ(0, getFeatureAccessCount(getAggregatedIndexUsage(), "sparse"));
    ASSERT_EQ(0, getFeatureAccessCount(getAggregatedIndexUsage(), "unique"));

    auto spec = BSON("key" << BSON("foo" << 1) << "unique" << true << "sparse" << true << "v" << 2);
    auto desc = IndexDescriptor("", spec);
    getTracker()->registerIndex("foo", spec, IndexFeatures::make(&desc, false /* internal */));
    getTracker()->recordIndexAccess("foo");

    ASSERT_EQ(2, getAggregatedIndexUsage()->getCount());
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "id"));
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "normal"));
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "single"));
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "sparse"));
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "unique"));

    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "id"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "normal"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "single"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "sparse"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "unique"));

    // Register an internal index and expect nothing to change.
    getTracker()->registerIndex("foo2", spec, IndexFeatures::make(&desc, true /* internal */));

    ASSERT_EQ(2, getAggregatedIndexUsage()->getCount());
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "id"));
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "normal"));
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "single"));
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "sparse"));
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "unique"));

    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "id"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "normal"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "single"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "sparse"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "unique"));

    getTracker()->unregisterIndex("foo2");
    ASSERT_EQ(2, getAggregatedIndexUsage()->getCount());
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "id"));
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "normal"));
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "single"));
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "sparse"));
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "unique"));

    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "id"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "normal"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "single"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "sparse"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "unique"));

    getTracker()->unregisterIndex("foo");
    ASSERT_EQ(1, getAggregatedIndexUsage()->getCount());
    ASSERT_EQ(1, getFeatureUseCount(getAggregatedIndexUsage(), "id"));
    ASSERT_EQ(0, getFeatureUseCount(getAggregatedIndexUsage(), "normal"));
    ASSERT_EQ(0, getFeatureUseCount(getAggregatedIndexUsage(), "single"));
    ASSERT_EQ(0, getFeatureUseCount(getAggregatedIndexUsage(), "sparse"));
    ASSERT_EQ(0, getFeatureUseCount(getAggregatedIndexUsage(), "unique"));

    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "id"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "normal"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "single"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "sparse"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "unique"));

    getTracker()->unregisterIndex("_id_");
    ASSERT_EQ(0, getAggregatedIndexUsage()->getCount());
    ASSERT_EQ(0, getFeatureUseCount(getAggregatedIndexUsage(), "id"));
    ASSERT_EQ(0, getFeatureUseCount(getAggregatedIndexUsage(), "normal"));
    ASSERT_EQ(0, getFeatureUseCount(getAggregatedIndexUsage(), "single"));
    ASSERT_EQ(0, getFeatureUseCount(getAggregatedIndexUsage(), "sparse"));
    ASSERT_EQ(0, getFeatureUseCount(getAggregatedIndexUsage(), "unique"));

    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "id"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "normal"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "single"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "sparse"));
    ASSERT_EQ(1, getFeatureAccessCount(getAggregatedIndexUsage(), "unique"));
}

}  // namespace
}  // namespace mongo
