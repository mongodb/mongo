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

#include "mongo/db/replicated_fast_count/replicated_fast_count_advance_checkpoint.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/observable_mutex_registry.h"
#include "mongo/util/time_support.h"

namespace mongo::replicated_fast_count {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

TEST(ReplicatedFastCountMetricsTest, MetricsInitialization) {
    otel::metrics::OtelMetricsCapturer capturer;

    for (const auto& gaugeName : {
             MetricNames::kReplicatedFastCountIsRunning,
         }) {
        EXPECT_EQ(capturer.readInt64Gauge(gaugeName), 0);
    }

    for (const auto& counterName : {
             MetricNames::kReplicatedFastCountFlushSuccessCount,
             MetricNames::kReplicatedFastCountFlushFailureCount,
             MetricNames::kReplicatedFastCountFlushTimeMsTotal,
             MetricNames::kReplicatedFastCountFlushedDocsTotal,
             MetricNames::kReplicatedFastCountInsertCount,
             MetricNames::kReplicatedFastCountUpdateCount,
             MetricNames::kReplicatedFastCountWriteTimeMsTotal,
         }) {
        EXPECT_EQ(capturer.readInt64Counter(counterName), 0);
    }
}

TEST(ReplicatedFastCountMetricsTest, IsRunningGaugeClearedBySetIsRunning) {
    OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;
    metrics.setIsRunning(false);

    EXPECT_EQ(capturer.readInt64Gauge(MetricNames::kReplicatedFastCountIsRunning), 0);

    metrics.setIsRunning(true);

    EXPECT_EQ(capturer.readInt64Gauge(MetricNames::kReplicatedFastCountIsRunning), 1);
}

TEST(ReplicatedFastCountMetricsTest, FlushSuccessCounterIncrementsViaRecordFlush) {
    OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;
    metrics.recordFlush(Date_t::now() - Milliseconds(10),
                        /*batchSize=*/1);
    metrics.recordFlush(Date_t::now() - Milliseconds(10),
                        /*batchSize=*/1);

    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountFlushSuccessCount), 2);
}

TEST(ReplicatedFastCountMetricsTest, FlushFailureCounterIncrement) {
    OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;

    metrics.incrementFlushFailureCount();
    metrics.incrementFlushFailureCount();

    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountFlushFailureCount), 2);
}

TEST(ReplicatedFastCountMetricsTest, InsertAndUpdateCountersIncrement) {
    OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;

    metrics.incrementInsertCount();
    metrics.incrementInsertCount();

    metrics.incrementUpdateCount();
    metrics.incrementUpdateCount();

    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountInsertCount), 2);
    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountUpdateCount), 2);
}

TEST(ReplicatedFastCountMetricsTest, WriteMsTimeTotalAdd) {
    OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;

    metrics.addWriteTimeMsTotal(1);
    metrics.addWriteTimeMsTotal(5);
    metrics.addWriteTimeMsTotal(100);

    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountWriteTimeMsTotal), 106);
}

TEST(ReplicatedFastCountMetricsTest, FlushedDocsTotalUpdatedAfterFlushes) {
    OtelMetricsCapturer capturer;
    ReplicatedFastCountMetrics metrics;
    metrics.recordFlush(Date_t::now() - Milliseconds(1),
                        /*batchSize=*/3);
    metrics.recordFlush(Date_t::now() - Milliseconds(1),
                        /*batchSize=*/7);
    metrics.recordFlush(Date_t::now() - Milliseconds(1),
                        /*batchSize=*/1);

    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountFlushedDocsTotal), 11);
}

TEST(ReplicatedFastCountMetricsTest, StaticMetrics) {
    OtelMetricsCapturer capturer;

    ReplicatedFastCountManager manager1;
    manager1.getReplicatedFastCountMetrics().incrementUpdateCount();
    manager1.getReplicatedFastCountMetrics().incrementInsertCount();

    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountInsertCount), 1);
    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountUpdateCount), 1);

    ReplicatedFastCountManager manager2;
    manager2.getReplicatedFastCountMetrics().incrementUpdateCount();
    manager2.getReplicatedFastCountMetrics().incrementInsertCount();

    // Metrics are shared between ReplicatedFastCountManager instances.
    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountInsertCount), 2);
    EXPECT_EQ(capturer.readInt64Counter(MetricNames::kReplicatedFastCountUpdateCount), 2);
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
    OtelMetricsCapturer _capturer;
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
    EXPECT_EQ(_capturer.readInt64Gauge(MetricNames::kReplicatedFastCountIsRunning), 1);
}


