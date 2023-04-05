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

#include "mongo/db/db_raii.h"

#include "mongo/db/catalog/catalog_helper.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/collection_yield_restore.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/curop.h"
#include "mongo/db/repl/collection_utils.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/storage/capped_snapshots.h"
#include "mongo/db/storage/snapshot_helper.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeAutoGetShardVersionCheck);
MONGO_FAIL_POINT_DEFINE(reachedAutoGetLockFreeShardConsistencyRetry);

const boost::optional<int> kDoNotChangeProfilingLevel = boost::none;

// TODO (SERVER-69813): Get rid of this when ShardServerCatalogCacheLoader will be removed.
// If set to false, secondary reads should wait behind the PBW lock.
const auto allowSecondaryReadsDuringBatchApplication_DONT_USE =
    OperationContext::declareDecoration<boost::optional<bool>>();

/**
 * Performs some checks to determine whether the operation is compatible with a lock-free read.
 * Multi-doc transactions are not supported, nor are operations holding an exclusive lock.
 */
bool supportsLockFreeRead(OperationContext* opCtx) {
    // Lock-free reads are not supported in multi-document transactions.
    // Lock-free reads are not supported under an exclusive lock (nested reads under exclusive lock
    // holding operations).
    // Lock-free reads are not supported if a storage txn is already open w/o the lock-free reads
    // operation flag set.
    return !storageGlobalParams.disableLockFreeReads && !opCtx->inMultiDocumentTransaction() &&
        !opCtx->lockState()->isWriteLocked() &&
        !(opCtx->recoveryUnit()->isActive() && !opCtx->isLockFreeReadsOp());
}

/**
 * Type that pretends to be a Collection. It implements the minimal interface used by
 * acquireCollectionAndConsistentSnapshot(). We are tricking acquireCollectionAndConsistentSnapshot
 * to establish a consistent snapshot with just the catalog and not for a specific Collection.
 */
class FakeCollection {
public:
    // We just need to return something that would not considered to be the oplog. A default
    // constructed NamespaceString is fine.
    const NamespaceString& ns() const {
        return _ns;
    };
    // We just need to return something that compares equal with itself here.
    boost::optional<Timestamp> getMinimumVisibleSnapshot() const {
        return boost::none;
    }

private:
    NamespaceString _ns;
};

/**
 * If the given collection exists, asserts that the minimum visible timestamp of 'collection' is
 * compatible with 'readTimestamp'. Throws a SnapshotUnavailable error if the assertion fails.
 */
void assertCollectionChangesCompatibleWithReadTimestamp(OperationContext* opCtx,
                                                        const Collection* collection,
                                                        boost::optional<Timestamp> readTimestamp) {
    // Check that the collection exists.
    if (!collection) {
        return;
    }

    // Ensure the readTimestamp is not older than the collection's minimum visible timestamp.
    auto minSnapshot = collection->getMinimumVisibleSnapshot();
    if (SnapshotHelper::collectionChangesConflictWithRead(minSnapshot, readTimestamp)) {
        // Note: SnapshotHelper::collectionChangesConflictWithRead returns false if either
        // minSnapshot or readTimestamp is not set, so it's safe to print them below.
        uasserted(
            ErrorCodes::SnapshotUnavailable,
            str::stream() << "Unable to read from a snapshot due to pending collection catalog "
                             "changes to collection '"
                          << collection->ns()
                          << "'; please retry the operation. Snapshot timestamp is "
                          << readTimestamp->toString() << ". Collection minimum timestamp is "
                          << minSnapshot->toString());
    }
}

/**
 * Performs validation of special locking requirements for certain namespaces.
 */
void verifyNamespaceLockingRequirements(OperationContext* opCtx,
                                        LockMode modeColl,
                                        const NamespaceString& resolvedNss) {
    // In most cases we expect modifications for system.views to upgrade MODE_IX to MODE_X before
    // taking the lock. One exception is a query by UUID of system.views in a transaction. Usual
    // queries of system.views (by name, not UUID) within a transaction are rejected. However, if
    // the query is by UUID we can't determine whether the namespace is actually system.views until
    // we take the lock here. So we have this one last assertion.
    uassert(7195700,
            "Modifications to system.views must take an exclusive lock",
            !resolvedNss.isSystemDotViews() || modeColl != MODE_IX);
}

/**
 * Returns true if 'nss' is a view. False if the view doesn't exist.
 */
bool isNssAView(OperationContext* opCtx,
                const CollectionCatalog* catalog,
                const NamespaceString& nss) {
    return catalog->lookupView(opCtx, nss).get();
}

/**
 * Returns true if 'nss' is sharded. False otherwise.
 */
bool isNssSharded(OperationContext* opCtx, const NamespaceString& nss) {
    return CollectionShardingState::acquire(opCtx, nss)
        ->getCollectionDescription(opCtx)
        .isSharded();
}

bool isNssAViewOrSharded(OperationContext* opCtx,
                         const CollectionCatalog* catalog,
                         const NamespaceString& nss) {
    auto collection = catalog->lookupCollectionByNamespace(opCtx, nss);
    bool isView = !collection && isNssAView(opCtx, catalog, nss);
    return isView || isNssSharded(opCtx, nss);
}

bool isAnyNssAViewOrSharded(OperationContext* opCtx,
                            const CollectionCatalog* catalog,
                            const std::vector<NamespaceString>& namespaces) {
    return std::any_of(namespaces.begin(), namespaces.end(), [&](auto&& nss) {
        return isNssAViewOrSharded(opCtx, catalog, nss);
    });
}

void assertAllNamespacesAreCompatibleForReadTimestamp(
    OperationContext* opCtx,
    const CollectionCatalog* catalog,
    const std::vector<NamespaceString>& namespaces,
    const boost::optional<Timestamp>& readTimestamp) {
    for (auto&& nss : namespaces) {
        auto collection = catalog->lookupCollectionByNamespace(opCtx, nss);
        // Check that the collection has not had a DDL operation since readTimestamp.
        assertCollectionChangesCompatibleWithReadTimestamp(opCtx, collection, readTimestamp);
    }
}

/**
 * Resolves all NamespaceStringOrUUIDs in the input vector by using the input catalog to call
 * CollectionCatalog::resolveSecondaryNamespacesOrUUIDs.
 *
 * If any of the input NamespaceStringOrUUIDs is found to correspond to a view, or to a sharded
 * collection, returns boost::none.
 *
 * Otherwise, returns a vector of NamespaceStrings that the input NamespaceStringOrUUIDs resolved
 * to.
 */
boost::optional<std::vector<NamespaceString>> resolveSecondaryNamespacesOrUUIDs(
    OperationContext* opCtx,
    const CollectionCatalog* catalog,
    const std::vector<NamespaceStringOrUUID>& secondaryNssOrUUIDs) {
    std::vector<NamespaceString> resolvedNamespaces;
    resolvedNamespaces.reserve(secondaryNssOrUUIDs.size());
    for (auto&& nssOrUUID : secondaryNssOrUUIDs) {
        auto nss = catalog->resolveNamespaceStringOrUUID(opCtx, nssOrUUID);
        resolvedNamespaces.emplace_back(nss);
    }

    auto isAnySecondaryNssShardedOrAView =
        isAnyNssAViewOrSharded(opCtx, catalog, resolvedNamespaces);

    if (isAnySecondaryNssShardedOrAView) {
        return boost::none;
    } else {
        return std::move(resolvedNamespaces);
    }
}

/*
 * Establish a capped snapshot if necessary on the provided namespace.
 */
void establishCappedSnapshotIfNeeded(OperationContext* opCtx,
                                     const std::shared_ptr<const CollectionCatalog>& catalog,
                                     const NamespaceStringOrUUID& nsOrUUID) {
    auto coll = catalog->lookupCollectionByNamespaceOrUUID(opCtx, nsOrUUID);
    if (coll && coll->usesCappedSnapshots()) {
        CappedSnapshots::get(opCtx).establish(opCtx, coll);
    }
}

