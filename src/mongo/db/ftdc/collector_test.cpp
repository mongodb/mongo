/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/ftdc/collector.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

constexpr auto kFieldName = "Field"_sd;
constexpr auto kAnotherFieldName = "AnotherField"_sd;
constexpr auto kCollectorName = "Collector1"_sd;
constexpr auto kAnotherCollectorName = "Collector2"_sd;
constexpr auto kSampleData = 1;
constexpr auto kMoreSampleData = 12;

class SampleCollectorCacheTestFixture : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = makeOperationContext();
    }

    BSONObj collect(SampleCollectorCache& collectorCache) {
        BSONObjBuilder bob;
        collectorCache.refresh(_opCtx.get(), &bob);
        return bob.obj();
    }

    SampleCollectorCache makeSampleCollectorCache(Milliseconds timeout = Milliseconds{200},
                                                  size_t minThreads = 1,
                                                  size_t maxThreads = 2) {
        return {timeout, minThreads, maxThreads};
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

class DummyFTDCCollector : public FTDCCollectorInterface {
public:
    std::string name() const override {
        return "dummy";
    }

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) override {
        builder.append("name", "dummy");
    }
};

TEST(FTDCCollectorTest, FilteredCollectorTest) {
    auto doCollect = [](FTDCCollectorInterface& collector) {
        BSONObjBuilder builder;
        if (collector.hasData()) {
            collector.collect(nullptr, builder);
        }
        return builder.done().toString();
    };

    bool enabled = true;
    FilteredFTDCCollector filter([&] { return enabled; }, std::make_unique<DummyFTDCCollector>());

    ASSERT_TRUE(filter.hasData());
    ASSERT_EQUALS(doCollect(filter), R"({ name: "dummy" })");

    enabled = false;
    ASSERT_FALSE(filter.hasData());
    ASSERT_EQUALS(doCollect(filter), R"({})");
}

TEST_F(SampleCollectorCacheTestFixture, NoCollectorsShouldReturnNothing) {
    auto collector = makeSampleCollectorCache();
    auto sample = collect(collector);
    ASSERT_EQ(sample.begin(), sample.end());
}

TEST_F(SampleCollectorCacheTestFixture, MultipleCollectors) {
    auto collector = makeSampleCollectorCache();
    collector.addCollector(kCollectorName, true, [&](OperationContext*, BSONObjBuilder* bob) {
        bob->append(kFieldName, kSampleData);
    });
    collector.addCollector(
        kAnotherCollectorName, true, [&](OperationContext*, BSONObjBuilder* bob) {
            bob->append(kAnotherFieldName, kMoreSampleData);
        });

    auto sample = collect(collector);
    ASSERT_TRUE(sample.hasField(kCollectorName));
    ASSERT_TRUE(sample.hasField(kAnotherCollectorName));
}

TEST_F(SampleCollectorCacheTestFixture, CollectorThrowsError) {
    auto collector = makeSampleCollectorCache(Seconds(5));
    collector.addCollector(kCollectorName, true, [&](OperationContext*, BSONObjBuilder*) {
        uasserted(ErrorCodes::CallbackCanceled, "Collection threw an error.");
    });

    // An error thrown in the collection thread should be passed into the main thread.
    ASSERT_THROWS_CODE(collect(collector), DBException, ErrorCodes::CallbackCanceled);
}

TEST_F(SampleCollectorCacheTestFixture, TimeoutDuringCollectionShouldFinishInTheNextCycle) {
    auto collector = makeSampleCollectorCache();

    Notification<void> stall;
    collector.addCollector(kCollectorName, true, [&](OperationContext*, BSONObjBuilder* bob) {
        stall.get();
        bob->append(kFieldName, kSampleData);
    });

    // Sample should be empty since collector is stalling.
    auto sample1 = collect(collector);
    ASSERT_EQ(sample1.begin(), sample1.end());

    stall.set();

    // After the stall, we should be able to get the data.
    auto sample2 = collect(collector);
    ASSERT_TRUE(sample2.hasField(kCollectorName));
}

TEST_F(SampleCollectorCacheTestFixture, TimeoutShouldNotAffectOtherSamples) {
    // Tracks the order of collections by recording the name of collectors in the order they are
    // invoked by `collector`.
    synchronized_value<std::vector<StringData>> collections;

    auto collector = makeSampleCollectorCache();

    Notification<void> collectionCanMakeProgress;
    collector.addCollector(kCollectorName, true, [&](OperationContext*, BSONObjBuilder* bob) {
        (**collections).push_back(kCollectorName);
        collectionCanMakeProgress.get();
        bob->append(kFieldName, kSampleData);
    });

    collector.addCollector(
        kAnotherCollectorName, true, [&](OperationContext*, BSONObjBuilder* bob) {
            (**collections).push_back(kAnotherCollectorName);
            bob->append(kAnotherFieldName, kMoreSampleData);
        });

    auto sample1 = collect(collector);

    // The following is set for the next round of tests -- i.e. `auto sample2 = collect(...)`, but
    // we set it early to make sure the test doesn't hang if any of the following assertions throw.
    collectionCanMakeProgress.set();

    ASSERT_FALSE(sample1.hasField(kCollectorName));
    ASSERT_TRUE(sample1.hasField(kAnotherCollectorName));

    using unittest::match::Eq;
    ASSERT_THAT(**collections, Eq(std::vector<StringData>{kCollectorName, kAnotherCollectorName}));

    auto sample2 = collect(collector);
    ASSERT_TRUE(sample2.hasField(kCollectorName));
    ASSERT_TRUE(sample2.hasField(kAnotherCollectorName));
}

TEST_F(SampleCollectorCacheTestFixture, ShutdownDrainsWork) {
    Notification<void> workDrained;
    {
        Notification<void> collectReturned;
        auto collector = makeSampleCollectorCache();
        ON_BLOCK_EXIT([&] { collectReturned.set(); });
        collector.addCollector(kCollectorName, true, [&](OperationContext* opCtx, BSONObjBuilder*) {
            collectReturned.get();
            // The following just increases the odds of running the destructor for `collector`
            // while still running this callback.
            opCtx->sleepFor(Milliseconds(100));
            workDrained.set();
        });
        collect(collector);
    }

    ASSERT_TRUE(workDrained);
}

}  // namespace
}  // namespace mongo
