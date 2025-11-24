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

#include "mongo/db/shard_role/shard_catalog/db_raii.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_role/direct_connection_util.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/shard_catalog/catalog_helper.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch.h"
#include "mongo/db/shard_role/shard_catalog/collection_yield_restore.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/snapshot_helper.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/message.h"
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

bool haveAcquiredConsistentCatalogAndSnapshot(
    OperationContext* opCtx,
    const CollectionCatalog* catalogBeforeSnapshot,
    const CollectionCatalog* catalogAfterSnapshot,
    long long replTermBeforeSnapshot,
    long long replTermAfterSnapshot,
    boost::optional<OperationContext*> activeStateTransitionBeforeSnapshot,
    boost::optional<OperationContext*> activeStateTransitionAfterSnapshot) {
    // The catalog and replication term are equal before and after opening the snapshot.
    bool catalogEqual = catalogBeforeSnapshot == catalogAfterSnapshot;
    bool replTermEqual = replTermBeforeSnapshot == replTermAfterSnapshot;

    // There was no active state transition while opening the snapshot.
    bool noStateTransition =
        !activeStateTransitionBeforeSnapshot && !activeStateTransitionAfterSnapshot;

    // If there is an active transition, if our opCtx is the interruption's opCtx, we permit the
    // read so the transition can complete.
    bool isStateTransitionThread = activeStateTransitionBeforeSnapshot
        ? (opCtx == activeStateTransitionBeforeSnapshot.get())
        : false;

    // If this operation should not be killed during an interruption (it's allowed to see an
    // inconsistent state), permit the read (ex: FTDC thread).
    bool canKillOperationInStepdown = opCtx->getClient()->canKillOperationInStepdown();

    // Confirm this thread has not been interrupted already.
    opCtx->checkForInterrupt();

    return catalogEqual && replTermEqual &&
        (noStateTransition || isStateTransitionThread || !canKillOperationInStepdown);
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

namespace {

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
        const long long replTermBeforeSnapshot =
            repl::ReplicationCoordinator::get(opCtx)->getTerm();

        boost::optional<OperationContext*> activeStateTransitionBeforeSnapshot = boost::none;
        if (gFeatureFlagIntentRegistration.isEnabled()) {
            activeStateTransitionBeforeSnapshot =
                rss::consensus::IntentRegistry::get(opCtx->getServiceContext())
                    .replicationStateTransitionInterruptionCtx();
        }
        auto catalog = CollectionCatalog::get(opCtx);

        // Check that the sharding database version matches our read.
        if (dbName) {
            // Check that the sharding database version matches our read.
            DatabaseShardingState::acquire(opCtx, *dbName)->checkDbVersionOrThrow(opCtx);
        }

        // We must open a storage snapshot consistent with the fetched in-memory Catalog instance.
        // The Catalog instance and replication state after opening a snapshot will be compared with
        // the previously acquired state. If either does not match, then this loop will retry lock
        // acquisition and read source selection until there is a match.
        shard_role_details::getRecoveryUnit(opCtx)->preallocateSnapshot();

        // Verify that the catalog has not changed while we opened the storage snapshot. If the
        // catalog is unchanged, then the requested Collection is also guaranteed to be the same.
        auto newCatalog = CollectionCatalog::get(opCtx);

        // Verify that that the replication state stayed the same while we opened the storage
        // snapshot.
        const auto replTermAfterSnapshot = repl::ReplicationCoordinator::get(opCtx)->getTerm();
        boost::optional<OperationContext*> activeStateTransitionAfterSnapshot = boost::none;
        if (gFeatureFlagIntentRegistration.isEnabled()) {
            activeStateTransitionAfterSnapshot =
                rss::consensus::IntentRegistry::get(opCtx->getServiceContext())
                    .replicationStateTransitionInterruptionCtx();
        }

        if (haveAcquiredConsistentCatalogAndSnapshot(opCtx,
                                                     catalog.get(),
                                                     newCatalog.get(),
                                                     replTermBeforeSnapshot,
                                                     replTermAfterSnapshot,
                                                     activeStateTransitionBeforeSnapshot,
                                                     activeStateTransitionAfterSnapshot)) {
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
                             const auto_get_collection::Options& options) {
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

}  // namespace

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

void assertReadConcernSupported(const CollectionPtr& coll,
                                const repl::ReadConcernArgs& readConcernArgs,
                                const RecoveryUnit::ReadSource& readSource) {
    const auto readConcernLevel = readConcernArgs.getLevel();
    const auto& ns = coll->ns();

    // The pre-images collection prunes old content using untimestamped truncates. A read
    // establishing a snapshot at a point in time (PIT) may see data inconsistent with that PIT:
    // data that should have been present at that PIT will be missing if it was truncated, since a
    // non-truncated operation effectively overwrites history.
    uassert(7829600,
            "Reading with readConcern snapshot from pre-images collection is "
            "not supported",
            !ns.isChangeStreamPreImagesCollection() ||
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