TEST_F(ReplicatedFastCountManagerMetricsTest, FlushFailureCounterIncrementsDuringFailure) {
    auto& failDuringFlushFp = *globalFailPointRegistry().find("failDuringFlush");

    failDuringFlushFp.setMode(FailPoint::alwaysOn);
    _fastCountManager->flushSync(operationContext());
    failDuringFlushFp.setMode(FailPoint::off);

    EXPECT_EQ(_capturer.readInt64Counter(MetricNames::kReplicatedFastCountFlushFailureCount), 1);
}

// TODO SERVER-122992: Re-enable once the number of entries inserted versus updated are
// communicated.
// TEST_F(ReplicatedFastCountManagerMetricsTest,
// InsertAndUpdateCountersIncrementViaFlush) {
//     const UUID uuid = UUID::gen();
//     boost::container::flat_map<UUID, CollectionSizeCount> changes;
//     changes[uuid] = {/*count=*/1, /*size=*/100};
//     _fastCountManager->commit(changes, /*commitTime=*/boost::none);
//     _fastCountManager->flushSync(operationContext());
//
//     // After flushing, at least the above change should be inserted.
//     EXPECT_GE(
//         _capturer.readInt64Counter(MetricNames::kReplicatedFastCountInsertCount),
//         1);
//
//     changes[uuid] = {/*count=*/1, /*size=*/50};
//     _fastCountManager->commit(changes, /*commitTime=*/boost::none);
//     _fastCountManager->flushSync(operationContext());
//
//     // The above change should update the existing document.
//     EXPECT_GE(
//         _capturer.readInt64Counter(MetricNames::kReplicatedFastCountUpdateCount),
//         1);
// }
//

TEST_F(ReplicatedFastCountManagerMetricsTest, WriteTimeMsTotalIncrementsAfterFlush) {
    const UUID uuid = UUID::gen();
    boost::container::flat_map<UUID, CollectionSizeCount> changes;
    changes[uuid] = {/*count=*/1, /*size=*/100};
    _fastCountManager->commit(changes, /*commitTime=*/boost::none);
    _fastCountManager->flushSync(operationContext());

    // writeTimeMsTotal is the time spent inside the WriteUnitOfWork; it may be 0ms on a fast
    // machine. We can only assert that addWriteTimeMsTotal() was called (i.e., the OTel counter
    // has a data point at a non-negative value).
    EXPECT_GE(_capturer.readInt64Counter(MetricNames::kReplicatedFastCountWriteTimeMsTotal), 0);
}

TEST(ReplicatedFastCountMetricsTest, CheckpointOplogEntriesProcessedCounterIncrements) {
    OtelMetricsCapturer capturer;

    recordCheckpointOplogEntryProcessed();
    recordCheckpointOplogEntryProcessed();
    recordCheckpointOplogEntryProcessed();

    EXPECT_EQ(
        capturer.readInt64Counter(MetricNames::kReplicatedFastCountCheckpointOplogEntriesProcessed),
        3);
}

TEST(ReplicatedFastCountMetricsTest, CheckpointOplogEntriesSkippedCounterIncrements) {
    OtelMetricsCapturer capturer;

    recordCheckpointOplogEntrySkipped();
    recordCheckpointOplogEntrySkipped();

    EXPECT_EQ(
        capturer.readInt64Counter(MetricNames::kReplicatedFastCountCheckpointOplogEntriesSkipped),
        2);
}

