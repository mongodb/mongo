// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/internode_validation_hash_utils.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier_dispatch.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/pipelined_applier_otel_metrics.h"
#include "mongo/db/repl/pipelined_oplog_applier.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_capture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/scopeguard.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace mongo::repl {
namespace {

using replicated_fast_count::SizeCountCheckpointValidator;
using replicated_fast_count::test_helpers::makeCheckpointEntry;
using replicated_fast_count::test_helpers::makeRepairNoopEntry;
using replicated_fast_count::test_helpers::makeWatermarkEntry;
using InflightBatch = PipelinedApplierBatchTracker::InflightBatch;
using WorkItem = PipelinedApplierWorkerPool::WorkItem;

// Exercises batch validation with real worker application and FIFO completion.
class PipelinedCheckpointValidationTest : public OplogApplierImplTest {
protected:
    Options makeServiceContextOptions() override {
        return OplogApplierImplTest::makeServiceContextOptions().setPersistenceProvider(
            std::make_unique<replicated_fast_count::test_helpers::
                                 ContinuousInternodeValidationTestPersistenceProvider>());
    }

    void setUp() override {
        OplogApplierImplTest::setUp();
        PipelinedApplyOtelMetrics::initialize();
        _stores = replicated_fast_count::test_helpers::createContainerFastCountStores(_opCtx.get());
        // Keep catalog setup untimestamped before applying timestamped oplog entries.
        {
            UnreplicatedWritesBlock noRep(_opCtx.get());
            createCollection(_opCtx.get(), kNss, {});
        }
        _uuid = getCollectionUUID(_opCtx.get(), kNss);
        _oplogUuid = getOplogUuid(_opCtx.get());
        _pool = std::make_unique<PipelinedApplierWorkerPool>(
            workerCount(), [this](size_t workerIdx, const WorkItem& item) {
                consumeWorkItem(workerIdx, _options, item, _tracker);
                // Recording after item consumption must not retain the previous slot.
                SizeCountCheckpointValidator::recordDelta(_uuid, Timestamp(999, 1), {});
                if (_afterConsume) {
                    _afterConsume(item);
                }
            });

        // Establish the baseline with a drained batch before testing pipelined validation.
        // TODO SERVER-139269: Add adoption, reset, and restart coverage.
        const auto watermark = makeWatermarkEntry(nextOpTime().getTimestamp());
        auto checkpoint = checkpointEntry(watermark.getTimestamp(), {0, 0}, 0);
        applyAndValidate({watermark, checkpoint});
    }

    void tearDown() override {
        _pool.reset();
        _stores = {};
        OplogApplierImplTest::tearDown();
    }

    virtual size_t workerCount() const {
        return 2;
    }

    // Creates a hash-bearing insert with replicated record ID and size metadata.
    OplogEntry insert(int id) {
        const auto doc = BSON("_id" << id << "x" << id);
        return makeInsertOplogEntryWithRecordIdSizeMetadataAndHash(
            nextOpTime(), kNss, _uuid, doc, RecordId(id), computeDocValidationHash(doc));
    }

    // Finds a document routed to the specified worker.
    OplogEntry insertOnWorker(size_t worker) {
        CachedCollectionProperties cache;
        for (int id = 1;; ++id) {
            auto op = insert(id);
            std::vector<std::vector<ApplierOperation>> writers(workerCount());
            if (OplogApplierUtils::addToWriterVector(_opCtx.get(), &op, &writers, &cache) ==
                worker) {
                return op;
            }
        }
    }

    // Gives a checkpoint its own oplog timestamp after the watermark.
    OplogEntry checkpointEntry(Timestamp watermark,
                               CollectionSizeCount sizeCount,
                               boost::optional<int64_t> hash,
                               boost::optional<UUID> uuid = boost::none) {
        const auto checkpoint = makeCheckpointEntry(
            watermark, {{uuid.value_or(_uuid), CollectionReplicatedMetadata{sizeCount, hash}}});
        BSONObjBuilder builder;
        builder.append("ts", nextOpTime().getTimestamp());
        builder.appendElementsUnique(checkpoint.getEntry().toBSON());
        return OplogEntry(builder.obj());
    }