bool haveAcquiredConsistentCatalogAndSnapshot(
    OperationContext* opCtx,
    const CollectionCatalog* catalogBeforeSnapshot,
    const CollectionCatalog* catalogAfterSnapshot,
    long long replTermBeforeSnapshot,
    long long replTermAfterSnapshot,
    const boost::optional<std::vector<NamespaceString>>& resolvedSecondaryNamespaces) {

    if (catalogBeforeSnapshot == catalogAfterSnapshot &&
        replTermBeforeSnapshot == replTermAfterSnapshot) {
        // At this point, we know all secondary namespaces map to the same collections/views,
        // because the catalog has not changed.
        //
        // It's still possible that some collection has become sharded since before opening the
        // snapshot, in which case we would need to retry and acquire a new snapshot, so we must
        // check for that as well.
        //
        // If some secondary namespace was already a view or sharded (i.e.
        // resolvedSecondaryNamespaces is boost::none), then we don't care whether any namespaces
        // are newly sharded, so this will be false.
        bool secondaryNamespaceBecameSharded = resolvedSecondaryNamespaces &&
            std::any_of(resolvedSecondaryNamespaces->begin(),
                        resolvedSecondaryNamespaces->end(),
                        [&](auto&& nss) { return isNssSharded(opCtx, nss); });

        // If no secondary namespace has become sharded since opening a snapshot, we have found a
        // consistent catalog and snapshot and can stop retrying.
        return !secondaryNamespaceBecameSharded;
    } else {
        return false;
    }
}

/**
 * Helper function to acquire a consistent catalog and storage snapshot without holding the RSTL or
 * collection locks.
 *
 * GetCollectionAndEstablishReadSourceFunc is called before we open a snapshot, it needs to fetch
 * the Collection from the catalog and select the read source.
 *
 * ResetFunc is called when we failed to achieve consistency and need to retry.
 *
 * SetSecondaryState sets any of the secondary state that the AutoGet* needs to know about.
 */
template <typename GetCollectionAndEstablishReadSourceFunc,
          typename ResetFunc,
          typename SetSecondaryState>
auto acquireCollectionAndConsistentSnapshot(
    OperationContext* opCtx,
    bool isLockFreeReadSubOperation,
    CollectionCatalogStasher& catalogStasher,
    GetCollectionAndEstablishReadSourceFunc getCollectionAndEstablishReadSource,
    ResetFunc reset,
    SetSecondaryState setSecondaryState,
    const std::vector<NamespaceStringOrUUID>& secondaryNssOrUUIDs = {}) {
    // Figure out what type of Collection GetCollectionAndEstablishReadSourceFunc returns. It needs
    // to behave like a pointer.
    using CollectionPtrT = decltype(std::declval<GetCollectionAndEstablishReadSourceFunc>()(
                                        std::declval<OperationContext*>(),
                                        std::declval<const CollectionCatalog&>(),
                                        std::declval<bool>())
                                        .first);

    CollectionPtrT collection;
    catalogStasher.reset();
    while (true) {
        // AutoGetCollectionForReadBase can choose a read source based on the current replication
        // state. Therefore we must fetch the repl state beforehand, to compare with afterwards.
        long long replTerm = repl::ReplicationCoordinator::get(opCtx)->getTerm();

        auto catalog = CollectionCatalog::get(opCtx);

        auto [localColl, isView] =
            getCollectionAndEstablishReadSource(opCtx, *catalog, isLockFreeReadSubOperation);
        collection = localColl;

        auto resolvedSecondaryNamespaces =
            resolveSecondaryNamespacesOrUUIDs(opCtx, catalog.get(), secondaryNssOrUUIDs);

        if (resolvedSecondaryNamespaces) {
            // Note that calling getPointInTimeReadTimestamp may open a snapshot if one is not
            // already open, depending on the current read source.
            const auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);
            assertAllNamespacesAreCompatibleForReadTimestamp(
                opCtx, catalog.get(), *resolvedSecondaryNamespaces, readTimestamp);
        }

        // A lock request does not always find a collection to lock. But if we found a view abort
        // LFR setup, we don't need to open a storage snapshot in this case as the lock helper will
        // be released and we will lock the Collection backing the view later on.
        if (!collection && isView)
            break;

        // If this is a nested lock acquisition, then we already have a consistent stashed catalog
        // and snapshot from which to read and we can skip the below logic.
        if (isLockFreeReadSubOperation) {
            // A consistent in-memory and on-disk state is already set up by a higher level AutoGet*
            // instance. We just need to return the requested Collection which has already been
            // checked by getCollectionAndEstablishReadSource above.
            return collection;
        }

        // We must open a storage snapshot consistent with the fetched in-memory Catalog instance
        // and chosen read source. The Catalog instance and replication state after opening a
        // snapshot will be compared with the previously acquired state. If either does not match,
        // then this loop will retry lock acquisition and read source selection until there is a
        // match.
        //
        // Note: getCollectionAndEstablishReadSource() may open a snapshot for PIT reads, so
        // preallocateSnapshot() may be a no-op, but that is OK because the snapshot is established
        // by getCollectionAndEstablishReadSource() after it fetches a Collection instance.
        if (collection && collection->ns().isOplog()) {
            // Signal to the RecoveryUnit that the snapshot will be used for reading the oplog.
            // Normally the snapshot is opened from a cursor that can take special action when
            // reading from the oplog.
            opCtx->recoveryUnit()->preallocateSnapshotForOplogRead();
        } else {
            opCtx->recoveryUnit()->preallocateSnapshot();
        }

        // Verify that the catalog has not changed while we opened the storage snapshot. If the
        // catalog is unchanged, then the requested Collection is also guaranteed to be the same.
        auto newCatalog = CollectionCatalog::get(opCtx);

        if (haveAcquiredConsistentCatalogAndSnapshot(
                opCtx,
                catalog.get(),
                newCatalog.get(),
                replTerm,
                repl::ReplicationCoordinator::get(opCtx)->getTerm(),
                resolvedSecondaryNamespaces)) {
            bool isAnySecondaryNssShardedOrAView = !resolvedSecondaryNamespaces.has_value();
            setSecondaryState(isAnySecondaryNssShardedOrAView);
            catalogStasher.stash(std::move(catalog));
            break;
        }

        LOGV2_DEBUG(5067701,
                    3,
                    "Retrying acquiring state for lock-free read because collection, catalog or "
                    "replication state changed.");
        reset();
        opCtx->recoveryUnit()->abandonSnapshot();
    }

    return collection;
}

void assertReadConcernSupported(const CollectionPtr& coll,
                                const repl::ReadConcernArgs& readConcernArgs,
                                const RecoveryUnit::ReadSource& readSource) {
    const auto readConcernLevel = readConcernArgs.getLevel();
    // Ban snapshot reads on capped collections.
    uassert(ErrorCodes::SnapshotUnavailable,
            "Reading from capped collections with readConcern snapshot is not supported",
            !coll->isCapped() || readConcernLevel != repl::ReadConcernLevel::kSnapshotReadConcern);

    // Disallow snapshot reads and causal consistent majority reads on config.transactions
    // outside of transactions to avoid running the collection at a point-in-time in the middle
    // of a secondary batch. Such reads are unsafe because config.transactions updates are
    // coalesced on secondaries. Majority reads without an afterClusterTime is allowed because
    // they are allowed to return arbitrarily stale data. We allow kNoTimestamp and kLastApplied
    // reads because they must be from internal readers given the snapshot/majority readConcern
    // (e.g. for session checkout).

    if (coll->ns() == NamespaceString::kSessionTransactionsTableNamespace &&
        readSource != RecoveryUnit::ReadSource::kNoTimestamp &&
        readSource != RecoveryUnit::ReadSource::kLastApplied &&
        ((readConcernLevel == repl::ReadConcernLevel::kSnapshotReadConcern &&
          !readConcernArgs.allowTransactionTableSnapshot()) ||
         (readConcernLevel == repl::ReadConcernLevel::kMajorityReadConcern &&
          readConcernArgs.getArgsAfterClusterTime()))) {
        uasserted(5557800,
                  "Snapshot reads and causal consistent majority reads on config.transactions "
                  "are not supported");
    }
}

