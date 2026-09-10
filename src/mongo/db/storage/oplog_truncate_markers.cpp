// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/oplog_truncate_markers.h"

#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/oplog_truncate_marker_parameters_gen.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/system_tick_source.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(hangDuringOplogSampling);
}  // namespace

std::shared_ptr<OplogTruncateMarkers> OplogTruncateMarkers::createEmptyOplogTruncateMarkers(
    RecordStore& rs) {
    return std::make_shared<OplogTruncateMarkers>(
        std::deque<CollectionTruncateMarkers::Marker>{},
        0,
        0,
        0,
        Microseconds{0},
        CollectionTruncateMarkers::MarkersCreationMethod::InProgress,
        false /* initialSamplingFinished */,
        *rs.oplog());
}

int64_t OplogTruncateMarkers::estimateOplogSize(RecordStore& rs) {
    return rs.oplog()->getMaxSize();
}

namespace {
struct MarkerSizing {
    uint64_t numTruncateMarkersToKeep;
    int64_t minBytesPerTruncateMarker;
};

MarkerSizing calculateMarkerSizing(int64_t oplogSize, long long maxOplogTruncateMarkers) {
    invariant(oplogSize > 0);

    // The minimum oplog truncate marker size should be BSONObjMaxInternalSize.
    // use 64-bit int to avoid possible overflow on multiplication
    const uint64_t oplogTruncateMarkerSize =
        std::max<uint64_t>(gOplogTruncateMarkerSizeMB * 1024ULL * 1024ULL, BSONObjMaxInternalSize);

    // IDL does not support unsigned types.
    const uint64_t kMinTruncateMarkersToKeep = static_cast<uint64_t>(gMinOplogTruncateMarkers);
    const uint64_t kMaxTruncateMarkersToKeep = static_cast<uint64_t>(maxOplogTruncateMarkers);
    const uint64_t kMaxBytesPerTruncateMarker =
        static_cast<uint64_t>(gMaxOplogTruncateMarkerSizeMB.load()) * 1024 * 1024;

    uint64_t numTruncateMarkers = oplogSize / oplogTruncateMarkerSize;
    uint64_t numTruncateMarkersToKeep = std::min(
        kMaxTruncateMarkersToKeep, std::max(kMinTruncateMarkersToKeep, numTruncateMarkers));

    // We sometimes support a very small oplog size, so round up to avoid zero.
    int64_t minBytesPerTruncateMarker =
        std::ceil(static_cast<double>(oplogSize) / numTruncateMarkersToKeep);

    // On a large enough oplog, dividing by the target number of markers produces markers so big
    // that truncating is disruptive. Cap the marker size and override the max count in this case.
    if (kMaxBytesPerTruncateMarker > 0 &&
        static_cast<uint64_t>(minBytesPerTruncateMarker) > kMaxBytesPerTruncateMarker) {
        minBytesPerTruncateMarker = static_cast<int64_t>(kMaxBytesPerTruncateMarker);
        numTruncateMarkersToKeep = static_cast<uint64_t>(
            std::ceil(static_cast<double>(oplogSize) / minBytesPerTruncateMarker));
    }

    return {.numTruncateMarkersToKeep = numTruncateMarkersToKeep,
            .minBytesPerTruncateMarker = minBytesPerTruncateMarker};
}
}  // namespace