    // Registers the original batch before handing its slices to persistent workers.
    std::shared_ptr<std::atomic<uint32_t>> dispatch(std::vector<OplogEntry> ops,
                                                    bool validation = true) {
        InflightBatch batch;
        batch.lastOpTime = {ops.back().getOpTime(), ops.back().getWallClockTime()};
        batch.ops = std::move(ops);
        if (validation) {
            preparePipelinedBatchValidation(_opCtx.get(), _options, batch, workerCount());
            ASSERT_TRUE(batch.validatorBuffers);
        }
        std::vector<std::vector<ApplierOperation>> writers(workerCount());
        fillWriterVectors(_options,
                          false,
                          _opCtx.get(),
                          &batch.ops,
                          &writers,
                          &batch.derivedOps,
                          batch.validatorBuffers ? &_validator : nullptr);
        batch.multikeyMetadata =
            std::make_shared<std::vector<WorkerMultikeyPathInfo>>(workerCount());
        std::vector<WorkItem> items(workerCount());
        uint32_t participating = 0;
        for (size_t worker = 0; worker < workerCount(); ++worker) {
            if (writers[worker].empty()) {
                continue;
            }
            ++participating;
            items[worker].ops = std::move(writers[worker]);
            items[worker].multikeyPathInfo = &(*batch.multikeyMetadata)[worker];
            if (batch.validatorBuffers) {
                items[worker].validatorBuffer = &(*batch.validatorBuffers)[worker];
            }
        }
        auto counter = _tracker.addBatch(std::move(batch), participating);
        ASSERT_TRUE(counter);
        for (size_t worker = 0; worker < workerCount(); ++worker) {
            if (!items[worker].ops.empty()) {
                items[worker].batchRemainingWorkers = counter;
                _pool->enqueue(worker, std::move(items[worker]));
            }
        }
        return counter;
    }

    // Applies a batch and returns its results while publication still owns the batch.
    InflightBatch applyAndPop(std::vector<OplogEntry> ops, bool validation = true) {
        dispatch(std::move(ops), validation);
        _pool->waitForIdle();
        auto batch = _tracker.popCompletedBatch(Milliseconds(0));
        ASSERT_TRUE(batch);
        return std::move(*batch);
    }

    // Validates a completed batch and finishes its publication barrier.
    void validateAndPublish(const InflightBatch& batch) {
        // TODO SERVER-139269: Test validation ordering through the run loop.
        validatePipelinedBatch(_opCtx.get(), batch, _validator, _oplogUuid);
        shard_role_details::getRecoveryUnit(_opCtx.get())->abandonSnapshot();
        _tracker.onBatchPublished();
    }

    void applyAndValidate(std::vector<OplogEntry> ops) {
        auto batch = applyAndPop(std::move(ops));
        validateAndPublish(batch);
    }

    // Replays equivalent data through POA and classic with different batch boundaries.
    void verifyReplayAgainstClassic(bool splitPipelinedBatches) {
        const auto makeStream = [&](const NamespaceString& nss, UUID uuid) {
            std::vector<OplogEntry> ops;
            int64_t size = 0;
            int64_t hash = 0;
            for (int id = 1; id <= 2; ++id) {
                const auto doc = BSON("_id" << id << "x" << id);
                size += doc.objsize();
                hash ^= computeDocValidationHash(doc);
                ops.push_back(makeInsertOplogEntryWithRecordIdSizeMetadataAndHash(
                    nextOpTime(), nss, uuid, doc, RecordId(id), computeDocValidationHash(doc)));
                const auto watermark = makeWatermarkEntry(nextOpTime().getTimestamp());
                ops.push_back(watermark);
                ops.push_back(checkpointEntry(watermark.getTimestamp(), {size, id}, hash, uuid));
            }
            return ops;
        };
        auto pipelined = makeStream(kNss, _uuid);
        {
            otel::metrics::OtelMetricsCapturer metrics;
            if (splitPipelinedBatches) {
                for (const auto& op : pipelined) {
                    dispatch({op});
                }
                _pool->waitForIdle();
                for (size_t i = 0; i < pipelined.size(); ++i) {
                    auto batch = _tracker.popCompletedBatch(Milliseconds(0));
                    ASSERT_TRUE(batch);
                    validateAndPublish(*batch);
                }
            } else {
                applyAndValidate(pipelined);
            }
            replicated_fast_count::test_helpers::expectValidatorMetrics(
                metrics, {.recorded = 2, .evicted = 2, .skipped = 0, .validated = 2});
        }

        const auto classicNss =
            NamespaceString::createNamespaceString_forTest("test.classic_validation");
        {
            UnreplicatedWritesBlock noRep(_opCtx.get());
            createCollection(_opCtx.get(), classicNss, {});
        }
        const auto classicUuid = getCollectionUUID(_opCtx.get(), classicNss);
        auto pool = makeReplWorkerPool(workerCount());
        OplogApplierImpl classic(nullptr,
                                 nullptr,
                                 &noopOplogApplierObserver,
                                 ReplicationCoordinator::get(_opCtx.get()),
                                 getConsistencyMarkers(),
                                 getStorageInterface(),
                                 _options,
                                 pool.get());
        const auto applyClassic = [&](std::vector<OplogEntry> ops) {
            auto result = classic.applyOplogBatch(_opCtx.get(), std::move(ops));
            ASSERT_OK(result.getStatus());
            getStorageInterface()->oplogDiskLocRegister(
                _opCtx.get(), result.getValue().getTimestamp(), /*orderedCommit=*/true);
        };
        const auto watermark = makeWatermarkEntry(nextOpTime().getTimestamp());
        applyClassic(
            {watermark, checkpointEntry(watermark.getTimestamp(), {0, 0}, 0, classicUuid)});
        const auto classicOps = makeStream(classicNss, classicUuid);
        otel::metrics::OtelMetricsCapturer metrics;
        if (splitPipelinedBatches) {
            applyClassic(classicOps);
        } else {
            for (const auto& op : classicOps) {
                applyClassic({op});
            }
        }
        replicated_fast_count::test_helpers::expectValidatorMetrics(
            metrics, {.recorded = 2, .evicted = 2, .skipped = 0, .validated = 2});
    }

