/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_recovery.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/session.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {

/**
 * Returns the timestamp at which we should start oplog application. Returns boost::none if
 * there are no oplog entries to apply.
 */
boost::optional<Timestamp> _getOplogApplicationStartPoint(Timestamp checkpointTimestamp,
                                                          OpTime appliedThrough) {
    if (!checkpointTimestamp.isNull() && !appliedThrough.isNull()) {
        // In versions that support "recover to stable timestamp" you should never see a
        // non-null appliedThrough in a checkpoint, since we never take checkpoints in the middle
        // of a secondary batch application, and a node that does not support "recover to stable
        // timestamp" should never see a non-null checkpointTimestamp.
        severe() << "checkpointTimestamp (" << checkpointTimestamp.toBSON()
                 << ") and appliedThrough (" << appliedThrough << ") cannot both be non-null.";
        fassertFailedNoTrace(40603);

    } else if (!checkpointTimestamp.isNull()) {
        // If appliedThrough is null and the checkpointTimestamp is not null, then we recovered
        // to a checkpoint and should use that checkpoint timestamp as the oplog application
        // start point.
        log() << "Starting recovery oplog application at the checkpointTimestamp: "
              << checkpointTimestamp.toBSON();
        return checkpointTimestamp;

    } else if (!appliedThrough.isNull()) {
        // If the checkpointTimestamp is null and the appliedThrough is not null, then we did not
        // recover to a checkpoint and we should use the appliedThrough as the oplog application
        // start point.
        log() << "Starting recovery oplog application at the appliedThrough: " << appliedThrough;
        return appliedThrough.getTimestamp();

    } else {
        log() << "No oplog entries to apply for recovery. appliedThrough and "
                 "checkpointTimestamp are both null.";
        // No follow-up work to do.
        return boost::none;
    }
    MONGO_UNREACHABLE;
}

}  // namespace

ReplicationRecoveryImpl::ReplicationRecoveryImpl(StorageInterface* storageInterface,
                                                 ReplicationConsistencyMarkers* consistencyMarkers)
    : _storageInterface(storageInterface), _consistencyMarkers(consistencyMarkers) {}

void ReplicationRecoveryImpl::recoverFromOplog(OperationContext* opCtx) try {
    if (_consistencyMarkers->getInitialSyncFlag(opCtx)) {
        log() << "No recovery needed. Initial sync flag set.";
        return;  // Initial Sync will take over so no cleanup is needed.
    }

    const auto truncateAfterPoint = _consistencyMarkers->getOplogTruncateAfterPoint(opCtx);
    const auto appliedThrough = _consistencyMarkers->getAppliedThrough(opCtx);

    if (!truncateAfterPoint.isNull()) {
        log() << "Removing unapplied entries starting at: " << truncateAfterPoint.toBSON();
        _truncateOplogTo(opCtx, truncateAfterPoint);
    }

    // Clear the truncateAfterPoint so that we don't truncate the next batch of oplog entries
    // erroneously.
    _consistencyMarkers->setOplogTruncateAfterPoint(opCtx, {});

    // TODO (SERVER-30556): Delete this line since the old oplog delete from point cannot exist.
    _consistencyMarkers->removeOldOplogDeleteFromPointField(opCtx);

    auto topOfOplogSW = _getLastAppliedOpTime(opCtx);
    boost::optional<OpTime> topOfOplog = boost::none;
    if (topOfOplogSW.getStatus() != ErrorCodes::CollectionIsEmpty &&
        topOfOplogSW.getStatus() != ErrorCodes::NamespaceNotFound) {
        fassertStatusOK(40290, topOfOplogSW);
        topOfOplog = topOfOplogSW.getValue();
    }

    // If we have a checkpoint timestamp, then we recovered to a timestamp and should set the
    // initial data timestamp to that. Otherwise, we simply recovered the data on disk so we should
    // set the initial data timestamp to the top OpTime in the oplog once the data is consistent
    // there. If there is nothing in the oplog, then we do not set the initial data timestamp.
    auto checkpointTimestamp = _consistencyMarkers->getCheckpointTimestamp(opCtx);
    if (!checkpointTimestamp.isNull()) {

        // If we have a checkpoint timestamp, we set the initial data timestamp now so that
        // the operations we apply below can be given the proper timestamps.
        _storageInterface->setInitialDataTimestamp(opCtx->getServiceContext(), checkpointTimestamp);
    }

    // Oplog is empty. There are no oplog entries to apply, so we exit recovery. If there was a
    // checkpointTimestamp then we already set the initial data timestamp. Otherwise, there is
    // nothing to set it to.
    if (!topOfOplog) {
        log() << "No oplog entries to apply for recovery. Oplog is empty.";
        return;
    }

    if (auto startPoint = _getOplogApplicationStartPoint(checkpointTimestamp, appliedThrough)) {
        // When `recoverFromOplog` truncates the oplog, that also happens to set the "oldest
        // timestamp" to the truncation point[1]. `_applyToEndOfOplog` will then perform writes
        // before the truncation point. Doing so violates the constraint that all updates must be
        // timestamped newer than the "oldest timestamp". This call will move the "oldest
        // timestamp" back to the `startPoint`.
        //
        // [1] This is arguably incorrect. On rollback for nodes that are not keeping history to
        // the "majority point", the "oldest timestamp" likely needs to go back in time. The
        // oplog's `cappedTruncateAfter` method was a convenient location for this logic, which,
        // unfortunately, conflicts with the usage above.
        opCtx->getServiceContext()->getGlobalStorageEngine()->setOldestTimestamp(startPoint.get());
        _applyToEndOfOplog(opCtx, startPoint.get(), topOfOplog->getTimestamp());
    }

    // If we don't have a checkpoint timestamp, then we are either not running a storage engine
    // that supports "recover to stable timestamp" or we just upgraded from a version that didn't.
    // In both cases, the data on disk is not consistent until we have applied all oplog entries to
    // the end of the oplog, since we do not know which ones actually got applied before shutdown.
    // As a result, we do not set the initial data timestamp until after we have applied to the end
    // of the oplog.
    if (checkpointTimestamp.isNull()) {
        _storageInterface->setInitialDataTimestamp(opCtx->getServiceContext(),
                                                   topOfOplog->getTimestamp());
    }

} catch (...) {
    severe() << "Caught exception during replication recovery: " << exceptionToStatus();
    std::terminate();
}

