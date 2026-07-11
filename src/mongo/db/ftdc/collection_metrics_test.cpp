// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
        _collectorCollection->add(std::move(dummyCollector));
    }

    void setSampleCollectionTime(Milliseconds sct) {
        invariant(_collector);
        _collector->setCollectionOverhead(sct);
    }

    void collect() {
        auto client = getService()->makeClient("collectionClient");
        std::vector<std::pair<std::string, int>> sectionSizes;
        auto result = _collectorCollection->collect(client.get(), sectionSizes);
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