    // Checks the committed contributions rather than the number of apply attempts.
    void assertInsertRecords(const InflightBatch& batch, const std::vector<OplogEntry>& inserts) {
        ASSERT_TRUE(batch.validatorBuffers);
        size_t count = 0;
        std::set<Timestamp> observed;
        for (const auto& slot : *batch.validatorBuffers) {
            for (const auto& record : slot) {
                ++count;
                ASSERT_TRUE(observed.insert(record.ts).second);
                const auto it = std::find_if(inserts.begin(), inserts.end(), [&](const auto& op) {
                    return record.ts == op.getTimestamp();
                });
                ASSERT_TRUE(it != inserts.end());
                ASSERT_EQ(record.uuid, _uuid);
                ASSERT_EQ(record.delta.metadata.sizeCount,
                          (CollectionSizeCount{it->getObject().objsize(), 1}));
                ASSERT_EQ(record.delta.metadata.hash, computeDocValidationHash(it->getObject()));
            }
        }
        ASSERT_EQ(count, inserts.size());
    }

    const NamespaceString kNss =
        NamespaceString::createNamespaceString_forTest("test.poa_validation");
    UUID _uuid = UUID::gen();
    UUID _oplogUuid = UUID::gen();
    OplogApplier::Options _options{OplogApplication::Mode::kSecondary};
    SizeCountCheckpointValidator _validator;
    PipelinedApplierBatchTracker _tracker;
    std::unique_ptr<PipelinedApplierWorkerPool> _pool;
    std::function<void(const WorkItem&)> _afterConsume;
    replicated_fast_count::test_helpers::ContainerFastCountStores _stores;

private:
    unittest::ServerParameterGuard _recordIdsFlag{"featureFlagRecordIdsReplicated", true};
    unittest::ServerParameterGuard _fastCountFlag{"featureFlagReplicatedFastCount", true};
    unittest::ServerParameterGuard _containerWritesFlag{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard _perDocumentFlag{
        "featureFlagContinuousInternodeValidationPerDocument", true};
    unittest::ServerParameterGuard _fassert{
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true};
};

class SingleWorkerPipelinedCheckpointValidationTest : public PipelinedCheckpointValidationTest {
protected:
    size_t workerCount() const override {
        return 1;
    }
};

TEST_F(SingleWorkerPipelinedCheckpointValidationTest,
       PersistentWorkerKeepsSuccessiveBuffersIsolated) {
    const auto first = insert(1);
    const auto second = insert(2);
    dispatch({first});
    dispatch({second});
    _pool->waitForIdle();

    auto batch = _tracker.popCompletedBatch(Milliseconds(0));
    ASSERT_TRUE(batch);
    assertInsertRecords(*batch, {first});
    validateAndPublish(*batch);
    auto later = _tracker.popCompletedBatch(Milliseconds(0));
    ASSERT_TRUE(later);
    assertInsertRecords(*later, {second});
    ASSERT_NE(&(*batch->validatorBuffers)[0], &(*later->validatorBuffers)[0]);
    validateAndPublish(*later);

    const auto watermark = makeWatermarkEntry(nextOpTime().getTimestamp());
    applyAndValidate(
        {watermark,
         checkpointEntry(watermark.getTimestamp(),
                         {first.getObject().objsize() + second.getObject().objsize(), 2},
                         computeDocValidationHash(first.getObject()) ^
                             computeDocValidationHash(second.getObject()))});
}

TEST_F(SingleWorkerPipelinedCheckpointValidationTest, WriteConflictRetryRecordsOnlyCommittedDelta) {
    int attempts = 0;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString&, const std::vector<BSONObj>&) {
            if (++attempts == 1) {
                throwWriteConflictException("injected validation retry");
            }
        };
    const auto op = insert(1);
    auto batch = applyAndPop({op});
    ASSERT_EQ(attempts, 2);
    assertInsertRecords(batch, {op});
    validateAndPublish(batch);
}

TEST_F(SingleWorkerPipelinedCheckpointValidationTest, GroupedInsertFallbackRecordsEveryCommitOnce) {
    std::vector<size_t> attempts;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString&, const std::vector<BSONObj>& docs) {
            attempts.push_back(docs.size());
            uassert(ErrorCodes::OperationFailed, "injected grouped failure", docs.size() == 1);
        };
    const std::vector<OplogEntry> ops{insert(1), insert(2), insert(3)};
    auto batch = applyAndPop(ops);
    ASSERT_EQ(attempts, (std::vector<size_t>{3, 1, 1, 1}));
    assertInsertRecords(batch, ops);
    validateAndPublish(batch);
    const auto watermark = makeWatermarkEntry(nextOpTime().getTimestamp());
    int64_t hash = 0;
    int64_t size = 0;
    for (const auto& op : ops) {
        hash ^= computeDocValidationHash(op.getObject());
        size += op.getObject().objsize();
    }
    applyAndValidate({watermark, checkpointEntry(watermark.getTimestamp(), {size, 3}, hash)});
}

