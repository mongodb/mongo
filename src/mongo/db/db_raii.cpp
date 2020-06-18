/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/db_raii.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/storage/snapshot_helper.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

const boost::optional<int> kDoNotChangeProfilingLevel = boost::none;

// TODO: SERVER-44105 remove
// If set to false, secondary reads should wait behind the PBW lock.
// Does nothing if gAllowSecondaryReadsDuringBatchApplication setting is false.
const auto allowSecondaryReadsDuringBatchApplication_DONT_USE =
    OperationContext::declareDecoration<boost::optional<bool>>();

}  // namespace

AutoStatsTracker::AutoStatsTracker(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   Top::LockType lockType,
                                   LogMode logMode,
                                   int dbProfilingLevel,
                                   Date_t deadline)
    : _opCtx(opCtx), _lockType(lockType), _nss(nss), _logMode(logMode) {
    if (_logMode == LogMode::kUpdateTop) {
        return;
    }

    stdx::lock_guard<Client> clientLock(*_opCtx->getClient());
    CurOp::get(_opCtx)->enter_inlock(_nss.ns().c_str(), dbProfilingLevel);
}

AutoStatsTracker::~AutoStatsTracker() {
    if (_logMode == LogMode::kUpdateCurOp) {
        return;
    }

    auto curOp = CurOp::get(_opCtx);
    Top::get(_opCtx->getServiceContext())
        .record(_opCtx,
                _nss.ns(),
                curOp->getLogicalOp(),
                _lockType,
                durationCount<Microseconds>(curOp->elapsedTimeExcludingPauses()),
                curOp->isCommand(),
                curOp->getReadWriteType());
}

