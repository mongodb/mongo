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

#include "mongo/db/collection_index_usage_tracker.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"

#include <string>

#include <absl/container/flat_hash_map.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class CollectionIndexUsageTrackerTest : public unittest::Test {
protected:
    CollectionIndexUsageTrackerTest()
        : _aggregatedIndexUsage(), _tracker(&_aggregatedIndexUsage, &_clockSource) {}

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

    void resetToZero() {
        _aggregatedIndexUsage.resetToZero();
    }

private:
    AggregatedIndexUsageTracker _aggregatedIndexUsage;
    ClockSourceMock _clockSource;
    CollectionIndexUsageTracker _tracker;
};

// Test that a newly contructed tracker has an empty map.
TEST_F(CollectionIndexUsageTrackerTest, Empty) {
    ASSERT(getTracker()->getUsageStats().empty());
}

// Test that recording of a single index hit is reflected in returned stats map.
TEST_F(CollectionIndexUsageTrackerTest, SingleHit) {
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    getTracker()->recordIndexAccess("foo");
    const auto& statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap.find("foo") != statsMap.end());
    ASSERT_EQUALS(1, statsMap.at("foo")->accesses.loadRelaxed());
}

// Test that recording of multiple index hits are reflected in stats map.
TEST_F(CollectionIndexUsageTrackerTest, MultipleHit) {
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    getTracker()->recordIndexAccess("foo");
    getTracker()->recordIndexAccess("foo");
    const auto& statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap.find("foo") != statsMap.end());
    ASSERT_EQUALS(2, statsMap.at("foo")->accesses.loadRelaxed());
}

// Test that an index is registered correctly with indexKey.
TEST_F(CollectionIndexUsageTrackerTest, IndexKey) {
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    const auto& statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap.find("foo") != statsMap.end());
    ASSERT_BSONOBJ_EQ(BSON("foo" << 1), statsMap.at("foo")->indexKey);
}

// Test that index registration generates an entry in the stats map.
TEST_F(CollectionIndexUsageTrackerTest, Register) {
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    ASSERT_EQUALS(1U, getTracker()->getUsageStats().size());
    getTracker()->registerIndex("bar", BSON("bar" << 1), {});
    ASSERT_EQUALS(2U, getTracker()->getUsageStats().size());
}

// Test that index deregistration results in removal of an entry from the stats map.
TEST_F(CollectionIndexUsageTrackerTest, Deregister) {
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    getTracker()->registerIndex("bar", BSON("bar" << 1), {});
    ASSERT_EQUALS(2U, getTracker()->getUsageStats().size());
    getTracker()->unregisterIndex("foo");
    ASSERT_EQUALS(1U, getTracker()->getUsageStats().size());
    getTracker()->unregisterIndex("bar");
    ASSERT_EQUALS(0U, getTracker()->getUsageStats().size());
}

// Test that index deregistration results in reset of the usage counter.
TEST_F(CollectionIndexUsageTrackerTest, HitAfterDeregister) {
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    getTracker()->recordIndexAccess("foo");
    getTracker()->recordIndexAccess("foo");
    const auto& statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap.find("foo") != statsMap.end());
    ASSERT_EQUALS(2, statsMap.at("foo")->accesses.loadRelaxed());

    getTracker()->unregisterIndex("foo");
    ASSERT(statsMap.find("foo") == statsMap.end());

    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    getTracker()->recordIndexAccess("foo");
    ASSERT(statsMap.find("foo") != statsMap.end());
    ASSERT_EQUALS(1, statsMap.at("foo")->accesses.loadRelaxed());
}

// Test that index tracker start date/time is reset on index deregistration/registration.
TEST_F(CollectionIndexUsageTrackerTest, DateTimeAfterDeregister) {
    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    const auto& statsMap = getTracker()->getUsageStats();
    ASSERT(statsMap.find("foo") != statsMap.end());
    ASSERT_EQUALS(statsMap.at("foo")->trackerStartTime, getClockSource()->now());

    getTracker()->unregisterIndex("foo");
    ASSERT(statsMap.find("foo") == statsMap.end());

    // Increment clock source so that a new index registration has different start time.
    getClockSource()->advance(Milliseconds(1));

    getTracker()->registerIndex("foo", BSON("foo" << 1), {});
    ASSERT(statsMap.find("foo") != statsMap.end());
    ASSERT_EQUALS(statsMap.at("foo")->trackerStartTime, getClockSource()->now());
}

