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


#include "mongo/platform/basic.h"

#include "mongo/db/db_raii.h"

#include "mongo/db/catalog/catalog_helper.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/curop.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/storage/snapshot_helper.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


MONGO_FAIL_POINT_DEFINE(hangBeforeAutoGetShardVersionCheck);
MONGO_FAIL_POINT_DEFINE(reachedAutoGetLockFreeShardConsistencyRetry);

namespace mongo {
namespace {

const boost::optional<int> kDoNotChangeProfilingLevel = boost::none;

// TODO: SERVER-44105 remove
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
 * Checks that the minimum visible timestamp of 'collection' is compatible with 'readTimestamp'.
 * Does nothing if collection does not exist.
 *
 * Returns OK or SnapshotUnavailable.
 */
Status checkSecondaryCollection(OperationContext* opCtx,
                                const CollectionPtr& collection,
                                boost::optional<Timestamp> readTimestamp) {
    // Check that the collection exists.
    if (!collection) {
        return Status::OK();
    }

    // Ensure the readTimestamp is not older than the collection's minimum visible timestamp.
    auto minSnapshot = collection->getMinimumVisibleSnapshot();
    if (SnapshotHelper::collectionChangesConflictWithRead(minSnapshot, readTimestamp)) {
        // Note: SnapshotHelper::collectionChangesConflictWithRead returns false if either
        // minSnapshot or readTimestamp is not set, so it's safe to print them below.
        return Status(ErrorCodes::SnapshotUnavailable,
                      str::stream()
                          << "Unable to read from a snapshot due to pending collection catalog "
                             "changes to collection '"
                          << collection->ns()
                          << "'; please retry the operation. Snapshot timestamp is "
                          << readTimestamp->toString() << ". Collection minimum timestamp is "
                          << minSnapshot->toString());
    }

    return Status::OK();
}

/**
 * Returns true if 'nss' is a view. False if the view doesn't exist.
 */
bool isSecondaryNssAView(OperationContext* opCtx, const NamespaceString& nss) {
    return CollectionCatalog::get(opCtx)->lookupView(opCtx, nss).get();
}

/**
 * Returns true if 'nss' is sharded. False otherwise.
 */
bool isSecondaryNssSharded(OperationContext* opCtx, const NamespaceString& nss) {
    return CollectionShardingState::getSharedForLockFreeReads(opCtx, nss)
        ->getCollectionDescription(opCtx)
        .isSharded();
}

/**
 * Takes a vector of secondary nssOrUUIDs and checks that they are consistently safe to use before
 * and after some external operation. Checks the namespaces on construction and then
 * isSecondaryStateStillConsistent() can be called to re-check that the namespaces have not changed.
 */
class SecondaryNamespaceStateChecker {
public:
    /**
     * Uasserts if any namespace has a minimum visible snapshot later than the operation's read
     * timestamp.
     *
     * Resolves the provided NamespaceStringOrUUIDs to NamespaceStrings and stores them, as well as
     * whether or not any namespace is a view or sharded, to compare against later in
     * isSecondaryStateStillConsistent().
     *
     * 'consistencyCheckBypass' can be used to bypass the before and after aspect and instead make a
     * single check on construction. The checks will be performed on construction only.
     *
     * It is safe for secondaryNssOrUUIDs to contain duplicates: namespaces will simply be
     * redundantly and benignly re-checked.
     */
    SecondaryNamespaceStateChecker(OperationContext* opCtx,
                                   const CollectionCatalog* catalog,
                                   const std::vector<NamespaceStringOrUUID>& secondaryNssOrUUIDs,
                                   bool consistencyCheckBypass = false)
        : _consistencyCheckBypass(consistencyCheckBypass) {
        const auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);
        for (const auto& secondaryNssOrUUID : secondaryNssOrUUIDs) {
            auto secondaryNss = catalog->resolveNamespaceStringOrUUID(opCtx, secondaryNssOrUUID);
            auto collection = catalog->lookupCollectionByNamespace(opCtx, secondaryNss);

            // Check that the secondary collection is safe to use.
            uassertStatusOK(checkSecondaryCollection(opCtx, collection, readTimestamp));

            if ((!collection && isSecondaryNssAView(opCtx, secondaryNss)) ||
                isSecondaryNssSharded(opCtx, secondaryNss)) {
                _haveAShardedOrViewSecondaryNss = true;
                _consistencyCheckBypass = true;

                // We early return once '_haveAShardedOrViewSecondaryNss' is set. We wish to avoid
                // extra shardVersion checks that can throw stale shard version errors.
                return;
            }

            // Create an entry for 'secondaryNss' if we have to perform the consistency check later.
            if (!_consistencyCheckBypass) {
                _namespaces.emplace_back(
                    secondaryNssOrUUID, secondaryNss, false /* pIsView */, false /* pIsSharded */);
            }
        }
    }