AutoGetCollectionForRead::AutoGetCollectionForRead(OperationContext* opCtx,
                                                   const NamespaceStringOrUUID& nsOrUUID,
                                                   AutoGetCollection::ViewMode viewMode,
                                                   Date_t deadline) {
    // Don't take the ParallelBatchWriterMode lock when the server parameter is set and our
    // storage engine supports snapshot reads.
    if (gAllowSecondaryReadsDuringBatchApplication.load() &&
        allowSecondaryReadsDuringBatchApplication_DONT_USE(opCtx).value_or(true) &&
        opCtx->getServiceContext()->getStorageEngine()->supportsReadConcernSnapshot()) {
        _shouldNotConflictWithSecondaryBatchApplicationBlock.emplace(opCtx->lockState());
    }
    const auto collectionLockMode = getLockModeForQuery(opCtx, nsOrUUID.nss());
    _autoColl.emplace(opCtx, nsOrUUID, collectionLockMode, viewMode, deadline);

    // If the read source is explicitly set to kNoTimestamp, we read the most up to date data and do
    // not consider changing our ReadSource (e.g. FTDC needs that).
    if (opCtx->recoveryUnit()->getTimestampReadSource() == RecoveryUnit::ReadSource::kNoTimestamp)
        return;

    repl::ReplicationCoordinator* const replCoord = repl::ReplicationCoordinator::get(opCtx);
    const auto readConcernLevel = repl::ReadConcernArgs::get(opCtx).getLevel();

    // If the collection doesn't exist or disappears after releasing locks and waiting, there is no
    // need to check for pending catalog changes.
    while (auto coll = _autoColl->getCollection()) {

        // TODO(SERVER-47824): Also ban transaction snapshot reads on capped collections.
        uassert(ErrorCodes::SnapshotUnavailable,
                "Reading from capped collections with readConcern snapshot is not supported "
                "outside of transactions",
                !coll->isCapped() ||
                    readConcernLevel != repl::ReadConcernLevel::kSnapshotReadConcern ||
                    opCtx->inMultiDocumentTransaction());

        // During batch application on secondaries, there is a potential to read inconsistent states
        // that would normally be protected by the PBWM lock. In order to serve secondary reads
        // during this period, we default to not acquiring the lock (by setting
        // _shouldNotConflictWithSecondaryBatchApplicationBlock). On primaries, we always read at a
        // consistent time, so not taking the PBWM lock is not a problem. On secondaries, we have to
        // guarantee we read at a consistent state, so we must read at the lastApplied timestamp,
        // which is set after each complete batch.
        //
        // If an attempt to read at the lastApplied timestamp is unsuccessful because there are
        // pending catalog changes that occur after that timestamp, we release our locks and try
        // again with the PBWM lock (by unsetting
        // _shouldNotConflictWithSecondaryBatchApplicationBlock).

        const NamespaceString nss = coll->ns();
        auto readSource = opCtx->recoveryUnit()->getTimestampReadSource();

        // Once we have our locks, check whether or not we should override the ReadSource that was
        // set before acquiring locks.
        if (auto newReadSource = SnapshotHelper::getNewReadSource(opCtx, nss)) {
            opCtx->recoveryUnit()->setTimestampReadSource(*newReadSource);
            readSource = *newReadSource;
        }

        const auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp();
        const auto afterClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAfterClusterTime();
        if (readTimestamp && afterClusterTime) {
            // Readers that use afterClusterTime have already waited at a higher level for the
            // all_durable time to advance to a specified optime, and they assume the read timestamp
            // of the operation is at least that waited-for timestamp. For kNoOverlap, which is
            // the minimum of lastApplied and all_durable, this invariant ensures that
            // afterClusterTime reads do not choose a read timestamp older than the one requested.
            invariant(*readTimestamp >= afterClusterTime->asTimestamp(),
                      str::stream() << "read timestamp " << readTimestamp->toString()
                                    << "was less than afterClusterTime: "
                                    << afterClusterTime->asTimestamp().toString());
        }

        auto minSnapshot = coll->getMinimumVisibleSnapshot();
        if (!SnapshotHelper::collectionChangesConflictWithRead(minSnapshot, readTimestamp)) {
            return;
        }

        // If we are reading at a provided timestamp earlier than the latest catalog changes,
        // then we must return an error.
        if (readSource == RecoveryUnit::ReadSource::kProvided) {
            uasserted(ErrorCodes::SnapshotUnavailable,
                      str::stream()
                          << "Unable to read from a snapshot due to pending collection catalog "
                             "changes; please retry the operation. Snapshot timestamp is "
                          << readTimestamp->toString() << ". Collection minimum is "
                          << minSnapshot->toString());
        }

        invariant(
            // The kMajorityCommitted and kLastApplied read sources already read from timestamps
            // that are safe with respect to concurrent secondary batch application, and are
            // eligible for retrying.
            readSource == RecoveryUnit::ReadSource::kMajorityCommitted ||
            readSource == RecoveryUnit::ReadSource::kNoOverlap ||
            readSource == RecoveryUnit::ReadSource::kLastApplied);
        invariant(readConcernLevel != repl::ReadConcernLevel::kSnapshotReadConcern);

        // Yield locks in order to do the blocking call below.
        _autoColl = boost::none;

        // If there are pending catalog changes when using a no-overlap or lastApplied read source,
        // we choose to take the PBWM lock to conflict with any in-progress batches. This prevents
        // us from idly spinning in this loop trying to get a new read timestamp ahead of the
        // minimum visible snapshot. This helps us guarantee liveness (i.e. we can eventually get a
        // suitable read timestamp) but should not be necessary for correctness. After initial sync
        // finishes, if we waited instead of retrying, readers would block indefinitely waiting for
        // their read timstamp to move forward. Instead we force the reader take the PBWM lock and
        // retry.
        if (readSource == RecoveryUnit::ReadSource::kLastApplied ||
            readSource == RecoveryUnit::ReadSource::kNoOverlap) {
            invariant(readTimestamp);
            LOGV2(20576,
                  "Tried reading at a timestamp, but future catalog changes are pending. "
                  "Trying again without reading at a timestamp",
                  "readTimestamp"_attr = *readTimestamp,
                  "collection"_attr = nss.ns(),
                  "collectionMinSnapshot"_attr = *minSnapshot);
            // Destructing the block sets _shouldConflictWithSecondaryBatchApplication back to the
            // previous value. If the previous value is false (because there is another
            // shouldNotConflictWithSecondaryBatchApplicationBlock outside of this function), this
            // does not take the PBWM lock.
            _shouldNotConflictWithSecondaryBatchApplicationBlock = boost::none;

            // As alluded to above, if we are AutoGetting multiple collections, it
            // is possible that our "reaquire the PBWM" trick doesn't work, since we've already done
            // some reads and locked in our snapshot.  At this point, the only way out is to fail
            // the operation. The client application will need to retry.
            uassert(
                ErrorCodes::SnapshotUnavailable,
                str::stream() << "Unable to read from a snapshot due to pending collection catalog "
                                 "changes; please retry the operation. Snapshot timestamp is "
                              << readTimestamp->toString() << ". Collection minimum is "
                              << minSnapshot->toString(),
                opCtx->lockState()->shouldConflictWithSecondaryBatchApplication());

            // Abandon our snapshot. We may select a new read timestamp or ReadSource in the next
            // loop iteration.
            opCtx->recoveryUnit()->abandonSnapshot();
        }

        if (readSource == RecoveryUnit::ReadSource::kMajorityCommitted) {
            replCoord->waitUntilSnapshotCommitted(opCtx, *minSnapshot);
            uassertStatusOK(opCtx->recoveryUnit()->obtainMajorityCommittedSnapshot());
        }

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->yielded();
        }

        _autoColl.emplace(opCtx, nsOrUUID, collectionLockMode, viewMode, deadline);
    }
}


