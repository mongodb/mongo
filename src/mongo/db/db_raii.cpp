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
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/storage/snapshot_helper.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

const boost::optional<int> kDoNotChangeProfilingLevel = boost::none;

// TODO: SERVER-44105 remove
// If set to false, secondary reads should wait behind the PBW lock.
const auto allowSecondaryReadsDuringBatchApplication_DONT_USE =
    OperationContext::declareDecoration<boost::optional<bool>>();

/**
 * Performs some checks to determine whether the operation is compatible with a lock-free read.
 * The DBDirectClient and multi-doc transactions are not supported.
 */
bool supportsLockFreeRead(OperationContext* opCtx) {
    // Lock-free reads are only supported for external queries, not internal via DBDirectClient.
    // Lock-free reads are not supported in multi-document transactions.
    return !opCtx->getClient()->isInDirectClient() && !opCtx->inMultiDocumentTransaction();
}

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

template <typename AutoGetCollectionType>
AutoGetCollectionForReadBase<AutoGetCollectionType>::AutoGetCollectionForReadBase(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollectionViewMode viewMode,
    Date_t deadline) {
    // The caller was expecting to conflict with batch application before entering this function.
    // i.e. the caller does not currently have a ShouldNotConflict... block in scope.
    bool callerWasConflicting = opCtx->lockState()->shouldConflictWithSecondaryBatchApplication();

    if (allowSecondaryReadsDuringBatchApplication_DONT_USE(opCtx).value_or(true) &&
        opCtx->getServiceContext()->getStorageEngine()->supportsReadConcernSnapshot()) {
        _shouldNotConflictWithSecondaryBatchApplicationBlock.emplace(opCtx->lockState());
    }

    // Multi-document transactions need MODE_IX locks, otherwise MODE_IS.
    const auto collectionLockMode = getLockModeForQuery(opCtx, nsOrUUID.nss());

    _autoColl.emplace(opCtx, nsOrUUID, collectionLockMode, viewMode, deadline);

    repl::ReplicationCoordinator* const replCoord = repl::ReplicationCoordinator::get(opCtx);
    const auto readConcernLevel = repl::ReadConcernArgs::get(opCtx).getLevel();

    // If the collection doesn't exist or disappears after releasing locks and waiting, there is no
    // need to check for pending catalog changes.
    while (const auto& coll = _autoColl->getCollection()) {
        // Ban snapshot reads on capped collections.
        uassert(ErrorCodes::SnapshotUnavailable,
                "Reading from capped collections with readConcern snapshot is not supported",
                !coll->isCapped() ||
                    readConcernLevel != repl::ReadConcernLevel::kSnapshotReadConcern);

        // During batch application on secondaries, there is a potential to read inconsistent states
        // that would normally be protected by the PBWM lock. In order to serve secondary reads
        // during this period, we default to not acquiring the lock (by setting
        // _shouldNotConflictWithSecondaryBatchApplicationBlock). On primaries, we always read at a
        // consistent time, so not taking the PBWM lock is not a problem. On secondaries, we have to
        // guarantee we read at a consistent state, so we must read at the lastApplied timestamp,
        // which is set after each complete batch.

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

        // This assertion protects operations from reading inconsistent data on secondaries when
        // using the default ReadSource of kNoTimestamp.

        // Reading at lastApplied on secondaries is the safest behavior and is enabled for all user
        // and DBDirectClient reads using 'local' and 'available' readConcerns. If an internal
        // operation wishes to read without a timestamp during a batch, a ShouldNotConflict can
        // suppress this fatal assertion with the following considerations:
        // * The operation is not reading replicated data in a replication state where batch
        //   application is active OR
        // * Reading inconsistent, out-of-order data is either inconsequential or required by
        //   the operation.

        // If the caller entered this function expecting to conflict with batch application
        // (i.e. no ShouldNotConflict block in scope), but they are reading without a timestamp and
        // not holding the PBWM lock, then there is a possibility that this reader may
        // unintentionally see inconsistent data during a batch. Certain namespaces are applied
        // serially in oplog application, and therefore can be safely read without taking the PBWM
        // lock or reading at a timestamp.
        if (readSource == RecoveryUnit::ReadSource::kNoTimestamp && callerWasConflicting &&
            !nss.mustBeAppliedInOwnOplogBatch() &&
            SnapshotHelper::shouldReadAtLastApplied(opCtx, nss)) {
            LOGV2_FATAL(4728700,
                        "Reading from replicated collection on a secondary without read timestamp "
                        "or PBWM lock",
                        "collection"_attr = nss);
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
        // we yield to get a new read timestamp ahead of the minimum visible snapshot.
        if (readSource == RecoveryUnit::ReadSource::kLastApplied ||
            readSource == RecoveryUnit::ReadSource::kNoOverlap) {
            invariant(readTimestamp);
            LOGV2(20576,
                  "Tried reading at a timestamp, but future catalog changes are pending. "
                  "Trying again",
                  "readTimestamp"_attr = *readTimestamp,
                  "collection"_attr = nss.ns(),
                  "collectionMinSnapshot"_attr = *minSnapshot);

            // If we are AutoGetting multiple collections, it is possible that we've already done
            // some reads and locked in our snapshot.  At this point, the only way out is to fail
            // the operation. The client application will need to retry.
            uassert(
                ErrorCodes::SnapshotUnavailable,
                str::stream() << "Unable to read from a snapshot due to pending collection catalog "
                                 "changes and holding multiple collection locks; please retry the "
                                 "operation. Snapshot timestamp is "
                              << readTimestamp->toString() << ". Collection minimum is "
                              << minSnapshot->toString(),
                !opCtx->lockState()->isLocked());

            // Abandon our snapshot. We may select a new read timestamp or ReadSource in the next
            // loop iteration.
            opCtx->recoveryUnit()->abandonSnapshot();
        }

        if (readSource == RecoveryUnit::ReadSource::kMajorityCommitted) {
            replCoord->waitUntilSnapshotCommitted(opCtx, *minSnapshot);
            uassertStatusOK(opCtx->recoveryUnit()->majorityCommittedSnapshotAvailable());
        }

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->yielded();
        }

        _autoColl.emplace(opCtx, nsOrUUID, collectionLockMode, viewMode, deadline);
    }
}