    /**
     * Uasserts if any namespace does not exist or has a minimum visible snapshot later than the
     * operation's read timestamp. Note: it is possible for the read timestamp to have changed since
     * construction.
     *
     * Returns false if the originally provided 'secondaryNssOrUUIDs' now resolve to different
     * NamespaceStrings or are found to now be a view or sharded when they previously where not.
     */
    bool isSecondaryStateStillConsistent(OperationContext* opCtx,
                                         const CollectionCatalog* catalog) {
        if (_consistencyCheckBypass) {
            // If we're bypassing the consistency check, we consider the secondary state to be
            // consistent.
            return true;
        }

        const auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);
        for (const auto& namespaceIt : _namespaces) {
            // Skip the consistency check if we've discovered that a secondary namespace is a view
            // or is sharded. At this point, it is not safe to use this AutoGet object to access
            // secondary namespaces.
            if (_haveAShardedOrViewSecondaryNss) {
                break;
            }

            auto secondaryNss = catalog->resolveNamespaceStringOrUUID(opCtx, namespaceIt.nssOrUUID);
            if (secondaryNss != namespaceIt.nss) {
                // A secondary collection UUID maps to a different namespace.
                return false;
            }

            auto collection = catalog->lookupCollectionByNamespace(opCtx, secondaryNss);
            uassertStatusOK(checkSecondaryCollection(opCtx, collection, readTimestamp));

            bool isView = collection ? false : isSecondaryNssAView(opCtx, secondaryNss);
            if (isView != namespaceIt.isView ||
                isSecondaryNssSharded(opCtx, secondaryNss) != namespaceIt.isSharded) {
                // A secondary namespace changed to/from sharded or to/from a view.
                return false;
            }

            if (!_haveAShardedOrViewSecondaryNss && (namespaceIt.isView || namespaceIt.isSharded)) {
                _haveAShardedOrViewSecondaryNss = true;
            }
        }

        _consistencyCheck = true;
        return true;
    }

    /**
     * Returns whether or not any of the secondary namespaces are views or sharded. Can only be
     * called after isSecondaryStateStillConsistent() has been called and returned true OR
     * 'consistencyCheckBypass' was set to true on construction.
     */
    bool isAnySecondaryNamespaceAViewOrSharded() {
        invariant(_consistencyCheck || _consistencyCheckBypass);
        return _haveAShardedOrViewSecondaryNss;
    }

private:
    /**
     * Saves a view of a NamespaceStringOrUUID: the resolved NamespaceString, and whether the
     * namespace is a view or sharded.
     */
    struct Namespace {
        Namespace(const NamespaceStringOrUUID& pNssOrUUID,
                  const NamespaceString& pNss,
                  bool pIsView,
                  bool pIsSharded)
            : nssOrUUID(pNssOrUUID), nss(pNss), isView(pIsView), isSharded(pIsSharded) {}

        NamespaceStringOrUUID nssOrUUID;
        NamespaceString nss;
        bool isView;
        bool isSharded;
    };