void ReplicationRecoveryImpl::_applyToEndOfOplog(OperationContext* opCtx,
                                                 const Timestamp& oplogApplicationStartPoint,
                                                 const Timestamp& topOfOplog) {
    invariant(!oplogApplicationStartPoint.isNull());
    invariant(!topOfOplog.isNull());

    // Check if we have any unapplied ops in our oplog. It is important that this is done after
    // deleting the ragged end of the oplog.
    if (oplogApplicationStartPoint == topOfOplog) {
        log()
            << "No oplog entries to apply for recovery. appliedThrough is at the top of the oplog.";
        return;  // We've applied all the valid oplog we have.
    } else if (oplogApplicationStartPoint > topOfOplog) {
        severe() << "Applied op " << oplogApplicationStartPoint.toBSON()
                 << " not found. Top of oplog is " << topOfOplog.toBSON() << '.';
        fassertFailedNoTrace(40313);
    }

    log() << "Replaying stored operations from " << oplogApplicationStartPoint.toBSON()
          << " (exclusive) to " << topOfOplog.toBSON() << " (inclusive).";

    DBDirectClient db(opCtx);
    auto cursor = db.query(NamespaceString::kRsOplogNamespace.ns(),
                           QUERY("ts" << BSON("$gte" << oplogApplicationStartPoint)),
                           /*batchSize*/ 0,
                           /*skip*/ 0,
                           /*projection*/ nullptr,
                           QueryOption_OplogReplay);

    // Check that the first document matches our appliedThrough point then skip it since it's
    // already been applied.
    if (!cursor->more()) {
        // This should really be impossible because we check above that the top of the oplog is
        // strictly > appliedThrough. If this fails it represents a serious bug in either the
        // storage engine or query's implementation of OplogReplay.
        severe() << "Couldn't find any entries in the oplog >= "
                 << oplogApplicationStartPoint.toBSON() << " which should be impossible.";
        fassertFailedNoTrace(40293);
    }

    auto firstTimestampFound =
        fassertStatusOK(40291, OpTime::parseFromOplogEntry(cursor->nextSafe())).getTimestamp();
    if (firstTimestampFound != oplogApplicationStartPoint) {
        severe() << "Oplog entry at " << oplogApplicationStartPoint.toBSON()
                 << " is missing; actual entry found is " << firstTimestampFound.toBSON();
        fassertFailedNoTrace(40292);
    }

    // Apply remaining ops one at at time, but don't log them because they are already logged.
    UnreplicatedWritesBlock uwb(opCtx);

    OpTime appliedThrough;
    while (cursor->more()) {
        auto entry = cursor->nextSafe();
        fassertStatusOK(40294,
                        SyncTail::syncApply(opCtx, entry, OplogApplication::Mode::kRecovering));

        auto oplogEntryStatus = OplogEntry::parse(entry);
        if (!oplogEntryStatus.isOK()) {
            fassertFailedWithStatus(50763, oplogEntryStatus.getStatus());
        }

        auto oplogEntry = oplogEntryStatus.getValue();
        if (auto txnTableOplog = Session::createMatchingTransactionTableUpdate(oplogEntry)) {
            fassert(50764,
                    SyncTail::syncApply(
                        opCtx, txnTableOplog->toBSON(), OplogApplication::Mode::kRecovering));
        }

        appliedThrough = fassertStatusOK(40295, OpTime::parseFromOplogEntry(entry));
        _consistencyMarkers->setAppliedThrough(opCtx, appliedThrough);
    }
    if (appliedThrough.getTimestamp() != topOfOplog) {
        severe() << "Did not apply to top of oplog. Applied through: " << appliedThrough.toString()
                 << ". Top of oplog: " << topOfOplog.toString();
        MONGO_UNREACHABLE;
    }
}