AutoGetCollectionForReadCommand::AutoGetCollectionForReadCommand(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollection::ViewMode viewMode,
    Date_t deadline,
    AutoStatsTracker::LogMode logMode)
    : _autoCollForRead(opCtx, nsOrUUID, viewMode, deadline),
      _statsTracker(
          opCtx,
          _autoCollForRead.getNss(),
          Top::LockType::ReadLocked,
          logMode,
          CollectionCatalog::get(opCtx).getDatabaseProfileLevel(_autoCollForRead.getNss().db()),
          deadline) {

    if (!_autoCollForRead.getView()) {
        auto* const css = CollectionShardingState::get(opCtx, _autoCollForRead.getNss());
        css->checkShardVersionOrThrow(opCtx);
    }
}

OldClientContext::OldClientContext(OperationContext* opCtx, const std::string& ns, bool doVersion)
    : _opCtx(opCtx), _db(DatabaseHolder::get(opCtx)->getDb(opCtx, ns)) {
    if (!_db) {
        const auto dbName = nsToDatabaseSubstring(ns);
        _db = DatabaseHolder::get(opCtx)->openDb(_opCtx, dbName, &_justCreated);
        invariant(_db);
    }

    auto const currentOp = CurOp::get(_opCtx);

    if (doVersion) {
        switch (currentOp->getNetworkOp()) {
            case dbGetMore:  // getMore is special and should be handled elsewhere
            case dbUpdate:   // update & delete check shard version as part of the write executor
            case dbDelete:   // path, so no need to check them here as well
                break;
            default:
                CollectionShardingState::get(_opCtx, NamespaceString(ns))
                    ->checkShardVersionOrThrow(_opCtx);
                break;
        }
    }

    stdx::lock_guard<Client> lk(*_opCtx->getClient());
    currentOp->enter_inlock(ns.c_str(),
                            CollectionCatalog::get(opCtx).getDatabaseProfileLevel(_db->name()));
}

OldClientContext::~OldClientContext() {
    // If in an interrupt, don't record any stats.
    // It is possible to have no lock after saving the lock state and being interrupted while
    // waiting to restore.
    if (_opCtx->getKillStatus() != ErrorCodes::OK)
        return;

    invariant(_opCtx->lockState()->isLocked());
    auto currentOp = CurOp::get(_opCtx);
    Top::get(_opCtx->getClient()->getServiceContext())
        .record(_opCtx,
                currentOp->getNS(),
                currentOp->getLogicalOp(),
                _opCtx->lockState()->isWriteLocked() ? Top::LockType::WriteLocked
                                                     : Top::LockType::ReadLocked,
                _timer.micros(),
                currentOp->isCommand(),
                currentOp->getReadWriteType());
}

LockMode getLockModeForQuery(OperationContext* opCtx, const boost::optional<NamespaceString>& nss) {
    invariant(opCtx);

    // Use IX locks for multi-statement transactions; otherwise, use IS locks.
    if (opCtx->inMultiDocumentTransaction()) {
        uassert(51071,
                "Cannot query system.views within a transaction",
                !nss || !nss->isSystemDotViews());
        return MODE_IX;
    }
    return MODE_IS;
}

BlockSecondaryReadsDuringBatchApplication_DONT_USE::
    BlockSecondaryReadsDuringBatchApplication_DONT_USE(OperationContext* opCtx)
    : _opCtx(opCtx) {
    auto allowSecondaryReads = &allowSecondaryReadsDuringBatchApplication_DONT_USE(opCtx);
    allowSecondaryReads->swap(_originalSettings);
    *allowSecondaryReads = false;
}

BlockSecondaryReadsDuringBatchApplication_DONT_USE::
    ~BlockSecondaryReadsDuringBatchApplication_DONT_USE() {
    auto allowSecondaryReads = &allowSecondaryReadsDuringBatchApplication_DONT_USE(_opCtx);
    allowSecondaryReads->swap(_originalSettings);
}

}  // namespace mongo