TEST_F(SingleWorkerPipelinedCheckpointValidationTest, AbandonedBatchNeverMergesOrPublishes) {
    const auto first = insert(1);
    const auto second = insert(2);
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString&, const std::vector<BSONObj>& docs) {
            uassert(ErrorCodes::OperationFailed, "force single insert fallback", docs.size() == 1);
            if (docs.front()["_id"].numberInt() == 2) {
                uasserted(ErrorCodes::InterruptedAtShutdown, "injected shutdown");
            }
        };
    // The first insert commits before the second abandons the slice, leaving partial results.
    auto abandoned = dispatch({first, second});
    auto later = dispatch({makeWatermarkEntry(nextOpTime().getTimestamp())});
    _pool->waitForIdle();
    ASSERT_EQ(abandoned->load(), 1);
    ASSERT_EQ(later->load(), 0);
    ASSERT_TRUE(_tracker.hasAbandonedBatch());
    ASSERT_FALSE(_tracker.popCompletedBatch(Milliseconds(0)));
    ASSERT_FALSE(_tracker.waitUntilIdle());
    _tracker.shutdown();
}

TEST_F(PipelinedCheckpointValidationTest, SplitBatchReplayMatchesClassicCheckpointResults) {
    verifyReplayAgainstClassic(/*splitPipelinedBatches=*/true);
}

TEST_F(PipelinedCheckpointValidationTest, CombinedBatchReplayMatchesClassicCheckpointResults) {
    verifyReplayAgainstClassic(/*splitPipelinedBatches=*/false);
}

TEST_F(SingleWorkerPipelinedCheckpointValidationTest, SplitReplayMatchesClassicWithOneWorker) {
    verifyReplayAgainstClassic(/*splitPipelinedBatches=*/true);
}

TEST_F(SingleWorkerPipelinedCheckpointValidationTest, CombinedReplayMatchesClassicWithOneWorker) {
    verifyReplayAgainstClassic(/*splitPipelinedBatches=*/false);
}

TEST_F(PipelinedCheckpointValidationTest, PreprocessingSkipsEntriesAtOrBeforeBeginApplying) {
    const auto oldWatermark = makeWatermarkEntry(nextOpTime().getTimestamp());
    const auto oldCheckpoint = checkpointEntry(oldWatermark.getTimestamp(), {0, 0}, 0);
    const auto op = insert(1);
    const auto watermark = makeWatermarkEntry(nextOpTime().getTimestamp());
    const auto checkpoint = checkpointEntry(watermark.getTimestamp(), {0, 0}, 0);
    InflightBatch batch{.ops = {oldWatermark, oldCheckpoint, op, watermark, checkpoint}};
    auto options = _options;
    options.beginApplyingOpTime = oldCheckpoint.getOpTime();
    preparePipelinedBatchValidation(_opCtx.get(), options, batch, workerCount());
    std::vector<std::vector<ApplierOperation>> writers(workerCount());
    otel::metrics::OtelMetricsCapturer metrics;
    fillWriterVectors(
        options, false, _opCtx.get(), &batch.ops, &writers, &batch.derivedOps, &_validator);
    replicated_fast_count::test_helpers::expectValidatorMetrics(
        metrics, {.recorded = 1, .evicted = 0, .skipped = 0, .validated = 0});
}