namespace {
struct TestCase {
    BSONObj spec;
    std::vector<std::string> features;
};
std::vector<TestCase> testCases = {
    {BSON("key" << BSON("_id" << 1) << "v" << 2), {"id"}},
    {BSON("key" << BSON("foo" << 1) << "v" << 2), {"normal", "single"}},
    {BSON("key" << BSON("foo" << 1) << "sparse" << true << "v" << 2),
     {"normal", "single", "sparse"}},
    {BSON("key" << BSON("foo" << 1) << "unique" << true << "v" << 2),
     {"normal", "single", "unique"}},
    {BSON("key" << BSON("foo" << 1) << "unique" << false << "v" << 2), {"normal", "single"}},
    {BSON("key" << BSON("foo" << 1) << "partialFilterExpression" << BSON("foo" << 1) << "v" << 2),
     {"normal", "single", "partial"}},
    {BSON("key" << BSON("foo" << 1) << "prepareUnique" << true << "v" << 2),
     {"normal", "single", "prepareUnique"}},
    {BSON("key" << BSON("foo" << 1) << "prepareUnique" << false << "v" << 2), {"normal", "single"}},
    {BSON("key" << BSON("foo" << "2d") << "v" << 2), {"2d", "single"}},
    {BSON("key" << BSON("foo" << "2dsphere") << "v" << 2), {"2dsphere", "single"}},
    {BSON("key" << BSON("foo" << "2dsphere_bucket") << "v" << 2), {"2dsphere_bucket", "single"}},
    {BSON("key" << BSON("foo" << 1) << "collation" << BSON("locale" << "en") << "v" << 2),
     {"normal", "single", "collation"}},
    {BSON("key" << BSON("foo" << 1 << "bar" << 1) << "v" << 2), {"normal", "compound"}},
    {BSON("key" << BSON("foo" << "hashed") << "v" << 2), {"hashed", "single"}},
    {BSON("key" << BSON("foo" << "text") << "v" << 2), {"text", "single"}},
    {BSON("key" << BSON("foo" << 1) << "expireAfterSeconds" << 100 << "v" << 2),
     {"normal", "single", "ttl"}},
    {BSON("key" << BSON("wild.$**" << 1) << "v" << 2), {"wildcard", "single"}},
};
}  // namespace

TEST_F(CollectionIndexUsageTrackerTest, CheckForUntestedFeatures) {
    std::set<std::string> testedFeatures;
    for (auto&& testCase : testCases) {
        for (auto&& feature : testCase.features) {
            testedFeatures.insert(feature);
        }
    }

    getAggregatedIndexUsage()->forEachFeature([&](auto f, auto& stats) {
        ASSERT(testedFeatures.contains(f)) << "untested feature: " << f;
    });
}

TEST_F(CollectionIndexUsageTrackerTest, ExhaustiveFeatures) {
    for (auto&& testCase : testCases) {
        resetToZero();

        const auto& spec = testCase.spec;
        const auto& features = testCase.features;

        const std::string pluginName = IndexNames::findPluginName(spec["key"].Obj());
        const auto desc = IndexDescriptor(pluginName, spec);
        const auto name = "test";
        getTracker()->registerIndex(name, spec, IndexFeatures::make(&desc, false /* internal */));
        ASSERT_EQ(1, getAggregatedIndexUsage()->getCount());

        getAggregatedIndexUsage()->forEachFeature([&](auto f, auto& stats) {
            if (std::find(features.begin(), features.end(), f) != features.end()) {
                ASSERT_EQ(1, stats.count.load()) << f << " in " << spec;
            } else {
                ASSERT_EQ(0, stats.count.load()) << f << " in " << spec;
            }
        });

        getTracker()->recordIndexAccess(name);
        getAggregatedIndexUsage()->forEachFeature([&](auto f, auto& stats) {
            if (std::find(features.begin(), features.end(), f) != features.end()) {
                ASSERT_EQ(1, stats.accesses.load()) << f << " in " << spec;
            } else {
                ASSERT_EQ(0, stats.accesses.load()) << f << " in " << spec;
            }
        });

        getTracker()->unregisterIndex(name);
        ASSERT_EQ(0, getAggregatedIndexUsage()->getCount());

        getAggregatedIndexUsage()->forEachFeature([&](auto f, auto& stats) {
            if (std::find(features.begin(), features.end(), f) != features.end()) {
                ASSERT_EQ(0, stats.count.load()) << f << " in " << spec;
            } else {
                ASSERT_EQ(0, stats.count.load()) << f << " in " << spec;
            }
        });

        getAggregatedIndexUsage()->forEachFeature([&](auto f, auto& stats) {
            if (std::find(features.begin(), features.end(), f) != features.end()) {
                ASSERT_EQ(1, stats.accesses.load()) << f << " in " << spec;
            } else {
                ASSERT_EQ(0, stats.accesses.load()) << f << " in " << spec;
            }
        });
    }
}

}  // namespace
}  // namespace mongo