void checkInvariantsForReadOptions(const NamespaceString& nss,
                                   const boost::optional<LogicalTime>& afterClusterTime,
                                   const RecoveryUnit::ReadSource& readSource,
                                   const boost::optional<Timestamp>& readTimestamp,
                                   bool callerWasConflicting,
                                   bool shouldReadAtLastApplied) {
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
        !nss.mustBeAppliedInOwnOplogBatch() && shouldReadAtLastApplied) {
        LOGV2_FATAL(4728700,
                    "Reading from replicated collection on a secondary without read timestamp "
                    "or PBWM lock",
                    "collection"_attr = nss);
    }
}

}  // namespace

AutoStatsTracker::AutoStatsTracker(
    OperationContext* opCtx,
    const NamespaceString& nss,
    Top::LockType lockType,
    LogMode logMode,
    int dbProfilingLevel,
    Date_t deadline,
    const std::vector<NamespaceStringOrUUID>& secondaryNssOrUUIDVector)
    : _opCtx(opCtx), _lockType(lockType), _logMode(logMode) {
    // Deduplicate all namespaces for Top reporting on destruct.
    _nssSet.insert(nss);
    auto catalog = CollectionCatalog::get(opCtx);
    for (auto&& secondaryNssOrUUID : secondaryNssOrUUIDVector) {
        _nssSet.insert(catalog->resolveNamespaceStringOrUUID(opCtx, secondaryNssOrUUID));
    }

    if (_logMode == LogMode::kUpdateTop) {
        return;
    }

    stdx::lock_guard<Client> clientLock(*_opCtx->getClient());
    CurOp::get(_opCtx)->enter_inlock(nss, dbProfilingLevel);
}

AutoStatsTracker::~AutoStatsTracker() {
    if (_logMode == LogMode::kUpdateCurOp) {
        return;
    }

    // Update stats for each namespace.
    auto curOp = CurOp::get(_opCtx);
    Top::get(_opCtx->getServiceContext())
        .record(_opCtx,
                _nssSet,
                curOp->getLogicalOp(),
                _lockType,
                durationCount<Microseconds>(curOp->elapsedTimeExcludingPauses()),
                curOp->isCommand(),
                curOp->getReadWriteType());
}

template <typename AutoGetCollectionType, typename EmplaceAutoCollFunc>
AutoGetCollectionForReadBase<AutoGetCollectionType, EmplaceAutoCollFunc>::
    AutoGetCollectionForReadBase(OperationContext* opCtx,
                                 const EmplaceAutoCollFunc& emplaceAutoColl,
                                 bool isLockFreeReadSubOperation) {
    // If this instance is nested and lock-free, then we do not want to adjust any setting, but we
    // do need to set up the Collection reference.
    if (isLockFreeReadSubOperation) {
        emplaceAutoColl.emplace(_autoColl);
        return;
    }

    // The caller was expecting to conflict with batch application before entering this function.
    // i.e. the caller does not currently have a ShouldNotConflict... block in scope.
    bool callerWasConflicting = opCtx->lockState()->shouldConflictWithSecondaryBatchApplication();

    if (allowSecondaryReadsDuringBatchApplication_DONT_USE(opCtx).value_or(true) &&
        opCtx->getServiceContext()->getStorageEngine()->supportsReadConcernSnapshot()) {
        _shouldNotConflictWithSecondaryBatchApplicationBlock.emplace(opCtx->lockState());
    }

    emplaceAutoColl.emplace(_autoColl);

    auto readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    // If the collection doesn't exist or disappears after releasing locks and waiting, there is no
    // need to check for pending catalog changes.
    while (const auto& coll = _autoColl->getCollection()) {
        assertReadConcernSupported(
            coll, readConcernArgs, opCtx->recoveryUnit()->getTimestampReadSource());

        if (coll->usesCappedSnapshots()) {
            CappedSnapshots::get(opCtx).establish(opCtx, coll);
        }

        // We make a copy of the namespace so we can use the variable after locks are released,
        // since releasing locks will allow the value of coll->ns() to change.
        const NamespaceString nss = coll->ns();
        // During batch application on secondaries, there is a potential to read inconsistent states
        // that would normally be protected by the PBWM lock. In order to serve secondary reads
        // during this period, we default to not acquiring the lock (by setting
        // _shouldNotConflictWithSecondaryBatchApplicationBlock). On primaries, we always read at a
        // consistent time, so not taking the PBWM lock is not a problem. On secondaries, we have to
        // guarantee we read at a consistent state, so we must read at the lastApplied timestamp,
        // which is set after each complete batch.

        // Once we have our locks, check whether or not we should override the ReadSource that was
        // set before acquiring locks.
        const bool shouldReadAtLastApplied = SnapshotHelper::changeReadSourceIfNeeded(opCtx, nss);
        // Update readSource in case it was updated.
        const auto readSource = opCtx->recoveryUnit()->getTimestampReadSource();

        const auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);

        checkInvariantsForReadOptions(nss,
                                      readConcernArgs.getArgsAfterClusterTime(),
                                      readSource,
                                      readTimestamp,
                                      callerWasConflicting,
                                      shouldReadAtLastApplied);

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

        invariant(readConcernArgs.getLevel() != repl::ReadConcernLevel::kSnapshotReadConcern);

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
            const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            replCoord->waitUntilSnapshotCommitted(opCtx, *minSnapshot);
            uassertStatusOK(opCtx->recoveryUnit()->majorityCommittedSnapshotAvailable());
        }

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->yielded();
        }

        emplaceAutoColl.emplace(_autoColl);
    }
}

EmplaceAutoGetCollectionForRead::EmplaceAutoGetCollectionForRead(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollection::Options options)
    : _opCtx(opCtx),
      _nsOrUUID(nsOrUUID),
      // Multi-document transactions need MODE_IX locks, otherwise MODE_IS.
      _collectionLockMode(getLockModeForQuery(opCtx, nsOrUUID.nss())),
      _options(std::move(options)) {}

void EmplaceAutoGetCollectionForRead::emplace(boost::optional<AutoGetCollection>& autoColl) const {
    autoColl.emplace(
        _opCtx, _nsOrUUID, _collectionLockMode, _options, AutoGetCollection::ForReadTag{});
}

AutoGetCollectionForReadLegacy::AutoGetCollectionForReadLegacy(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollection::Options options)
    : AutoGetCollectionForReadBase(opCtx,
                                   EmplaceAutoGetCollectionForRead(opCtx, nsOrUUID, options)) {
    const auto& secondaryNssOrUUIDs = options._secondaryNssOrUUIDs;

    // All relevant locks are held. Check secondary collections and verify they are valid for
    // use.
    if (getCollection() && !secondaryNssOrUUIDs.empty()) {
        auto catalog = CollectionCatalog::get(opCtx);

        auto resolvedNamespaces =
            resolveSecondaryNamespacesOrUUIDs(opCtx, catalog.get(), secondaryNssOrUUIDs);

        _secondaryNssIsAViewOrSharded = !resolvedNamespaces.has_value();

        // If no secondary namespace is a view or is sharded, resolve namespaces and check their
        // that their minVisible timestamps are compatible with the read timestamp.
        if (resolvedNamespaces) {
            // Note that calling getPointInTimeReadTimestamp may open a snapshot if one is not
            // already open, depending on the current read source.
            const auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);
            assertAllNamespacesAreCompatibleForReadTimestamp(
                opCtx, catalog.get(), *resolvedNamespaces, readTimestamp);
        }
    }
}

