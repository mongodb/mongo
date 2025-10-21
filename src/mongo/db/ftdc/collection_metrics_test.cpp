/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/ftdc/collection_metrics.h"

#include "mongo/db/ftdc/collector.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class FTDCCollectionMetricsFixture : public ClockSourceMockServiceContextTest {
public:
    static constexpr Milliseconds kCollectionPeriod{1000};

    class DummyCollector : public FTDCCollectorInterface {
    public:
        explicit DummyCollector(ServiceContext* svcCtx) {
            _clockSource = dynamic_cast<ClockSourceMock*>(svcCtx->getPreciseClockSource());
            ASSERT(_clockSource) << "DummyCollector requires using the mocked clock source!";
        }

        std::string name() const override {
            return "dummyCollector";
        }

        void collect(OperationContext*, BSONObjBuilder&) override {
            _clockSource->advance(_collectionOverhead);
        }

        void setCollectionOverhead(Milliseconds overhead) {
            _collectionOverhead = overhead;
        }

    private:
        ClockSourceMock* _clockSource;
        Milliseconds _collectionOverhead{0};
    };

    using ClockSourceMockServiceContextTest::ClockSourceMockServiceContextTest;

    void setUp() override {
        ClockSourceMockServiceContextTest::setUp();

        collectionMetrics().reset_forTest();
        collectionMetrics().onUpdatingCollectionPeriod(kCollectionPeriod);

        auto dummyCollector = std::make_unique<DummyCollector>(getServiceContext());
        _collector = dummyCollector.get();

        // The following will release any collector from the previous run.
        _collectorCollection = std::make_unique<SyncFTDCCollectorCollection>();
        _collectorCollection->add(std::move(dummyCollector), getService()->role());
    }

    void tearDown() override {
        // Other tests in the same build target may depend on what `ClockSourceMock` returns without
        // initializing it, so we cleanup after each test to make sure no such state is leaked.
        dynamic_cast<ClockSourceMock*>(getServiceContext()->getPreciseClockSource())->reset();
    }

    void setSampleCollectionTime(Milliseconds sct) {
        invariant(_collector);
        _collector->setCollectionOverhead(sct);
    }

    void collect() {
        auto client = getService()->makeClient("collectionClient");
        auto result = _collectorCollection->collect(client.get(), UseMultiServiceSchema{false});
        LOGV2(11113101, "Collected FTDC sample", "obj"_attr = std::get<0>(result));
    }

    FTDCCollectionMetrics& collectionMetrics() {
        return FTDCCollectionMetrics::get(getServiceContext());
    }

    void verifyMetrics(long long count, long long durationMicros, long long stalled) {
        const auto expected =
            BSON(FTDCCollectionMetrics::kCollectionsCountTag
                 << count << FTDCCollectionMetrics::kCollectionsDurationTag << durationMicros
                 << FTDCCollectionMetrics::kCollectionsStalledTag << stalled);
        const auto observed = collectionMetrics().report();
        ASSERT_BSONOBJ_EQ(observed, expected);
    }

private:
    std::unique_ptr<FTDCCollectorCollection> _collectorCollection;
    DummyCollector* _collector = nullptr;
};

TEST_F(FTDCCollectionMetricsFixture, ZeroInitialization) {
    verifyMetrics(0, 0, 0);
}

TEST_F(FTDCCollectionMetricsFixture, NormalCollection) {
    const auto kDuration = Milliseconds{100};
    ASSERT_LT(kDuration, kCollectionPeriod);
    setSampleCollectionTime(kDuration);
    collect();
    verifyMetrics(1, durationCount<Microseconds>(kDuration), 0);
}

TEST_F(FTDCCollectionMetricsFixture, StalledCollection) {
    const auto kDuration = Seconds{5};
    ASSERT_GT(kDuration, kCollectionPeriod);
    setSampleCollectionTime(kDuration);
    collect();
    verifyMetrics(1, durationCount<Microseconds>(kDuration), 1);
}

}  // namespace
}  // namespace mongo