AutoGetCollectionForReadLockFree::AutoGetCollectionForReadLockFree(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollectionViewMode viewMode,
    Date_t deadline) {
    // Supported lock-free reads should never have an open storage snapshot prior to calling
    // this helper. The storage snapshot and in-memory state fetched here must be consistent.
    invariant(supportsLockFreeRead(opCtx) && !opCtx->recoveryUnit()->isActive());

    while (true) {
        // AutoGetCollectionForReadBase can choose a read source based on the current replication
        // state. Therefore we must fetch the repl state beforehand, to compare with afterwards.
        long long replTerm = repl::ReplicationCoordinator::get(opCtx)->getTerm();

        _autoGetCollectionForReadBase.emplace(opCtx, nsOrUUID, viewMode, deadline);

        // A lock request does not always find a collection to lock.
        if (!_autoGetCollectionForReadBase.get()) {
            break;
        }

        // We must open a storage snapshot consistent with the fetched in-memory Collection instance
        // and chosen read source. The Collection instance and replication state after opening a
        // snapshot will be compared with the previously acquired state. If either does not match,
        // then this loop will retry lock acquisition and read source selection until there is a
        // match.
        //
        // Note: AutoGetCollectionForReadBase may open a snapshot for PIT reads, so
        // preallocateSnapshot() may be a no-op, but that is OK because the snapshot is established
        // by _autoGetCollectionForReadBase after it fetches a Collection instance.

        if (_autoGetCollectionForReadBase->getNss().isOplog()) {
            // Signal to the RecoveryUnit that the snapshot will be used for reading the oplog.
            // Normally the snapshot is opened from a cursor that can take special action when
            // reading from the oplog.
            opCtx->recoveryUnit()->preallocateSnapshotForOplogRead();
        } else {
            opCtx->recoveryUnit()->preallocateSnapshot();
        }

        auto newCollection = CollectionCatalog::get(opCtx).lookupCollectionByUUIDForRead(
            opCtx, _autoGetCollectionForReadBase.get()->uuid());

        // The collection may have been dropped since the previous lookup, run the loop one more
        // time to cleanup if newCollection is nullptr
        if (newCollection &&
            _autoGetCollectionForReadBase.get()->getMinimumVisibleSnapshot() ==
                newCollection->getMinimumVisibleSnapshot() &&
            replTerm == repl::ReplicationCoordinator::get(opCtx)->getTerm()) {
            break;
        }

        LOGV2_DEBUG(5067701,
                    3,
                    "Retrying acquiring state for lock-free read because collection or replication "
                    "state changed.");
        _autoGetCollectionForReadBase.reset();
        opCtx->recoveryUnit()->abandonSnapshot();
    }
}