TEST_F(PipelinedCheckpointValidationTest, DisabledValidationDoesNotAllocateBuffers) {
    // Restore the atomic directly; runtime re-enablement is forbidden.
    const bool previous = disableContinuousInternodeValidationPerCollection.load();
    disableContinuousInternodeValidationPerCollection.store(true);
    ON_BLOCK_EXIT(
        [previous] { disableContinuousInternodeValidationPerCollection.store(previous); });
    InflightBatch batch{.ops = {makeWatermarkEntry(nextOpTime().getTimestamp())}};
    preparePipelinedBatchValidation(_opCtx.get(), _options, batch, workerCount());
    ASSERT_FALSE(batch.validatorBuffers);
    // Batches with no buffers skip validation, even without a completion counter.
    validatePipelinedBatch(_opCtx.get(), batch, _validator, _oplogUuid);
}

TEST_F(PipelinedCheckpointValidationTest, EarlierBatchRetainsValidationAfterGateIsDisabled) {
    const auto first = insert(1);
    dispatch({first});
    // Restore the atomic directly; runtime re-enablement is forbidden.
    const bool previous = disableContinuousInternodeValidationPerCollection.load();
    disableContinuousInternodeValidationPerCollection.store(true);
    ON_BLOCK_EXIT(
        [previous] { disableContinuousInternodeValidationPerCollection.store(previous); });
    _pool->waitForIdle();
    auto batch = _tracker.popCompletedBatch(Milliseconds(0));
    ASSERT_TRUE(batch);
    assertInsertRecords(*batch, {first});
    validateAndPublish(*batch);
    InflightBatch next{.ops = {makeWatermarkEntry(nextOpTime().getTimestamp())}};
    preparePipelinedBatchValidation(_opCtx.get(), _options, next, workerCount());
    ASSERT_FALSE(next.validatorBuffers);
}

TEST_F(PipelinedCheckpointValidationTest,
       FuturePreprocessingDefersCheckpointUntilItsWatermarkIsApplied) {
    const auto first = insert(1);
    auto batch = applyAndPop({first});
    const auto watermark = makeWatermarkEntry(nextOpTime().getTimestamp());
    const auto checkpoint = checkpointEntry(watermark.getTimestamp(),
                                            {first.getObject().objsize(), 1},
                                            computeDocValidationHash(first.getObject()));
    otel::metrics::OtelMetricsCapturer metrics;
    // The future checkpoint is recorded on dispatch but cannot validate with only this batch's
    // deltas.
    dispatch({watermark, checkpoint});
    validateAndPublish(batch);
    replicated_fast_count::test_helpers::expectValidatorMetrics(
        metrics, {.recorded = 1, .evicted = 0, .skipped = 0, .validated = 0});
    _pool->waitForIdle();
    auto later = _tracker.popCompletedBatch(Milliseconds(0));
    ASSERT_TRUE(later);
    validateAndPublish(*later);
    replicated_fast_count::test_helpers::expectValidatorMetrics(
        metrics, {.recorded = 1, .evicted = 1, .skipped = 0, .validated = 1});
}

TEST_F(PipelinedCheckpointValidationTest, RepairHashSetPrecedesEarlierBatchMerge) {
    const auto first = insert(1);
    const auto firstHash = computeDocValidationHash(first.getObject());
    const auto repairedHash = firstHash ^ 1;
    auto earlier = applyAndPop({first});
    const auto repair =
        makeRepairNoopEntry(nextOpTime().getTimestamp(), _uuid, BSON("hSet" << repairedHash));
    const auto second = insert(2);

    // The repair is preprocessed while the earlier insert's delta is still unmerged.
    dispatch({repair, second});
    validateAndPublish(earlier);
    _pool->waitForIdle();
    auto later = _tracker.popCompletedBatch(Milliseconds(0));
    ASSERT_TRUE(later);
    assertInsertRecords(*later, {second});
    validateAndPublish(*later);

    // Earlier size/count deltas survive, but their hashes must not be folded into hSet again.
    const auto watermark = makeWatermarkEntry(nextOpTime().getTimestamp());
    applyAndValidate(
        {watermark,
         checkpointEntry(watermark.getTimestamp(),
                         {first.getObject().objsize() + second.getObject().objsize(), 2},
                         repairedHash ^ computeDocValidationHash(second.getObject()))});
}