    // Ensures that UUID->Nss, Nss->isSharded and Nss->isView do not change. Duplicate namespaces
    // are OK, the namespace will just be checked twice. It is possible that a duplicate UUID can
    // match to two different namespaces and pass this class' checks (suppose a lot of concurrent
    // renames), but that is also OK because external checks will catch catalog changes.
    std::vector<Namespace> _namespaces;

    bool _haveAShardedOrViewSecondaryNss = false;
    // Guards access to _haveAShardedOrViewSecondaryNss.
    bool _consistencyCheck = false;
    // Bypasses the _consistencyCheck guard.
    bool _consistencyCheckBypass = false;
};

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

        SecondaryNamespaceStateChecker secondaryNssStateChecker(
            opCtx, catalog.get(), secondaryNssOrUUIDs);

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
        if (catalog == newCatalog &&
            replTerm == repl::ReplicationCoordinator::get(opCtx)->getTerm() &&
            secondaryNssStateChecker.isSecondaryStateStillConsistent(opCtx, newCatalog.get())) {
            setSecondaryState(secondaryNssStateChecker.isAnySecondaryNamespaceAViewOrSharded());
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
    CurOp::get(_opCtx)->enter_inlock(nss.ns().c_str(), dbProfilingLevel);
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

        // Disallow snapshot reads and causal consistent majority reads on config.transactions
        // outside of transactions to avoid running the collection at a point-in-time in the middle
        // of a secondary batch. Such reads are unsafe because config.transactions updates are
        // coalesced on secondaries. Majority reads without an afterClusterTime is allowed because
        // they are allowed to return arbitrarily stale data. We allow kNoTimestamp and kLastApplied
        // reads because they must be from internal readers given the snapshot/majority readConcern
        // (e.g. for session checkout).
        const NamespaceString nss = coll->ns();
        const auto afterClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAfterClusterTime();
        const auto allowTransactionTableSnapshot =
            repl::ReadConcernArgs::get(opCtx).allowTransactionTableSnapshot();
        auto readSource = opCtx->recoveryUnit()->getTimestampReadSource();
        if (nss == NamespaceString::kSessionTransactionsTableNamespace &&
            readSource != RecoveryUnit::ReadSource::kNoTimestamp &&
            readSource != RecoveryUnit::ReadSource::kLastApplied &&
            ((readConcernLevel == repl::ReadConcernLevel::kSnapshotReadConcern &&
              !allowTransactionTableSnapshot) ||
             (readConcernLevel == repl::ReadConcernLevel::kMajorityReadConcern &&
              afterClusterTime))) {
            uasserted(5557800,
                      "Snapshot reads and causal consistent majority reads on config.transactions "
                      "are not supported");
        }

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
        readSource = opCtx->recoveryUnit()->getTimestampReadSource();

        const auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);
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

        emplaceAutoColl.emplace(_autoColl);
    }
}

EmplaceAutoGetCollectionForRead::EmplaceAutoGetCollectionForRead(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollectionViewMode viewMode,
    Date_t deadline,
    const std::vector<NamespaceStringOrUUID>& secondaryNssOrUUIDs)
    : _opCtx(opCtx),
      _nsOrUUID(nsOrUUID),
      _viewMode(viewMode),
      _deadline(deadline),
      _secondaryNssOrUUIDs(secondaryNssOrUUIDs) {
    // Multi-document transactions need MODE_IX locks, otherwise MODE_IS.
    _collectionLockMode = getLockModeForQuery(opCtx, nsOrUUID.nss());
}

void EmplaceAutoGetCollectionForRead::emplace(boost::optional<AutoGetCollection>& autoColl) const {
    autoColl.emplace(
        _opCtx, _nsOrUUID, _collectionLockMode, _viewMode, _deadline, _secondaryNssOrUUIDs);
}

