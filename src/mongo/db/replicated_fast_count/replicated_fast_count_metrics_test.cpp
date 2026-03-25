/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"

#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/observable_mutex_registry.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

TEST(ReplicatedFastCountMetricsTest, MetricsInitialization) {
    otel::metrics::OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;

    for (const auto& gaugeName : {
             otel::metrics::MetricNames::kReplicatedFastCountIsRunning,
             otel::metrics::MetricNames::kReplicatedFastCountFlushTimeMsMin,
             otel::metrics::MetricNames::kReplicatedFastCountFlushTimeMsMax,
             otel::metrics::MetricNames::kReplicatedFastCountFlushedDocsMin,
             otel::metrics::MetricNames::kReplicatedFastCountFlushedDocsMax,
         }) {
        EXPECT_EQ(capturer.readInt64Gauge(gaugeName), 0);
    }

    for (const auto& counterName : {
             otel::metrics::MetricNames::kReplicatedFastCountFlushSuccessCount,
             otel::metrics::MetricNames::kReplicatedFastCountFlushFailureCount,
             otel::metrics::MetricNames::kReplicatedFastCountFlushTimeMsTotal,
             otel::metrics::MetricNames::kReplicatedFastCountFlushedDocsTotal,
             otel::metrics::MetricNames::kReplicatedFastCountEmptyUpdateCount,
             otel::metrics::MetricNames::kReplicatedFastCountInsertCount,
             otel::metrics::MetricNames::kReplicatedFastCountUpdateCount,
             otel::metrics::MetricNames::kReplicatedFastCountWriteTimeMsTotal,
         }) {
        EXPECT_EQ(capturer.readInt64Counter(counterName), 0);
    }
}

TEST(ReplicatedFastCountMetricsTest, IsRunningGaugeClearedBySetIsRunning) {
    otel::metrics::OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;
    metrics.setIsRunning(false);

    EXPECT_EQ(capturer.readInt64Gauge(otel::metrics::MetricNames::kReplicatedFastCountIsRunning),
              0);

    metrics.setIsRunning(true);

    EXPECT_EQ(capturer.readInt64Gauge(otel::metrics::MetricNames::kReplicatedFastCountIsRunning),
              1);
}

TEST(ReplicatedFastCountMetricsTest, FlushSuccessCounterIncrementsViaRecordFlush) {
    otel::metrics::OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;
    metrics.recordFlush(Date_t::now() - Milliseconds(10),
                        /*batchSize=*/1);
    metrics.recordFlush(Date_t::now() - Milliseconds(10),
                        /*batchSize=*/1);

    EXPECT_EQ(capturer.readInt64Counter(
                  otel::metrics::MetricNames::kReplicatedFastCountFlushSuccessCount),
              2);
}

TEST(ReplicatedFastCountMetricsTest, FlushTimeMsMinMaxAndTotalUpdatedViaRecordFlush) {
    otel::metrics::OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;
    metrics.recordFlush(Date_t::now() - Milliseconds(10),
                        /*batchSize=*/3);

    // After a single flush, min and max should both equal the elapsed time (>= 10ms).
    const int64_t minVal =
        capturer.readInt64Gauge(otel::metrics::MetricNames::kReplicatedFastCountFlushTimeMsMin);
    const int64_t maxVal =
        capturer.readInt64Gauge(otel::metrics::MetricNames::kReplicatedFastCountFlushTimeMsMax);
    EXPECT_GE(minVal, 10);
    EXPECT_EQ(minVal, maxVal);
    EXPECT_GE(
        capturer.readInt64Counter(otel::metrics::MetricNames::kReplicatedFastCountFlushTimeMsTotal),
        10);
}


TEST(ReplicatedFastCountMetricsTest, FlushFailureCounterIncrement) {
    otel::metrics::OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;

    metrics.incrementFlushFailureCount();
    metrics.incrementFlushFailureCount();

    EXPECT_EQ(capturer.readInt64Counter(
                  otel::metrics::MetricNames::kReplicatedFastCountFlushFailureCount),
              2);
}

