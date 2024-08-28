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

#include <memory>

#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/wiredtiger/oplog_truncate_marker_parameters_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_truncate_markers.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_options.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/concurrency/idle_thread_block.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace {
const double kNumMSInHour = 1000 * 60 * 60;
}

std::shared_ptr<WiredTigerOplogTruncateMarkers>
WiredTigerOplogTruncateMarkers::createOplogTruncateMarkers(OperationContext* opCtx,
                                                           WiredTigerRecordStore* rs,
                                                           const NamespaceString& ns) {
    long long maxSize = rs->getOplogMaxSize();
    invariant(maxSize > 0);
    invariant(rs->keyFormat() == KeyFormat::Long);

    // The minimum oplog truncate marker size should be BSONObjMaxInternalSize.
    const unsigned int oplogTruncateMarkerSize =
        std::max(gOplogTruncateMarkerSizeMB * 1024 * 1024, BSONObjMaxInternalSize);

    // IDL does not support unsigned long long types.
    const unsigned long long kMinTruncateMarkersToKeep =
        static_cast<unsigned long long>(gMinOplogTruncateMarkers);
    const unsigned long long kMaxTruncateMarkersToKeep =
        static_cast<unsigned long long>(gMaxOplogTruncateMarkersDuringStartup);

    unsigned long long numTruncateMarkers = maxSize / oplogTruncateMarkerSize;
    size_t numTruncateMarkersToKeep = std::min(
        kMaxTruncateMarkersToKeep, std::max(kMinTruncateMarkersToKeep, numTruncateMarkers));
    auto minBytesPerTruncateMarker = maxSize / numTruncateMarkersToKeep;
    uassert(7206300,
            fmt::format("Cannot create oplog of size less than {} bytes", numTruncateMarkersToKeep),
            minBytesPerTruncateMarker > 0);

    // We need to read the whole oplog, override the recoveryUnit's oplogVisibleTimestamp.
    ScopedOplogVisibleTimestamp scopedOplogVisibleTimestamp(
        shard_role_details::getRecoveryUnit(opCtx), boost::none);
    UnyieldableCollectionIterator iterator(opCtx, rs);
    auto initialSetOfMarkers = CollectionTruncateMarkers::createFromCollectionIterator(
        opCtx,
        iterator,
        ns,
        minBytesPerTruncateMarker,
        [](const Record& record) {
            BSONObj obj = record.data.toBson();
            auto wallTime = obj.hasField(repl::DurableOplogEntry::kWallClockTimeFieldName)
                ? obj[repl::DurableOplogEntry::kWallClockTimeFieldName].Date()
                : obj[repl::DurableOplogEntry::kTimestampFieldName].timestampTime();
            return RecordIdAndWallTime(record.id, wallTime);
        },
        numTruncateMarkersToKeep);
    LOGV2(22382,
          "WiredTiger record store oplog processing finished",
          "duration"_attr = duration_cast<Milliseconds>(initialSetOfMarkers.timeTaken));
    return std::make_shared<WiredTigerOplogTruncateMarkers>(
        std::move(initialSetOfMarkers.markers),
        initialSetOfMarkers.leftoverRecordsCount,
        initialSetOfMarkers.leftoverRecordsBytes,
        minBytesPerTruncateMarker,
        initialSetOfMarkers.timeTaken,
        initialSetOfMarkers.methodUsed,
        rs);
}

WiredTigerOplogTruncateMarkers::WiredTigerOplogTruncateMarkers(
    std::deque<CollectionTruncateMarkers::Marker> markers,
    int64_t partialMarkerRecords,
    int64_t partialMarkerBytes,
    int64_t minBytesPerMarker,
    Microseconds totalTimeSpentBuilding,
    CollectionTruncateMarkers::MarkersCreationMethod creationMethod,
    WiredTigerRecordStore* rs)
    : CollectionTruncateMarkers(
          std::move(markers), partialMarkerRecords, partialMarkerBytes, minBytesPerMarker),
      _rs(rs),
      _totalTimeProcessing(totalTimeSpentBuilding),
      _processBySampling(creationMethod ==
                         CollectionTruncateMarkers::MarkersCreationMethod::Sampling) {}

bool WiredTigerOplogTruncateMarkers::isDead() {
    stdx::lock_guard<Latch> lk(_reclaimMutex);
    return _isDead;
}

void WiredTigerOplogTruncateMarkers::kill() {
    stdx::lock_guard<Latch> lk(_reclaimMutex);
    _isDead = true;
    _reclaimCv.notify_one();
}