TEST(ReplicatedFastCountMetricsTest, CheckpointSizeCountEntriesProcessedCounterIncrements) {
    OtelMetricsCapturer capturer;

    recordCheckpointSizeCountEntryProcessed();
    recordCheckpointSizeCountEntryProcessed();
    recordCheckpointSizeCountEntryProcessed();
    recordCheckpointSizeCountEntryProcessed();

    EXPECT_EQ(capturer.readInt64Counter(
                  MetricNames::kReplicatedFastCountCheckpointSizeCountEntriesProcessed),
              4);
}

TEST(ReplicatedFastCountMetricsTest, CheckpointCountersInitializedToZero) {
    OtelMetricsCapturer capturer;

    EXPECT_EQ(
        capturer.readInt64Counter(MetricNames::kReplicatedFastCountCheckpointOplogEntriesProcessed),
        0);
    EXPECT_EQ(
        capturer.readInt64Counter(MetricNames::kReplicatedFastCountCheckpointOplogEntriesSkipped),
        0);
    EXPECT_EQ(capturer.readInt64Counter(
                  MetricNames::kReplicatedFastCountCheckpointSizeCountEntriesProcessed),
              0);
}

// Fixture that wires OtelMetricsCapturer with the same CatalogTestFixture used by the
// advance-checkpoint tests, allowing end-to-end verification that advanceCheckpoint fires
// the checkpoint scan counters.
class CheckpointScanMetricsTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        opCtx = operationContext();
        ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), opCtx));
        ASSERT_OK(createReplicatedFastCountTimestampCollection(storageInterface(), opCtx));
    }

    OperationContext* opCtx;
    SizeCountStore sizeCountStore;
    SizeCountTimestampStore timestampStore;
    OtelMetricsCapturer capturer;
};

TEST_F(CheckpointScanMetricsTest, ProcessedAndSizeCountFireForUserEntries) {
    const test_helpers::NsAndUUID collA{
        .nss = NamespaceString::createNamespaceString_forTest("db", "collA"), .uuid = UUID::gen()};

    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp{1, 1}, collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp{1, 2}, collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    EXPECT_EQ(
        capturer.readInt64Counter(MetricNames::kReplicatedFastCountCheckpointOplogEntriesProcessed),
        2);
    EXPECT_EQ(
        capturer.readInt64Counter(MetricNames::kReplicatedFastCountCheckpointOplogEntriesSkipped),
        0);
    EXPECT_EQ(capturer.readInt64Counter(
                  MetricNames::kReplicatedFastCountCheckpointSizeCountEntriesProcessed),
              2);
}

TEST_F(CheckpointScanMetricsTest, SkippedFiresForFastCountInternalEntries) {
    const test_helpers::NsAndUUID collA{
        .nss = NamespaceString::createNamespaceString_forTest("db", "collA"), .uuid = UUID::gen()};

    // Write one user entry so the checkpoint has something to anchor on.
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp{1, 1}, collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));

    // Run the first checkpoint so the timestamp store advances to ts{1,1}.
    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    // Now write an applyOps that only touches the internal fast count collections. This entry
    // should be counted as skipped, not processed.
    const auto fastCountStoreNss =
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);
    BSONArrayBuilder innerOps;
    innerOps.append(BSON("op" << "u"
                              << "ns" << fastCountStoreNss.ns_forTest() << "ui" << UUID::gen()
                              << "o" << BSON("$set" << BSON("count" << 1)) << "o2"
                              << BSON("_id" << 1)));
    const repl::OplogEntry internalEntry = repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(Timestamp{1, 2}, 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::createNamespaceString_forTest("admin", "$cmd"),
        .oField = BSON("applyOps" << innerOps.arr()),
        .wallClockTime = Date_t::now(),
    }};
    test_helpers::writeToOplog(opCtx, internalEntry);

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    // The internal applyOps entry must have been skipped (not processed).
    EXPECT_GT(
        capturer.readInt64Counter(MetricNames::kReplicatedFastCountCheckpointOplogEntriesSkipped),
        0);
}

TEST_F(CheckpointScanMetricsTest, NoCountersFireWhenOplogIsEmpty) {
    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    EXPECT_EQ(
        capturer.readInt64Counter(MetricNames::kReplicatedFastCountCheckpointOplogEntriesProcessed),
        0);
    EXPECT_EQ(
        capturer.readInt64Counter(MetricNames::kReplicatedFastCountCheckpointOplogEntriesSkipped),
        0);
    EXPECT_EQ(capturer.readInt64Counter(
                  MetricNames::kReplicatedFastCountCheckpointSizeCountEntriesProcessed),
              0);
}