AutoGetCollectionForRead::AutoGetCollectionForRead(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollectionViewMode viewMode,
    Date_t deadline,
    const std::vector<NamespaceStringOrUUID>& secondaryNssOrUUIDs)
    : AutoGetCollectionForReadBase(opCtx,
                                   EmplaceAutoGetCollectionForRead(
                                       opCtx, nsOrUUID, viewMode, deadline, secondaryNssOrUUIDs)) {
    // All relevant locks are held. Check secondary collections and verify they are valid for
    // use.
    if (getCollection() && !secondaryNssOrUUIDs.empty()) {
        auto catalog = CollectionCatalog::get(opCtx);
        SecondaryNamespaceStateChecker secondaryNamespaceStateChecker(
            opCtx, catalog.get(), secondaryNssOrUUIDs, true /* consistencyCheckBypass */);
        _secondaryNssIsAViewOrSharded =
            secondaryNamespaceStateChecker.isAnySecondaryNamespaceAViewOrSharded();
    }
}

AutoGetCollectionForReadLockFree::EmplaceHelper::EmplaceHelper(
    OperationContext* opCtx,
    CollectionCatalogStasher& catalogStasher,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollectionViewMode viewMode,
    Date_t deadline,
    bool isLockFreeReadSubOperation)
    : _opCtx(opCtx),
      _catalogStasher(catalogStasher),
      _nsOrUUID(nsOrUUID),
      _viewMode(viewMode),
      _deadline(deadline),
      _isLockFreeReadSubOperation(isLockFreeReadSubOperation) {}