TEST_F(PipelinedCheckpointValidationTest, FutureRepairDoesNotChangeEarlierCheckpoint) {
    const auto first = insert(1);
    const auto firstHash = computeDocValidationHash(first.getObject());
    const auto firstWatermark = makeWatermarkEntry(nextOpTime().getTimestamp());
    auto earlier = applyAndPop({first,
                                firstWatermark,
                                checkpointEntry(firstWatermark.getTimestamp(),
                                                {first.getObject().objsize(), 1},
                                                firstHash)});
    const auto repairedHash = firstHash ^ 1;
    const auto repair =
        makeRepairNoopEntry(nextOpTime().getTimestamp(), _uuid, BSON("hSet" << repairedHash));
    const auto second = insert(2);
    const auto secondWatermark = makeWatermarkEntry(nextOpTime().getTimestamp());
    otel::metrics::OtelMetricsCapturer metrics;
    dispatch({repair,
              second,
              secondWatermark,
              checkpointEntry(secondWatermark.getTimestamp(),
                              {first.getObject().objsize() + second.getObject().objsize(), 2},
                              repairedHash ^ computeDocValidationHash(second.getObject()))});

    // The later repair and checkpoint are recorded before the earlier checkpoint validates.
    validateAndPublish(earlier);
    replicated_fast_count::test_helpers::expectValidatorMetrics(
        metrics, {.recorded = 1, .evicted = 1, .skipped = 0, .validated = 1});
    _pool->waitForIdle();
    auto later = _tracker.popCompletedBatch(Milliseconds(0));
    ASSERT_TRUE(later);
    assertInsertRecords(*later, {second});
    validateAndPublish(*later);
    replicated_fast_count::test_helpers::expectValidatorMetrics(
        metrics, {.recorded = 1, .evicted = 2, .skipped = 0, .validated = 2});
}

TEST_F(PipelinedCheckpointValidationTest, LaterCompletedBatchCannotMergeAheadOfSlowWorker) {
    const auto slow = insertOnWorker(0);
    const auto fast = insertOnWorker(1);
    Notification<void> slowEntered;
    Notification<void> releaseSlow;
    Notification<void> fastFinished;
    int slowAttempts = 0;
    bool released = false;
    ON_BLOCK_EXIT([&] {
        if (!released) {
            releaseSlow.set();
        }
        _pool->waitForIdle();
        _afterConsume = {};
    });
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString&, const std::vector<BSONObj>& docs) {
            if (docs.front()["_id"].numberInt() == slow.getObject()["_id"].numberInt()) {
                if (!slowEntered) {
                    slowEntered.set();
                }
                releaseSlow.get();
                if (++slowAttempts == 1) {
                    throwWriteConflictException("retry the held insert after releasing its gate");
                }
            }
        };
    _afterConsume = [&](const WorkItem& item) {
        if (item.ops.back()->getTimestamp() == fast.getTimestamp()) {
            fastFinished.set();
        }
    };
    auto first = dispatch({slow});
    ASSERT_TRUE(slowEntered.waitFor(_opCtx.get(), Seconds(60)));
    auto second = dispatch({fast});
    const auto watermark = makeWatermarkEntry(nextOpTime().getTimestamp());
    dispatch({watermark});
    ASSERT_TRUE(fastFinished.waitFor(_opCtx.get(), Seconds(60)));
    ASSERT_EQ(first->load(), 1);
    ASSERT_EQ(second->load(), 0);
    ASSERT_FALSE(_tracker.popCompletedBatch(Milliseconds(0)));
    releaseSlow.set();
    released = true;
    _pool->waitForIdle();
    ASSERT_GTE(slowAttempts, 2);
    for (const auto& expected : {slow, fast, watermark}) {
        auto batch = _tracker.popCompletedBatch(Milliseconds(0));
        ASSERT_TRUE(batch);
        ASSERT_EQ(batch->lastOpTime.opTime, expected.getOpTime());
        validateAndPublish(*batch);
    }
    applyAndValidate({checkpointEntry(watermark.getTimestamp(),
                                      {slow.getObject().objsize() + fast.getObject().objsize(), 2},
                                      computeDocValidationHash(slow.getObject()) ^
                                          computeDocValidationHash(fast.getObject()))});
}