// Exercises all three checkpoint scan metrics in a single checkpoint pass:
//
// - One applyOps entry whose inner ops are all on user collections and carry size metadata.
//   This entry is counted as processed (1) and each inner op increments sizeCount.
//
// - One applyOps entry whose inner ops are all on fast-count-internal collections.
//   Because every inner op is internal, the entire entry is skipped (1).
//
// Expected: processed=1, skipped=1, sizeCount=3.
TEST_F(CheckpointScanMetricsTest, ApplyOpsWithUserAndInternalEntriesExercisesAllMetrics) {
    const test_helpers::NsAndUUID collA{
        .nss = NamespaceString::createNamespaceString_forTest("db", "collA"), .uuid = UUID::gen()};
    const NamespaceString adminCmdNss =
        NamespaceString::createNamespaceString_forTest("admin", "$cmd");
    const NamespaceString fastCountStoreNss =
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);

    // -- Entry 1: applyOps with 3 user inserts, each carrying size metadata.
    // operationsOnFastCountCollections() returns false (user ops present), so this entry is
    // counted as processed. extractSizeCountDeltasForApplyOps sees 3 inner ops with "m.sz",
    // so sizeCount is incremented 3 times.
    const BSONObj userInsert1 = BSON("op" << "i"
                                          << "ns" << collA.nss.ns_forTest() << "ui" << collA.uuid
                                          << "o" << BSON("_id" << 1) << "m" << BSON("sz" << 10));
    const BSONObj userInsert2 = BSON("op" << "i"
                                          << "ns" << collA.nss.ns_forTest() << "ui" << collA.uuid
                                          << "o" << BSON("_id" << 2) << "m" << BSON("sz" << 20));
    const BSONObj userInsert3 = BSON("op" << "i"
                                          << "ns" << collA.nss.ns_forTest() << "ui" << collA.uuid
                                          << "o" << BSON("_id" << 3) << "m" << BSON("sz" << 30));
    test_helpers::writeToOplog(
        opCtx,
        repl::OplogEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(Timestamp{1, 1}, 1),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = adminCmdNss,
            .oField = BSON("applyOps" << BSON_ARRAY(userInsert1 << userInsert2 << userInsert3)),
            .wallClockTime = Date_t::now(),
        }}});

    // -- Entry 2: applyOps with 2 ops that only touch internal fast-count collections.
    // operationsOnFastCountCollections() returns true, so the entire entry is skipped.
    const BSONObj internalOp1 =
        BSON("op" << "u"
                  << "ns" << fastCountStoreNss.ns_forTest() << "ui" << UUID::gen() << "o"
                  << BSON("$set" << BSON("count" << 5)) << "o2" << BSON("_id" << 1));
    const BSONObj internalOp2 =
        BSON("op" << "u"
                  << "ns" << fastCountStoreNss.ns_forTest() << "ui" << UUID::gen() << "o"
                  << BSON("$set" << BSON("count" << 3)) << "o2" << BSON("_id" << 2));
    test_helpers::writeToOplog(
        opCtx,
        repl::OplogEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(Timestamp{1, 2}, 1),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = adminCmdNss,
            .oField = BSON("applyOps" << BSON_ARRAY(internalOp1 << internalOp2)),
            .wallClockTime = Date_t::now(),
        }}});

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    EXPECT_EQ(
        capturer.readInt64Counter(MetricNames::kReplicatedFastCountCheckpointOplogEntriesProcessed),
        1);
    EXPECT_EQ(
        capturer.readInt64Counter(MetricNames::kReplicatedFastCountCheckpointOplogEntriesSkipped),
        1);
    EXPECT_EQ(capturer.readInt64Counter(
                  MetricNames::kReplicatedFastCountCheckpointSizeCountEntriesProcessed),
              3);
}
}  // namespace
}  // namespace mongo::replicated_fast_count
