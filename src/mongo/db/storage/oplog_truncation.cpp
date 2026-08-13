// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/oplog_truncation.h"

#include "mongo/db/repl/local_oplog_info.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_uncommitted_changes.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/storage/collection_truncate_markers.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::oplog_truncation {

bool checkOplogTruncationBounds(OperationContext* opCtx,
                                RecordStore& oplog,
                                const CollectionTruncateMarkers::Marker& marker,
                                RecordId mayTruncateUpTo) {
    auto seekableCursor =
        oplog.oplog()->getRawCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    auto firstRecordSeekable = seekableCursor->next();
    if (!firstRecordSeekable) {
        LOGV2_WARNING(8841100, "The oplog is empty, there is nothing to truncate");
        return false;
    }
    auto firstRecordId = firstRecordSeekable.get().id;

    LOGV2_DEBUG(7420100,
                1,
                "Assessing oplog truncation bounds",
                "firstRecordId"_attr = firstRecordId,
                "lastRecord"_attr = marker.lastRecord,
                "numRecords"_attr = marker.records,
                "numBytes"_attr = marker.bytes);

    // The first record in the oplog should be within the truncate range.
    if (firstRecordId > marker.lastRecord) {
        LOGV2_WARNING(7420101,
                      "First oplog record is after truncation range",
                      "firstRecord"_attr = firstRecordId,
                      "lastRecord"_attr = marker.lastRecord,
                      "numRecords"_attr = marker.records,
                      "numBytes"_attr = marker.bytes);
    }

    // It is necessary that there exists a record after the truncate marker but before
    // or including the mayTruncateUpTo point.  Since the mayTruncateUpTo point may fall
    // between records, the truncate marker check is not sufficient.
    auto nextRecordAfterTruncateMarker =
        seekableCursor->seek(marker.lastRecord, SeekableRecordCursor::BoundInclusion::kExclude);
    if (!nextRecordAfterTruncateMarker) {
        LOGV2(5140900,
              "Assessed oplog truncation bound: Cannot truncate entire oplog",
              "firstRecord"_attr = firstRecordId,
              "lastRecord"_attr = marker.lastRecord,
              "numRecords"_attr = marker.records,
              "numBytes"_attr = marker.bytes);
        return false;
    }

    if (nextRecordAfterTruncateMarker->id > mayTruncateUpTo) {
        LOGV2(5140901,
              "Assessed oplog truncation bound: Cannot truncate, there are no oplog entries after "
              "the marker but before the truncate-up-to point",
              "firstRecord"_attr = firstRecordId,
              "lastRecord"_attr = marker.lastRecord,
              "nextRecordAfterMarker"_attr = nextRecordAfterTruncateMarker->id,
              "mayTruncateUpTo"_attr = mayTruncateUpTo);
        return false;
    }
    return true;
}

RecordId truncateByMarkerQueue(OperationContext* opCtx,
                               RecordStore& oplog,
                               RecordId mayTruncateUpTo,
                               TruncateFn truncateFn) {
    RecordId highestTruncated;
    for (auto getNextMarker = true; getNextMarker;) {
        auto truncateMarkers = LocalOplogInfo::get(opCtx)->getTruncateMarkers();
        auto truncateMarker = truncateMarkers->peekOldestMarkerIfNeeded(opCtx);
        if (!truncateMarker) {
            break;
        }
        invariant(truncateMarker->lastRecord.isValid());

        getNextMarker =
            writeConflictRetry(opCtx, "reclaimOplog", NamespaceString::kRsOplogNamespace, [&] {
                if (!truncateFn(opCtx, *truncateMarker)) {
                    return false;
                }

                // Remove the truncate marker after a successful truncation.
                truncateMarkers->popOldestMarker();

                // Update the current first record as we truncate.
                highestTruncated = truncateMarker->lastRecord;
                return true;
            });
    }

    return highestTruncated;
}

namespace {

bool performUnreplicatedTruncate(OperationContext* opCtx,
                                 RecordStore& oplog,
                                 RecordId mayTruncateUpTo,
                                 const CollectionTruncateMarkers::Marker& marker) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    StorageWriteTransaction txn(ru);

    if (!checkOplogTruncationBounds(opCtx, oplog, marker, mayTruncateUpTo)) {
        return false;
    }

    auto status = oplog.rangeTruncate(
        opCtx, ru, RecordId(), marker.lastRecord, -marker.bytes, -marker.records);

    if (!status.isOK()) {
        LOGV2_WARNING(8841101,
                      "Did not successfully perform range truncation ",
                      "truncateMarkerLastRecord"_attr = marker.lastRecord.getLong(),
                      "error"_attr = status);
        return false;
    }

    // The replicated truncate path's truncate API handles updating the replicated fast count
    // internally. rangeTruncate does not, so we update it manually here.
    if (isReplicatedFastCountEnabled(opCtx)) {
        const boost::optional<UUID> uuid = oplog.uuid();
        invariant(uuid.has_value());
        UncommittedFastCountChange::getForWrite(opCtx).record(
            NamespaceString::kRsOplogNamespace, *uuid, -marker.records, -marker.bytes);
    }

    txn.commit();
    return true;
}

}  // namespace

RecordId reclaimOplog(OperationContext* opCtx, RecordStore& oplog, RecordId mayTruncateUpTo) {
    return truncateByMarkerQueue(
        opCtx,
        oplog,
        mayTruncateUpTo,
        [&oplog, mayTruncateUpTo](OperationContext* opCtx,
                                  const CollectionTruncateMarkers::Marker& marker) {
            return performUnreplicatedTruncate(opCtx, oplog, mayTruncateUpTo, marker);
        });
}

Timestamp computeTruncationBound(OperationContext* opCtx) {
    const Timestamp ts = opCtx->getServiceContext()->getStorageEngine()->getPinnedOplog();
    if (isReplicatedFastCountEnabled(opCtx)) {
        Lock::GlobalLock readLock(opCtx, MODE_IS);
        auto persistedTs = writeConflictRetry(
            opCtx, "computeTruncationBound", NamespaceString::kRsOplogNamespace, [&] {
                return replicated_fast_count::ReplicatedFastCountManager::get(
                           opCtx->getServiceContext())
                    .findPersistedTimestampStoreTs(opCtx)
                    .value_or(Timestamp::min());
            });
        if (persistedTs < ts) {
            LOGV2(13258300,
                  "Using replicated size and count persisted timestamp as exclusive oplog "
                  "truncation bound",
                  "persistedTs"_attr = persistedTs,
                  "pinnedOplogTs"_attr = ts);
            return persistedTs;
        }
    }
    return ts;
}

}  // namespace mongo::oplog_truncation