StatusWith<OpTime> ReplicationRecoveryImpl::_getLastAppliedOpTime(OperationContext* opCtx) const {
    const auto docsSW = _storageInterface->findDocuments(opCtx,
                                                         NamespaceString::kRsOplogNamespace,
                                                         boost::none,  // Collection scan
                                                         StorageInterface::ScanDirection::kBackward,
                                                         {},
                                                         BoundInclusion::kIncludeStartKeyOnly,
                                                         1U);
    if (!docsSW.isOK()) {
        return docsSW.getStatus();
    }
    const auto docs = docsSW.getValue();
    if (docs.empty()) {
        return Status(ErrorCodes::CollectionIsEmpty, "oplog is empty");
    }
    invariant(1U == docs.size());

    return OpTime::parseFromOplogEntry(docs.front());
}

void ReplicationRecoveryImpl::_truncateOplogTo(OperationContext* opCtx,
                                               Timestamp truncateTimestamp) {
    const NamespaceString oplogNss(NamespaceString::kRsOplogNamespace);
    AutoGetDb autoDb(opCtx, oplogNss.db(), MODE_IX);
    Lock::CollectionLock oplogCollectionLoc(opCtx->lockState(), oplogNss.ns(), MODE_X);
    Collection* oplogCollection = autoDb.getDb()->getCollection(opCtx, oplogNss);
    if (!oplogCollection) {
        fassertFailedWithStatusNoTrace(
            34418,
            Status(ErrorCodes::NamespaceNotFound,
                   str::stream() << "Can't find " << NamespaceString::kRsOplogNamespace.ns()));
    }

    // Scan through oplog in reverse, from latest entry to first, to find the truncateTimestamp.
    RecordId oldestIDToDelete;  // Non-null if there is something to delete.
    auto oplogRs = oplogCollection->getRecordStore();
    auto oplogReverseCursor = oplogRs->getCursor(opCtx, /*forward=*/false);
    size_t count = 0;
    while (auto next = oplogReverseCursor->next()) {
        const BSONObj entry = next->data.releaseToBson();
        const RecordId id = next->id;
        count++;

        const auto tsElem = entry["ts"];
        if (count == 1) {
            if (tsElem.eoo())
                LOG(2) << "Oplog tail entry: " << redact(entry);
            else
                LOG(2) << "Oplog tail entry ts field: " << tsElem;
        }

        if (tsElem.timestamp() < truncateTimestamp) {
            // If count == 1, that means that we have nothing to delete because everything in the
            // oplog is < truncateTimestamp.
            if (count != 1) {
                invariant(!oldestIDToDelete.isNull());
                oplogCollection->cappedTruncateAfter(opCtx, oldestIDToDelete, /*inclusive=*/true);
            }
            return;
        }

        oldestIDToDelete = id;
    }

    severe() << "Reached end of oplog looking for oplog entry before " << truncateTimestamp.toBSON()
             << " but couldn't find any after looking through " << count << " entries.";
    fassertFailedNoTrace(40296);
}

}  // namespace repl
}  // namespace mongo