AutoGetCollectionForReadMaybeLockFree::AutoGetCollectionForReadMaybeLockFree(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollectionViewMode viewMode,
    Date_t deadline) {
    if (supportsLockFreeRead(opCtx)) {
        _autoGetLockFree.emplace(opCtx, nsOrUUID, viewMode, deadline);
    } else {
        _autoGet.emplace(opCtx, nsOrUUID, viewMode, deadline);
    }
}

ViewDefinition* AutoGetCollectionForReadMaybeLockFree::getView() const {
    if (_autoGet) {
        return _autoGet->getView();
    } else {
        return _autoGetLockFree->getView();
    }
}

const NamespaceString& AutoGetCollectionForReadMaybeLockFree::getNss() const {
    if (_autoGet) {
        return _autoGet->getNss();
    } else {
        return _autoGetLockFree->getNss();
    }
}

const CollectionPtr& AutoGetCollectionForReadMaybeLockFree::getCollection() const {
    if (_autoGet) {
        return _autoGet->getCollection();
    } else {
        return _autoGetLockFree->getCollection();
    }
}

template <typename AutoGetCollectionForReadType>
AutoGetCollectionForReadCommandBase<AutoGetCollectionForReadType>::
    AutoGetCollectionForReadCommandBase(OperationContext* opCtx,
                                        const NamespaceStringOrUUID& nsOrUUID,
                                        AutoGetCollectionViewMode viewMode,
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

AutoGetCollectionForReadCommandMaybeLockFree::AutoGetCollectionForReadCommandMaybeLockFree(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollectionViewMode viewMode,
    Date_t deadline,
    AutoStatsTracker::LogMode logMode) {
    if (supportsLockFreeRead(opCtx)) {
        _autoGetLockFree.emplace(opCtx, nsOrUUID, viewMode, deadline, logMode);
    } else {
        _autoGet.emplace(opCtx, nsOrUUID, viewMode, deadline, logMode);
    }
}

const CollectionPtr& AutoGetCollectionForReadCommandMaybeLockFree::getCollection() const {
    if (_autoGet) {
        return _autoGet->getCollection();
    } else {
        return _autoGetLockFree->getCollection();
    }
}

ViewDefinition* AutoGetCollectionForReadCommandMaybeLockFree::getView() const {
    if (_autoGet) {
        return _autoGet->getView();
    } else {
        return _autoGetLockFree->getView();
    }
}

const NamespaceString& AutoGetCollectionForReadCommandMaybeLockFree::getNss() const {
    if (_autoGet) {
        return _autoGet->getNss();
    } else {
        return _autoGetLockFree->getNss();
    }
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

template class AutoGetCollectionForReadBase<AutoGetCollection>;
template class AutoGetCollectionForReadCommandBase<AutoGetCollectionForRead>;
template class AutoGetCollectionForReadBase<AutoGetCollectionLockFree>;
template class AutoGetCollectionForReadCommandBase<AutoGetCollectionForReadLockFree>;

}  // namespace mongo