AutoGetCollectionForReadPITCatalog::AutoGetCollectionForReadPITCatalog(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollection::Options options)
    : _callerWasConflicting(opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()),
      _shouldNotConflictWithSecondaryBatchApplicationBlock(
          [&]() -> boost::optional<ShouldNotConflictWithSecondaryBatchApplicationBlock> {
              if (allowSecondaryReadsDuringBatchApplication_DONT_USE(opCtx).value_or(true) &&
                  opCtx->getServiceContext()->getStorageEngine()->supportsReadConcernSnapshot()) {
                  return boost::optional<ShouldNotConflictWithSecondaryBatchApplicationBlock>(
                      opCtx->lockState());
              }

              return boost::none;
          }()),
      _autoDb(AutoGetDb::createForAutoGetCollection(
          opCtx, nsOrUUID, getLockModeForQuery(opCtx, nsOrUUID.nss()), options)) {

    const auto modeColl = getLockModeForQuery(opCtx, nsOrUUID.nss());
    const auto viewMode = options._viewMode;
    const auto deadline = options._deadline;
    const auto& secondaryNssOrUUIDs = options._secondaryNssOrUUIDs;

    // Acquire the collection locks. If there's only one lock, then it can simply be taken. If
    // there are many, however, the locks must be taken in _ascending_ ResourceId order to avoid
    // deadlocks across threads.
    if (secondaryNssOrUUIDs.empty()) {
        uassertStatusOK(nsOrUUID.isNssValid());
        _collLocks.emplace_back(opCtx, nsOrUUID, modeColl, deadline);
    } else {
        catalog_helper::acquireCollectionLocksInResourceIdOrder(
            opCtx, nsOrUUID, modeColl, deadline, secondaryNssOrUUIDs, &_collLocks);
    }

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    catalog_helper::setAutoGetCollectionWaitFailpointExecute(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); });

    auto catalog = CollectionCatalog::get(opCtx);

    _resolvedNss = catalog->resolveNamespaceStringOrUUID(opCtx, nsOrUUID);

    // During batch application on secondaries, there is a potential to read inconsistent states
    // that would normally be protected by the PBWM lock. In order to serve secondary reads
    // during this period, we default to not acquiring the lock (by setting
    // _shouldNotConflictWithSecondaryBatchApplicationBlock). On primaries, we always read at a
    // consistent time, so not taking the PBWM lock is not a problem. On secondaries, we have to
    // guarantee we read at a consistent state, so we must read at the lastApplied timestamp,
    // which is set after each complete batch.

    // Once we have our locks, check whether or not we should override the ReadSource that was
    // set before acquiring locks.
    const bool shouldReadAtLastApplied =
        SnapshotHelper::changeReadSourceIfNeeded(opCtx, _resolvedNss);
    // Update readSource in case it was updated.
    const auto readSource = opCtx->recoveryUnit()->getTimestampReadSource();
    const auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);

    // Check that the collections are all safe to use. First acquire collection from our catalog
    // compatible with the specified 'readTimestamp'. Creates and places a compatible PIT collection
    // reference in the 'catalog' if needed and the collection exists at that PIT.
    _coll = CollectionPtr(catalog->establishConsistentCollection(opCtx, nsOrUUID, readTimestamp));
    _coll.makeYieldable(opCtx, LockedCollectionYieldRestore{opCtx, _coll});

    // Validate primary collection.
    checkCollectionUUIDMismatch(opCtx, _resolvedNss, _coll, options._expectedUUID);
    verifyNamespaceLockingRequirements(opCtx, modeColl, _resolvedNss);

    // Check secondary collections and verify they are valid for use.
    if (!secondaryNssOrUUIDs.empty()) {
        // Check that none of the namespaces are views or sharded collections, which are not
        // supported for secondary namespaces.
        auto resolvedSecondaryNamespaces =
            resolveSecondaryNamespacesOrUUIDs(opCtx, catalog.get(), secondaryNssOrUUIDs);
        _secondaryNssIsAViewOrSharded = !resolvedSecondaryNamespaces.has_value();

        if (!_secondaryNssIsAViewOrSharded) {
            // Ensure that the readTimestamp is compatible with the latest Collection instances or
            // create PIT instances in the 'catalog' (if the collections existed at that PIT).
            for (const auto& secondaryNssOrUUID : secondaryNssOrUUIDs) {
                auto secondaryCollectionAtPIT = catalog->establishConsistentCollection(
                    opCtx, secondaryNssOrUUID, readTimestamp);
                if (secondaryCollectionAtPIT) {
                    invariant(secondaryCollectionAtPIT->ns().dbName() == _resolvedNss.dbName());
                    verifyNamespaceLockingRequirements(
                        opCtx, MODE_IS, secondaryCollectionAtPIT->ns());
                }
            }
        }
    }

    if (_coll) {
        // Fetch and store the sharding collection description data needed for use during the
        // operation. The shardVersion will be checked later if the shard filtering metadata is
        // fetched, ensuring both that the collection description info used here and the routing
        // table are consistent with the read request's shardVersion.
        //
        // Note: sharding versioning for an operation has no concept of multiple collections.
        auto scopedCss = CollectionShardingState::acquire(opCtx, _resolvedNss);
        scopedCss->checkShardVersionOrThrow(opCtx);

        auto collDesc = scopedCss->getCollectionDescription(opCtx);
        if (collDesc.isSharded()) {
            _coll.setShardKeyPattern(collDesc.getKeyPattern());
        }

        auto readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        assertReadConcernSupported(
            _coll, readConcernArgs, opCtx->recoveryUnit()->getTimestampReadSource());

        checkInvariantsForReadOptions(_coll->ns(),
                                      readConcernArgs.getArgsAfterClusterTime(),
                                      readSource,
                                      readTimestamp,
                                      _callerWasConflicting,
                                      shouldReadAtLastApplied);

        return;
    }

    // No Collection found, try and lookup view.
    const auto receivedShardVersion{
        OperationShardingState::get(opCtx).getShardVersion(_resolvedNss)};

    if (!options._expectedUUID) {
        // We only need to look up a view if an expected collection UUID was not provided. If this
        // namespace were a view, the collection UUID mismatch check would have failed above.
        if ((_view = catalog->lookupView(opCtx, _resolvedNss))) {
            uassert(ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "Taking " << _resolvedNss.ns()
                                  << " lock for timeseries is not allowed",
                    viewMode == auto_get_collection::ViewMode::kViewsPermitted ||
                        !_view->timeseries());

            uassert(ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "Namespace " << _resolvedNss.ns()
                                  << " is a view, not a collection",
                    viewMode == auto_get_collection::ViewMode::kViewsPermitted);

            uassert(StaleConfigInfo(_resolvedNss,
                                    *receivedShardVersion,
                                    ShardVersion::UNSHARDED() /* wantedVersion */,
                                    ShardingState::get(opCtx)->shardId()),
                    str::stream() << "Namespace " << _resolvedNss
                                  << " is a view therefore the shard "
                                  << "version attached to the request must be unset or UNSHARDED",
                    !receivedShardVersion || *receivedShardVersion == ShardVersion::UNSHARDED());

            return;
        }
    }

    // There is neither a collection nor a view for the namespace, so if we reached to this point
    // there are the following possibilities depending on the received shard version:
    //   1. ShardVersion::UNSHARDED: The request comes from a router and the operation entails the
    //      implicit creation of an unsharded collection. We can continue.
    //   2. ChunkVersion::IGNORED: The request comes from a router that broadcasted the same to all
    //      shards, but this shard doesn't own any chunks for the collection. We can continue.
    //   3. boost::none: The request comes from client directly connected to the shard. We can
    //      continue.
    //   4. Any other value: The request comes from a stale router on a collection or a view which
    //      was deleted time ago (or the user manually deleted it from from underneath of sharding).
    //      We return a stale config error so that the router recovers.

    uassert(StaleConfigInfo(_resolvedNss,
                            *receivedShardVersion,
                            boost::none /* wantedVersion */,
                            ShardingState::get(opCtx)->shardId()),
            str::stream() << "No metadata for namespace " << _resolvedNss << " therefore the shard "
                          << "version attached to the request must be unset, UNSHARDED or IGNORED",
            !receivedShardVersion || *receivedShardVersion == ShardVersion::UNSHARDED() ||
                ShardVersion::isPlacementVersionIgnored(*receivedShardVersion));
}

const CollectionPtr& AutoGetCollectionForReadPITCatalog::getCollection() const {
    return _coll;
}

const ViewDefinition* AutoGetCollectionForReadPITCatalog::getView() const {
    return _view.get();
}

const NamespaceString& AutoGetCollectionForReadPITCatalog::getNss() const {
    return _resolvedNss;
}

