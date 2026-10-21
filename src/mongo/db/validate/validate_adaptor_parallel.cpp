// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/validate_adaptor_parallel.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/validate/collection_validation.h"
#include "mongo/db/validate/record_store_slicer.h"
#include "mongo/db/validate/validate_gen.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/db/validate/validate_state.h"
#include "mongo/logv2/log.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/processinfo.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>

#include <fmt/format.h>
#include <fmt/ranges.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage
namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeRecordStoreSliceTraversal);
/**
 * The snapshot settings a slice worker has to adopt from the thread that started the validation.
 *
 * Each worker runs on an OperationContext of its own, so its RecoveryUnit starts out with the
 * defaults (untimestamped, latest committed data, no oplog visibility barrier) rather than whatever
 * the caller established. Left alone, every slice would read from a snapshot of its own, taken at
 * whatever point in time the worker happened to start, and the slices would neither agree with each
 * other nor with the counting traversal that runs on the caller's snapshot.
 */
struct SnapshotSettings {
    static SnapshotSettings from(RecoveryUnit& ru) {
        return {.readSource = ru.getTimestampReadSource(),
                .readTimestamp = ru.getPointInTimeReadTimestamp(),
                .prepareConflictBehavior = ru.getPrepareConflictBehavior(),
                .oplogVisibilityTs = ru.getOplogVisibilityTs()};
    }

    void applyTo(RecoveryUnit& ru) const {
        // Must happen before the slice cursor opens a transaction on this RecoveryUnit.
        invariant(!ru.isActive());
        ru.setPrepareConflictBehavior(prepareConflictBehavior);
        ru.setOplogVisibilityTs(oplogVisibilityTs);
        // A point-in-time read source is reproduced as kProvided so that every worker pins the
        // same timestamp instead of independently re-deriving one that may have advanced.
        if (readTimestamp) {
            ru.setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, *readTimestamp);
        } else {
            ru.setTimestampReadSource(readSource);
        }
    }

    RecoveryUnit::ReadSource readSource;
    boost::optional<Timestamp> readTimestamp;
    PrepareConflictBehavior prepareConflictBehavior;
    boost::optional<int64_t> oplogVisibilityTs;
};


}  // namespace

namespace collection_validation {

/**
 * Returns the thread pool that slice traversals run on, shared by every validation in flight and
 * torn down once the last of them lets go.
 */
std::shared_ptr<ThreadPool> getValidateThreadPool() {
    static std::mutex mutex;
    static std::weak_ptr<ThreadPool> weakPool;
    std::lock_guard lk(mutex);

    if (auto pool = weakPool.lock()) {
        return pool;
    }
    auto pool = std::make_shared<ThreadPool>(ThreadPool::Options{
        .poolName = "ValidateRecordStore",
        .threadNamePrefix = "ValidateRecordStoreWorker",
        .maxThreads = ProcessInfo::getNumAvailableCores(),
        .onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName, getGlobalServiceContext()->getService());
        }});
    pool->startup();
    weakPool = pool;
    return pool;
}

SliceValidateCursor::SliceValidateCursor(OperationContext* opCtx,
                                         std::unique_ptr<SeekableRecordCursor> cursor,
                                         CancellationToken cancellation)
    : _opCtx(opCtx), _cursor(std::move(cursor)), _cancellation(std::move(cancellation)) {}

boost::optional<Record> SliceValidateCursor::seek(const RecordId& recordId) {
    return _cursor->seek(recordId, SeekableRecordCursor::BoundInclusion::kInclude);
}

boost::optional<Record> SliceValidateCursor::next() {
    return _cursor->next();
}

void SliceValidateCursor::yield() {
    // The worker runs on an OperationContext of its own, so an interrupt of the validation
    // reaches it only through the cancellation token.
    uassert(ErrorCodes::Interrupted,
            "Interrupted due to: record store validation was interrupted",
            !_cancellation.isCanceled());
    _cursor->save();
    // The slice range is enforced by the traversal loop rather than by cursor bounds, so
    // nothing but the position needs to survive the reset. Foreground validation holds the
    // collection lock exclusively, which is what lets the cursor expect to land back where it
    // was: no writer can have removed the record it was sitting on.
    uassert(ErrorCodes::Interrupted,
            "Interrupted due to: failure to restore yielded slice cursor",
            _cursor->restore(*shard_role_details::getRecoveryUnit(_opCtx)));
}

std::unique_ptr<ValidateCursor> ValidateAdaptorParallel::makeSliceCursor(
    OperationContext* workerOpCtx,
    const RecordStore& recordStore,
    CancellationToken cancellation) const {
    return std::make_unique<SliceValidateCursor>(
        workerOpCtx,
        recordStore.getCursor(workerOpCtx, *shard_role_details::getRecoveryUnit(workerOpCtx)),
        std::move(cancellation));
}