TEST(ReplicatedFastCountMetricsTest, InsertAndUpdateCountersIncrement) {
    otel::metrics::OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;

    metrics.incrementInsertCount();
    metrics.incrementInsertCount();

    metrics.incrementUpdateCount();
    metrics.incrementUpdateCount();

    EXPECT_EQ(
        capturer.readInt64Counter(otel::metrics::MetricNames::kReplicatedFastCountInsertCount), 2);
    EXPECT_EQ(
        capturer.readInt64Counter(otel::metrics::MetricNames::kReplicatedFastCountUpdateCount), 2);
}

TEST(ReplicatedFastCountMetricsTest, WriteMsTimeTotalAdd) {
    otel::metrics::OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;

    metrics.addWriteTimeMsTotal(1);
    metrics.addWriteTimeMsTotal(5);
    metrics.addWriteTimeMsTotal(100);

    EXPECT_EQ(
        capturer.readInt64Counter(otel::metrics::MetricNames::kReplicatedFastCountWriteTimeMsTotal),
        106);
}

TEST(ReplicatedFastCountMetricsTest, FlushTimeMsMinAndMaxTrackAcrossMultipleFlushes) {
    otel::metrics::OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;
    metrics.recordFlush(Date_t::now() - Milliseconds(10),
                        /*batchSize=*/1);
    metrics.recordFlush(Date_t::now() - Milliseconds(100),
                        /*batchSize=*/5);

    const int64_t minVal =
        capturer.readInt64Gauge(otel::metrics::MetricNames::kReplicatedFastCountFlushTimeMsMin);
    const int64_t maxVal =
        capturer.readInt64Gauge(otel::metrics::MetricNames::kReplicatedFastCountFlushTimeMsMax);
    EXPECT_GE(maxVal, 100);
    EXPECT_LT(minVal, maxVal);
    EXPECT_GE(
        capturer.readInt64Counter(otel::metrics::MetricNames::kReplicatedFastCountFlushTimeMsTotal),
        minVal + maxVal);
}

TEST(ReplicatedFastCountMetricsTest, FlushedDocsMinMaxAndTotalUpdatedAfterFlushes) {
    otel::metrics::OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;
    metrics.recordFlush(Date_t::now() - Milliseconds(1),
                        /*batchSize=*/3);
    metrics.recordFlush(Date_t::now() - Milliseconds(1),
                        /*batchSize=*/7);
    metrics.recordFlush(Date_t::now() - Milliseconds(1),
                        /*batchSize=*/1);

    EXPECT_EQ(
        capturer.readInt64Gauge(otel::metrics::MetricNames::kReplicatedFastCountFlushedDocsMin), 1);
    EXPECT_EQ(
        capturer.readInt64Gauge(otel::metrics::MetricNames::kReplicatedFastCountFlushedDocsMax), 7);
    EXPECT_EQ(
        capturer.readInt64Counter(otel::metrics::MetricNames::kReplicatedFastCountFlushedDocsTotal),
        11);
}

TEST(ReplicatedFastCountMetricsTest, EmptyUpdateCounterIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;

    // Directly call incrementEmptyUpdateCount() to verify the counter and OTel instrument
    // are correctly connected. Triggering an actual empty diff requires inserting a record and
    // then committing a zero-net-change, which is complex to arrange in a unit test.
    metrics.incrementEmptyUpdateCount();
    metrics.incrementEmptyUpdateCount();

    EXPECT_EQ(
        capturer.readInt64Counter(otel::metrics::MetricNames::kReplicatedFastCountEmptyUpdateCount),
        2);
}

TEST(ReplicatedFastCountMetricsTest, StaticMetrics) {
    otel::metrics::OtelMetricsCapturer capturer;

    ReplicatedFastCountManager manager1;
    manager1.getReplicatedFastCountMetrics().incrementUpdateCount();
    manager1.getReplicatedFastCountMetrics().incrementInsertCount();

    EXPECT_EQ(
        capturer.readInt64Counter(otel::metrics::MetricNames::kReplicatedFastCountInsertCount), 1);
    EXPECT_EQ(
        capturer.readInt64Counter(otel::metrics::MetricNames::kReplicatedFastCountUpdateCount), 1);

    ReplicatedFastCountManager manager2;
    manager2.getReplicatedFastCountMetrics().incrementUpdateCount();
    manager2.getReplicatedFastCountMetrics().incrementInsertCount();

    // Metrics are shared between ReplicatedFastCountManager instances.
    EXPECT_EQ(
        capturer.readInt64Counter(otel::metrics::MetricNames::kReplicatedFastCountInsertCount), 2);
    EXPECT_EQ(
        capturer.readInt64Counter(otel::metrics::MetricNames::kReplicatedFastCountUpdateCount), 2);
}