AutoGetCollectionForRead::AutoGetCollectionForRead(OperationContext* opCtx,
                                                   const NamespaceStringOrUUID& nsOrUUID,
                                                   AutoGetCollection::Options options) {
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe()) {
        _legacy.emplace(opCtx, nsOrUUID, options);
    } else {
        _pitCatalog.emplace(opCtx, nsOrUUID, options);
    }
}

const CollectionPtr& AutoGetCollectionForRead::getCollection() const {
    return _pitCatalog ? _pitCatalog->getCollection() : _legacy->getCollection();
}

const ViewDefinition* AutoGetCollectionForRead::getView() const {
    return _pitCatalog ? _pitCatalog->getView() : _legacy->getView();
}

const NamespaceString& AutoGetCollectionForRead::getNss() const {
    return _pitCatalog ? _pitCatalog->getNss() : _legacy->getNss();
}

bool AutoGetCollectionForRead::isAnySecondaryNamespaceAViewOrSharded() const {
    return _pitCatalog ? _pitCatalog->isAnySecondaryNamespaceAViewOrSharded()
                       : _legacy->isAnySecondaryNamespaceAViewOrSharded();
}

AutoGetCollectionForReadLockFreeLegacy::EmplaceHelper::EmplaceHelper(
    OperationContext* opCtx,
    CollectionCatalogStasher& catalogStasher,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollectionLockFree::Options options,
    bool isLockFreeReadSubOperation)
    : _opCtx(opCtx),
      _catalogStasher(catalogStasher),
      _nsOrUUID(nsOrUUID),
      _options(std::move(options)),
      _isLockFreeReadSubOperation(isLockFreeReadSubOperation) {}

void AutoGetCollectionForReadLockFreeLegacy::EmplaceHelper::emplace(
    boost::optional<AutoGetCollectionLockFree>& autoColl) const {
    autoColl.emplace(
        _opCtx,
        _nsOrUUID,
        /* restoreFromYield */
        [&catalogStasher = _catalogStasher, isSubOperation = _isLockFreeReadSubOperation](
            std::shared_ptr<const Collection>& collection, OperationContext* opCtx, UUID uuid) {
            // A sub-operation should never yield because it would break the consistent in-memory
            // and on-disk view of the higher level operation.
            invariant(!isSubOperation);

            collection = acquireCollectionAndConsistentSnapshot(
                opCtx,
                /* isLockFreeReadSubOperation */
                isSubOperation,
                /* CollectionCatalogStasher */
                catalogStasher,
                /* GetCollectionAndEstablishReadSourceFunc */
                [uuid](OperationContext* opCtx,
                       const CollectionCatalog& catalog,
                       bool isLockFreeReadSubOperation) {
                    // There should only ever be one helper recovering from a query yield, so it
                    // should never be nested.
                    invariant(!isLockFreeReadSubOperation);

                    auto coll = catalog.lookupCollectionByUUIDForRead_DONT_USE(opCtx, uuid);

                    if (coll) {
                        if (coll->usesCappedSnapshots()) {
                            CappedSnapshots::get(opCtx).establish(opCtx, coll.get());
                        }

                        // After yielding and reacquiring locks, the preconditions that were used to
                        // select our ReadSource initially need to be checked again. We select a
                        // ReadSource based on replication state. After a query yields its locks,
                        // the replication state may have changed, invalidating our current choice
                        // of ReadSource. Using the same preconditions, change our ReadSource if
                        // necessary.
                        SnapshotHelper::changeReadSourceIfNeeded(opCtx, coll->ns());
                    }

                    return std::make_pair(coll, /* isView */ false);
                },
                /* ResetFunc */
                []() {},
                /* SetSecondaryState */
                [](bool isAnySecondaryNamespaceAViewOrSharded) {
                    // Not necessary to check for views or sharded secondary collections, which are
                    // unsupported. If a read is running, changing a namespace to a view would
                    // require dropping the collection first, which trips other checks. A secondary
                    // collection becoming sharded during a read is ignored to parallel existing
                    // behavior for the primary collection.
                });
        },
        _options);
}

AutoGetCollectionForReadLockFreeLegacy::AutoGetCollectionForReadLockFreeLegacy(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollection::Options options)
    : _catalogStash(opCtx) {
    bool isLockFreeReadSubOperation = opCtx->isLockFreeReadsOp();

    // Supported lock-free reads should only ever have an open storage snapshot prior to calling
    // this helper if it is a nested lock-free operation. The storage snapshot and in-memory state
    // used across lock=free reads must be consistent.
    invariant(supportsLockFreeRead(opCtx) &&
              (!opCtx->recoveryUnit()->isActive() || isLockFreeReadSubOperation));

    EmplaceHelper emplaceFunc(opCtx,
                              _catalogStash,
                              nsOrUUID,
                              AutoGetCollectionLockFree::Options{}
                                  .viewMode(options._viewMode)
                                  .deadline(options._deadline)
                                  .expectedUUID(options._expectedUUID),
                              isLockFreeReadSubOperation);
    acquireCollectionAndConsistentSnapshot(
        opCtx,
        /* isLockFreeReadSubOperation */
        isLockFreeReadSubOperation,
        /* CollectionCatalogStasher */
        _catalogStash,
        /* GetCollectionAndEstablishReadSourceFunc */
        [this, &emplaceFunc](
            OperationContext* opCtx, const CollectionCatalog&, bool isLockFreeReadSubOperation) {
            _autoGetCollectionForReadBase.emplace(opCtx, emplaceFunc, isLockFreeReadSubOperation);
            return std::make_pair(_autoGetCollectionForReadBase->getCollection().get(),
                                  _autoGetCollectionForReadBase->getView());
        },
        /* ResetFunc */
        [this]() { _autoGetCollectionForReadBase.reset(); },
        /* SetSecondaryState */
        [this](bool isAnySecondaryNamespaceAViewOrSharded) {
            _secondaryNssIsAViewOrSharded = isAnySecondaryNamespaceAViewOrSharded;
        },
        options._secondaryNssOrUUIDs);
}