ValidateAdaptorParallel::ValidateAdaptorParallel(
    OperationContext* opCtx,
    collection_validation::ValidateState& validateState,
    boost::optional<int64_t> targetRecordsPerSlice)
    : ValidateAdaptor(opCtx, validateState), _targetRecordsPerSlice(targetRecordsPerSlice) {}

void ValidateAdaptorParallel::traverseRecordStore(OperationContext* opCtx,
                                                  ValidateResults& validateResults,
                                                  ValidationVersion validationVersion) {
    auto& validateState = getValidateState();
    const auto& coll = validateState.getCollection();
    const int64_t totalRecords = coll->getRecordStore()->numRecords();

    int64_t dataSizeTotal{0};
    int64_t numRecords{0};

    // Split the record store into slices targeting ~targetRecordsPerSlice records each. Small
    // collections collapse to a single slice and run single-threaded, larger collections are
    // split into enough slices to keep the worker pool busy and allow "work stealing", as no
    // guarantee can be made that the cost of each slice is equal.

    const int64_t sliceCount = _targetRecordsPerSlice.has_value() && (*_targetRecordsPerSlice > 0)
        ? std::clamp<int64_t>((totalRecords + *_targetRecordsPerSlice - 1) /
                                  *_targetRecordsPerSlice,
                              1,
                              gValidateParallelMaxRecordStoreSlices.load())
        : 1;

    const bool hashDrillDown = validateState.getHashPrefixes().has_value() ||
        validateState.getRevealHashedIds().has_value();
    const bool parallelEligible = !validateState.isBackground() &&
        validateState.getRepairMode() == collection_validation::RepairMode::kNone &&
        getKeyBasedIndexConsistency().canMergeResults() && !hashDrillDown && sliceCount > 1;

    const auto slicePivotResults = parallelEligible
        ? collection_validation::computeSlicePivots(opCtx,
                                                    *shard_role_details::getRecoveryUnit(opCtx),
                                                    *coll->getRecordStore(),
                                                    sliceCount)
        : collection_validation::SlicePivotResults{{},
                                                   collection_validation::SliceStrategy::kOneSlice};
    const std::vector<RecordId>& rsSlicePivots = slicePivotResults.pivots;
    if (rsSlicePivots.size() <= 1) {
        traverseRecordStoreSerial(opCtx, validateResults, validationVersion);
        return;
    }

    invariant(!validateState.getFirstRecordId().isNull());
    const auto threadPool = getValidateThreadPool();
    onThreadPoolAcquired_forTest();

    auto progressGuard = preTraverseRecordStoreImpl(opCtx);

    // Turn the pivots into half-open [begin, end) ranges, with the final slice left open-ended
    // so that the ranges cover the whole record store no matter how many pivots the slicer
    // produced. Deduplication of the pivots means there may be fewer slices than 'sliceCount'.
    //
    // Every slice gets the same factory, which each worker calls on its own thread to open a
    // cursor of its own.
    CancellationSource sliceCancellation;
    const ValidateCursor::Factory sliceCursorFactory =
        [this,
         recordStore = getValidateState().getCollection()->getRecordStore(),
         cancellation = sliceCancellation.token()](
            OperationContext* traversalOpCtx) -> std::unique_ptr<ValidateCursor> {
        return makeSliceCursor(traversalOpCtx, *recordStore, cancellation);
    };
    const auto trsOpts = ([&rsSlicePivots, &sliceCursorFactory] {
        std::vector<TraverseRecordStoreOptions> trsOpts;
        auto itRangeBegin = rsSlicePivots.begin();
        auto itRangeEnd = rsSlicePivots.begin();
        ++itRangeEnd;
        while (itRangeEnd != rsSlicePivots.end()) {
            trsOpts.push_back({.beginRecordId = *itRangeBegin,
                               .endRecordId = *itRangeEnd,
                               .cursorFactory = sliceCursorFactory});
            ++itRangeBegin;
            ++itRangeEnd;
        }
        trsOpts.push_back({.beginRecordId = *itRangeBegin, .cursorFactory = sliceCursorFactory});
        return trsOpts;
    })();

    std::vector<PromiseAndFuture<TraverseRecordStoreResults>> rsSliceResults(trsOpts.size());

    const auto snapshotSettings =
        SnapshotSettings::from(*shard_role_details::getRecoveryUnit(opCtx));
    for (size_t i = 0; i < trsOpts.size(); ++i) {
        threadPool->schedule([this,
                              &rsSliceResults,
                              i,
                              validationVersion,
                              snapshotSettings,
                              cancellation = sliceCancellation.token(),
                              opts = trsOpts[i]](Status status) {
            if (!status.isOK()) {
                rsSliceResults[i].promise.setError(status);
                return;
            }
            try {
                hangBeforeRecordStoreSliceTraversal.pauseWhileSet();
                uassert(ErrorCodes::Interrupted,
                        "Interrupted due to: record store validation was interrupted",
                        !cancellation.isCanceled());
                auto workerOpCtxHolder = cc().makeOperationContext();
                auto* workerOpCtx = workerOpCtxHolder.get();
                snapshotSettings.applyTo(*shard_role_details::getRecoveryUnit(workerOpCtx));
                // Slice starts from an empty ValidateResults rather than the
                // caller's; traverseRecordStoreImpl() stamps the identity fields that
                // merge() requires to match.
                auto innerResults = traverseRecordStoreImpl(workerOpCtx,
                                                            ValidateResults{},
                                                            getKeyBasedIndexConsistency(),
                                                            opts,
                                                            getProgress(),
                                                            validationVersion);
                innerResults.storageStats = shard_role_details::getRecoveryUnit(workerOpCtx)
                                                ->computeOperationStatisticsSinceLastCall();
                rsSliceResults[i].promise.emplaceValue(std::move(innerResults));

            } catch (const DBException& e) {
                rsSliceResults[i].promise.setError(e.toStatus());
            }
        });
    }

    std::vector<TraverseRecordStoreResults> implResults;
    implResults.reserve(rsSliceResults.size());
    std::vector<Status> sliceFailures;
    // Every slice must finish before returning, as the workers reference this frame and adaptor.
    // An interrupt of the caller is instead forwarded to the slices, which stop at their next
    // yield.
    Status interruptStatus = Status::OK();
    for (auto& pf : rsSliceResults) {
        if (interruptStatus.isOK()) {
            interruptStatus = pf.future.waitNoThrow(opCtx);
            if (!interruptStatus.isOK()) {
                sliceCancellation.cancel();
                sliceFailures.push_back(interruptStatus);
            }
        }
        auto swResults = std::move(pf.future).getNoThrow();
        if (!swResults.isOK()) {
            sliceFailures.push_back(swResults.getStatus());
            continue;
        }
        implResults.push_back(std::move(swResults.getValue()));
    }

    // Combine the per-slice results before surfacing any slice's failure, so that a partial
    // failure still reports everything the other slices accumulated.
    for (const auto& ir : implResults) {
        validateResults.merge(ir.validateResults);
        getKeyBasedIndexConsistency().merge(ir.keyStringIndexConsistency);
        dataSizeTotal += ir.dataSizeTotal;
        numRecords += ir.validateResults.getNumRecords().value_or(0);
        if (ir.storageStats) {
            addSliceStorageStats(*ir.storageStats);
        }
    }

    for (const auto& ir : implResults) {
        if (!ir.status.isOK()) {
            sliceFailures.push_back(ir.status);
        }
    }

    if (!implResults.empty()) {
        RecordStoreTraversalParallelMetrics parallelMetrics;
        parallelMetrics.sliceStrategy = slicePivotResults.strategy;
        parallelMetrics.sliceCount = implResults.size();
        parallelMetrics.minSliceSize = std::numeric_limits<long long>::max();
        for (const auto& ir : implResults) {
            const long long size = ir.validateResults.getNumRecords().value_or(0);
            parallelMetrics.minSliceSize = std::min(parallelMetrics.minSliceSize, size);
            parallelMetrics.maxSliceSize = std::max(parallelMetrics.maxSliceSize, size);
            parallelMetrics.meanSliceSize += size;
        }
        parallelMetrics.meanSliceSize /= parallelMetrics.sliceCount;
        for (const auto& ir : implResults) {
            const double delta =
                ir.validateResults.getNumRecords().value_or(0) - parallelMetrics.meanSliceSize;
            parallelMetrics.stddevSliceSize += delta * delta;
        }
        parallelMetrics.stddevSliceSize =
            std::sqrt(parallelMetrics.stddevSliceSize / parallelMetrics.sliceCount);
        validateResults.setRecordStoreTraversalParallelMetrics(parallelMetrics);
    }

    // Surface every slice's failures.
    if (!sliceFailures.empty()) {
        for (const auto& status : sliceFailures) {
            LOGV2_ERROR(11157101,
                        "Record store slice traversal failed",
                        logAttrs(coll->ns()),
                        "error"_attr = status);
        }
        const auto& reported = *std::min_element(
            sliceFailures.begin(), sliceFailures.end(), [](const auto& a, const auto& b) {
                // An interrupt outranks the failures it caused in the other slices.
                return ErrorCodes::isCancellationError(a.code()) &&
                    !ErrorCodes::isCancellationError(b.code());
            });
        std::vector<std::string> reasons;
        reasons.reserve(sliceFailures.size());
        std::transform(sliceFailures.begin(),
                       sliceFailures.end(),
                       std::back_inserter(reasons),
                       [](const Status& status) { return status.toString(); });
        uasserted(reported.code(),
                  fmt::format("{} of {} record store slices failed to traverse: {}",
                              sliceFailures.size(),
                              trsOpts.size(),
                              fmt::join(reasons, "; ")));
    }

    postTraverseRecordStoreImpl(opCtx, validateResults, numRecords, dataSizeTotal);
}

}  // namespace collection_validation
}  // namespace mongo