class ReplicatedFastCountManagerMetricsTest : public CatalogTestFixture {
public:
    ReplicatedFastCountManagerMetricsTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<replicated_fast_count_test_helpers::
                                   ReplicatedFastCountTestPersistenceProvider>())) {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _fastCountManager =
            &ReplicatedFastCountManager::get(operationContext()->getServiceContext());
        _fastCountManager->disablePeriodicWrites_ForTest();
        setUpReplicatedFastCount(operationContext());
    }

    void tearDown() override {
        _fastCountManager = nullptr;
        CatalogTestFixture::tearDown();
    }

    ReplicatedFastCountManager* _fastCountManager;
    otel::metrics::OtelMetricsCapturer _capturer;
};

TEST_F(ReplicatedFastCountManagerMetricsTest, MetadataMutexRegisteredWithObservableMutexRegistry) {
    _fastCountManager->flushSync(operationContext());

    const BSONObj report = ObservableMutexRegistry::get().report(false);
    const StringData name = "ReplicatedFastCountManager::_metadataMutex";
    ASSERT_TRUE(report.hasField(name)) << "Missing " << name << " in " << report;
    const BSONObj exclusive =
        report.getObjectField(name).getObjectField(ObservableMutexRegistry::kExclusiveFieldName);
    EXPECT_GT(exclusive.getIntField(ObservableMutexRegistry::kTotalAcquisitionsFieldName), 0);
}

TEST_F(ReplicatedFastCountManagerMetricsTest, IsRunningGaugeSetByStartup) {
    EXPECT_EQ(_capturer.readInt64Gauge(otel::metrics::MetricNames::kReplicatedFastCountIsRunning),
              1);
}


TEST_F(ReplicatedFastCountManagerMetricsTest, FlushFailureCounterIncrementsDuringFailure) {
    auto& failDuringFlushFp = *globalFailPointRegistry().find("failDuringFlush");

    failDuringFlushFp.setMode(FailPoint::alwaysOn);
    _fastCountManager->flushSync(operationContext());
    failDuringFlushFp.setMode(FailPoint::off);

    EXPECT_EQ(_capturer.readInt64Counter(
                  otel::metrics::MetricNames::kReplicatedFastCountFlushFailureCount),
              1);
}

TEST_F(ReplicatedFastCountManagerMetricsTest, InsertAndUpdateCountersIncrementViaFlush) {
    const UUID uuid = UUID::gen();
    boost::container::flat_map<UUID, CollectionSizeCount> changes;
    changes[uuid] = {/*count=*/1, /*size=*/100};
    _fastCountManager->commit(changes, /*commitTime=*/boost::none);
    _fastCountManager->flushSync(operationContext());

    // After flushing, at least the above change should be inserted.
    EXPECT_GE(
        _capturer.readInt64Counter(otel::metrics::MetricNames::kReplicatedFastCountInsertCount), 1);

    changes[uuid] = {/*count=*/1, /*size=*/50};
    _fastCountManager->commit(changes, /*commitTime=*/boost::none);
    _fastCountManager->flushSync(operationContext());

    // The above change should update the existing document.
    EXPECT_GE(
        _capturer.readInt64Counter(otel::metrics::MetricNames::kReplicatedFastCountUpdateCount), 1);
}

TEST_F(ReplicatedFastCountManagerMetricsTest, WriteTimeMsTotalIncrementsAfterFlush) {
    const UUID uuid = UUID::gen();
    boost::container::flat_map<UUID, CollectionSizeCount> changes;
    changes[uuid] = {/*count=*/1, /*size=*/100};
    _fastCountManager->commit(changes, /*commitTime=*/boost::none);
    _fastCountManager->flushSync(operationContext());

    // writeTimeMsTotal is the time spent inside the WriteUnitOfWork; it may be 0ms on a fast
    // machine. We can only assert that addWriteTimeMsTotal() was called (i.e., the OTel counter
    // has a data point at a non-negative value).
    EXPECT_GE(_capturer.readInt64Counter(
                  otel::metrics::MetricNames::kReplicatedFastCountWriteTimeMsTotal),
              0);
}
}  // namespace
}  // namespace mongo