namespace {

void openSnapshot(OperationContext* opCtx, bool isForOplogRead) {
    if (isForOplogRead) {
        opCtx->recoveryUnit()->preallocateSnapshotForOplogRead();
    } else {
        opCtx->recoveryUnit()->preallocateSnapshot();
    }
}

/**
 * Used as a return value for the following function.
 */
struct ConsistentCatalogAndSnapshot {
    std::shared_ptr<const CollectionCatalog> catalog;
    bool isAnySecondaryNamespaceAViewOrSharded;
    RecoveryUnit::ReadSource readSource;
    boost::optional<Timestamp> readTimestamp;
};

/**
 * This function is responsible for acquiring an in-memory version of the CollectionCatalog and
 * opening a snapshot such that the data contained in the in-memory CollectionCatalog matches the
 * data in the durable catalog in that snapshot.
 *
 * It is used by readers who do not have any collection locks (a.k.a lock-free readers), and so
 * there may be ongoing DDL operations concurrent with this function being called. This means we
 * must take care to handle cases where the state of the catalog changes during the course of
 * execution of this function.
 *
 * The high-level algorithm here is:
 *  * Get the latest version of the catalog
 *  * Open a snapshot
 *  * Get the latest version of the catalog and check if it changed since opening the snapshot. If
 *    it did, we need to retry, because that means that the version of the durable catalog that
 *    would be read from the snapshot would be different from the in-memory CollectionCatalog.
 *
 * Note that it is still possible for the version of the CollectionCatalog obtained to be
 * different from the durable catalog if there's a DDL operation pending commit at precisely the
 * right time. This is okay because the CollectionCatalog tracks DDL entries pending commit and
 * lock-free readers will check this state for the namespace they care about before deciding whether
 * to use an entry from the CollectionCatalog or whether to read catalog information directly from
 * the durable catalog instead.
 *
 * Also note that this retry algorithm is only necessary for readers who are not reading with a
 * timestamp. Readers at points in time in the past (e.g. readers with the kProvided ReadSource)
 * always will read directly from the durable catalog, and so it is not important for the in-memory
 * CollectionCatalog to match the durable catalog for these readers. In the future, we may want to
 * consider separating the code paths for timestamped and untimestamped writes, but for now, both
 * cases flow through this same function.
 *
 * We also check for replication state changes before and after opening a snapshot, since the
 * replication state determines the whether readers without a timestamp must read from the storage
 * engine without a timestamp or whether they should read at the last applied timestamp. If the
 * replication state changes, the opened snapshot is abandoned and the process is retried.
 */
ConsistentCatalogAndSnapshot getConsistentCatalogAndSnapshot(
    OperationContext* opCtx,
    NamespaceStringOrUUID nsOrUUID,
    const std::vector<NamespaceStringOrUUID>& secondaryNssOrUUIDs,
    const repl::ReadConcernArgs& readConcernArgs,
    bool callerExpectedToConflictWithSecondaryBatchApplication) {
    // Loop until we get a consistent catalog and snapshot or throw an exception.
    while (true) {
        // The read source used can change depending on replication state, so we must fetch the repl
        // state beforehand, to compare with afterwards.
        const auto replTermBeforeSnapshot = repl::ReplicationCoordinator::get(opCtx)->getTerm();

        const auto catalogBeforeSnapshot = CollectionCatalog::get(opCtx);

        // When a query yields it releases its snapshot, and any point-in-time instantiated
        // collections stored on the snapshot decoration are destructed. At the start of a query,
        // collections are fetched using a namespace. However, when a query is restoring from
        // yield it attempts to fetch collections by UUID. It's possible for a UUID to no longer
        // resolve to a namespace in the latest collection catalog if that collection was dropped
        // while the query was yielding. This doesn't conclude that the collection is inaccessible
        // at an earlier point-in-time as the data files may still be on disk. This namespace is
        // used to determine if the read source needs to be changed and we only do this if the
        // original read source is kNoTimestamp or kLastApplied. If it's neither of the two we can
        // safely continue.
        NamespaceString nss;
        try {
            nss = catalogBeforeSnapshot->resolveNamespaceStringOrUUID(opCtx, nsOrUUID);
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            invariant(nsOrUUID.uuid());

            const auto readSource = opCtx->recoveryUnit()->getTimestampReadSource();
            if (readSource == RecoveryUnit::ReadSource::kNoTimestamp ||
                readSource == RecoveryUnit::ReadSource::kLastApplied) {
                throw;
            }
        }

        // This may modify the read source on the recovery unit for opCtx if the current read source
        // is either kNoTimestamp or kLastApplied.
        const bool shouldReadAtLastApplied = SnapshotHelper::changeReadSourceIfNeeded(opCtx, nss);

        const auto resolvedSecondaryNamespaces = resolveSecondaryNamespacesOrUUIDs(
            opCtx, catalogBeforeSnapshot.get(), secondaryNssOrUUIDs);

        // If the collection requires capped snapshots (i.e. it is unreplicated, capped, not the
        // oplog, and not clustered), establish a capped snapshot. This must happen before opening
        // the storage snapshot to ensure a reader using tailable cursors would not miss any writes.
        // See comments in getCollectionFromCatalog for why it is safe to establish the capped
        // snapshot here, on the Collection object in the latest version of the catalog, even if
        // openCollection is eventually called to construct a Collection object from the durable
        // catalog.
        establishCappedSnapshotIfNeeded(opCtx, catalogBeforeSnapshot, nsOrUUID);

        openSnapshot(opCtx, nss.isOplog());

        const auto readSource = opCtx->recoveryUnit()->getTimestampReadSource();
        const auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);

        checkInvariantsForReadOptions(nss,
                                      readConcernArgs.getArgsAfterClusterTime(),
                                      readSource,
                                      readTimestamp,
                                      callerExpectedToConflictWithSecondaryBatchApplication,
                                      shouldReadAtLastApplied);

        // TODO (SERVER-71660): Use a catalog version instead of pointer comparison for catalog
        // before and after snapshot.
        const auto catalogAfterSnapshot = CollectionCatalog::get(opCtx);

        const auto replTermAfterSnapshot = repl::ReplicationCoordinator::get(opCtx)->getTerm();

        if (haveAcquiredConsistentCatalogAndSnapshot(opCtx,
                                                     catalogBeforeSnapshot.get(),
                                                     catalogAfterSnapshot.get(),
                                                     replTermBeforeSnapshot,
                                                     replTermAfterSnapshot,
                                                     resolvedSecondaryNamespaces)) {
            bool isAnySecondaryNssShardedOrAView = !resolvedSecondaryNamespaces.has_value();
            return {
                catalogBeforeSnapshot, isAnySecondaryNssShardedOrAView, readSource, readTimestamp};
        } else {
            opCtx->recoveryUnit()->abandonSnapshot();

            CurOp::get(opCtx)->yielded();
        }
    }
}

std::shared_ptr<const ViewDefinition> lookupView(
    OperationContext* opCtx,
    const std::shared_ptr<const CollectionCatalog>& catalog,
    const NamespaceString& nss,
    auto_get_collection::ViewMode viewMode) {
    auto view = catalog->lookupView(opCtx, nss);
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "Taking " << nss.ns() << " lock for timeseries is not allowed",
            !view || viewMode == auto_get_collection::ViewMode::kViewsPermitted ||
                !view->timeseries());
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "Namespace " << nss.ns() << " is a view, not a collection",
            !view || viewMode == auto_get_collection::ViewMode::kViewsPermitted);
    return view;
}

std::tuple<NamespaceString, const Collection*, std::shared_ptr<const ViewDefinition>>
getCollectionForLockFreeRead(OperationContext* opCtx,
                             const std::shared_ptr<const CollectionCatalog>& catalog,
                             boost::optional<Timestamp> readTimestamp,
                             const NamespaceStringOrUUID& nsOrUUID,
                             AutoGetCollection::Options options) {

    auto& hangBeforeAutoGetCollectionLockFreeShardedStateAccess =
        *globalFailPointRegistry().find("hangBeforeAutoGetCollectionLockFreeShardedStateAccess");

    hangBeforeAutoGetCollectionLockFreeShardedStateAccess.executeIf(
        [&](auto&) { hangBeforeAutoGetCollectionLockFreeShardedStateAccess.pauseWhileSet(opCtx); },
        [&](const BSONObj& data) {
            return opCtx->getLogicalSessionId() &&
                opCtx->getLogicalSessionId()->getId() == UUID::fromCDR(data["lsid"].uuid());
        });


    // Returns a collection reference compatible with the specified 'readTimestamp'. Creates and
    // places a compatible PIT collection reference in the 'catalog' if needed and the collection
    // exists at that PIT.
    const Collection* coll = catalog->establishConsistentCollection(opCtx, nsOrUUID, readTimestamp);
    // Note: This call to resolveNamespaceStringOrUUID must happen after getCollectionFromCatalog
    // above, since getCollectionFromCatalog may call openCollection, which could change the result
    // of namespace resolution.
    const auto nss = catalog->resolveNamespaceStringOrUUID(opCtx, nsOrUUID);
    checkCollectionUUIDMismatch(opCtx, catalog, nss, coll, options._expectedUUID);

    std::shared_ptr<const ViewDefinition> viewDefinition =
        coll ? nullptr : lookupView(opCtx, catalog, nss, options._viewMode);
    return {nss, coll, std::move(viewDefinition)};
}

static const Lock::GlobalLockSkipOptions kLockFreeReadsGlobalLockOptions{[] {
    Lock::GlobalLockSkipOptions options;
    options.skipRSTLLock = true;
    return options;
}()};

struct CatalogStateForNamespace {
    std::shared_ptr<const CollectionCatalog> catalog;
    bool isAnySecondaryNssShardedOrAView;
    NamespaceString resolvedNss;
    const Collection* collection;
    std::shared_ptr<const ViewDefinition> view;
};

