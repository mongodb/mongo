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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

constexpr auto fieldName = "Field"_sd;
constexpr auto anotherFieldName = "AnotherField"_sd;
constexpr auto collectorName = "Collector1"_sd;
constexpr auto anotherCollectorName = "Collector2"_sd;
constexpr auto sampleData = 1;
constexpr auto moreSampleData = 12;

class SampleCollectorCacheTestFixture : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        opCtxPtr = makeOperationContext();
    }

    BSONObj collect(SampleCollectorCache& collectorCache) {
        BSONObjBuilder bob;
        collectorCache.refresh(getOpCtx(), &bob);
        return bob.obj();
    }

    OperationContext* getOpCtx() {
        return opCtxPtr.get();
    }

private:
    ServiceContext::UniqueOperationContext opCtxPtr;
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

std::string doCollect(FTDCCollectorInterface& collector) {
    BSONObjBuilder builder;
    if (collector.hasData()) {
        collector.collect(nullptr, builder);
    }
    return builder.done().toString();
}

TEST(FTDCCollectorTest, FilteredCollectorTest) {
    bool enabled = true;
    FilteredFTDCCollector filter([&] { return enabled; }, std::make_unique<DummyFTDCCollector>());

    enabled = true;
    ASSERT_TRUE(filter.hasData());
    ASSERT_EQUALS(doCollect(filter), R"({ name: "dummy" })");

    enabled = false;
    ASSERT_FALSE(filter.hasData());
    ASSERT_EQUALS(doCollect(filter), R"({})");
}

TEST_F(SampleCollectorCacheTestFixture, NoCollectorsShouldReturnNothing) {
    SampleCollectorCache collector(
        ClusterRole::None, Milliseconds(100), 1 /*minThreads*/, 1 /*maxThreads*/);

    auto sample = collect(collector);
    ASSERT_EQ(sample.begin(), sample.end());
}

TEST_F(SampleCollectorCacheTestFixture, MultipleCollectors) {
    auto role = ClusterRole::None;
    SampleCollectorCache collector(role, Milliseconds(100), 1 /*minThreads*/, 1 /*maxThreads*/);

    collector.addCollector(collectorName, true, role, [&](OperationContext*, BSONObjBuilder* bob) {
        bob->append(fieldName, sampleData);
    });
    collector.addCollector(
        anotherCollectorName, true, role, [&](OperationContext*, BSONObjBuilder* bob) {
            bob->append(anotherFieldName, moreSampleData);
        });

    auto sample = collect(collector);
    auto firstSubObj = sample.getObjectField(collectorName);
    auto secondSubObj = sample.getObjectField(anotherCollectorName);
    ASSERT_TRUE(firstSubObj.hasField(fieldName));
    ASSERT_TRUE(secondSubObj.hasField(anotherFieldName));
    ASSERT_EQ(firstSubObj.getField(fieldName).Int(), sampleData);
    ASSERT_EQ(secondSubObj.getField(anotherFieldName).Int(), moreSampleData);
}

TEST_F(SampleCollectorCacheTestFixture, CollectorThrowsError) {
    auto role = ClusterRole::None;
    SampleCollectorCache collector(role, Milliseconds(100), 1 /*minThreads*/, 1 /*maxThreads*/);

    collector.addCollector(collectorName, true, role, [&](OperationContext*, BSONObjBuilder* bob) {
        uasserted(ErrorCodes::CallbackCanceled, "Collection threw an error.");
    });

    // An error thrown in the collection thread should be passed into the main thread.
    ASSERT_THROWS_CODE(collect(collector), DBException, ErrorCodes::CallbackCanceled);
}

TEST_F(SampleCollectorCacheTestFixture, TimeoutDuringCollectionShouldFinishInTheNextCycle) {
    auto timeout = Milliseconds(100);
    auto role = ClusterRole::None;
    SampleCollectorCache collector(ClusterRole::None, timeout, 1 /*minThreads*/, 1 /*maxThreads*/);

    Notification<void> stall;
    collector.addCollector(collectorName, true, role, [&](OperationContext*, BSONObjBuilder* bob) {
        stall.get();
        bob->append(fieldName, sampleData);
    });

    // Sample should be empty since collector is stalling.
    auto sample = collect(collector);
    ASSERT_EQ(sample.begin(), sample.end());

    stall.set();

    // After the stall, we should be able to get the data.
    auto nextSample = collect(collector);
    auto subObj = nextSample.getObjectField(collectorName);
    ASSERT_TRUE(subObj.hasField(fieldName));
    ASSERT_TRUE(subObj.hasField(kFTDCCollectStartField));
    ASSERT_TRUE(subObj.hasField(kFTDCCollectEndField));
    ASSERT_EQ(subObj.getField(fieldName).Int(), sampleData);
}

TEST_F(SampleCollectorCacheTestFixture, TimeoutShouldNotAffectOtherSamples) {
    auto timeout = Milliseconds(100);
    auto role = ClusterRole::None;
    SampleCollectorCache collector(role, timeout, 1 /*minThreads*/, 2 /*maxThreads*/);

    collector.addCollector(collectorName, true, role, [&](OperationContext*, BSONObjBuilder* bob) {
        bob->append(fieldName, sampleData);
    });

    Notification<void> stall;
    collector.addCollector(
        anotherCollectorName, true, role, [&](OperationContext*, BSONObjBuilder* bob) {
            stall.get();
            bob->append(anotherFieldName, moreSampleData);
        });

    // Our goal is to check that a collection can start and finish successfully while another
    // collection is currently stalling. We need to run two samples here because there is no
    // guarantee about which collector runs first. By running a second, we guarantee that the slow
    // collector is already running when we want to start the fast collector.
    auto sample1 = collect(collector);
    ASSERT_TRUE(sample1.hasField(collectorName));
    ASSERT_FALSE(sample1.hasField(anotherCollectorName));
    auto sample2 = collect(collector);
    ASSERT_TRUE(sample2.hasField(collectorName));
    ASSERT_FALSE(sample2.hasField(anotherCollectorName));

    stall.set();

    // After the stall ends, collection should behave normally.
    auto sample3 = collect(collector);
    ASSERT_TRUE(sample3.hasField(collectorName));
    ASSERT_TRUE(sample3.hasField(anotherCollectorName));
}

TEST_F(SampleCollectorCacheTestFixture, ShutdownDrainsWork) {
    bool finished = false;
    auto timeout = Milliseconds(100);
    auto role = ClusterRole::None;
    {
        SampleCollectorCache collector(role, timeout, 1 /*minThreads*/, 1 /*maxThreads*/);

        collector.addCollector(
            collectorName, true, role, [&](OperationContext*, BSONObjBuilder* bob) {
                sleepmillis(5 * timeout.count());
                finished = true;
            });
        collect(collector);
    }

    ASSERT_TRUE(finished);
}

}  // namespace
}  // namespace mongo