TEST_F(PipelinedCheckpointValidationTest, FailureClearsThreadBufferBeforeOperationContextReuse) {
    const auto missing = NamespaceString::createNamespaceString_forTest("test.poa_missing");
    InflightBatch batch{
        .ops = {makeInsertDocumentOplogEntry(nextOpTime(), missing, BSON("_id" << 1))}};
    preparePipelinedBatchValidation(_opCtx.get(), _options, batch, workerCount());
    WorkItem item;
    item.ops.emplace_back(&batch.ops.front());
    item.validatorBuffer = &(*batch.validatorBuffers)[0];
    WorkerMultikeyPathInfo multikey;
    ASSERT_EQ(applyWorkItem(_opCtx.get(), _options, item, &multikey),
              ErrorCodes::NamespaceNotFound);
    SizeCountCheckpointValidator::recordDelta(_uuid, Timestamp(999, 1), {});
    ASSERT_TRUE(item.validatorBuffer->empty());
    batch.ops.front() = insert(1);
    ASSERT_OK(applyWorkItem(_opCtx.get(), _options, item, &multikey));
    SizeCountCheckpointValidator::recordDelta(_uuid, Timestamp(999, 1), {});
    assertInsertRecords(batch, {batch.ops.front()});
}

TEST_F(PipelinedCheckpointValidationTest, WorkItemsWithoutBuffersDoNotRecordDeltas) {
    // TODO SERVER-139269: Test production validation controls.
    const auto op = insert(1);
    auto batch = applyAndPop({op}, /*validation=*/false);
    ASSERT_FALSE(batch.validatorBuffers);
    validateAndPublish(batch);
}

TEST_F(PipelinedCheckpointValidationTest, ValidationPreservesDeferredMultikeyResults) {
    {
        UnreplicatedWritesBlock noRep(_opCtx.get());
        createIndex(_opCtx.get(),
                    kNss,
                    _uuid,
                    BSON("v" << 2 << "name" << "idx" << "key" << BSON("x" << 1)));
    }
    const auto doc = BSON("_id" << 1 << "x" << BSON_ARRAY(1 << 2));
    const auto op = makeInsertOplogEntryWithRecordIdSizeMetadataAndHash(
        nextOpTime(), kNss, _uuid, doc, RecordId(1), computeDocValidationHash(doc));
    auto batch = applyAndPop({op});
    assertInsertRecords(batch, {op});
    WorkerMultikeyPathInfo results;
    for (const auto& slot : *batch.multikeyMetadata) {
        results.insert(results.end(), slot.begin(), slot.end());
    }
    const WorkerMultikeyPathInfo expected{{kNss, _uuid, "idx", {}, {{0U}}, op.getTimestamp()}};
    ASSERT_EQ(results, expected);
    const auto originalMetadata = *batch.multikeyMetadata;
    validatePipelinedBatch(_opCtx.get(), batch, _validator, _oplogUuid);
    shard_role_details::getRecoveryUnit(_opCtx.get())->abandonSnapshot();
    ASSERT_EQ(*batch.multikeyMetadata, originalMetadata);
    {
        UnreplicatedWritesBlock noRep(_opCtx.get());
        ASSERT_OK(flushMultikeyPathInfo(_opCtx.get(),
                                        getStorageInterface(),
                                        *batch.multikeyMetadata,
                                        op.getTimestamp(),
                                        op.getTimestamp()));
    }
    _tracker.onBatchPublished();
    {
        const auto coll =
            acquireCollection(_opCtx.get(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  _opCtx.get(), kNss, AcquisitionPrerequisites::kRead),
                              MODE_IS);
        const auto* index =
            coll.getCollectionPtr()->getIndexCatalog()->findIndexByName(_opCtx.get(), "idx");
        ASSERT_TRUE(index);
        ASSERT_TRUE(index->isMultikey(_opCtx.get(), coll.getCollectionPtr()));
        ASSERT_EQ(index->getMultikeyPaths(_opCtx.get(), coll.getCollectionPtr()),
                  MultikeyPaths({{0U}}));
    }
    const auto watermark = makeWatermarkEntry(nextOpTime().getTimestamp());
    applyAndValidate({watermark,
                      checkpointEntry(watermark.getTimestamp(),
                                      {doc.objsize(), 1},
                                      computeDocValidationHash(doc))});
}

TEST_F(PipelinedCheckpointValidationTest, NonSecondaryBatchesDoNotAllocateValidationBuffers) {
    InflightBatch batch{.ops = {makeWatermarkEntry(nextOpTime().getTimestamp())}};
    const OplogApplier::Options initialSync{OplogApplication::Mode::kInitialSync};
    preparePipelinedBatchValidation(_opCtx.get(), initialSync, batch, workerCount());
    ASSERT_FALSE(batch.validatorBuffers);
}

