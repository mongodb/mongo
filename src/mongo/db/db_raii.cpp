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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/catalog_helper.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/collection_yield_restore.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/snapshot_helper.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/direct_connection_util.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/message.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

const boost::optional<int> kDoNotChangeProfilingLevel = boost::none;

/**
 * Performs some checks to determine whether the operation is compatible with a lock-free read.
 * Multi-doc transactions are not supported, nor are operations holding an exclusive lock.
 */
bool supportsLockFreeRead(OperationContext* opCtx) {
    // Lock-free reads are not supported in multi-document transactions.
    // Lock-free reads are not supported when performing a write.
    // Lock-free reads are not supported if a storage txn is already open w/o the lock-free reads
    // operation flag set.
    return !opCtx->inMultiDocumentTransaction() &&
        !shard_role_details::getLocker(opCtx)->isWriteLocked() &&
        !(shard_role_details::getRecoveryUnit(opCtx)->isActive() && !opCtx->isLockFreeReadsOp());
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


bool isAnySecondaryNamespaceAView(OperationContext* opCtx,
                                  const CollectionCatalog* catalog,
                                  const std::vector<NamespaceString>& secondaryNamespaces) {
    return std::any_of(secondaryNamespaces.begin(), secondaryNamespaces.end(), [&](auto&& nss) {
        auto collection = catalog->lookupCollectionByNamespace(opCtx, nss);
        return !collection && catalog->lookupView(opCtx, nss).get();
    });
}

/**
 * Resolves all NamespaceStringOrUUIDs in the input vector by using the input catalog to call
 * CollectionCatalog::resolveSecondaryNamespacesOrUUIDs.
 *
 * If any of the input NamespaceStringOrUUIDs is found to correspond to a view, returns boost::none.
 *
 * Otherwise, returns a vector of NamespaceStrings that the input NamespaceStringOrUUIDs resolved
 * to.
 */
boost::optional<std::vector<NamespaceString>> resolveSecondaryNamespacesOrUUIDs(
    OperationContext* opCtx,
    const CollectionCatalog* catalog,
    std::vector<NamespaceStringOrUUID>::const_iterator secondaryNssOrUUIDsBegin,
    std::vector<NamespaceStringOrUUID>::const_iterator secondaryNssOrUUIDsEnd) {
    std::vector<NamespaceString> resolvedSecondaryNamespaces;
    resolvedSecondaryNamespaces.reserve(
        std::distance(secondaryNssOrUUIDsBegin, secondaryNssOrUUIDsEnd));
    for (auto iter = secondaryNssOrUUIDsBegin; iter != secondaryNssOrUUIDsEnd; ++iter) {
        const auto& nssOrUUID = *iter;
        auto nss = catalog->resolveNamespaceStringOrUUID(opCtx, nssOrUUID);
        resolvedSecondaryNamespaces.emplace_back(nss);
    }

    if (isAnySecondaryNamespaceAView(opCtx, catalog, resolvedSecondaryNamespaces)) {
        return boost::none;
    }
    return std::move(resolvedSecondaryNamespaces);
}

bool haveAcquiredConsistentCatalogAndSnapshot(OperationContext* opCtx,
                                              const CollectionCatalog* catalogBeforeSnapshot,
                                              const CollectionCatalog* catalogAfterSnapshot,
                                              long long replTermBeforeSnapshot,
                                              long long replTermAfterSnapshot) {
    return catalogBeforeSnapshot == catalogAfterSnapshot &&
        replTermBeforeSnapshot == replTermAfterSnapshot;
}

void checkInvariantsForReadOptions(boost::optional<const NamespaceString&> nss,
                                   const boost::optional<LogicalTime>& afterClusterTime,
                                   const RecoveryUnit::ReadSource& readSource,
                                   const boost::optional<Timestamp>& readTimestamp,
                                   bool shouldReadAtLastApplied,
                                   bool isEnforcingConstraints) {
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

    // If the caller is reading without a timestamp, then there is a possibility that this reader
    // may unintentionally see inconsistent data during a batch. However there are a couple
    // exceptions to this:
    // * If we are not enforcing contraints, then we are ourselves within batch application or some
    //   similar state where this is expected
    // * Certain namespaces are applied serially in oplog application, and therefore can be safely
    //   read without a timestamp
    if (readSource == RecoveryUnit::ReadSource::kNoTimestamp && isEnforcingConstraints && nss &&
        !nss->mustBeAppliedInOwnOplogBatch() && shouldReadAtLastApplied) {
        LOGV2_FATAL(4728700,
                    "Reading from replicated collection on a secondary without read timestamp",
                    logAttrs(*nss));
    }
}

void checkSecondaryNssShardVersions(
    OperationContext* opCtx,
    std::vector<NamespaceStringOrUUID>::const_iterator secondaryCollItBegin,
    std::vector<NamespaceStringOrUUID>::const_iterator secondaryCollItEnd) {
    for (auto iter = secondaryCollItBegin; iter != secondaryCollItEnd; ++iter) {
        if (iter->isNamespaceString()) {
            CollectionShardingState::acquire(opCtx, iter->nss())->checkShardVersionOrThrow(opCtx);
        }
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
    boost::optional<std::vector<NamespaceStringOrUUID>::const_iterator> secondaryNssVectorBegin,
    boost::optional<std::vector<NamespaceStringOrUUID>::const_iterator> secondaryNssVectorEnd)
    : _opCtx(opCtx), _lockType(lockType), _logMode(logMode) {
    // Deduplicate all namespaces for Top reporting on destruct.
    _nssSet.insert(nss);

    if (secondaryNssVectorBegin && secondaryNssVectorEnd) {
        auto catalog = CollectionCatalog::get(opCtx);
        for (auto iter = *secondaryNssVectorBegin; iter != *secondaryNssVectorEnd; ++iter) {
            const auto& secondaryNssOrUUID = *iter;
            _nssSet.insert(catalog->resolveNamespaceStringOrUUID(opCtx, secondaryNssOrUUID));
        }
    }

    if (_logMode == LogMode::kUpdateTop) {
        return;
    }

    stdx::lock_guard<Client> clientLock(*_opCtx->getClient());
    CurOp::get(_opCtx)->enter(clientLock, nss, dbProfilingLevel);
}

AutoStatsTracker::~AutoStatsTracker() {
    if (_logMode == LogMode::kUpdateCurOp) {
        return;
    }

    // Update stats for each namespace.
    auto curOp = CurOp::get(_opCtx);
    Top::getDecoration(_opCtx).record(
        _opCtx,
        std::span<const NamespaceString>{_nssSet.begin(), _nssSet.end()},
        curOp->getLogicalOp(),
        _lockType,
        curOp->elapsedTimeExcludingPauses(),
        curOp->isCommand(),
        curOp->getReadWriteType());
}

AutoGetCollectionForRead::AutoGetCollectionForRead(OperationContext* opCtx,
                                                   const NamespaceStringOrUUID& nsOrUUID,
                                                   const AutoGetCollection::Options& options)
    : _autoDb(AutoGetDb::createForAutoGetCollection(
          opCtx, nsOrUUID, getLockModeForQuery(opCtx, nsOrUUID), options)) {

    const auto modeColl = getLockModeForQuery(opCtx, nsOrUUID);
    const auto viewMode = options._viewMode;
    const auto deadline = options._deadline;
    const auto& secondaryNssOrUUIDsBegin = options._secondaryNssOrUUIDsBegin;
    const auto& secondaryNssOrUUIDsEnd = options._secondaryNssOrUUIDsEnd;

    // Acquire the collection locks, this will also ensure that the CollectionCatalog has been
    // correctly mapped to the given collections.
    uassert(
        ErrorCodes::InvalidNamespace,
        fmt::format("Namespace {} is not a valid collection name", nsOrUUID.toStringForErrorMsg()),
        nsOrUUID.isUUID() || (nsOrUUID.isNamespaceString() && nsOrUUID.nss().isValid()));

    catalog_helper::acquireCollectionLocksInResourceIdOrder(opCtx,
                                                            nsOrUUID,
                                                            modeColl,
                                                            deadline,
                                                            secondaryNssOrUUIDsBegin,
                                                            secondaryNssOrUUIDsEnd,
                                                            &_collLocks);

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    catalog_helper::setAutoGetCollectionWaitFailpointExecute(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); });

    auto catalog = CollectionCatalog::get(opCtx);

    // We also check commit pending entries since we could be racing with a concurrent collection
    // creation. We will later establish a consistent collection and check again that we correctly
    // got the collection. This is only necessary for UUID lookups since a collection not existing
    // causes a NamespaceNotFound error.
    _resolvedNss =
        catalog->resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(opCtx, nsOrUUID);

    // On secondaries, we have to guarantee we read at a consistent state, so we must read at the
    // lastApplied timestamp, which is set after each complete batch.

    // Once we have our locks, check whether or not we should override the ReadSource that was
    // set before acquiring locks.
    const bool shouldReadAtLastApplied =
        SnapshotHelper::changeReadSourceIfNeeded(opCtx, _resolvedNss);
    // Update readSource in case it was updated.
    const auto readSource = shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource();
    const auto readTimestamp =
        shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();

    // Check that the collections are all safe to use. First acquire collection from our catalog
    // compatible with the specified 'readTimestamp'. Creates and places a compatible PIT collection
    // reference in the 'catalog' if needed and the collection exists at that PIT.
    _coll = CollectionPtr(catalog->establishConsistentCollection(opCtx, nsOrUUID, readTimestamp));
    _coll.makeYieldable(opCtx, LockedCollectionYieldRestore{opCtx, _coll});

    // Verify the collection exists once we have acquired the consistent collection and performed a
    // UUID lookup.
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Namespace " << _resolvedNss.dbName().toStringForErrorMsg() << ":"
                          << nsOrUUID.uuid() << " not found",
            !(!_coll && nsOrUUID.isUUID()));

    // Validate primary collection.
    verifyNamespaceLockingRequirements(opCtx, modeColl, _resolvedNss);

    // Check secondary collections and verify they are valid for use.
    if (secondaryNssOrUUIDsBegin != secondaryNssOrUUIDsEnd) {
        // Check that none of the namespaces are views, which are not supported for secondary
        // namespaces.
        auto resolvedSecondaryNamespaces = resolveSecondaryNamespacesOrUUIDs(
            opCtx, catalog.get(), secondaryNssOrUUIDsBegin, secondaryNssOrUUIDsEnd);
        _isAnySecondaryNamespaceAView = !resolvedSecondaryNamespaces.has_value();
        if (!_isAnySecondaryNamespaceAView) {
            // Ensure that the readTimestamp is compatible with the latest Collection instances or
            // create PIT instances in the 'catalog' (if the collections existed at that PIT).
            for (auto iter = secondaryNssOrUUIDsBegin; iter != secondaryNssOrUUIDsEnd; ++iter) {
                const auto& secondaryNssOrUUID = *iter;
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

    // Recheck if this operation is a direct connection and if it is authorized to be one after
    // acquiring collection locks.
    direct_connection_util::checkDirectShardOperationAllowed(opCtx, _resolvedNss);

    const auto receivedShardVersion{
        OperationShardingState::get(opCtx).getShardVersion(_resolvedNss)};
    if (receivedShardVersion) {
        auto scopedCss = CollectionShardingState::acquire(opCtx, _resolvedNss);
        scopedCss->checkShardVersionOrThrow(opCtx);

        if (receivedShardVersion == ShardVersion::UNSHARDED()) {
            shard_role_details::checkLocalCatalogIsValidForUnshardedShardVersion(
                opCtx, *catalog, _coll, _resolvedNss);
        }
    }

    // Check that we have valid shard versions for all of our secondary namespaces, if any was
    // specified on the OperationShardingState.
    checkSecondaryNssShardVersions(
        opCtx, options._secondaryNssOrUUIDsBegin, options._secondaryNssOrUUIDsEnd);

    if (_coll) {
        // Fetch and store the sharding collection description data needed for use during the
        // operation. The shardVersion will be checked later if the shard filtering metadata is
        // fetched, ensuring both that the collection description info used here and the routing
        // table are consistent with the read request's shardVersion.
        //
        // Note: sharding versioning for an operation has no concept of multiple collections.
        auto scopedCss = CollectionShardingState::acquire(opCtx, _resolvedNss);
        auto collDesc = scopedCss->getCollectionDescription(opCtx);
        if (collDesc.isSharded()) {
            _coll.setShardKeyPattern(collDesc.getKeyPattern());
        }

        auto readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        assertReadConcernSupported(
            _coll,
            readConcernArgs,
            shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource());

        checkInvariantsForReadOptions(_coll->ns(),
                                      readConcernArgs.getArgsAfterClusterTime(),
                                      readSource,
                                      readTimestamp,
                                      shouldReadAtLastApplied,
                                      opCtx->isEnforcingConstraints());

        checkCollectionUUIDMismatch(opCtx, *catalog, _resolvedNss, _coll, options._expectedUUID);

        if (receivedShardVersion) {
            shard_role_details::checkShardingAndLocalCatalogCollectionUUIDMatch(
                opCtx, _resolvedNss, *receivedShardVersion, collDesc, _coll);
        }

        return;
    }

    // No Collection found, try and lookup view.
    if (!options._expectedUUID) {
        // We only need to look up a view if an expected collection UUID was not provided. If this
        // namespace were a view, the collection UUID mismatch check would have failed above.
        if ((_view = catalog->lookupView(opCtx, _resolvedNss))) {
            uassert(ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "Taking " << _resolvedNss.toStringForErrorMsg()
                                  << " lock for timeseries is not allowed",
                    viewMode == auto_get_collection::ViewMode::kViewsPermitted ||
                        !_view->timeseries());

            uassert(ErrorCodes::CommandNotSupportedOnView,
                    str::stream() << "Namespace " << _resolvedNss.toStringForErrorMsg()
                                  << " is a view, not a collection",
                    viewMode == auto_get_collection::ViewMode::kViewsPermitted);

            uassert(StaleConfigInfo(_resolvedNss,
                                    *receivedShardVersion,
                                    ShardVersion::UNSHARDED() /* wantedVersion */,
                                    ShardingState::get(opCtx)->shardId()),
                    str::stream() << "Namespace " << _resolvedNss.toStringForErrorMsg()
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
            str::stream() << "No metadata for namespace " << _resolvedNss.toStringForErrorMsg()
                          << " therefore the shard "
                          << "version attached to the request must be unset, UNSHARDED or IGNORED",
            !receivedShardVersion || *receivedShardVersion == ShardVersion::UNSHARDED() ||
                ShardVersion::isPlacementVersionIgnored(*receivedShardVersion));

    checkCollectionUUIDMismatch(opCtx, *catalog, _resolvedNss, _coll, options._expectedUUID);
}

const CollectionPtr& AutoGetCollectionForRead::getCollection() const {
    return _coll;
}

const ViewDefinition* AutoGetCollectionForRead::getView() const {
    return _view.get();
}

const NamespaceString& AutoGetCollectionForRead::getNss() const {
    return _resolvedNss;
}

namespace {

/**
 * Used as a return value for the following function.
 */
struct ConsistentCatalogAndSnapshot {
    std::shared_ptr<const CollectionCatalog> catalog;
    bool isAnySecondaryNamespaceAView;
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
    const NamespaceStringOrUUID& nsOrUUID,
    std::vector<NamespaceStringOrUUID>::const_iterator secondaryNssOrUUIDsBegin,
    std::vector<NamespaceStringOrUUID>::const_iterator secondaryNssOrUUIDsEnd,
    const repl::ReadConcernArgs& readConcernArgs,
    bool allowReadSourceChange) {
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
        NamespaceString nssStorage;
        boost::optional<const NamespaceString&> nss = nssStorage;

        try {
            // This can lookup into the commit pending entries without establishing a consistent
            // collection. This is safe because we only use this resolved namespace to check if the
            // collection is replicated or not in order to change read source if needed. As we do
            // not allow changing this setting by the user this is independent of the actual
            // collection namespace. Note that a later check in the Acquisition API will establish
            // the collection as consistent.
            nssStorage =
                catalogBeforeSnapshot->resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(
                    opCtx, nsOrUUID);
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            // The UUID cannot be resolved to a namespace in the latest catalog, but we allow the
            // call to continue which eventually calls
            // CollectionCatalog::establishConsistentCollection() which will read from the durable
            // catalog to create a new collection instance if it exists in the snapshot. This allows
            // query yields, which use UUID, to restore where the collection is committed to the
            // storage engine but not yet committed into the local catalog.
            invariant(nsOrUUID.isUUID());
            nss = boost::none;
        }

        // This may modify the read source on the recovery unit for opCtx if the current read source
        // is either kNoTimestamp or kLastApplied.
        bool shouldReadAtLastApplied = false;
        if (allowReadSourceChange) {
            shouldReadAtLastApplied = SnapshotHelper::changeReadSourceIfNeeded(opCtx, nss);
        }

        const auto resolvedSecondaryNamespaces = resolveSecondaryNamespacesOrUUIDs(
            opCtx, catalogBeforeSnapshot.get(), secondaryNssOrUUIDsBegin, secondaryNssOrUUIDsEnd);

        shard_role_details::getRecoveryUnit(opCtx)->preallocateSnapshot();

        const auto readSource =
            shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource();
        const auto readTimestamp =
            shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();

        checkInvariantsForReadOptions(nss,
                                      readConcernArgs.getArgsAfterClusterTime(),
                                      readSource,
                                      readTimestamp,
                                      shouldReadAtLastApplied,
                                      opCtx->isEnforcingConstraints());

        const auto catalogAfterSnapshot = CollectionCatalog::get(opCtx);

        const auto replTermAfterSnapshot = repl::ReplicationCoordinator::get(opCtx)->getTerm();

        if (haveAcquiredConsistentCatalogAndSnapshot(opCtx,
                                                     catalogBeforeSnapshot.get(),
                                                     catalogAfterSnapshot.get(),
                                                     replTermBeforeSnapshot,
                                                     replTermAfterSnapshot)) {
            bool isAnySecondaryNamespaceAView = !resolvedSecondaryNamespaces.has_value();
            return {catalogBeforeSnapshot, isAnySecondaryNamespaceAView, readSource, readTimestamp};
        } else {
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

            CurOp::get(opCtx)->yielded();
        }
    }
}

/**
 * Helper function to acquire a consistent catalog and storage snapshot without holding the RSTL or
 * collection locks.
 *
 * Should only be used to setup lock-free reads in a global/db context. Not safe to use for reading
 * on collections.
 *
 * Pass in boost::none as dbName to setup for a global context
 */
void acquireConsistentCatalogAndSnapshotUnsafe(OperationContext* opCtx,
                                               boost::optional<const DatabaseName&> dbName) {

    while (true) {
        // AutoGetCollectionForReadBase can choose a read source based on the current replication
        // state. Therefore we must fetch the repl state beforehand, to compare with afterwards.
        long long replTerm = repl::ReplicationCoordinator::get(opCtx)->getTerm();

        auto catalog = CollectionCatalog::get(opCtx);

        // Check that the sharding database version matches our read.
        if (dbName) {
            // Check that the sharding database version matches our read.
            const auto scopedSs =
                ShardingState::ScopedTransitionalShardingState::acquireShared(opCtx);
            if (scopedSs.isInTransitionalPhase(opCtx)) {
                scopedSs.checkDbVersionOrThrow(opCtx, *dbName);
            } else {
                DatabaseShardingState::acquire(opCtx, *dbName)->checkDbVersionOrThrow(opCtx);
            }
        }

        // We must open a storage snapshot consistent with the fetched in-memory Catalog instance.
        // The Catalog instance and replication state after opening a snapshot will be compared with
        // the previously acquired state. If either does not match, then this loop will retry lock
        // acquisition and read source selection until there is a match.
        shard_role_details::getRecoveryUnit(opCtx)->preallocateSnapshot();

        // Verify that the catalog has not changed while we opened the storage snapshot. If the
        // catalog is unchanged, then the requested Collection is also guaranteed to be the same.
        auto newCatalog = CollectionCatalog::get(opCtx);

        if (haveAcquiredConsistentCatalogAndSnapshot(
                opCtx,
                catalog.get(),
                newCatalog.get(),
                replTerm,
                repl::ReplicationCoordinator::get(opCtx)->getTerm())) {
            CollectionCatalog::stash(opCtx, std::move(catalog));
            return;
        }

        LOGV2_DEBUG(5067701,
                    3,
                    "Retrying acquiring state for lock-free read because collection, catalog or "
                    "replication state changed.");
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
    }
}

std::shared_ptr<const ViewDefinition> lookupView(
    OperationContext* opCtx,
    const std::shared_ptr<const CollectionCatalog>& catalog,
    const NamespaceString& nss,
    auto_get_collection::ViewMode viewMode) {
    auto view = catalog->lookupView(opCtx, nss);
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "Taking " << nss.toStringForErrorMsg()
                          << " lock for timeseries is not allowed",
            !view || viewMode == auto_get_collection::ViewMode::kViewsPermitted ||
                !view->timeseries());
    uassert(ErrorCodes::CommandNotSupportedOnView,
            str::stream() << "Namespace " << nss.toStringForErrorMsg()
                          << " is a view, not a collection",
            !view || viewMode == auto_get_collection::ViewMode::kViewsPermitted);
    return view;
}

std::tuple<NamespaceString, ConsistentCollection, std::shared_ptr<const ViewDefinition>>
getCollectionForLockFreeRead(OperationContext* opCtx,
                             const std::shared_ptr<const CollectionCatalog>& catalog,
                             boost::optional<Timestamp> readTimestamp,
                             const NamespaceStringOrUUID& nsOrUUID,
                             const AutoGetCollection::Options& options) {
    // Returns a collection reference compatible with the specified 'readTimestamp'. Creates and
    // places a compatible PIT collection reference in the 'catalog' if needed and the collection
    // exists at that PIT.
    auto coll = catalog->establishConsistentCollection(opCtx, nsOrUUID, readTimestamp);
    // Note: This call to resolveNamespaceStringOrUUID must happen after getCollectionFromCatalog
    // above, since getCollectionFromCatalog may call openCollection, which could change the result
    // of namespace resolution.
    auto nss = catalog->resolveNamespaceStringOrUUID(opCtx, nsOrUUID);
    checkCollectionUUIDMismatch(opCtx, *catalog, nss, coll.get(), options._expectedUUID);

    std::shared_ptr<const ViewDefinition> viewDefinition =
        coll ? nullptr : lookupView(opCtx, catalog, nss, options._viewMode);

    return {std::move(nss), std::move(coll), std::move(viewDefinition)};
}

static const Lock::GlobalLockOptions kLockFreeReadsGlobalLockOptions{[] {
    Lock::GlobalLockOptions options;
    options.skipRSTLLock = true;
    return options;
}()};

struct CatalogStateForNamespace {
    std::shared_ptr<const CollectionCatalog> catalog;
    bool isAnySecondaryNamespaceAView;
    NamespaceString resolvedNss;
    ConsistentCollection collection;
    std::shared_ptr<const ViewDefinition> view;
};

inline CatalogStateForNamespace acquireCatalogStateForNamespace(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    const repl::ReadConcernArgs& readConcernArgs,
    const AutoGetCollection::Options& options) {

    bool needsRetry = false;
    while (true) {
        auto [catalog, isAnySecondaryNamespaceAView, readSource, readTimestamp] =
            getConsistentCatalogAndSnapshot(opCtx,
                                            nsOrUUID,
                                            options._secondaryNssOrUUIDsBegin,
                                            options._secondaryNssOrUUIDsEnd,
                                            readConcernArgs,
                                            /*allowReadSourceChange=*/!needsRetry);

        auto [resolvedNss, collection, view] =
            getCollectionForLockFreeRead(opCtx, catalog, readTimestamp, nsOrUUID, options);

        // If we setup using UUID and the resolved namespace is pointing to an unreplicated
        // namespace (or the oplog), then we should retry the setup after resetting the read source
        // here using the resolved namespace. This only needs to be done once.
        if (nsOrUUID.isUUID() && !collection->ns().isReplicated() && !needsRetry) {
            // We reset the consistent collection since we can't hold one open while advancing the
            // snapshot.
            const auto ns = collection->ns();
            collection = ConsistentCollection{};
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
            [[maybe_unused]] bool shouldReadAtLastApplied =
                SnapshotHelper::changeReadSourceIfNeeded(opCtx, ns);
            needsRetry = true;
            continue;
        }

        return CatalogStateForNamespace{std::move(catalog),
                                        isAnySecondaryNamespaceAView,
                                        std::move(resolvedNss),
                                        std::move(collection),
                                        std::move(view)};
    }
}

}  // namespace

ConsistentCollection AutoGetCollectionForReadLockFree::_restoreFromYield(
    OperationContext* opCtx, boost::optional<UUID> optUuid, bool hasSecondaryNamespaces) {

    if (!optUuid) {
        tassert(9233200,
                "Restoring a non existent namespace in the presence of requested secondary "
                "namespaces in a lock-free context",
                !hasSecondaryNamespaces);
        return ConsistentCollection{};
    }

    const auto& uuid = *optUuid;
    auto nsOrUUID = NamespaceStringOrUUID(_resolvedDbName, uuid);
    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);

    try {
        auto catalogStateForNamespace =
            acquireCatalogStateForNamespace(opCtx, nsOrUUID, readConcernArgs, _options);

        // Check if this operation is a direct connection and if it is authorized to be one after
        // acquiring the snapshot.
        direct_connection_util::checkDirectShardOperationAllowed(
            opCtx, catalogStateForNamespace.resolvedNss);

        _resolvedNss = std::move(catalogStateForNamespace.resolvedNss);
        _view = std::move(catalogStateForNamespace.view);
        CollectionCatalog::stash(opCtx, std::move(catalogStateForNamespace.catalog));

        return catalogStateForNamespace.collection;
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // Calls to CollectionCatalog::resolveNamespaceStringOrUUID (called from
        // acquireCatalogStateForNamespace) will result in a NamespaceNotFound error if the
        // collection corresponding to the UUID passed as a parameter no longer exists. This can
        // happen if this collection was dropped while the query was yielding.
        //
        // In this case, the query subsystem expects that this CollectionPtr::RestoreFn will
        // result in a nullptr, so NamespaceNotFound errors are converted to nullptr here.
        return ConsistentCollection{};
    }
}

AutoGetCollectionForReadLockFree::AutoGetCollectionForReadLockFree(
    OperationContext* opCtx, NamespaceStringOrUUID nsOrUUID, AutoGetCollection::Options options)
    : _isLockFreeReadSubOperation(opCtx->isLockFreeReadsOp()),  // This has to come before LFRBlock.
      _lockFreeReadsBlock(opCtx),
      _globalLock(opCtx,
                  MODE_IS,
                  options._deadline,
                  Lock::InterruptBehavior::kThrow,
                  kLockFreeReadsGlobalLockOptions),
      _options(std::move(options)) {

    catalog_helper::setAutoGetCollectionWaitFailpointExecute(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); });

    // Supported lock-free reads should only ever have an open storage snapshot prior to
    // calling this helper if it is a nested lock-free operation. The storage snapshot and
    // in-memory state used across lock=free reads must be consistent.
    invariant(
        supportsLockFreeRead(opCtx) &&
        (!shard_role_details::getRecoveryUnit(opCtx)->isActive() || _isLockFreeReadSubOperation));

    // Pre-snapshot shard version checks.
    {
        const auto scopedSs = ShardingState::ScopedTransitionalShardingState::acquireShared(opCtx);
        if (scopedSs.isInTransitionalPhase(opCtx)) {
            scopedSs.checkDbVersionOrThrow(opCtx, nsOrUUID.dbName());
        } else {
            DatabaseShardingState::acquire(opCtx, nsOrUUID.dbName())->checkDbVersionOrThrow(opCtx);
        }
    }
    if (nsOrUUID.isNamespaceString()) {
        CollectionShardingState::acquire(opCtx, nsOrUUID.nss())->checkShardVersionOrThrow(opCtx);
    }

    checkSecondaryNssShardVersions(
        opCtx, _options._secondaryNssOrUUIDsBegin, _options._secondaryNssOrUUIDsEnd);

    auto readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    if (_isLockFreeReadSubOperation) {
        // If this instance is nested and lock-free, then we do not want to adjust any setting, but
        // we do need to set up the Collection reference.
        auto catalog = CollectionCatalog::get(opCtx);

        auto [resolvedNss, collection, view] = getCollectionForLockFreeRead(
            opCtx,
            catalog,
            shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp(),
            nsOrUUID,
            _options);
        _resolvedNss = resolvedNss;
        _resolvedDbName = _resolvedNss.dbName();
        _view = view;

        if (_view) {
            _lockFreeReadsBlock.reset();
        }
        _collectionPtr = CollectionPtr(std::move(collection));
        // Nested operations should never yield as we don't yield when the global lock is held
        // recursively. But this is not known when we create the Query plan for this sub operation.
        // Pretend that we are yieldable but don't allow yield to actually be called.
        _collectionPtr.makeYieldable(opCtx, [](OperationContext*, boost::optional<UUID>) {
            MONGO_UNREACHABLE_TASSERT(10083511);
            return ConsistentCollection{};
        });
    } else {
        auto catalogStateForNamespace =
            acquireCatalogStateForNamespace(opCtx, nsOrUUID, readConcernArgs, _options);

        _resolvedNss = std::move(catalogStateForNamespace.resolvedNss);
        _resolvedDbName = _resolvedNss.dbName();
        _view = std::move(catalogStateForNamespace.view);

        if (_view) {
            _lockFreeReadsBlock.reset();
        }
        CollectionCatalog::stash(opCtx, std::move(catalogStateForNamespace.catalog));
        _isAnySecondaryNamespaceAView = catalogStateForNamespace.isAnySecondaryNamespaceAView;

        _collectionPtr = CollectionPtr(std::move(catalogStateForNamespace.collection));

        _collectionPtr.makeYieldable(
            opCtx,
            [this,
             hasSecondaryNamespaces = (std::distance(_options._secondaryNssOrUUIDsBegin,
                                                     _options._secondaryNssOrUUIDsEnd) > 0)](
                OperationContext* opCtx, boost::optional<UUID> uuid) {
                return _restoreFromYield(opCtx, std::move(uuid), hasSecondaryNamespaces);
            });
    }

    // Check if this operation is a direct connection and if it is authorized to be one after
    // acquiring the snapshot.
    direct_connection_util::checkDirectShardOperationAllowed(opCtx, _resolvedNss);

    // Post-snapshot shard version checks.
    checkSecondaryNssShardVersions(
        opCtx, _options._secondaryNssOrUUIDsBegin, _options._secondaryNssOrUUIDsEnd);
    auto scopedCss = CollectionShardingState::acquire(opCtx, _resolvedNss);
    scopedCss->checkShardVersionOrThrow(opCtx);

    if (_collectionPtr) {
        assertReadConcernSupported(
            _collectionPtr,
            readConcernArgs,
            shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource());

        auto collDesc = scopedCss->getCollectionDescription(opCtx);
        if (collDesc.isSharded()) {
            _collectionPtr.setShardKeyPattern(collDesc.getKeyPattern());
        }
    } else {
        invariant(!_options._expectedUUID);
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

bool AutoGetCollectionForReadMaybeLockFree::isAnySecondaryNamespaceAView() const {
    if (_autoGet) {
        return _autoGet->isAnySecondaryNamespaceAView();
    } else {
        return _autoGetLockFree->isAnySecondaryNamespaceAView();
    }
}

template <typename AutoGetCollectionForReadType>
AutoGetCollectionForReadCommandBase<AutoGetCollectionForReadType>::
    AutoGetCollectionForReadCommandBase(OperationContext* opCtx,
                                        const NamespaceStringOrUUID& nsOrUUID,
                                        const AutoGetCollection::Options& options,
                                        AutoStatsTracker::LogMode logMode)
    :  // Initialize _statsTracker here only if we are acquiring by nss. In the by-uuid case we need
       // to first resolve the uuid to nss, so we defer the construction of _statsTracker.
      _statsTracker(boost::in_place_init_if,
                    nsOrUUID.isNamespaceString(),
                    opCtx,
                    nsOrUUID.isNamespaceString() ? nsOrUUID.nss() : NamespaceString::kEmpty,
                    Top::LockType::ReadLocked,
                    logMode,
                    DatabaseProfileSettings::get(opCtx->getServiceContext())
                        .getDatabaseProfileLevel(nsOrUUID.dbName()),
                    options._deadline,
                    options._secondaryNssOrUUIDsBegin,
                    options._secondaryNssOrUUIDsEnd),
      // We disable the expectedUUID option as we must check it after all the shard versioning
      // checks.
      _autoCollForRead(
          opCtx, nsOrUUID, AutoGetCollection::Options{options}.expectedUUID(boost::none)) {

    // For acquisitions by uuid, construct _statsTracker after having resolved the uuid to a
    // nss (i.e. after having constructed _autoCollForRead).
    if (nsOrUUID.isUUID()) {
        _statsTracker.emplace(opCtx,
                              _autoCollForRead.getNss(),
                              Top::LockType::ReadLocked,
                              logMode,
                              DatabaseProfileSettings::get(opCtx->getServiceContext())
                                  .getDatabaseProfileLevel(_autoCollForRead.getNss().dbName()),
                              options._deadline,
                              options._secondaryNssOrUUIDsBegin,
                              options._secondaryNssOrUUIDsEnd);
    }

    if (!_autoCollForRead.getView()) {
        auto scopedCss = CollectionShardingState::acquire(opCtx, _autoCollForRead.getNss());
        scopedCss->checkShardVersionOrThrow(opCtx);
    }

    const auto receivedShardVersion{
        OperationShardingState::get(opCtx).getShardVersion(_autoCollForRead.getNss())};
    if (receivedShardVersion == ShardVersion::UNSHARDED()) {
        const auto catalog = CollectionCatalog::get(opCtx);
        shard_role_details::checkLocalCatalogIsValidForUnshardedShardVersion(
            opCtx, *catalog, _autoCollForRead.getCollection(), _autoCollForRead.getNss());
    }

    checkCollectionUUIDMismatch(
        opCtx, _autoCollForRead.getNss(), _autoCollForRead.getCollection(), options._expectedUUID);
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

query_shape::CollectionType AutoGetCollectionForReadCommandMaybeLockFree::getCollectionType()
    const {
    if (auto&& view = getView()) {
        return view->timeseries() ? query_shape::CollectionType::kTimeseries
                                  : query_shape::CollectionType::kView;
    }
    auto&& collection = getCollection();
    return collection ? query_shape::CollectionType::kCollection
                      : query_shape::CollectionType::kNonExistent;
}

bool AutoGetCollectionForReadCommandMaybeLockFree::isAnySecondaryNamespaceAView() const {
    return _autoGet ? _autoGet->isAnySecondaryNamespaceAView()
                    : _autoGetLockFree->isAnySecondaryNamespaceAView();
}

AutoReadLockFree::AutoReadLockFree(OperationContext* opCtx, Date_t deadline)
    : _lockFreeReadsBlock(opCtx),
      _globalLock(opCtx, MODE_IS, deadline, Lock::InterruptBehavior::kThrow, [] {
          Lock::GlobalLockOptions options;
          options.skipRSTLLock = true;
          return options;
      }()) {

    acquireConsistentCatalogAndSnapshotUnsafe(opCtx, /*dbName*/ boost::none);
}

AutoGetDbForReadLockFree::AutoGetDbForReadLockFree(OperationContext* opCtx,
                                                   const DatabaseName& dbName,
                                                   Date_t deadline)
    : _lockFreeReadsBlock(opCtx),
      _globalLock(opCtx, MODE_IS, deadline, Lock::InterruptBehavior::kThrow, [] {
          Lock::GlobalLockOptions options;
          options.skipRSTLLock = true;
          return options;
      }()) {

    acquireConsistentCatalogAndSnapshotUnsafe(opCtx, dbName);

    // Check if this operation is a direct connection and if it is authorized to be one after
    // acquiring the snapshot.
    direct_connection_util::checkDirectShardOperationAllowed(opCtx, NamespaceString(dbName));
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

LockMode getLockModeForQuery(OperationContext* opCtx, const NamespaceStringOrUUID& nssOrUUID) {
    invariant(opCtx);

    // Use IX locks for multi-statement transactions; otherwise, use IS locks.
    if (opCtx->inMultiDocumentTransaction()) {
        uassert(51071,
                "Cannot query system.views within a transaction",
                !nssOrUUID.isNamespaceString() || !nssOrUUID.nss().isSystemDotViews());
        return MODE_IX;
    }
    return MODE_IS;
}

template class AutoGetCollectionForReadCommandBase<AutoGetCollectionForRead>;
template class AutoGetCollectionForReadCommandBase<AutoGetCollectionForReadLockFree>;

void assertReadConcernSupported(const CollectionPtr& coll,
                                const repl::ReadConcernArgs& readConcernArgs,
                                const RecoveryUnit::ReadSource& readSource) {
    const auto readConcernLevel = readConcernArgs.getLevel();
    const auto& ns = coll->ns();

    // Pre-images and change collection tables prune old content using untimestamped truncates. A
    // read establishing a snapshot at a point in time (PIT) may see data inconsistent with that
    // PIT: data that should have been present at that PIT will be missing if it was truncated,
    // since a non-truncated operation effectively overwrites history.
    uassert(7829600,
            "Reading with readConcern snapshot from pre-images collection is "
            "not supported",
            !ns.isChangeStreamPreImagesCollection() ||
                readConcernLevel != repl::ReadConcernLevel::kSnapshotReadConcern);
    uassert(7829601,
            "Reading with readConcern snapshot from change collection is "
            "not supported",
            !ns.isChangeCollection() ||
                readConcernLevel != repl::ReadConcernLevel::kSnapshotReadConcern);
    // Ban snapshot reads on capped collections.
    uassert(
        ErrorCodes::SnapshotUnavailable,
        "Reading from non replicated capped collections with readConcern snapshot is not supported",
        !coll->isCapped() || ns.isReplicated() ||
            readConcernLevel != repl::ReadConcernLevel::kSnapshotReadConcern);

    // Disallow snapshot reads and causal consistent majority reads on config.transactions
    // outside of transactions to avoid running the collection at a point-in-time in the middle
    // of a secondary batch. Such reads are unsafe because config.transactions updates are
    // coalesced on secondaries. Majority reads without an afterClusterTime is allowed because
    // they are allowed to return arbitrarily stale data. We allow kNoTimestamp and kLastApplied
    // reads because they must be from internal readers given the snapshot/majority readConcern
    // (e.g. for session checkout).
    if (ns == NamespaceString::kSessionTransactionsTableNamespace &&
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

}  // namespace mongo