OplogTruncateMarkers::InitialSetOfOplogMarkers OplogTruncateMarkers::beginMarkerCreation(
    OperationContext* opCtx, RecordStore& rs, int64_t estimatedOplogSize) {
    auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
    bool asyncGenerationEnabled = provider.supportsAsyncOplogMarkerGeneration();
    bool samplingEnabled = provider.supportsOplogSampling();
    bool scanningEnabled = provider.supportsOplogScanning();

    tassert(13283601,
            "The persistence provider must allow either scanning or sampling as an initial marker "
            "generation method.",
            samplingEnabled || scanningEnabled);

    auto policy = CollectionTruncateMarkers::MarkersCreationPolicy::kAuto;
    if (!samplingEnabled) {
        policy = CollectionTruncateMarkers::MarkersCreationPolicy::kScanOnly;
    } else if (!scanningEnabled) {
        policy = CollectionTruncateMarkers::MarkersCreationPolicy::kSampleOnly;
    }

    LOGV2(10621000,
          "Beginning initial marker creation.",
          "asyncGenerationEnabled"_attr = asyncGenerationEnabled,
          "samplingEnabled"_attr = samplingEnabled,
          "scanningEnabled"_attr = scanningEnabled,
          "estimatedOplogSize"_attr = estimatedOplogSize);

    invariant(estimatedOplogSize > 0);
    invariant(rs.keyFormat() == KeyFormat::Long);

    auto [numTruncateMarkersToKeep, minBytesPerTruncateMarker] =
        calculateMarkerSizing(estimatedOplogSize, gMaxOplogTruncateMarkersDuringStartup);
    invariant(minBytesPerTruncateMarker > 0);

    // We need to read the whole oplog, override the recoveryUnit's oplogVisibleTimestamp.
    ScopedOplogVisibleTimestamp scopedOplogVisibleTimestamp(
        shard_role_details::getRecoveryUnit(opCtx), boost::none);
    auto yieldInterval = asyncGenerationEnabled && gOplogSamplingAsyncYieldIntervalMs >= 0
        ? boost::optional<Milliseconds>(gOplogSamplingAsyncYieldIntervalMs)
        : boost::none;
    auto iterator = CollectionTruncateMarkers::makeIterator(
        opCtx, &rs, globalSystemTickSource(), yieldInterval);

    if (MONGO_unlikely(hangDuringOplogSampling.shouldFail())) {
        LOGV2(11211900,
              "Hanging the oplog cap maintainer thread during initial sampling due "
              "to fail point");
        hangDuringOplogSampling.pauseWhileSet(opCtx);
    }

    auto initialSetOfMarkers = CollectionTruncateMarkers::createFromCollectionIterator(
        opCtx,
        *iterator,
        minBytesPerTruncateMarker,
        [](const Record& record) {
            BSONObj obj = record.data.toBson();
            auto wallTime = obj.hasField(repl::DurableOplogEntry::kWallClockTimeFieldName)
                ? obj[repl::DurableOplogEntry::kWallClockTimeFieldName].Date()
                : obj[repl::DurableOplogEntry::kTimestampFieldName].timestampTime();
            return RecordIdAndWallTime(record.id, wallTime);
        },
        policy,
        numTruncateMarkersToKeep);
    LOGV2(22382,
          "Record store oplog processing finished",
          "markersCount"_attr = initialSetOfMarkers.markers.size(),
          "markerCreationMethod"_attr = toString(initialSetOfMarkers.methodUsed),
          "duration"_attr = duration_cast<Milliseconds>(initialSetOfMarkers.timeTaken));
    LOGV2(10621110, "Initial set of markers created.", "oplogSizeBytes"_attr = rs.dataSize());

    return {std::move(initialSetOfMarkers),
            minBytesPerTruncateMarker,
            true /* initialSamplingFinished */};
}

std::shared_ptr<OplogTruncateMarkers> OplogTruncateMarkers::createOplogTruncateMarkers(
    OperationContext* opCtx, RecordStore& rs) {
    auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
    if (!provider.supportsAsyncOplogMarkerGeneration()) {
        // Synchronous path: build the full initial marker set before returning.
        auto initialSetOfMarkers =
            beginMarkerCreation(opCtx, rs, OplogTruncateMarkers::estimateOplogSize(rs));
        return std::make_shared<OplogTruncateMarkers>(std::move(initialSetOfMarkers), *rs.oplog());
    }
    // Asynchronous path: return an empty placeholder. The cap maintainer thread will later call
    // beginMarkerCreation() and replace this object's state with the populated marker set.
    return createEmptyOplogTruncateMarkers(rs);
}

OplogTruncateMarkers::OplogTruncateMarkers(
    std::deque<CollectionTruncateMarkers::Marker>&& markers,
    int64_t partialMarkerRecords,
    int64_t partialMarkerBytes,
    int64_t minBytesPerMarker,
    Microseconds totalTimeSpentBuilding,
    CollectionTruncateMarkers::MarkersCreationMethod creationMethod,
    bool initialSamplingFinished,
    const RecordStore::Oplog& oplog)
    : CollectionTruncateMarkers(std::move(markers),
                                partialMarkerRecords,
                                partialMarkerBytes,
                                minBytesPerMarker,
                                totalTimeSpentBuilding,
                                creationMethod,
                                initialSamplingFinished),
      _oplog(oplog) {}

OplogTruncateMarkers::OplogTruncateMarkers(
    OplogTruncateMarkers::InitialSetOfOplogMarkers&& initialSetOfMarkers,
    const RecordStore::Oplog& oplog)
    : CollectionTruncateMarkers(std::move(initialSetOfMarkers.markers),
                                initialSetOfMarkers.leftoverRecordsCount,
                                initialSetOfMarkers.leftoverRecordsBytes,
                                initialSetOfMarkers.minBytesPerTruncateMarker,
                                initialSetOfMarkers.timeTaken,
                                initialSetOfMarkers.methodUsed,
                                initialSetOfMarkers.initialSamplingFinished),
      _oplog(oplog) {}