TEST_F(PipelinedCheckpointValidationTest, AdvancerValidatesAllWorkerBuffersBeforePublication) {
    const std::vector<OplogEntry> ops{insertOnWorker(0), insertOnWorker(1)};
    int64_t size = 0;
    int64_t hash = 0;
    for (const auto& op : ops) {
        size += op.getObject().objsize();
        hash ^= computeDocValidationHash(op.getObject());
    }
    const auto watermark = makeWatermarkEntry(nextOpTime().getTimestamp());
    const auto checkpoint = checkpointEntry(watermark.getTimestamp(), {size, 2}, hash);
    std::vector<OpTime> published;
    std::vector<size_t> contributingWorkers;
    otel::metrics::OtelMetricsCapturer metrics;
    PipelinedApplierAdvancer advancer(_tracker, [&](const InflightBatch& batch) {
        const auto opCtx = cc().makeOperationContext();
        validatePipelinedBatch(opCtx.get(), batch, _validator, _oplogUuid);
        contributingWorkers.push_back(std::count_if(
            batch.validatorBuffers->begin(), batch.validatorBuffers->end(), [](const auto& slot) {
                return !slot.empty();
            }));
        published.push_back(batch.lastOpTime.opTime);
    });
    advancer.startup();
    ON_BLOCK_EXIT([&] { advancer.shutdownAndJoin(); });
    dispatch(ops);
    dispatch({watermark});
    dispatch({checkpoint});
    _pool->waitForIdle();
    ASSERT_TRUE(_tracker.waitUntilIdle());
    ASSERT_EQ(published,
              (std::vector<OpTime>{
                  ops.back().getOpTime(), watermark.getOpTime(), checkpoint.getOpTime()}));
    ASSERT_EQ(contributingWorkers, (std::vector<size_t>{2, 0, 0}));
    replicated_fast_count::test_helpers::expectValidatorMetrics(
        metrics, {.recorded = 1, .evicted = 1, .skipped = 0, .validated = 1});
}

TEST_F(PipelinedCheckpointValidationTest, CompletedBatchSizeMismatchLogsError) {
    unittest::ServerParameterGuard logOnly{
        "fassertOnPerCollectionContinuousInternodeValidationFailure", false};
    const auto op = insert(1);
    const auto watermark = makeWatermarkEntry(nextOpTime().getTimestamp());
    auto batch = applyAndPop({op,
                              watermark,
                              checkpointEntry(watermark.getTimestamp(),
                                              {999, 1},
                                              computeDocValidationHash(op.getObject()))});
    unittest::LogCaptureGuard logs;
    validateAndPublish(batch);
    logs.stop();
    ASSERT_EQ(logs.countBSONContainingSubset(BSON("id" << 13626005)), 1);
}

using PipelinedCheckpointValidationDeathTest = PipelinedCheckpointValidationTest;

DEATH_TEST_F(PipelinedCheckpointValidationDeathTest,
             CompletedBatchSizeMismatchFasserts,
             "13626005") {
    const auto op = insert(1);
    const auto watermark = makeWatermarkEntry(nextOpTime().getTimestamp());
    applyAndValidate({op,
                      watermark,
                      checkpointEntry(watermark.getTimestamp(),
                                      {999, 1},
                                      computeDocValidationHash(op.getObject()))});
}

DEATH_TEST_F(PipelinedCheckpointValidationDeathTest,
             CompletedBatchHashMismatchFasserts,
             "13626005") {
    const auto op = insert(1);
    const auto watermark = makeWatermarkEntry(nextOpTime().getTimestamp());
    applyAndValidate({op,
                      watermark,
                      checkpointEntry(watermark.getTimestamp(),
                                      {op.getObject().objsize(), 1},
                                      computeDocValidationHash(op.getObject()) ^ 1)});
}

DEATH_TEST_F(PipelinedCheckpointValidationDeathTest,
             WorkItemRequiresAnEmptyValidationSlot,
             "item.validatorBuffer->empty()") {
    WorkItem item;
    replicated_fast_count::ValidatorBuffers::value_type records{{Timestamp(1, 1), _uuid, {}}};
    item.validatorBuffer = &records;
    WorkerMultikeyPathInfo multikey;
    applyWorkItem(_opCtx.get(), _options, item, &multikey).ignore();
}

}  // namespace
}  // namespace mongo::repl