CatalogStateForNamespace acquireCatalogStateForNamespace(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    const repl::ReadConcernArgs& readConcernArgs,
    bool callerExpectedToConflictWithSecondaryBatchApplication,
    AutoGetCollection::Options options) {

    auto [catalog, isAnySecondaryNssShardedOrAView, readSource, readTimestamp] =
        getConsistentCatalogAndSnapshot(opCtx,
                                        nsOrUUID,
                                        options._secondaryNssOrUUIDs,
                                        readConcernArgs,
                                        callerExpectedToConflictWithSecondaryBatchApplication);

    auto [resolvedNss, collection, view] =
        getCollectionForLockFreeRead(opCtx, catalog, readTimestamp, nsOrUUID, options);

    return CatalogStateForNamespace{
        catalog, isAnySecondaryNssShardedOrAView, resolvedNss, collection, view};
}

boost::optional<ShouldNotConflictWithSecondaryBatchApplicationBlock>
makeShouldNotConflictWithSecondaryBatchApplicationBlock(OperationContext* opCtx,
                                                        bool isLockFreeReadSubOperation) {
    if (!isLockFreeReadSubOperation &&
        allowSecondaryReadsDuringBatchApplication_DONT_USE(opCtx).value_or(true) &&
        opCtx->getServiceContext()->getStorageEngine()->supportsReadConcernSnapshot()) {
        return boost::optional<ShouldNotConflictWithSecondaryBatchApplicationBlock>(
            opCtx->lockState());
    } else {
        return boost::none;
    }
}

}  // namespace

CollectionPtr::RestoreFn AutoGetCollectionForReadLockFreePITCatalog::_makeRestoreFromYieldFn(
    const AutoGetCollection::Options& options,
    bool callerExpectedToConflictWithSecondaryBatchApplication,
    const DatabaseName& dbName) {
    return [this, options, callerExpectedToConflictWithSecondaryBatchApplication, dbName](
               OperationContext* opCtx, UUID uuid) -> const Collection* {
        _catalogStasher.reset();

        auto nsOrUUID = NamespaceStringOrUUID(dbName, uuid);
        try {
            auto catalogStateForNamespace = acquireCatalogStateForNamespace(
                opCtx,
                nsOrUUID,
                repl::ReadConcernArgs::get(opCtx),
                callerExpectedToConflictWithSecondaryBatchApplication,
                options);

            _resolvedNss = catalogStateForNamespace.resolvedNss;
            _view = catalogStateForNamespace.view;
            _catalogStasher.stash(std::move(catalogStateForNamespace.catalog));

            return catalogStateForNamespace.collection;
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            // Calls to CollectionCatalog::resolveNamespaceStringOrUUID (called from
            // acquireCatalogStateForNamespace) will result in a NamespaceNotFound error if the
            // collection corresponding to the UUID passed as a parameter no longer exists. This can
            // happen if this collection was dropped while the query was yielding.
            //
            // In this case, the query subsystem expects that this CollectionPtr::RestoreFn will
            // result in a nullptr, so NamespaceNotFound errors are converted to nullptr here.
            return nullptr;
        }
    };
}

AutoGetCollectionForReadLockFreePITCatalog::AutoGetCollectionForReadLockFreePITCatalog(
    OperationContext* opCtx, NamespaceStringOrUUID nsOrUUID, AutoGetCollection::Options options)
    : _originalReadSource(opCtx->recoveryUnit()->getTimestampReadSource()),
      _isLockFreeReadSubOperation(opCtx->isLockFreeReadsOp()),  // This has to come before LFRBlock.
      _catalogStasher(opCtx),
      _callerExpectedToConflictWithSecondaryBatchApplication(
          opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()),
      _shouldNotConflictWithSecondaryBatchApplicationBlock(
          makeShouldNotConflictWithSecondaryBatchApplicationBlock(opCtx,
                                                                  _isLockFreeReadSubOperation)),
      _lockFreeReadsBlock(opCtx),
      _globalLock(opCtx,
                  MODE_IS,
                  options._deadline,
                  Lock::InterruptBehavior::kThrow,
                  kLockFreeReadsGlobalLockOptions) {

    catalog_helper::setAutoGetCollectionWaitFailpointExecute(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); });

    // Supported lock-free reads should only ever have an open storage snapshot prior to
    // calling this helper if it is a nested lock-free operation. The storage snapshot and
    // in-memory state used across lock=free reads must be consistent.
    invariant(supportsLockFreeRead(opCtx) &&
              (!opCtx->recoveryUnit()->isActive() || _isLockFreeReadSubOperation));

    DatabaseShardingState::assertMatchingDbVersion(opCtx, nsOrUUID.db());

    auto readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    if (_isLockFreeReadSubOperation) {
        // If this instance is nested and lock-free, then we do not want to adjust any setting, but
        // we do need to set up the Collection reference.
        auto catalog = CollectionCatalog::get(opCtx);

        auto [resolvedNss, collection, view] =
            getCollectionForLockFreeRead(opCtx,
                                         catalog,
                                         opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx),
                                         nsOrUUID,
                                         options);
        _resolvedNss = resolvedNss;
        _view = view;

        if (_view) {
            _lockFreeReadsBlock.reset();
        }
        _collectionPtr = CollectionPtr(collection);
        // Nested operations should never yield as we don't yield when the global lock is held
        // recursively. But this is not known when we create the Query plan for this sub operation.
        // Pretend that we are yieldable but don't allow yield to actually be called.
        _collectionPtr.makeYieldable(opCtx, [](OperationContext*, UUID) {
            MONGO_UNREACHABLE;
            return nullptr;
        });
    } else {
        auto catalogStateForNamespace =
            acquireCatalogStateForNamespace(opCtx,
                                            nsOrUUID,
                                            readConcernArgs,
                                            _callerExpectedToConflictWithSecondaryBatchApplication,
                                            options);

        _resolvedNss = catalogStateForNamespace.resolvedNss;
        _view = catalogStateForNamespace.view;

        if (_view) {
            _lockFreeReadsBlock.reset();
        }
        _catalogStasher.stash(std::move(catalogStateForNamespace.catalog));
        _secondaryNssIsAViewOrSharded = catalogStateForNamespace.isAnySecondaryNssShardedOrAView;

        _collectionPtr = CollectionPtr(catalogStateForNamespace.collection);
        _collectionPtr.makeYieldable(
            opCtx,
            _makeRestoreFromYieldFn(options,
                                    _callerExpectedToConflictWithSecondaryBatchApplication,
                                    _resolvedNss.dbName()));
    }

    if (_collectionPtr) {
        assertReadConcernSupported(
            _collectionPtr, readConcernArgs, opCtx->recoveryUnit()->getTimestampReadSource());

        auto scopedCss = CollectionShardingState::acquire(opCtx, _collectionPtr->ns());
        auto collDesc = scopedCss->getCollectionDescription(opCtx);
        if (collDesc.isSharded()) {
            _collectionPtr.setShardKeyPattern(collDesc.getKeyPattern());
        }
    } else {
        invariant(!options._expectedUUID);
    }
}

AutoGetCollectionForReadLockFree::AutoGetCollectionForReadLockFree(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollection::Options options) {
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe()) {
        _impl.emplace<AutoGetCollectionForReadLockFreePITCatalog>(
            opCtx, nsOrUUID, std::move(options));
    } else {
        _impl.emplace<AutoGetCollectionForReadLockFreeLegacy>(opCtx, nsOrUUID, std::move(options));
    }
}

AutoGetCollectionForReadMaybeLockFree::AutoGetCollectionForReadMaybeLockFree(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollection::Options options) {
    if (supportsLockFreeRead(opCtx)) {
        _autoGetLockFree.emplace(opCtx, nsOrUUID, std::move(options));
    } else {
        _autoGet.emplace(opCtx, nsOrUUID, std::move(options));
    }
}