void AutoGetCollectionForReadLockFree::EmplaceHelper::emplace(
    boost::optional<AutoGetCollectionLockFree>& autoColl) const {
    autoColl.emplace(
        _opCtx,
        _nsOrUUID,
        /* restoreFromYield */
        [& catalogStasher = _catalogStasher, isSubOperation = _isLockFreeReadSubOperation](
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

                    auto coll = catalog.lookupCollectionByUUIDForRead(opCtx, uuid);

                    // After yielding and reacquiring locks, the preconditions that were used to
                    // select our ReadSource initially need to be checked again. We select a
                    // ReadSource based on replication state. After a query yields its locks, the
                    // replication state may have changed, invalidating our current choice of
                    // ReadSource. Using the same preconditions, change our ReadSource if necessary.
                    if (coll) {
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
        _viewMode,
        _deadline);
}

AutoGetCollectionForReadLockFree::AutoGetCollectionForReadLockFree(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollectionViewMode viewMode,
    Date_t deadline,
    const std::vector<NamespaceStringOrUUID>& secondaryNssOrUUIDs)
    : _catalogStash(opCtx) {
    bool isLockFreeReadSubOperation = opCtx->isLockFreeReadsOp();

    // Supported lock-free reads should only ever have an open storage snapshot prior to calling
    // this helper if it is a nested lock-free operation. The storage snapshot and in-memory state
    // used across lock=free reads must be consistent.
    invariant(supportsLockFreeRead(opCtx) &&
              (!opCtx->recoveryUnit()->isActive() || isLockFreeReadSubOperation));

    EmplaceHelper emplaceFunc(
        opCtx, _catalogStash, nsOrUUID, viewMode, deadline, isLockFreeReadSubOperation);
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
        secondaryNssOrUUIDs);
}

AutoGetCollectionForReadMaybeLockFree::AutoGetCollectionForReadMaybeLockFree(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollectionViewMode viewMode,
    Date_t deadline,
    const std::vector<NamespaceStringOrUUID>& secondaryNssOrUUIDs) {
    if (supportsLockFreeRead(opCtx)) {
        _autoGetLockFree.emplace(opCtx, nsOrUUID, viewMode, deadline, secondaryNssOrUUIDs);
    } else {
        _autoGet.emplace(opCtx, nsOrUUID, viewMode, deadline, secondaryNssOrUUIDs);
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
    AutoGetCollectionForReadCommandBase(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nsOrUUID,
        AutoGetCollectionViewMode viewMode,
        Date_t deadline,
        AutoStatsTracker::LogMode logMode,
        const std::vector<NamespaceStringOrUUID>& secondaryNssOrUUIDs)
    : _autoCollForRead(opCtx, nsOrUUID, viewMode, deadline, secondaryNssOrUUIDs),
      _statsTracker(opCtx,
                    _autoCollForRead.getNss(),
                    Top::LockType::ReadLocked,
                    logMode,
                    CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(
                        _autoCollForRead.getNss().dbName()),
                    deadline,
                    secondaryNssOrUUIDs) {

    hangBeforeAutoGetShardVersionCheck.executeIf(
        [&](auto&) { hangBeforeAutoGetShardVersionCheck.pauseWhileSet(opCtx); },
        [&](const BSONObj& data) {
            return opCtx->getLogicalSessionId() &&
                opCtx->getLogicalSessionId()->getId() == UUID::fromCDR(data["lsid"].uuid());
        });

    if (!_autoCollForRead.getView()) {
        auto css =
            CollectionShardingState::getSharedForLockFreeReads(opCtx, _autoCollForRead.getNss());
        css->checkShardVersionOrThrow(opCtx);
    }
}

AutoGetCollectionForReadCommandLockFree::AutoGetCollectionForReadCommandLockFree(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollectionViewMode viewMode,
    Date_t deadline,
    AutoStatsTracker::LogMode logMode,
    const std::vector<NamespaceStringOrUUID>& secondaryNssOrUUIDs) {
    _autoCollForReadCommandBase.emplace(
        opCtx, nsOrUUID, viewMode, deadline, logMode, secondaryNssOrUUIDs);
    auto receivedShardVersion =
        OperationShardingState::get(opCtx).getShardVersion(_autoCollForReadCommandBase->getNss());

    while (_autoCollForReadCommandBase->getCollection() &&
           _autoCollForReadCommandBase->getCollection().isSharded() && receivedShardVersion &&
           receivedShardVersion.get() == ChunkVersion::UNSHARDED()) {
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
        _autoCollForReadCommandBase.emplace(
            opCtx, nsOrUUID, viewMode, deadline, logMode, secondaryNssOrUUIDs);
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
                CollectionShardingState::get(_opCtx, nss)->checkShardVersionOrThrow(_opCtx);
                break;
        }
    }

    stdx::lock_guard<Client> lk(*_opCtx->getClient());
    currentOp->enter_inlock(nss.toString().c_str(),
                            CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(_db->name()));
}

AutoGetCollectionForReadCommandMaybeLockFree::AutoGetCollectionForReadCommandMaybeLockFree(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AutoGetCollectionViewMode viewMode,
    Date_t deadline,
    AutoStatsTracker::LogMode logMode,
    const std::vector<NamespaceStringOrUUID>& secondaryNssOrUUIDs) {
    if (supportsLockFreeRead(opCtx)) {
        _autoGetLockFree.emplace(opCtx, nsOrUUID, viewMode, deadline, logMode, secondaryNssOrUUIDs);
    } else {
        _autoGet.emplace(opCtx, nsOrUUID, viewMode, deadline, logMode, secondaryNssOrUUIDs);
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
      _globalLock(
          opCtx, MODE_IS, deadline, Lock::InterruptBehavior::kThrow, true /* skipRSTLLock */) {
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
      _globalLock(
          opCtx, MODE_IS, deadline, Lock::InterruptBehavior::kThrow, true /* skipRSTLLock */) {
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
            // TODO SERVER-63706 Pass dbName directly.
            catalog_helper::assertMatchingDbVersion(opCtx, dbName.toStringWithTenantId());
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
                                            AutoGetCollectionForReadLockFree::EmplaceHelper>;
template class AutoGetCollectionForReadCommandBase<AutoGetCollectionForReadLockFree>;

}  // namespace mongo
