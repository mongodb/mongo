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
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

ReplicationRecoveryImpl::ReplicationRecoveryImpl(StorageInterface* storageInterface,
                                                 ReplicationConsistencyMarkers* consistencyMarkers)
    : _storageInterface(storageInterface), _consistencyMarkers(consistencyMarkers) {}

void ReplicationRecoveryImpl::recoverFromOplog(OperationContext* opCtx) {
    if (_consistencyMarkers->getInitialSyncFlag(opCtx)) {
        log() << "No recovery needed. Initial sync flag set.";
        return;  // Initial Sync will take over so no cleanup is needed.
    }

    const auto truncateAfterPoint = _consistencyMarkers->getOplogTruncateAfterPoint(opCtx);
    const auto appliedThrough = _consistencyMarkers->getAppliedThrough(opCtx);

    const bool needToDeleteEndOfOplog = !truncateAfterPoint.isNull() &&
        // This version should never have a non-null truncateAfterPoint with a null appliedThrough.
        // This scenario means that we downgraded after unclean shutdown, then the downgraded node
        // deleted the ragged end of our oplog, then did a clean shutdown.
        !appliedThrough.isNull() &&
        // Similarly we should never have an appliedThrough higher than the truncateAfterPoint. This
        // means that the downgraded node deleted our ragged end then applied ahead of our
        // truncateAfterPoint and then had an unclean shutdown before upgrading. We are ok with
        // applying these ops because older versions wrote to the oplog from a single thread so we
        // know they are in order.
        !(appliedThrough.getTimestamp() >= truncateAfterPoint);
    if (needToDeleteEndOfOplog) {
        log() << "Removing unapplied entries starting at: " << truncateAfterPoint.toBSON();
        _truncateOplogTo(opCtx, truncateAfterPoint);
    }
    _consistencyMarkers->setOplogTruncateAfterPoint(opCtx, {});  // clear the truncateAfterPoint

    if (appliedThrough.isNull()) {
        log() << "No oplog entries to apply for recovery. appliedThrough is null.";
        // No follow-up work to do.
        return;
    }

    // Check if we have any unapplied ops in our oplog. It is important that this is done after
    // deleting the ragged end of the oplog.
    const auto topOfOplog = fassertStatusOK(40290, _getLastAppliedOpTime(opCtx));
    if (appliedThrough == topOfOplog) {
        log()
            << "No oplog entries to apply for recovery. appliedThrough is at the top of the oplog.";
        return;  // We've applied all the valid oplog we have.
    } else if (appliedThrough > topOfOplog) {
        severe() << "Applied op " << appliedThrough << " not found. Top of oplog is " << topOfOplog
                 << '.';
        fassertFailedNoTrace(40313);
    }

    log() << "Replaying stored operations from " << appliedThrough << " (exclusive) to "
          << topOfOplog << " (inclusive).";

    DBDirectClient db(opCtx);
    auto cursor = db.query(NamespaceString::kRsOplogNamespace.ns(),
                           QUERY("ts" << BSON("$gte" << appliedThrough.getTimestamp())),
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
        severe() << "Couldn't find any entries in the oplog >= " << appliedThrough
                 << " which should be impossible.";
        fassertFailedNoTrace(40293);
    }
    auto firstOpTimeFound = fassertStatusOK(40291, OpTime::parseFromOplogEntry(cursor->nextSafe()));
    if (firstOpTimeFound != appliedThrough) {
        severe() << "Oplog entry at " << appliedThrough << " is missing; actual entry found is "
                 << firstOpTimeFound;
        fassertFailedNoTrace(40292);
    }

    // Apply remaining ops one at at time, but don't log them because they are already logged.
    UnreplicatedWritesBlock uwb(opCtx);

    while (cursor->more()) {
        auto entry = cursor->nextSafe();
        fassertStatusOK(40294, SyncTail::syncApply(opCtx, entry, true));
        _consistencyMarkers->setAppliedThrough(
            opCtx, fassertStatusOK(40295, OpTime::parseFromOplogEntry(entry)));
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