bool OplogTruncateMarkers::isDead() {
    std::lock_guard<std::mutex> lk(_reclaimMutex);
    return _isDead;
}

void OplogTruncateMarkers::kill() {
    std::lock_guard<std::mutex> lk(_reclaimMutex);
    _isDead = true;
    _reclaimCv.notify_one();
}

Date_t OplogTruncateMarkers::newestExpiredWallTime(OperationContext* opCtx) {
    // retention times can be fractional, so use floating point arithmetic for this calculation.
    static const double kNumMSInHour = durationCount<Milliseconds>(Hours(1));
    long minRetentionMS = storageGlobalParams.oplogMinRetentionHours.load() * kNumMSInHour;
    // If retention by time is disabled, consumers with space-based retention will consider all
    // records expired and consumers without will consider no records expired.
    // The former case is handled separately in _hasExcessMarkers() in this file.
    // The latter case is not handled elsewhere, so handle it here.
    if (minRetentionMS == 0) {
        return Date_t();
    }
    return opCtx->fastClockSource().now() - Milliseconds(minRetentionMS);
}

void OplogTruncateMarkers::clearMarkersOnCommit(OperationContext* opCtx) {
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [this](OperationContext*, boost::optional<Timestamp>) {
            modifyMarkersWith([&](std::deque<CollectionTruncateMarkers::Marker>& markers) {
                markers.clear();
                modifyPartialMarker([&](CollectionTruncateMarkers::PartialMarkerMetrics metrics) {
                    metrics.currentRecords->store(0);
                    metrics.currentBytes->store(0);
                });
            });
        });
}

void OplogTruncateMarkers::updateMarkersAfterCappedTruncateAfter(int64_t recordsRemoved,
                                                                 int64_t bytesRemoved,
                                                                 const RecordId& firstRemovedId) {
    modifyMarkersWith([&](std::deque<CollectionTruncateMarkers::Marker>& markers) {
        int64_t numMarkersToRemove = 0;
        int64_t recordsInMarkersToRemove = 0;
        int64_t bytesInMarkersToRemove = 0;

        // Compute the number and associated sizes of the records from markers that are either fully
        // or partially truncated.
        for (auto it = markers.rbegin(); it != markers.rend(); ++it) {
            if (it->lastRecord < firstRemovedId) {
                break;
            }
            numMarkersToRemove++;
            recordsInMarkersToRemove += it->records;
            bytesInMarkersToRemove += it->bytes;
        }

        // Remove the markers corresponding to the records that were deleted.
        int64_t offset = markers.size() - numMarkersToRemove;
        markers.erase(markers.begin() + offset, markers.end());

        // Account for any remaining records from a partially truncated marker in the marker
        // currently being filled.
        modifyPartialMarker([&](CollectionTruncateMarkers::PartialMarkerMetrics metrics) {
            metrics.currentRecords->fetchAndAdd(recordsInMarkersToRemove - recordsRemoved);
            metrics.currentBytes->fetchAndAdd(bytesInMarkersToRemove - bytesRemoved);
        });
    });
}

bool OplogTruncateMarkers::awaitHasExcessMarkersOrDead(OperationContext* opCtx) {
    // Wait until kill() is called or there are too many collection markers.
    std::unique_lock<std::mutex> lock(_reclaimMutex);
    MONGO_IDLE_THREAD_BLOCK;
    LOGV2_DEBUG(10621102, 1, "OplogCapMaintainerThread is idle");
    auto isWaitConditionSatisfied = opCtx->waitForConditionOrInterruptFor(
        _reclaimCv, lock, Seconds(gOplogTruncationCheckPeriodSeconds), [this, opCtx] {
            if (_isDead) {
                LOGV2_DEBUG(10621103, 1, "OplogCapMaintainerThread is active");
                return true;
            }

            if (auto marker = peekOldestMarkerIfNeeded(opCtx)) {
                invariant(marker->lastRecord.isValid());

                LOGV2_DEBUG(7393215,
                            2,
                            "Collection has excess markers",
                            "lastRecord"_attr = marker->lastRecord,
                            "wallTime"_attr = marker->wallTime);
                LOGV2_DEBUG(10621104, 1, "OplogCapMaintainerThread is active");
                return true;
            }

            LOGV2_DEBUG(10621105, 1, "OplogCapMaintainerThread is active");
            return false;
        });

    LOGV2_DEBUG(10621106, 1, "OplogCapMaintainerThread is active");
    // Return true only when we have detected excess markers, not because the record store
    // is being destroyed (_isDead) or we timed out waiting on the condition variable.
    return !(_isDead || !isWaitConditionSatisfied);
}