void WiredTigerOplogTruncateMarkers::clearMarkersOnCommit(OperationContext* opCtx) {
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

void WiredTigerOplogTruncateMarkers::updateMarkersAfterCappedTruncateAfter(
    int64_t recordsRemoved, int64_t bytesRemoved, const RecordId& firstRemovedId) {
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

void WiredTigerOplogTruncateMarkers::getOplogTruncateMarkersStats(BSONObjBuilder& builder) const {
    builder.append("totalTimeProcessingMicros", _totalTimeProcessing.count());
    builder.append("processingMethod", _processBySampling ? "sampling" : "scanning");
    if (auto oplogMinRetentionHours = storageGlobalParams.oplogMinRetentionHours.load()) {
        builder.append("oplogMinRetentionHours", oplogMinRetentionHours);
    }
}

bool WiredTigerOplogTruncateMarkers::awaitHasExcessMarkersOrDead(OperationContext* opCtx) {
    // Wait until kill() is called or there are too many collection markers.
    stdx::unique_lock<Latch> lock(_reclaimMutex);
    MONGO_IDLE_THREAD_BLOCK;
    auto isWaitConditionSatisfied = opCtx->waitForConditionOrInterruptFor(
        _reclaimCv, lock, Seconds(gOplogTruncationCheckPeriodSeconds), [this, opCtx] {
            if (_isDead) {
                return true;
            }

            if (auto marker = peekOldestMarkerIfNeeded(opCtx)) {
                invariant(marker->lastRecord.isValid());

                LOGV2_DEBUG(7393215,
                            2,
                            "Collection has excess markers",
                            "lastRecord"_attr = marker->lastRecord,
                            "wallTime"_attr = marker->wallTime);
                return true;
            }

            return false;
        });

    // Return true only when we have detected excess markers, not because the record store
    // is being destroyed (_isDead) or we timed out waiting on the condition variable.
    return !(_isDead || !isWaitConditionSatisfied);
}

bool WiredTigerOplogTruncateMarkers::_hasExcessMarkers(OperationContext* opCtx) const {
    int64_t totalBytes = 0;
    for (const auto& marker : getMarkers()) {
        totalBytes += marker.bytes;
    }

    // check that oplog truncate markers is at capacity
    if (totalBytes <= _rs->getOplogMaxSize()) {
        return false;
    }

    const auto& truncateMarker = getMarkers().front();

    // The pinned oplog is inside the earliest marker, so we cannot remove the marker range.
    if (static_cast<std::uint64_t>(truncateMarker.lastRecord.getLong()) >=
        _rs->getPinnedOplog().asULL()) {
        return false;
    }

    double minRetentionHours = storageGlobalParams.oplogMinRetentionHours.load();

    // If we are not checking for time, then yes, there is a truncate marker to be reaped
    // because oplog is at capacity.
    if (minRetentionHours == 0.0) {
        return true;
    }

    auto nowWall = Date_t::now();
    auto lastTruncateMarkerWallTime = truncateMarker.wallTime;

    auto currRetentionMS = durationCount<Milliseconds>(nowWall - lastTruncateMarkerWallTime);
    double currRetentionHours = currRetentionMS / kNumMSInHour;
    return currRetentionHours >= minRetentionHours;
}

void WiredTigerOplogTruncateMarkers::adjust(OperationContext* opCtx, int64_t maxSize) {
    const unsigned int oplogTruncateMarkerSize =
        std::max(gOplogTruncateMarkerSizeMB * 1024 * 1024, BSONObjMaxInternalSize);

    // IDL does not support unsigned long long types.
    const unsigned long long kMinTruncateMarkersToKeep =
        static_cast<unsigned long long>(gMinOplogTruncateMarkers);
    const unsigned long long kMaxTruncateMarkersToKeep =
        static_cast<unsigned long long>(gMaxOplogTruncateMarkersAfterStartup);

    unsigned long long numTruncateMarkers = maxSize / oplogTruncateMarkerSize;
    size_t numTruncateMarkersToKeep = std::min(
        kMaxTruncateMarkersToKeep, std::max(kMinTruncateMarkersToKeep, numTruncateMarkers));
    setMinBytesPerMarker(maxSize / numTruncateMarkersToKeep);
    // Notify the reclaimer thread as there might be an opportunity to recover space.
    _reclaimCv.notify_all();
}

}  // namespace mongo