const ViewDefinition* AutoGetCollectionForReadMaybeLockFree::getView() const {
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

bool AutoGetCollectionForReadMaybeLockFree::isAnySecondaryNamespaceAViewOrSharded() const {
    if (_autoGet) {
        return _autoGet->isAnySecondaryNamespaceAViewOrSharded();
    } else {
        return _autoGetLockFree->isAnySecondaryNamespaceAViewOrSharded();
    }
}

template <typename AutoGetCollectionForReadType>
AutoGetCollectionForReadCommandBase<AutoGetCollectionForReadType>::
    AutoGetCollectionForReadCommandBase(OperationContext* opCtx,
                                        const NamespaceStringOrUUID& nsOrUUID,
                                        AutoGetCollection::Options options,
                                        AutoStatsTracker::LogMode logMode)
    : _autoCollForRead(opCtx, nsOrUUID, options),
      _statsTracker(opCtx,
                    _autoCollForRead.getNss(),
                    Top::LockType::ReadLocked,
                    logMode,
                    CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(
                        _autoCollForRead.getNss().dbName()),
                    options._deadline,
                    options._secondaryNssOrUUIDs) {
    hangBeforeAutoGetShardVersionCheck.executeIf(
        [&](auto&) { hangBeforeAutoGetShardVersionCheck.pauseWhileSet(opCtx); },
        [&](const BSONObj& data) {
            return opCtx->getLogicalSessionId() &&
                opCtx->getLogicalSessionId()->getId() == UUID::fromCDR(data["lsid"].uuid());
        });

    if (!_autoCollForRead.getView()) {
        auto scopedCss = CollectionShardingState::acquire(opCtx, _autoCollForRead.getNss());
        scopedCss->checkShardVersionOrThrow(opCtx);
    }
}

AutoGetCollectionForReadCommandLockFree::AutoGetCollectionForReadCommandLockFree(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollection::Options options,
    AutoStatsTracker::LogMode logMode) {
    _autoCollForReadCommandBase.emplace(opCtx, nsOrUUID, options, logMode);
    auto receivedShardVersion =
        OperationShardingState::get(opCtx).getShardVersion(_autoCollForReadCommandBase->getNss());

    while (_autoCollForReadCommandBase->getCollection() &&
           _autoCollForReadCommandBase->getCollection().isSharded() && receivedShardVersion &&
           receivedShardVersion.value() == ShardVersion::UNSHARDED()) {
        reachedAutoGetLockFreeShardConsistencyRetry.executeIf(
            [&](auto&) { reachedAutoGetLockFreeShardConsistencyRetry.pauseWhileSet(opCtx); },
            [&](const BSONObj& data) {
                return opCtx->getLogicalSessionId() &&
                    opCtx->getLogicalSessionId()->getId() == UUID::fromCDR(data["lsid"].uuid());
            });

        // A request may arrive with an UNSHARDED shard version for the namespace, and then running
        // lock-free it is possible that the lock-free state finds a sharded collection but
        // subsequently the namespace was dropped and recreated UNSHARDED again, in time for the SV
        // check performed in AutoGetCollectionForReadCommandBase. We must check here whether
        // sharded state was found by the lock-free state setup, and make sure that the collection
        // state in-use matches the shard version in the request. If there is an issue, we can
        // simply retry: the scenario is very unlikely.
        //
        // It's possible for there to be no SV for the namespace in the command request. That's OK
        // because shard versioning isn't needed in that case. See SERVER-63009 for more details.
        _autoCollForReadCommandBase.emplace(opCtx, nsOrUUID, options, logMode);
        receivedShardVersion = OperationShardingState::get(opCtx).getShardVersion(
            _autoCollForReadCommandBase->getNss());
    }
}

OldClientContext::OldClientContext(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   bool doVersion)
    : _opCtx(opCtx) {
    const auto dbName = nss.dbName();
    _db = DatabaseHolder::get(opCtx)->getDb(opCtx, dbName);

    if (!_db) {
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
                CollectionShardingState::assertCollectionLockedAndAcquire(_opCtx, nss)
                    ->checkShardVersionOrThrow(_opCtx);
                break;
        }
    }

    stdx::lock_guard<Client> lk(*_opCtx->getClient());
    currentOp->enter_inlock(nss,
                            CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(_db->name()));
}

AutoGetCollectionForReadCommandMaybeLockFree::AutoGetCollectionForReadCommandMaybeLockFree(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollection::Options options,
    AutoStatsTracker::LogMode logMode) {
    if (supportsLockFreeRead(opCtx)) {
        _autoGetLockFree.emplace(opCtx, nsOrUUID, std::move(options), logMode);
    } else {
        _autoGet.emplace(opCtx, nsOrUUID, std::move(options), logMode);
    }
}

const CollectionPtr& AutoGetCollectionForReadCommandMaybeLockFree::getCollection() const {
    if (_autoGet) {
        return _autoGet->getCollection();
    } else {
        return _autoGetLockFree->getCollection();
    }
}

const ViewDefinition* AutoGetCollectionForReadCommandMaybeLockFree::getView() const {
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

bool AutoGetCollectionForReadCommandMaybeLockFree::isAnySecondaryNamespaceAViewOrSharded() const {
    return _autoGet ? _autoGet->isAnySecondaryNamespaceAViewOrSharded()
                    : _autoGetLockFree->isAnySecondaryNamespaceAViewOrSharded();
}

AutoReadLockFree::AutoReadLockFree(OperationContext* opCtx, Date_t deadline)
    : _catalogStash(opCtx),
      _lockFreeReadsBlock(opCtx),
      _globalLock(opCtx, MODE_IS, deadline, Lock::InterruptBehavior::kThrow, [] {
          Lock::GlobalLockSkipOptions options;
          options.skipRSTLLock = true;
          return options;
      }()) {
    // The catalog will be stashed inside the CollectionCatalogStasher.
    FakeCollection fakeColl;
    acquireCollectionAndConsistentSnapshot(
        opCtx,
        /* isLockFreeReadSubOperation */
        false,
        /* CollectionCatalogStasher */
        _catalogStash,
        /* GetCollectionAndEstablishReadSourceFunc */
        [&](OperationContext* opCtx, const CollectionCatalog&, bool) {
            return std::make_pair(&fakeColl, /* isView */ false);
        },
        /* ResetFunc */
        []() {},
        /* SetSecondaryState */
        [](bool isAnySecondaryNamespaceAViewOrSharded) {});
}

AutoGetDbForReadLockFree::AutoGetDbForReadLockFree(OperationContext* opCtx,
                                                   const DatabaseName& dbName,
                                                   Date_t deadline)
    : _catalogStash(opCtx),
      _lockFreeReadsBlock(opCtx),
      _globalLock(opCtx, MODE_IS, deadline, Lock::InterruptBehavior::kThrow, [] {
          Lock::GlobalLockSkipOptions options;
          options.skipRSTLLock = true;
          return options;
      }()) {
    // The catalog will be stashed inside the CollectionCatalogStasher.
    FakeCollection fakeColl;
    acquireCollectionAndConsistentSnapshot(
        opCtx,
        /* isLockFreeReadSubOperation */
        false,
        /* CollectionCatalogStasher */
        _catalogStash,
        /* GetCollectionAndEstablishReadSourceFunc */
        [&](OperationContext* opCtx, const CollectionCatalog&, bool) {
            // Check that the sharding database version matches our read.
            // Note: this must always be checked, regardless of whether the collection exists, so
            // that the dbVersion of this node or the caller gets updated quickly in case either is
            // stale.
            DatabaseShardingState::assertMatchingDbVersion(opCtx, dbName);
            return std::make_pair(&fakeColl, /* isView */ false);
        },
        /* ResetFunc */
        []() {},
        /* SetSecondaryState */
        [](bool isAnySecondaryNamespaceAViewOrSharded) {});
}

AutoGetDbForReadMaybeLockFree::AutoGetDbForReadMaybeLockFree(OperationContext* opCtx,
                                                             const DatabaseName& dbName,
                                                             Date_t deadline) {
    if (supportsLockFreeRead(opCtx)) {
        _autoGetLockFree.emplace(opCtx, dbName, deadline);
    } else {
        _autoGet.emplace(opCtx, dbName, MODE_IS, deadline);
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

template class AutoGetCollectionForReadBase<AutoGetCollection, EmplaceAutoGetCollectionForRead>;
template class AutoGetCollectionForReadCommandBase<AutoGetCollectionForRead>;
template class AutoGetCollectionForReadBase<AutoGetCollectionLockFree,
                                            AutoGetCollectionForReadLockFreeLegacy::EmplaceHelper>;
template class AutoGetCollectionForReadCommandBase<AutoGetCollectionForReadLockFree>;

}  // namespace mongo