bool OplogTruncateMarkers::awaitHasExpiredOplogOrDead(OperationContext* opCtx, RecordStore& rs) {
    // The same mechanism (and the same private data) is used for canceling this wait as for
    // awaitHasExcessMarkersOrDead, but since this is waiting for time-based truncation, factor
    // the retention time into the wait period and not just the configured thread wake interval.
    // Note that oplogMinRetentionHours can be fractional, so don't use Interval to calculate this.
    double minRetentionSeconds = storageGlobalParams.oplogMinRetentionHours.load() * 60 * 60;
    // When truncating by time, use a smaller wake time to prevent too much oplog from accumulating.
    Seconds checkPeriod((minRetentionSeconds != 0.0) ? std::min<int>(30, minRetentionSeconds) : 30);
    // Wait until kill() is called or oplog can be truncated.
    std::unique_lock<std::mutex> lock(_reclaimMutex);
    MONGO_IDLE_THREAD_BLOCK;
    auto isWaitConditionSatisfied =
        opCtx->waitForConditionOrInterruptFor(_reclaimCv, lock, checkPeriod, [&] {
            if (_isDead) {
                return true;
            }
            RecordId pin(opCtx->getServiceContext()->getStorageEngine()->getPinnedOplog().asULL());

            // Wrap newestExpiredRecord in a writeConflictRetry because it opens a reverse oplog
            // cursor, which implicitly starts a read transaction, and is therefore susceptible to a
            // WriteConflictException.
            bool hasExpiredRecord = writeConflictRetry(
                opCtx, "awaitHasExpiredOplogOrDead", NamespaceString::kRsOplogNamespace, [&] {
                    return newestExpiredRecord(opCtx, rs, pin, newestExpiredWallTime(opCtx))
                        .has_value();
                });

            // Abandon the snapshot opened during newestExpiredRecord so the idle wait that follows
            // holds no snapshot and does not pin oldest_id for the entire check period. Abandoning
            // on every evaluation also ensures the next check observes oplog appended during the
            // wait rather than reusing a stale snapshot.
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
            return hasExpiredRecord;
        });

    // Return true only when we have detected excess oplog, not because the record store
    // is being destroyed (_isDead) or we timed out waiting on the condition variable.
    return !(_isDead || !isWaitConditionSatisfied);
}

bool OplogTruncateMarkers::_hasExcessMarkers(OperationContext* opCtx) const {
    int64_t totalBytes = 0;
    for (const auto& marker : getMarkers()) {
        totalBytes += marker.bytes;
    }

    // check that oplog truncate markers is at capacity
    if (totalBytes <= _oplog.getMaxSize()) {
        return false;
    }

    const auto& truncateMarker = getMarkers().front();

    // The pinned oplog is inside the earliest marker, so we cannot remove the marker range.
    if (static_cast<std::uint64_t>(truncateMarker.lastRecord.getLong()) >=
        opCtx->getServiceContext()->getStorageEngine()->getPinnedOplog().asULL()) {
        return false;
    }

    // If we are not checking for time, then yes, there is a truncate marker to be reaped
    // because oplog is at capacity.
    if (storageGlobalParams.oplogMinRetentionHours.load() == 0.0) {
        return true;
    }
    return truncateMarker.wallTime <= newestExpiredWallTime(opCtx);
}

void OplogTruncateMarkers::recomputeMinBytesPerMarker(RecordStore& rs) {
    int64_t oplogSize = getEstimatedOplogSize(rs);

    auto [_, minBytesPerTruncateMarker] =
        calculateMarkerSizing(oplogSize, gMaxOplogTruncateMarkersAfterStartup);

    LOGV2_DEBUG(13190501,
                2,
                "Updating minBytesPerMarker.",
                "estimatedOplogSize"_attr = oplogSize,
                "minBytesPerMarker"_attr = minBytesPerTruncateMarker);
    setMinBytesPerMarker(minBytesPerTruncateMarker);
}

void OplogTruncateMarkers::adjust(RecordStore& rs) {
    recomputeMinBytesPerMarker(rs);

    // Notify the reclaimer thread as there might be an opportunity to recover space.
    _reclaimCv.notify_all();
}

}  // namespace mongo
