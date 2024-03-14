/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/shard_role.h"

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/utility/in_place_factory.hpp>  // IWYU pragma: keep
#include <fmt/format.h>
#include <iterator>
#include <list>
#include <map>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/catalog_helper.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/direct_connection_util.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/capped_snapshots.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot_helper.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

using namespace fmt::literals;

using TransactionResources = shard_role_details::TransactionResources;

namespace {

enum class ResolutionType { kUUID, kNamespace };

struct ResolvedNamespaceOrViewAcquisitionRequest {
    // Populated in the first phase of collection(s) acquisition.
    AcquisitionPrerequisites prerequisites;
    ResolutionType resolvedBy;

    // Populated only for locked acquisitions in the second phase of collection(s) acquisition.
    std::shared_ptr<Lock::DBLock> dbLock;
    boost::optional<Lock::CollectionLock> collLock;

    // Resources for lock free reads.
    struct LockFreeReadsResources {
        std::shared_ptr<LockFreeReadsBlock> lockFreeReadsBlock;
        std::shared_ptr<Lock::GlobalLock> globalLock;
    } lockFreeReadsResources;

    shard_role_details::AcquisitionLocks acquisitionLocks;
};

using ResolvedNamespaceOrViewAcquisitionRequestsMap =
    std::map<ResourceId, ResolvedNamespaceOrViewAcquisitionRequest>;

void validateResolvedCollectionByUUID(OperationContext* opCtx,
                                      CollectionOrViewAcquisitionRequest ar,
                                      const Collection* coll) {
    invariant(ar.nssOrUUID.isUUID());
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Namespace " << ar.nssOrUUID.dbName().toStringForErrorMsg() << ":"
                          << ar.nssOrUUID.uuid() << " not found",
            coll);
    auto shardVersion = OperationShardingState::get(opCtx).getShardVersion(coll->ns());
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Collection " << ar.nssOrUUID.dbName().toStringForErrorMsg() << ":"
                          << ar.nssOrUUID.uuid()
                          << " acquired by UUID has a ShardVersion attached.",
            !shardVersion || shardVersion == ShardVersion::UNSHARDED());
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Database name mismatch for "
                          << ar.nssOrUUID.dbName().toStringForErrorMsg() << ":"
                          << ar.nssOrUUID.uuid()
                          << ". Expected: " << ar.nssOrUUID.dbName().toStringForErrorMsg()
                          << " Actual: " << coll->ns().dbName().toStringForErrorMsg(),
            coll->ns().dbName() == ar.nssOrUUID.dbName());
}

/**
 * Takes the input acquisitions, populates the NSS and returns a map sorted by NSS, suitable for
 * locking them in NSS order.
 */
ResolvedNamespaceOrViewAcquisitionRequestsMap resolveNamespaceOrViewAcquisitionRequests(
    OperationContext* opCtx,
    const CollectionCatalog& catalog,
    const std::vector<CollectionOrViewAcquisitionRequest>& acquisitionRequests) {

    ResolvedNamespaceOrViewAcquisitionRequestsMap sortedAcquisitionRequests;

    for (const auto& ar : acquisitionRequests) {
        if (ar.nssOrUUID.isNamespaceString()) {
            AcquisitionPrerequisites prerequisites(ar.nssOrUUID.nss(),
                                                   ar.expectedUUID,
                                                   ar.readConcern,
                                                   ar.placementConcern,
                                                   ar.operationType,
                                                   ar.viewMode,
                                                   ar.lockAcquisitionDeadline);

            ResolvedNamespaceOrViewAcquisitionRequest resolvedAcquisitionRequest{
                prerequisites, ResolutionType::kNamespace, nullptr, boost::none};

            sortedAcquisitionRequests.emplace(ResourceId(RESOURCE_COLLECTION, ar.nssOrUUID.nss()),
                                              std::move(resolvedAcquisitionRequest));
        } else if (ar.nssOrUUID.isUUID()) {
            auto coll = catalog.lookupCollectionByUUID(opCtx, ar.nssOrUUID.uuid());

            validateResolvedCollectionByUUID(opCtx, ar, coll);

            AcquisitionPrerequisites prerequisites(coll->ns(),
                                                   coll->uuid(),
                                                   ar.readConcern,
                                                   ar.placementConcern,
                                                   ar.operationType,
                                                   ar.viewMode);

            ResolvedNamespaceOrViewAcquisitionRequest resolvedAcquisitionRequest{
                prerequisites, ResolutionType::kUUID, nullptr, boost::none};

            sortedAcquisitionRequests.emplace(ResourceId(RESOURCE_COLLECTION, coll->ns()),
                                              std::move(resolvedAcquisitionRequest));
        } else {
            MONGO_UNREACHABLE;
        }
    }

    return sortedAcquisitionRequests;
}

void verifyDbAndCollection(OperationContext* opCtx,
                           const NamespaceString& nss,
                           CollectionPtr& coll,
                           AcquisitionPrerequisites::OperationType operationType) {
    invariant(coll);

    // In most cases we expect modifications for system.views to upgrade MODE_IX to MODE_X
    // before taking the lock. One exception is a query by UUID of system.views in a
    // transaction. Usual queries of system.views (by name, not UUID) within a transaction are
    // rejected. However, if the query is by UUID we can't determine whether the namespace is
    // actually system.views until we take the lock here. So we have these last two assertions.
    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "Cannot access system.views collection in a transaction",
            !(opCtx->inMultiDocumentTransaction() && nss.isSystemDotViews()));
    uassert(6944500,
            "Modifications to system.views must take an exclusive lock",
            operationType == AcquisitionPrerequisites::OperationType::kRead ||
                !nss.isSystemDotViews() ||
                shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_X));

    // Verify that we are using the latest instance if we intend to perform writes.
    if (operationType == AcquisitionPrerequisites::OperationType::kWrite) {
        auto latest = CollectionCatalog::latest(opCtx);
        if (!latest->isLatestCollection(opCtx, coll.get())) {
            throwWriteConflictException(str::stream() << "Unable to write to collection '"
                                                      << coll->ns().toStringForErrorMsg()
                                                      << "' due to catalog changes; please "
                                                         "retry the operation");
        }
        if (shard_role_details::getRecoveryUnit(opCtx)->isActive()) {
            const auto mySnapshot =
                shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp(opCtx);
            if (mySnapshot && *mySnapshot < coll->getMinimumValidSnapshot()) {
                throwWriteConflictException(str::stream()
                                            << "Unable to write to collection '"
                                            << coll->ns().toStringForErrorMsg()
                                            << "' due to snapshot timestamp " << *mySnapshot
                                            << " being older than collection minimum "
                                            << *coll->getMinimumValidSnapshot()
                                            << "; please retry the operation");
            }
        }
    }
}

void checkPlacementVersion(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const PlacementConcern& placementConcern) {
    const auto& receivedDbVersion = placementConcern.dbVersion;
    if (receivedDbVersion) {
        const auto scopedDss = DatabaseShardingState::acquireShared(opCtx, nss.dbName());
        scopedDss->assertMatchingDbVersion(opCtx, *receivedDbVersion);
    }

    const auto& receivedShardVersion = placementConcern.shardVersion;
    if (receivedShardVersion) {
        const auto scopedCSS = CollectionShardingState::acquire(opCtx, nss);
        scopedCSS->checkShardVersionOrThrow(opCtx, *receivedShardVersion);
    }
}

std::variant<CollectionPtr, std::shared_ptr<const ViewDefinition>> acquireLocalCollectionOrView(
    OperationContext* opCtx,
    const CollectionCatalog& catalog,
    const AcquisitionPrerequisites& prerequisites) {
    const auto& nss = prerequisites.nss;

    auto readTimestamp =
        shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp(opCtx);
    auto coll = CollectionPtr(
        catalog.establishConsistentCollection(opCtx, NamespaceStringOrUUID(nss), readTimestamp));
    checkCollectionUUIDMismatch(opCtx, catalog, nss, coll, prerequisites.uuid);

    if (coll) {
        verifyDbAndCollection(opCtx, nss, coll, prerequisites.operationType);

        // TODO SERVER-79401: To mimic the previous behaviour with AutoGetCollectionForRead we only
        // check for usage of the correct read concern on reads. Otherwise multi-document
        // transactions cannot perform commits since they perform writes with an "invalid" read
        // concern for the user (snapshot).
        if (prerequisites.operationType == AcquisitionPrerequisites::kRead) {
            assertReadConcernSupported(
                coll,
                prerequisites.readConcern,
                shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource());
        }

        return coll;
    } else if (auto view = catalog.lookupView(opCtx, nss)) {
        uassert(ErrorCodes::CommandNotSupportedOnView,
                str::stream() << "Namespace " << nss.toStringForErrorMsg()
                              << " is a view, not a collection",
                prerequisites.viewMode == AcquisitionPrerequisites::kCanBeView);
        return view;
    } else {
        return CollectionPtr();
    }
}

struct SnapshotedServices {
    std::variant<CollectionPtr, std::shared_ptr<const ViewDefinition>> collectionPtrOrView;
    boost::optional<ScopedCollectionDescription> collectionDescription;
    boost::optional<ScopedCollectionFilter> ownershipFilter;
};

SnapshotedServices acquireServicesSnapshot(OperationContext* opCtx,
                                           const CollectionCatalog& catalog,
                                           const AcquisitionPrerequisites& prerequisites) {
    if (holds_alternative<AcquisitionPrerequisites::PlacementConcernPlaceholder>(
            prerequisites.placementConcern)) {
        return SnapshotedServices{
            acquireLocalCollectionOrView(opCtx, catalog, prerequisites), boost::none, boost::none};
    }

    const auto& placementConcern = get<PlacementConcern>(prerequisites.placementConcern);

    auto collOrView = acquireLocalCollectionOrView(opCtx, catalog, prerequisites);
    const auto& nss = prerequisites.nss;

    const auto scopedCSS = CollectionShardingState::acquire(opCtx, nss);
    auto collectionDescription =
        scopedCSS->getCollectionDescription(opCtx, placementConcern.shardVersion.has_value());

    invariant(!collectionDescription.isSharded() || placementConcern.shardVersion);
    auto optOwnershipFilter = collectionDescription.isSharded()
        ? boost::optional<ScopedCollectionFilter>(scopedCSS->getOwnershipFilter(
              opCtx,
              prerequisites.operationType == AcquisitionPrerequisites::OperationType::kRead
                  ? CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup
                  : CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup,
              *placementConcern.shardVersion))
        : boost::none;

    // TODO: This will be removed when we no longer snapshot sharding state on CollectionPtr.
    if (holds_alternative<CollectionPtr>(collOrView) && collectionDescription.isSharded()) {
        get<CollectionPtr>(collOrView).setShardKeyPattern(collectionDescription.getKeyPattern());
    }

    return SnapshotedServices{
        std::move(collOrView), std::move(collectionDescription), std::move(optOwnershipFilter)};
}

const Lock::GlobalLockSkipOptions kLockFreeReadsGlobalLockOptions{[] {
    Lock::GlobalLockSkipOptions options;
    options.skipRSTLLock = true;
    return options;
}()};

CollectionOrViewAcquisitions acquireResolvedCollectionsOrViewsWithoutTakingLocks(
    OperationContext* opCtx,
    const CollectionCatalog& catalog,
    ResolvedNamespaceOrViewAcquisitionRequestsMap sortedAcquisitionRequests) {
    CollectionOrViewAcquisitions acquisitions;

    auto& txnResources = TransactionResources::get(opCtx);
    invariant(txnResources.state != shard_role_details::TransactionResources::State::YIELDED,
              "Cannot make a new acquisition in the YIELDED state");
    invariant(txnResources.state != shard_role_details::TransactionResources::State::FAILED,
              "Cannot make a new acquisition in the FAILED state");

    auto currentAcquireCallNum = txnResources.increaseAcquireCollectionCallCount();

    for (auto& acquisitionRequest : sortedAcquisitionRequests) {
        auto& prerequisites = acquisitionRequest.second.prerequisites;

        auto snapshotedServices = acquireServicesSnapshot(opCtx, catalog, prerequisites);
        const bool isCollection =
            holds_alternative<CollectionPtr>(snapshotedServices.collectionPtrOrView);

        const boost::optional<ShardVersion> placementConcernShardVersion =
            holds_alternative<PlacementConcern>(prerequisites.placementConcern)
            ? get<PlacementConcern>(prerequisites.placementConcern).shardVersion
            : boost::none;

        if (placementConcernShardVersion == ShardVersion::UNSHARDED()) {
            shard_role_details::checkLocalCatalogIsValidForUnshardedShardVersion(
                opCtx,
                catalog,
                isCollection ? get<CollectionPtr>(snapshotedServices.collectionPtrOrView)
                             : CollectionPtr::null,
                prerequisites.nss);
        }

        if (isCollection) {
            const auto& collectionPtr = get<CollectionPtr>(snapshotedServices.collectionPtrOrView);

            if (placementConcernShardVersion && snapshotedServices.collectionDescription) {
                shard_role_details::checkShardingAndLocalCatalogCollectionUUIDMatch(
                    opCtx,
                    prerequisites.nss,
                    *placementConcernShardVersion,
                    *snapshotedServices.collectionDescription,
                    collectionPtr);
            }

            invariant(!prerequisites.uuid || prerequisites.uuid == collectionPtr->uuid());
            if (!prerequisites.uuid && collectionPtr) {
                // If the uuid wasn't originally set on the AcquisitionRequest, set it now on the
                // prerequisites so that on restore from yield we can check we are restoring the
                // same instance of the ns.
                prerequisites.uuid = collectionPtr->uuid();
            }

            acquisitionRequest.second.acquisitionLocks.hasLockFreeReadsBlock =
                bool(acquisitionRequest.second.lockFreeReadsResources.lockFreeReadsBlock);

            if (const auto& ptr = acquisitionRequest.second.lockFreeReadsResources.globalLock) {
                acquisitionRequest.second.acquisitionLocks.globalLock =
                    ptr->isLocked() ? MODE_IS : MODE_NONE;
                acquisitionRequest.second.acquisitionLocks.globalLockOptions =
                    kLockFreeReadsGlobalLockOptions;
            }

            shard_role_details::AcquiredCollection& acquiredCollection =
                txnResources.addAcquiredCollection(
                    {currentAcquireCallNum,
                     prerequisites,
                     std::move(acquisitionRequest.second.dbLock),
                     std::move(acquisitionRequest.second.collLock),
                     std::move(acquisitionRequest.second.lockFreeReadsResources.lockFreeReadsBlock),
                     std::move(acquisitionRequest.second.lockFreeReadsResources.globalLock),
                     std::move(acquisitionRequest.second.acquisitionLocks),
                     std::move(snapshotedServices.collectionDescription),
                     std::move(snapshotedServices.ownershipFilter),
                     std::move(get<CollectionPtr>(snapshotedServices.collectionPtrOrView))});

            CollectionAcquisition acquisition(txnResources, acquiredCollection);
            acquisitions.emplace(prerequisites.nss, std::move(acquisition));
        } else {
            // It's a view.
            auto& acquiredView =
                txnResources.addAcquiredView({prerequisites,
                                              std::move(acquisitionRequest.second.dbLock),
                                              std::move(acquisitionRequest.second.collLock),
                                              std::move(get<std::shared_ptr<const ViewDefinition>>(
                                                  snapshotedServices.collectionPtrOrView))});

            ViewAcquisition acquisition(txnResources, acquiredView);
            acquisitions.emplace(prerequisites.nss, std::move(acquisition));
        }
    }

    // Check if this operation is a direct connection and if it is authorized to be one.
    const auto& dbName = sortedAcquisitionRequests.begin()->second.prerequisites.nss.dbName();
    direct_connection_util::checkDirectShardOperationAllowed(opCtx, dbName);

    return acquisitions;
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

bool haveAcquiredConsistentCatalogAndSnapshot(const CollectionCatalog* catalogBeforeSnapshot,
                                              const CollectionCatalog* catalogAfterSnapshot,
                                              long long replTermBeforeSnapshot,
                                              long long replTermAfterSnapshot) {
    return catalogBeforeSnapshot == catalogAfterSnapshot &&
        replTermBeforeSnapshot == replTermAfterSnapshot;
}

std::shared_ptr<const CollectionCatalog> getConsistentCatalogAndSnapshot(
    OperationContext* opCtx, const std::vector<NamespaceStringOrUUID>& acquisitionRequests) {
    while (true) {
        shard_role_details::SnapshotAttempt snapshotAttempt(opCtx, acquisitionRequests);
        snapshotAttempt.snapshotInitialState();
        snapshotAttempt.changeReadSourceForSecondaryReads();
        snapshotAttempt.openStorageSnapshot();
        if (auto catalog = snapshotAttempt.getConsistentCatalog()) {
            return catalog;
        }
    }
}

std::vector<NamespaceStringOrUUID> toNamespaceStringOrUUIDs(
    const std::list<shard_role_details::AcquiredCollection>& acquiredCollections) {
    std::vector<NamespaceStringOrUUID> requests;
    for (const auto& acquiredCollection : acquiredCollections) {
        const auto& prerequisites = acquiredCollection.prerequisites;
        requests.emplace_back(prerequisites.nss);
    }
    return requests;
}

std::vector<NamespaceStringOrUUID> toNamespaceStringOrUUIDs(
    const std::vector<CollectionOrViewAcquisitionRequest>& acquisitionRequests) {
    std::vector<NamespaceStringOrUUID> requests;
    for (const auto& ar : acquisitionRequests) {
        requests.emplace_back(ar.nssOrUUID);
    }
    return requests;
}

void validateRequests(const std::vector<CollectionOrViewAcquisitionRequest>& acquisitionRequests) {
    for (const auto& ar : acquisitionRequests) {
        if (ar.nssOrUUID.isNamespaceString()) {
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Namespace " << ar.nssOrUUID.nss().toStringForErrorMsg()
                                  << "is not a valid collection name",
                    ar.nssOrUUID.nss().isValid());
        } else if (ar.nssOrUUID.isUUID()) {
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid db name "
                                  << ar.nssOrUUID.dbName().toStringForErrorMsg(),
                    DatabaseName::isValid(ar.nssOrUUID.dbName(),
                                          DatabaseName::DollarInDbNameBehavior::Allow));
        } else {
            MONGO_UNREACHABLE;
        }
    }
}

void checkShardingPlacement(
    OperationContext* opCtx,
    const std::vector<CollectionOrViewAcquisitionRequest>& acquisitionRequests) {
    for (const auto& ar : acquisitionRequests) {
        // We only have to check placement for collections that come from a router, which
        // will have the namespace set.
        if (ar.nssOrUUID.isNamespaceString()) {
            checkPlacementVersion(opCtx, ar.nssOrUUID.nss(), ar.placementConcern);
        }
    }
}

ResolvedNamespaceOrViewAcquisitionRequest::LockFreeReadsResources takeGlobalLock(
    OperationContext* opCtx,
    const std::vector<CollectionOrViewAcquisitionRequest>& acquisitionRequests) {
    auto lockFreeReadsBlock = std::make_shared<LockFreeReadsBlock>(opCtx);
    auto globalLock = std::make_shared<Lock::GlobalLock>(opCtx,
                                                         MODE_IS,
                                                         Date_t::max(),
                                                         Lock::InterruptBehavior::kThrow,
                                                         kLockFreeReadsGlobalLockOptions);
    return {lockFreeReadsBlock, globalLock};
}

std::shared_ptr<const CollectionCatalog> stashConsistentCatalog(
    OperationContext* opCtx,
    const std::vector<CollectionOrViewAcquisitionRequest>& acquisitionRequests) {
    auto requests = toNamespaceStringOrUUIDs(acquisitionRequests);
    auto catalog = getConsistentCatalogAndSnapshot(opCtx, requests);
    // Stash the catalog, it will be automatically unstashed when the snapshot is released.
    CollectionCatalog::stash(opCtx, catalog);
    return catalog;
}

// TODO SERVER-77067 simplify conditions
bool supportsLockFreeRead(OperationContext* opCtx) {
    // Lock-free reads are not supported:
    //   * in multi-document transactions.
    //   * under an IX lock (nested reads under IX lock holding operations).
    //   * if a storage txn is already open w/o the lock-free reads operation flag set.
    return !storageGlobalParams.disableLockFreeReads && !opCtx->inMultiDocumentTransaction() &&
        !shard_role_details::getLocker(opCtx)->isWriteLocked() &&
        !(shard_role_details::getRecoveryUnit(opCtx)->isActive() && !opCtx->isLockFreeReadsOp());
}

}  // namespace

CollectionOrViewAcquisitionRequest CollectionOrViewAcquisitionRequest::fromOpCtx(
    OperationContext* opCtx,
    NamespaceStringOrUUID nssOrUUID,
    AcquisitionPrerequisites::OperationType operationType,
    AcquisitionPrerequisites::ViewMode viewMode) {
    auto& oss = OperationShardingState::get(opCtx);
    auto& readConcern = repl::ReadConcernArgs::get(opCtx);

    // Acquisitions by uuid cannot possibly have a corresponding ShardVersion attached.
    PlacementConcern placementConcern = nssOrUUID.isNamespaceString()
        ? PlacementConcern{oss.getDbVersion(nssOrUUID.dbName()),
                           oss.getShardVersion(nssOrUUID.nss())}
        : PlacementConcern{oss.getDbVersion(nssOrUUID.dbName()), {}};

    return CollectionOrViewAcquisitionRequest(
        nssOrUUID, placementConcern, readConcern, operationType, viewMode);
}

CollectionAcquisitionRequest CollectionAcquisitionRequest::fromOpCtx(
    OperationContext* opCtx,
    NamespaceString nss,
    AcquisitionPrerequisites::OperationType operationType,
    boost::optional<UUID> expectedUUID,
    Date_t lockAcquisitionDeadline) {
    auto& oss = OperationShardingState::get(opCtx);
    auto& readConcern = repl::ReadConcernArgs::get(opCtx);

    return CollectionAcquisitionRequest(nss,
                                        expectedUUID,
                                        {oss.getDbVersion(nss.dbName()), oss.getShardVersion(nss)},
                                        readConcern,
                                        operationType,
                                        lockAcquisitionDeadline);
}

CollectionAcquisitionRequest CollectionAcquisitionRequest::fromOpCtx(
    OperationContext* opCtx,
    NamespaceStringOrUUID nssOrUUID,
    AcquisitionPrerequisites::OperationType operationType,
    Date_t lockAcquisitionDeadline) {
    auto& oss = OperationShardingState::get(opCtx);
    auto& readConcern = repl::ReadConcernArgs::get(opCtx);

    // Acquisitions by uuid cannot possibly have a corresponding ShardVersion attached.
    PlacementConcern placementConcern = nssOrUUID.isNamespaceString()
        ? PlacementConcern{oss.getDbVersion(nssOrUUID.dbName()),
                           oss.getShardVersion(nssOrUUID.nss())}
        : PlacementConcern{oss.getDbVersion(nssOrUUID.dbName()), {}};

    return CollectionAcquisitionRequest(
        nssOrUUID, placementConcern, readConcern, operationType, lockAcquisitionDeadline);
}

CollectionAcquisition::CollectionAcquisition(
    shard_role_details::TransactionResources& txnResources,
    shard_role_details::AcquiredCollection& acquiredCollection)
    : _txnResources(&txnResources), _acquiredCollection(&acquiredCollection) {
    _txnResources->collectionAcquisitionReferences++;
    _acquiredCollection->refCount++;
}

CollectionAcquisition::CollectionAcquisition(const CollectionAcquisition& other)
    : _txnResources(other._txnResources), _acquiredCollection(other._acquiredCollection) {
    _txnResources->collectionAcquisitionReferences++;
    _acquiredCollection->refCount++;
}

CollectionAcquisition::CollectionAcquisition(CollectionAcquisition&& other)
    : _txnResources(other._txnResources), _acquiredCollection(other._acquiredCollection) {
    other._txnResources = nullptr;
    other._acquiredCollection = nullptr;
}

CollectionAcquisition& CollectionAcquisition::operator=(const CollectionAcquisition& other) {
    this->~CollectionAcquisition();
    _txnResources = other._txnResources;
    _acquiredCollection = other._acquiredCollection;
    _txnResources->collectionAcquisitionReferences++;
    _acquiredCollection->refCount++;
    return *this;
}

CollectionAcquisition& CollectionAcquisition::operator=(CollectionAcquisition&& other) {
    this->~CollectionAcquisition();
    _txnResources = other._txnResources;
    other._txnResources = nullptr;
    _acquiredCollection = other._acquiredCollection;
    other._acquiredCollection = nullptr;
    return *this;
}

CollectionAcquisition::CollectionAcquisition(CollectionOrViewAcquisition&& other) {
    invariant(other.isCollection());
    auto& acquisition = get<CollectionAcquisition>(other._collectionOrViewAcquisition);
    _txnResources = acquisition._txnResources;
    acquisition._txnResources = nullptr;
    _acquiredCollection = acquisition._acquiredCollection;
    acquisition._acquiredCollection = nullptr;
    other._collectionOrViewAcquisition = std::monostate();
}

CollectionAcquisition::~CollectionAcquisition() {
    if (!_txnResources) {
        return;
    }

    auto& transactionResources = *_txnResources;

    // If the TransactionResources have failed to restore or yield we've released all the resources.
    // Our reference to the acquisition is invalid and we've already removed it from the list of
    // acquisitions.
    if (transactionResources.state == shard_role_details::TransactionResources::State::ACTIVE) {
        auto currentRefCount = --_acquiredCollection->refCount;
        if (currentRefCount == 0)
            transactionResources.acquiredCollections.remove_if(
                [&](const shard_role_details::AcquiredCollection& txnResourceAcquiredColl) {
                    return &txnResourceAcquiredColl == _acquiredCollection;
                });
    }

    transactionResources.collectionAcquisitionReferences--;
    if (transactionResources.acquiredCollections.empty() &&
        transactionResources.acquiredViews.empty()) {
        transactionResources.releaseAllResourcesOnCommitOrAbort();
        transactionResources.state = shard_role_details::TransactionResources::State::EMPTY;
    }
}

const NamespaceString& CollectionAcquisition::nss() const {
    return _acquiredCollection->prerequisites.nss;
}

bool CollectionAcquisition::exists() const {
    return bool(_acquiredCollection->collectionPtr);
}

UUID CollectionAcquisition::uuid() const {
    invariant(exists(),
              str::stream() << "Collection " << nss().toStringForErrorMsg()
                            << " doesn't exist, so its UUID cannot be obtained");
    return _acquiredCollection->collectionPtr->uuid();
}

const ScopedCollectionDescription& CollectionAcquisition::getShardingDescription() const {
    // The collectionDescription will only not be set if the caller as acquired the acquisition
    // using the kLocalCatalogOnlyWithPotentialDataLoss placement concern
    invariant(_acquiredCollection->collectionDescription);
    return *_acquiredCollection->collectionDescription;
}

const boost::optional<ScopedCollectionFilter>& CollectionAcquisition::getShardingFilter() const {
    // The collectionDescription will only not be set if the caller has acquired the acquisition
    // using the kLocalCatalogOnlyWithPotentialDataLoss placement concern
    tassert(7740800,
            "Getting shard filter on non-sharded or invalid collection",
            _acquiredCollection->collectionDescription &&
                _acquiredCollection->collectionDescription->isSharded());
    return _acquiredCollection->ownershipFilter;
}

const CollectionPtr& CollectionAcquisition::getCollectionPtr() const {
    tassert(ErrorCodes::InternalError,
            "Collection acquisition has been invalidated",
            !_acquiredCollection->invalidated);
    return _acquiredCollection->collectionPtr;
}

ViewAcquisition::ViewAcquisition(shard_role_details::TransactionResources& txnResources,
                                 const shard_role_details::AcquiredView& acquiredView)
    : _txnResources(&txnResources), _acquiredView(&acquiredView) {
    _txnResources->viewAcquisitionReferences++;
    _acquiredView->refCount++;
}

ViewAcquisition::ViewAcquisition(const ViewAcquisition& other)
    : _txnResources(other._txnResources), _acquiredView(other._acquiredView) {
    _txnResources->viewAcquisitionReferences++;
    _acquiredView->refCount++;
}

ViewAcquisition::ViewAcquisition(ViewAcquisition&& other)
    : _txnResources(other._txnResources), _acquiredView(other._acquiredView) {
    other._txnResources = nullptr;
    other._acquiredView = nullptr;
}

ViewAcquisition& ViewAcquisition::operator=(const ViewAcquisition& other) {
    this->~ViewAcquisition();
    _txnResources = other._txnResources;
    _acquiredView = other._acquiredView;
    _txnResources->viewAcquisitionReferences++;
    _acquiredView->refCount++;
    return *this;
}

ViewAcquisition& ViewAcquisition::operator=(ViewAcquisition&& other) {
    this->~ViewAcquisition();
    _txnResources = other._txnResources;
    _acquiredView = other._acquiredView;
    other._txnResources = nullptr;
    other._acquiredView = nullptr;
    return *this;
}

ViewAcquisition::~ViewAcquisition() {
    if (!_txnResources) {
        return;
    }

    auto& transactionResources = *_txnResources;

    // If the TransactionResources have failed to restore or yield we've released all the resources.
    // Our reference to the acquisition is invalid and we've already removed it from the list of
    // acquisitions.
    if (transactionResources.state == shard_role_details::TransactionResources::State::ACTIVE) {
        auto currentRefCount = --_acquiredView->refCount;
        if (currentRefCount == 0) {
            transactionResources.acquiredViews.remove_if(
                [&](const shard_role_details::AcquiredView& txnResourceAcquiredView) {
                    return &txnResourceAcquiredView == _acquiredView;
                });
        }
    }

    transactionResources.viewAcquisitionReferences--;
    if (transactionResources.acquiredCollections.empty() &&
        transactionResources.acquiredViews.empty()) {
        transactionResources.releaseAllResourcesOnCommitOrAbort();
        transactionResources.state = shard_role_details::TransactionResources::State::EMPTY;
    }
}

const NamespaceString& ViewAcquisition::nss() const {
    return _acquiredView->prerequisites.nss;
}

const ViewDefinition& ViewAcquisition::getViewDefinition() const {
    invariant(_acquiredView->viewDefinition);
    return *_acquiredView->viewDefinition;
}

CollectionAcquisition acquireCollection(OperationContext* opCtx,
                                        CollectionAcquisitionRequest acquisitionRequest,
                                        LockMode mode) {
    return CollectionAcquisition(acquireCollectionOrView(opCtx, acquisitionRequest, mode));
}

CollectionAcquisitions acquireCollections(
    OperationContext* opCtx,
    std::vector<CollectionAcquisitionRequest> acquisitionRequests,
    LockMode mode) {
    // Transform the CollectionAcquisitionRequests to NamespaceOrViewAcquisitionRequests.
    std::vector<CollectionOrViewAcquisitionRequest> namespaceOrViewAcquisitionRequests;
    std::move(acquisitionRequests.begin(),
              acquisitionRequests.end(),
              std::back_inserter(namespaceOrViewAcquisitionRequests));

    // Acquire the collections
    auto acquisitions = acquireCollectionsOrViews(opCtx, namespaceOrViewAcquisitionRequests, mode);

    // Transform the acquisitions to CollectionAcquisitions
    CollectionAcquisitions collectionAcquisitions;
    for (auto& acquisition : acquisitions) {
        // It must be a collection, because that's what the acquisition request stated.
        invariant(acquisition.second.isCollection());
        collectionAcquisitions.emplace(std::move(acquisition));
    }
    return collectionAcquisitions;
}

CollectionOrViewAcquisition acquireCollectionOrView(
    OperationContext* opCtx, CollectionOrViewAcquisitionRequest acquisitionRequest, LockMode mode) {
    auto acquisition = acquireCollectionsOrViews(opCtx, {std::move(acquisitionRequest)}, mode);
    invariant(acquisition.size() == 1);
    return std::move(acquisition.begin()->second);
}

CollectionAcquisition acquireCollectionMaybeLockFree(
    OperationContext* opCtx, CollectionAcquisitionRequest acquisitionRequest) {
    return CollectionAcquisition(acquireCollectionOrViewMaybeLockFree(opCtx, acquisitionRequest));
}

CollectionAcquisitions acquireCollectionsMaybeLockFree(
    OperationContext* opCtx, std::vector<CollectionAcquisitionRequest> acquisitionRequests) {
    // Transform the CollectionAcquisitionRequests to NamespaceOrViewAcquisitionRequests.
    std::vector<CollectionOrViewAcquisitionRequest> namespaceOrViewAcquisitionRequests;
    std::move(acquisitionRequests.begin(),
              acquisitionRequests.end(),
              std::back_inserter(namespaceOrViewAcquisitionRequests));

    // Acquire the collections
    auto acquisitions =
        acquireCollectionsOrViewsMaybeLockFree(opCtx, namespaceOrViewAcquisitionRequests);

    // Transform the acquisitions to CollectionAcquisitions
    CollectionAcquisitions collectionAcquisitions;
    for (auto& acquisition : acquisitions) {
        // It must be a collection, because that's what the acquisition request stated.
        invariant(acquisition.second.isCollection());
        collectionAcquisitions.emplace(std::move(acquisition));
    }
    return collectionAcquisitions;
}

CollectionOrViewAcquisition acquireCollectionOrViewMaybeLockFree(
    OperationContext* opCtx, CollectionOrViewAcquisitionRequest acquisitionRequest) {
    auto acquisition =
        acquireCollectionsOrViewsMaybeLockFree(opCtx, {std::move(acquisitionRequest)});
    invariant(acquisition.size() == 1);
    return std::move(acquisition.begin()->second);
}

namespace shard_role_details {
void SnapshotAttempt::snapshotInitialState() {
    // The read source used can change depending on replication state, so we must fetch the repl
    // state beforehand, to compare with afterwards.
    _replTermBeforeSnapshot = repl::ReplicationCoordinator::get(_opCtx)->getTerm();

    _catalogBeforeSnapshot = CollectionCatalog::get(_opCtx);
}

void SnapshotAttempt::changeReadSourceForSecondaryReads() {
    invariant(_replTermBeforeSnapshot && _catalogBeforeSnapshot);
    auto catalog = *_catalogBeforeSnapshot;

    for (auto& nsOrUUID : _acquisitionRequests) {
        NamespaceString nss;
        try {
            nss = catalog->resolveNamespaceStringOrUUID(_opCtx, nsOrUUID);
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            invariant(nsOrUUID.isUUID());

            const auto readSource =
                shard_role_details::getRecoveryUnit(_opCtx)->getTimestampReadSource();
            if (readSource == RecoveryUnit::ReadSource::kNoTimestamp ||
                readSource == RecoveryUnit::ReadSource::kLastApplied) {
                throw;
            }
        }
        _shouldReadAtLastApplied = SnapshotHelper::changeReadSourceIfNeeded(_opCtx, nss);
        if (*_shouldReadAtLastApplied)
            return;
    }
}

void SnapshotAttempt::openStorageSnapshot() {
    invariant(_shouldReadAtLastApplied);

    // If the collection requires capped snapshots (i.e. it is unreplicated, capped, not the
    // oplog, and not clustered), establish a capped snapshot. This must happen before opening
    // the storage snapshot to ensure a reader using tailable cursors would not miss any writes.
    //
    // It is safe to establish the capped snapshot here, on the Collection object in the latest
    // version of the catalog, even if establishConsistentCollection is eventually called to
    // construct a Collection object from the durable catalog because the only way that can be
    // required for a collection that uses capped snapshots (i.e. a collection that is
    // unreplicated and capped) is:
    //  * The present read operation is reading without a timestamp (since unreplicated
    //  collections don't support timestamped reads), and
    //  * When opening the storage snapshot (and thus when establishing the capped snapshot),
    //  there was a DDL operation pending on the namespace or UUID requested for this read (because
    //  this is the only time we need to construct a Collection object from the durable catalog for
    //  an untimestamped read).
    //
    // Because DDL operations require a collection X lock, there cannot have been any ongoing
    // concurrent writes to the collection while establishing the capped snapshot. This means
    // that if there was a capped snapshot, it should not have contained any uncommitted writes,
    // and so the _lowestUncommittedRecord must be null.
    //
    // The exception to the above is collection creation, which only requires an IX lock. Concurrent
    // readers will have to open a Collection object from the durable catalog, and at that point it
    // is assumed safe to establish an empty CappedSnapshot (even if the storage snapshot is already
    // open) and cause a reader's cursor to return no data.
    for (auto& nssOrUUID : _acquisitionRequests) {
        establishCappedSnapshotIfNeeded(_opCtx, *_catalogBeforeSnapshot, nssOrUUID);
    }

    if (!shard_role_details::getRecoveryUnit(_opCtx)->isActive()) {
        shard_role_details::getRecoveryUnit(_opCtx)->preallocateSnapshot();
        _openedSnapshot = true;
    }
}

std::shared_ptr<const CollectionCatalog> SnapshotAttempt::getConsistentCatalog() {
    auto catalogAfterSnapshot = CollectionCatalog::get(_opCtx);
    const auto replTermAfterSnapshot = repl::ReplicationCoordinator::get(_opCtx)->getTerm();

    if (!haveAcquiredConsistentCatalogAndSnapshot(_catalogBeforeSnapshot->get(),
                                                  catalogAfterSnapshot.get(),
                                                  *_replTermBeforeSnapshot,
                                                  replTermAfterSnapshot)) {
        return nullptr;
    }
    _successful = true;
    return catalogAfterSnapshot;
}

SnapshotAttempt::~SnapshotAttempt() {
    if (_successful) {
        // We were successful, nothing to clean up.
        return;
    }

    if (_openedSnapshot && !shard_role_details::getLocker(_opCtx)->inAWriteUnitOfWork()) {
        shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
    }
    CurOp::get(_opCtx)->yielded();
}

ResolvedNamespaceOrViewAcquisitionRequestsMap generateSortedAcquisitionRequests(
    OperationContext* opCtx,
    const CollectionCatalog& catalog,
    const std::vector<CollectionOrViewAcquisitionRequest>& acquisitionRequests,
    const ResolvedNamespaceOrViewAcquisitionRequest::LockFreeReadsResources&
        lockFreeReadsResources) {
    ResolvedNamespaceOrViewAcquisitionRequestsMap sortedAcquisitionRequests;

    auto readTimestamp =
        shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp(opCtx);

    int counter = 0;
    for (const auto& ar : acquisitionRequests) {
        const auto resolvedBy =
            ar.nssOrUUID.isNamespaceString() ? ResolutionType::kNamespace : ResolutionType::kUUID;
        auto coll = catalog.establishConsistentCollection(opCtx, ar.nssOrUUID, readTimestamp);

        if (ar.nssOrUUID.isUUID()) {
            validateResolvedCollectionByUUID(opCtx, ar, coll);
        }

        const auto& nss = ar.nssOrUUID.isNamespaceString() ? ar.nssOrUUID.nss() : coll->ns();
        const auto& prerequisiteUUID =
            ar.nssOrUUID.isUUID() ? ar.nssOrUUID.uuid() : ar.expectedUUID;
        AcquisitionPrerequisites prerequisites(nss,
                                               prerequisiteUUID,
                                               ar.readConcern,
                                               ar.placementConcern,
                                               ar.operationType,
                                               ar.viewMode);

        ResolvedNamespaceOrViewAcquisitionRequest resolvedAcquisitionRequest{
            prerequisites, resolvedBy, nullptr, boost::none, lockFreeReadsResources};
        // We don't care about ordering in this case, use a mock ResourceId as the key.
        sortedAcquisitionRequests.emplace(ResourceId(RESOURCE_COLLECTION, counter++),
                                          std::move(resolvedAcquisitionRequest));
    }
    return sortedAcquisitionRequests;
}

CollectionOrViewAcquisitions acquireCollectionsOrViewsLockFree(
    OperationContext* opCtx, std::vector<CollectionOrViewAcquisitionRequest> acquisitionRequests) {
    if (acquisitionRequests.size() == 0) {
        return {};
    }

    validateRequests(acquisitionRequests);

    // We shouldn't have an open snapshot unless a previous lock-free acquisition opened and
    // stashed it already.
    invariant(!shard_role_details::getRecoveryUnit(opCtx)->isActive() ||
              opCtx->isLockFreeReadsOp());

    auto lockFreeReadsResources = takeGlobalLock(opCtx, acquisitionRequests);

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    catalog_helper::setAutoGetCollectionWaitFailpointExecute(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); });

    // Make sure the sharding placement is correct before opening the storage snapshot, we will
    // check it again after opening it to make sure it is consistent. This is specially
    // important in secondaries since they can be lagging and might not be aware of the latests
    // routing changes.
    checkShardingPlacement(opCtx, acquisitionRequests);

    // Open a consistent catalog snapshot if needed.
    bool openSnapshot = !shard_role_details::getRecoveryUnit(opCtx)->isActive();
    auto catalog = openSnapshot ? stashConsistentCatalog(opCtx, acquisitionRequests)
                                : CollectionCatalog::get(opCtx);

    try {
        // Second sharding placement check.
        checkShardingPlacement(opCtx, acquisitionRequests);

        auto sortedAcquisitionRequests = shard_role_details::generateSortedAcquisitionRequests(
            opCtx, *catalog, acquisitionRequests, std::move(lockFreeReadsResources));
        return acquireResolvedCollectionsOrViewsWithoutTakingLocks(
            opCtx, *catalog, std::move(sortedAcquisitionRequests));
    } catch (...) {
        if (openSnapshot && !shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork())
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
        throw;
    }
}
}  // namespace shard_role_details

CollectionOrViewAcquisitions acquireCollectionsOrViews(
    OperationContext* opCtx,
    std::vector<CollectionOrViewAcquisitionRequest> acquisitionRequests,
    LockMode mode) {
    if (acquisitionRequests.size() == 0) {
        return {};
    }

    validateRequests(acquisitionRequests);

    while (true) {
        // Optimistically populate the nss and uuid parts of the resolved acquisition requests and
        // sort them
        auto sortedAcquisitionRequests = resolveNamespaceOrViewAcquisitionRequests(
            opCtx, *CollectionCatalog::get(opCtx), acquisitionRequests);

        // At this point, sortedAcquisitionRequests contains fully resolved (both nss and uuid)
        // namespace or view requests in sorted order. However, there is still no guarantee that the
        // nss <-> uuid mapping won't change from underneath.
        //
        // Lock the collection locks in the sorted order and recheck the UUIDS. If it fails, we need
        // to start over.
        const auto& dbName = sortedAcquisitionRequests.begin()->second.prerequisites.nss.dbName();
        Lock::DBLockSkipOptions dbLockOptions = [&]() {
            Lock::DBLockSkipOptions dbLockOptions;
            dbLockOptions.skipRSTLLock =
                std::all_of(sortedAcquisitionRequests.begin(),
                            sortedAcquisitionRequests.end(),
                            [](const auto& ar) {
                                return AutoGetDb::canSkipRSTLLock(ar.second.prerequisites.nss);
                            });
            dbLockOptions.skipFlowControlTicket = std::all_of(
                sortedAcquisitionRequests.begin(),
                sortedAcquisitionRequests.end(),
                [](const auto& ar) {
                    return AutoGetDb::canSkipFlowControlTicket(ar.second.prerequisites.nss);
                });
            return dbLockOptions;
        }();

        const auto lockAcquisitionDeadline =
            sortedAcquisitionRequests.begin()->second.prerequisites.lockAcquisitionDeadline;
        const auto dbLockMode = isSharedLockMode(mode) ? MODE_IS : MODE_IX;
        const auto dbLock = std::make_shared<Lock::DBLock>(
            opCtx, dbName, dbLockMode, lockAcquisitionDeadline, dbLockOptions);

        for (auto& ar : sortedAcquisitionRequests) {
            const auto& nss = ar.second.prerequisites.nss;
            tassert(7300400,
                    str::stream()
                        << "Cannot acquire locks for collections across different databases ('"
                        << dbName.toStringForErrorMsg() << "' vs '"
                        << nss.dbName().toStringForErrorMsg() << "'",
                    dbName == nss.dbName());
            const auto& lockAcquisitionDeadline = ar.second.prerequisites.lockAcquisitionDeadline;

            ar.second.dbLock = dbLock;
            ar.second.acquisitionLocks.dbLock = dbLockMode;
            ar.second.acquisitionLocks.dbLockOptions = dbLockOptions;
            ar.second.collLock.emplace(opCtx, nss, mode, lockAcquisitionDeadline);
            ar.second.acquisitionLocks.collLock = mode;
        }

        // Wait for a configured amount of time after acquiring locks if the failpoint is
        // enabled
        catalog_helper::setAutoGetCollectionWaitFailpointExecute([&](const BSONObj& data) {
            sleepFor(Milliseconds(data["waitForMillis"].numberInt()));
        });

        checkShardingPlacement(opCtx, acquisitionRequests);

        // Recheck UUIDs. We only do this for resolutions performed via UUID exclusively as
        // otherwise we have the correct mapping between nss <-> uuid since the nss is already the
        // user provided one. Note that multi-document transactions will get a WCE thrown later
        // during the checks performed by verifyDbAndCollection if the collection metadata has
        // changed.
        bool hasOptimisticResolutionFailed = false;
        for (auto& ar : sortedAcquisitionRequests) {
            const auto& prerequisites = ar.second.prerequisites;
            if (ar.second.resolvedBy != ResolutionType::kUUID) {
                continue;
            }
            const auto& currentCatalog = CollectionCatalog::get(opCtx);
            const auto coll = currentCatalog->lookupCollectionByNamespace(opCtx, prerequisites.nss);
            if (prerequisites.uuid && (!coll || coll->uuid() != prerequisites.uuid)) {
                hasOptimisticResolutionFailed = true;
                break;
            }
        }

        if (MONGO_unlikely(hasOptimisticResolutionFailed)) {
            // Retry optimistic resolution.
            continue;
        }

        // Open a consistent catalog snapshot if needed.
        bool openSnapshot = !shard_role_details::getRecoveryUnit(opCtx)->isActive();
        auto catalog = openSnapshot ? stashConsistentCatalog(opCtx, acquisitionRequests)
                                    : CollectionCatalog::get(opCtx);

        try {
            return acquireResolvedCollectionsOrViewsWithoutTakingLocks(
                opCtx, *catalog, std::move(sortedAcquisitionRequests));
        } catch (...) {
            if (openSnapshot && !shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork())
                shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
            throw;
        }
    }
}

CollectionOrViewAcquisitions acquireCollectionsOrViewsMaybeLockFree(
    OperationContext* opCtx, std::vector<CollectionOrViewAcquisitionRequest> acquisitionRequests) {
    const bool allAcquisitionsForRead =
        std::all_of(acquisitionRequests.begin(), acquisitionRequests.end(), [](const auto& ar) {
            return ar.operationType == AcquisitionPrerequisites::kRead;
        });
    tassert(7740500, "Cannot acquire for write without locks", allAcquisitionsForRead);

    if (supportsLockFreeRead(opCtx)) {
        return shard_role_details::acquireCollectionsOrViewsLockFree(
            opCtx, std::move(acquisitionRequests));
    } else {
        const auto lockMode = opCtx->inMultiDocumentTransaction() ? MODE_IX : MODE_IS;
        return acquireCollectionsOrViews(opCtx, std::move(acquisitionRequests), lockMode);
    }
}

CollectionAcquisition acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(
    OperationContext* opCtx, const NamespaceString& nss, LockMode mode) {
    invariant(!OperationShardingState::isComingFromRouter(opCtx));

    auto& txnResources = TransactionResources::get(opCtx);

    auto currentAcquireCallNum = txnResources.increaseAcquireCollectionCallCount();

    txnResources.assertNoAcquiredCollections();

    const auto dbLockMode = isSharedLockMode(mode) ? MODE_IS : MODE_IX;
    auto dbLock = std::make_shared<Lock::DBLock>(opCtx, nss.dbName(), dbLockMode);
    Lock::CollectionLock collLock(opCtx, nss, mode);

    const auto catalog = CollectionCatalog::get(opCtx);
    auto prerequisites =
        AcquisitionPrerequisites(nss,
                                 boost::none,
                                 repl::ReadConcernArgs::get(opCtx),
                                 AcquisitionPrerequisites::kLocalCatalogOnlyWithPotentialDataLoss,
                                 AcquisitionPrerequisites::OperationType::kWrite,
                                 AcquisitionPrerequisites::ViewMode::kMustBeCollection);

    auto collOrView = acquireLocalCollectionOrView(opCtx, *catalog, prerequisites);
    invariant(holds_alternative<CollectionPtr>(collOrView));

    auto& coll = get<CollectionPtr>(collOrView);
    if (coll)
        prerequisites.uuid = boost::optional<UUID>(coll->uuid());

    shard_role_details::AcquisitionLocks lockRequirements;
    lockRequirements.dbLock = dbLockMode;
    lockRequirements.collLock = mode;

    shard_role_details::AcquiredCollection& acquiredCollection =
        txnResources.addAcquiredCollection({currentAcquireCallNum,
                                            prerequisites,
                                            std::move(dbLock),
                                            std::move(collLock),
                                            std::move(lockRequirements),
                                            std::move(coll)});

    return CollectionAcquisition(txnResources, acquiredCollection);
}

ScopedLocalCatalogWriteFence::ScopedLocalCatalogWriteFence(OperationContext* opCtx,
                                                           CollectionAcquisition* acquisition)
    : _opCtx(opCtx), _acquiredCollection(acquisition->_acquiredCollection) {
    // Clear the collectionPtr from the acquisition to indicate that it should not be used until
    // the caller is done with the DDL modifications
    _acquiredCollection->collectionPtr = CollectionPtr();
}

ScopedLocalCatalogWriteFence::~ScopedLocalCatalogWriteFence() {
    _updateAcquiredLocalCollection(_opCtx, _acquiredCollection);
}

void ScopedLocalCatalogWriteFence::_updateAcquiredLocalCollection(
    OperationContext* opCtx, shard_role_details::AcquiredCollection* acquiredCollection) {
    try {
        const auto catalog = CollectionCatalog::latest(opCtx);
        const auto& nss = acquiredCollection->prerequisites.nss;
        auto collection =
            catalog->lookupCollectionByNamespace(opCtx, acquiredCollection->prerequisites.nss);
        checkCollectionUUIDMismatch(opCtx, nss, collection, acquiredCollection->prerequisites.uuid);
        if (!acquiredCollection->collectionPtr && collection) {
            // If the uuid wasn't originally set on the prerequisites, because the collection didn't
            // exist, set it now so that on restore from yield we can check we are restoring the
            // same instance of the ns.
            acquiredCollection->prerequisites.uuid = collection->uuid();
        }
        acquiredCollection->collectionPtr = CollectionPtr(collection);
    } catch (const DBException& ex) {
        LOGV2_DEBUG(7653800,
                    1,
                    "Failed to update ScopedLocalCatalogWriteFence",
                    "ex"_attr = redact(ex.toString()));
        acquiredCollection->invalidated = true;
    }
}

YieldedTransactionResources::~YieldedTransactionResources() {
    invariant(!_yieldedResources);
}

YieldedTransactionResources::YieldedTransactionResources(
    std::unique_ptr<shard_role_details::TransactionResources> yieldedResources,
    shard_role_details::TransactionResources::State originalState)
    : _yieldedResources(std::move(yieldedResources)), _originalState(originalState) {}

void YieldedTransactionResources::transitionTransactionResourcesToFailedState(
    OperationContext* opCtx) {
    if (_yieldedResources) {
        _yieldedResources->releaseAllResourcesOnCommitOrAbort();
        _yieldedResources->state = shard_role_details::TransactionResources::State::FAILED;
        TransactionResources::attachToOpCtx(opCtx, std::move(_yieldedResources));
    }
}

void StashedTransactionResources::dispose() {
    if (_yieldedResources) {
        _yieldedResources->releaseAllResourcesOnCommitOrAbort();
        _yieldedResources.reset();
    }
}

YieldedTransactionResources yieldTransactionResourcesFromOperationContext(OperationContext* opCtx) {
    auto& transactionResources = TransactionResources::get(opCtx);
    invariant(
        !(transactionResources.yielded ||
          transactionResources.state == shard_role_details::TransactionResources::State::YIELDED));

    invariant(transactionResources.state ==
                  shard_role_details::TransactionResources::State::ACTIVE ||
              transactionResources.state == shard_role_details::TransactionResources::State::EMPTY);

    for (auto& acquisition : transactionResources.acquiredCollections) {
        // Yielding kLocalCatalogOnlyWithPotentialDataLoss acquisitions is not allowed.
        invariant(
            !holds_alternative<AcquisitionPrerequisites::PlacementConcernPlaceholder>(
                acquisition.prerequisites.placementConcern),
            str::stream() << "Collection " << acquisition.prerequisites.nss.toStringForErrorMsg()
                          << " acquired with special placement concern and cannot be yielded");
    }

    // Yielding view acquisitions is not supported.
    tassert(7300502,
            "Yielding view acquisitions is forbidden",
            transactionResources.acquiredViews.empty());

    Locker::LockSnapshot lockSnapshot;
    shard_role_details::getLocker(opCtx)->saveLockStateAndUnlock(&lockSnapshot);
    transactionResources.yielded.emplace(
        TransactionResources::YieldedStateHolder{std::move(lockSnapshot)});

    auto originalState = std::exchange(transactionResources.state,
                                       shard_role_details::TransactionResources::State::YIELDED);

    return YieldedTransactionResources(TransactionResources::detachFromOpCtx(opCtx), originalState);
}

void stashTransactionResourcesFromOperationContext(OperationContext* opCtx,
                                                   TransactionResourcesStasher* stasher) {
    auto& transactionResources = TransactionResources::get(opCtx);
    invariant(
        !(transactionResources.yielded ||
          transactionResources.state == shard_role_details::TransactionResources::State::YIELDED));

    invariant(transactionResources.state ==
                  shard_role_details::TransactionResources::State::ACTIVE ||
              transactionResources.state == shard_role_details::TransactionResources::State::EMPTY);

    for (auto& acquisition : transactionResources.acquiredCollections) {
        // Yielding kLocalCatalogOnlyWithPotentialDataLoss acquisitions is not allowed.
        invariant(
            !holds_alternative<AcquisitionPrerequisites::PlacementConcernPlaceholder>(
                acquisition.prerequisites.placementConcern),
            str::stream() << "Collection " << acquisition.prerequisites.nss.toStringForErrorMsg()
                          << " acquired with special placement concern and cannot be yielded");
    }

    // Yielding view acquisitions is not supported.
    tassert(7750701,
            "Yielding view acquisitions is forbidden",
            transactionResources.acquiredViews.empty());

    // TODO SERVER-77213: This should mostly go away once the Locker resides inside
    // TransactionResources and the underlying locks point to it instead of the opCtx.
    //
    // Release all locks acquired since we are going to yield externally and our opCtx is going to
    // be destroyed.
    for (auto& acquisition : transactionResources.acquiredCollections) {
        acquisition.collectionLock.reset();
        acquisition.dbLock.reset();
        acquisition.globalLock.reset();
        acquisition.lockFreeReadsBlock.reset();
    }

    auto originalState =
        std::exchange(transactionResources.state, TransactionResources::State::STASHED);

    auto stashedResources =
        StashedTransactionResources{TransactionResources::detachFromOpCtx(opCtx), originalState};

    stasher->stashTransactionResources(std::move(stashedResources));
}

void restoreTransactionResourcesToOperationContext(
    OperationContext* opCtx, YieldedTransactionResources yieldedResourcesHolder) {
    if (!yieldedResourcesHolder._yieldedResources) {
        // Nothing to restore.
        return;
    }

    TransactionResources::attachToOpCtx(opCtx, std::move(yieldedResourcesHolder._yieldedResources));
    auto& transactionResources = TransactionResources::get(opCtx);

    // On failure to restore, release the yielded resources.
    ScopeGuard scopeGuard([&] {
        transactionResources.releaseAllResourcesOnCommitOrAbort();
        transactionResources.state = shard_role_details::TransactionResources::State::FAILED;
    });

    auto restoreFn = [&] {
        // Reacquire locks. External yields do not have a lock snapshot so we only restore for
        // internal yields.
        if (auto ptr = transactionResources.yielded.get_ptr()) {
            shard_role_details::getLocker(opCtx)->restoreLockState(opCtx, ptr->yieldedLocker);
            transactionResources.yielded.reset();
        }

        // Reestablish a consistent catalog snapshot (multi document transactions don't yield).
        auto requests = toNamespaceStringOrUUIDs(transactionResources.acquiredCollections);
        auto catalog = getConsistentCatalogAndSnapshot(opCtx, requests);

        // Reacquire service snapshots. Will throw if placement concern can no longer be met.
        for (auto& acquiredCollection : transactionResources.acquiredCollections) {
            const auto& prerequisites = acquiredCollection.prerequisites;

            auto uassertCollectionAppearedAfterRestore = [&] {
                uasserted(743870,
                          str::stream()
                              << "Collection " << prerequisites.nss.toStringForErrorMsg()
                              << " appeared after a restore, which violates the semantics of "
                                 "restore");
            };

            if (prerequisites.operationType == AcquisitionPrerequisites::OperationType::kRead) {
                // Just reacquire the CollectionPtr. Reads don't care about placement changes
                // because they have already established a ScopedCollectionFilter that acts as
                // RangePreserver.
                auto collOrView = acquireLocalCollectionOrView(opCtx, *catalog, prerequisites);

                // We do not support yielding view acquisitions. Therefore it is not possible
                // that upon restore 'acquireLocalCollectionOrView' snapshoted a view -- it
                // would not have met the prerequisite that the collection instance is still the
                // same as the one before yielding.
                invariant(holds_alternative<CollectionPtr>(collOrView));
                if (!acquiredCollection.collectionPtr != !get<CollectionPtr>(collOrView)) {
                    uassertCollectionAppearedAfterRestore();
                }

                // Update the services snapshot on TransactionResources
                acquiredCollection.collectionPtr = std::move(get<CollectionPtr>(collOrView));
            } else {
                // Make sure that the placement is still correct.
                if (holds_alternative<PlacementConcern>(prerequisites.placementConcern)) {
                    checkPlacementVersion(opCtx,
                                          prerequisites.nss,
                                          get<PlacementConcern>(prerequisites.placementConcern));
                }

                auto reacquiredServicesSnapshot =
                    acquireServicesSnapshot(opCtx, *catalog, prerequisites);

                // We do not support yielding view acquisitions. Therefore it is not possible
                // that upon restore 'acquireLocalCollectionOrView' snapshoted a view -- it
                // would not have met the prerequisite that the collection instance is still the
                // same as the one before yielding.
                invariant(holds_alternative<CollectionPtr>(
                    reacquiredServicesSnapshot.collectionPtrOrView));
                if (!acquiredCollection.collectionPtr !=
                    !get<CollectionPtr>(reacquiredServicesSnapshot.collectionPtrOrView)) {
                    uassertCollectionAppearedAfterRestore();
                }

                // Update the services snapshot on TransactionResources
                acquiredCollection.collectionPtr =
                    std::move(get<CollectionPtr>(reacquiredServicesSnapshot.collectionPtrOrView));
                acquiredCollection.collectionDescription =
                    std::move(reacquiredServicesSnapshot.collectionDescription);
                acquiredCollection.ownershipFilter =
                    std::move(reacquiredServicesSnapshot.ownershipFilter);
            }

            // Check if this operation is a direct connection and if it is authorized to be one
            // after reacquiring locks or snapshots.
            direct_connection_util::checkDirectShardOperationAllowed(
                opCtx, transactionResources.acquiredCollections.front().prerequisites.nss.dbName());

            // TODO: This will be removed when we no longer snapshot sharding state on CollectionPtr
            invariant(acquiredCollection.collectionDescription);
            if (acquiredCollection.collectionDescription->isSharded()) {
                acquiredCollection.collectionPtr.setShardKeyPattern(
                    acquiredCollection.collectionDescription->getKeyPattern());
            }
        }
        return catalog;
    };

    auto catalog = [&]() {
        while (true) {
            try {
                return restoreFn();
            } catch (const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
                if (ShardVersion::isPlacementVersionIgnored(ex->getVersionReceived()) &&
                    ex->getCriticalSectionSignal()) {
                    // If ShardVersion is IGNORED and we encountered a critical section, then yield,
                    // wait for the critical section to finish and then we'll resume the write from
                    // the point we had left. We do this to prevent large multi-writes from
                    // repeatedly failing due to StaleConfig and exhausting the mongos retry
                    // attempts. Yield the locks.
                    Locker::LockSnapshot lockSnapshot;
                    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
                    shard_role_details::getLocker(opCtx)->saveLockStateAndUnlock(&lockSnapshot);
                    transactionResources.yielded.emplace(
                        TransactionResources::YieldedStateHolder{std::move(lockSnapshot)});
                    // Wait for the critical section to finish.
                    OperationShardingState::waitForCriticalSectionToComplete(
                        opCtx, *ex->getCriticalSectionSignal())
                        .ignore();
                    // Try again to restore.
                    continue;
                }
                throw;
            } catch (const ExceptionFor<ErrorCodes::CollectionUUIDMismatch>& ex) {
                const auto extraInfo = ex.extraInfo<CollectionUUIDMismatchInfo>();
                const auto dbName = extraInfo->dbName();
                if (extraInfo->actualCollection()) {
                    NamespaceString oldNss =
                        NamespaceStringUtil::deserialize(dbName, extraInfo->expectedCollection());
                    NamespaceString newNss =
                        NamespaceStringUtil::deserialize(dbName, *extraInfo->actualCollection());
                    PlanYieldPolicy::throwCollectionRenamedError(
                        oldNss, newNss, extraInfo->collectionUUID());
                } else {
                    PlanYieldPolicy::throwCollectionDroppedError(extraInfo->collectionUUID());
                }
            }
        }
    }();

    transactionResources.state = yieldedResourcesHolder._originalState;

    if (!opCtx->inMultiDocumentTransaction()) {
        CollectionCatalog::stash(opCtx, catalog);
    }

    scopeGuard.dismiss();
}

StashTransactionResourcesForDBDirect::StashTransactionResourcesForDBDirect(OperationContext* opCtx)
    : _opCtx(opCtx) {
    if (TransactionResources::isPresent(opCtx)) {
        _originalTransactionResources = TransactionResources::detachFromOpCtx(opCtx);
        TransactionResources::attachToOpCtx(opCtx, std::make_unique<TransactionResources>());
    }
}

StashTransactionResourcesForDBDirect::~StashTransactionResourcesForDBDirect() {
    if (TransactionResources::isPresent(_opCtx)) {
        TransactionResources::detachFromOpCtx(_opCtx);
    }
    TransactionResources::attachToOpCtx(_opCtx, std::move(_originalTransactionResources));
}

HandleTransactionResourcesFromStasher::HandleTransactionResourcesFromStasher(
    OperationContext* opCtx, TransactionResourcesStasher* stasher)
    : _opCtx(opCtx), _stasher(stasher) {
    auto stashedResources = stasher->releaseStashedTransactionResources();

    if (TransactionResources::isPresent(opCtx)) {
        _originalTransactionResources = TransactionResources::detachFromOpCtx(opCtx);
    }

    ScopeGuard restoreFailedGuard([&] {
        if (TransactionResources::isPresent(opCtx)) {
            // We have attempted to restore the resources but failed, the transaction resources have
            // been moved to the opCtx, so we must move them back to the stashed object.
            invariant(!stashedResources._yieldedResources);
            stashedResources._yieldedResources = TransactionResources::detachFromOpCtx(opCtx);
        }
        stashedResources._yieldedResources->releaseAllResourcesOnCommitOrAbort();
        stashedResources._yieldedResources->state = TransactionResources::State::FAILED;
        stasher->stashTransactionResources(std::move(stashedResources));
        TransactionResources::attachToOpCtx(opCtx, std::move(_originalTransactionResources));
    });

    // Reacquire the locks requested by the acquisitions. All acquisitions with the same
    // acquireCallCount share the same Global/DB/Lock-free locks. Acquisitions are inserted
    // in order, so we can perform a single pass over the list to rebuild the locks.
    //
    // The following shared_ptrs are set by the first acquisition with a different
    // acquireCollectionCallNum different from the previous one.
    std::shared_ptr<Lock::GlobalLock> commonGlobalLock;
    std::shared_ptr<Lock::DBLock> commonDbLock;
    std::shared_ptr<LockFreeReadsBlock> lockFreeReadsBlock;
    int previousAcquireCollectionCallNum = -1;

    // TODO SERVER-77213: This should mostly go away once the Locker resides inside
    // TransactionResources and the underlying locks point to it instead of the opCtx.
    for (auto& acquisition : stashedResources._yieldedResources->acquiredCollections) {
        const auto& locks = acquisition.locks;
        bool isFromDifferentAcquireCall =
            previousAcquireCollectionCallNum != acquisition.acquireCollectionCallNum;
        if (locks.hasLockFreeReadsBlock) {
            if (isFromDifferentAcquireCall) {
                lockFreeReadsBlock = std::make_shared<LockFreeReadsBlock>(opCtx);
            }
            acquisition.lockFreeReadsBlock = lockFreeReadsBlock;
        }
        if (locks.globalLock != MODE_NONE) {
            if (isFromDifferentAcquireCall) {
                commonGlobalLock =
                    std::make_shared<Lock::GlobalLock>(opCtx,
                                                       locks.globalLock,
                                                       Date_t::max(),
                                                       Lock::InterruptBehavior::kThrow,
                                                       locks.globalLockOptions);
            }
            acquisition.globalLock = commonGlobalLock;
        }
        if (locks.dbLock != MODE_NONE) {
            if (isFromDifferentAcquireCall) {
                commonDbLock =
                    std::make_shared<Lock::DBLock>(opCtx,
                                                   acquisition.prerequisites.nss.dbName(),
                                                   locks.dbLock,
                                                   Date_t::max(),
                                                   locks.dbLockOptions);
            }
            acquisition.dbLock = commonDbLock;
        }
        // Unlike the other locks, this one is always acquired per acquisition if it is not a
        // lock-free acquisition.
        if (locks.collLock != MODE_NONE) {
            acquisition.collectionLock.emplace(
                opCtx, acquisition.prerequisites.nss, locks.collLock);
        }

        previousAcquireCollectionCallNum = acquisition.acquireCollectionCallNum;
    }

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    catalog_helper::setAutoGetCollectionWaitFailpointExecute(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); });

    restoreTransactionResourcesToOperationContext(
        opCtx,
        YieldedTransactionResources{std::move(stashedResources._yieldedResources),
                                    stashedResources._originalState});

    restoreFailedGuard.dismiss();
}

HandleTransactionResourcesFromStasher::~HandleTransactionResourcesFromStasher() {
    if (TransactionResources::isPresent(_opCtx)) {
        auto& txnResources = TransactionResources::get(_opCtx);
        if (_stasher && txnResources.state != TransactionResources::State::FAILED) {
            // If the resources for the entire transaction are still valid and we haven't dismissed
            // the resources due to a failure, we yield and stash them.
            stashTransactionResourcesFromOperationContext(_opCtx, _stasher);
        } else {
            // Otherwise, the transaction resources for this operation have to be destroyed since
            // the operation has failed.
            TransactionResources::detachFromOpCtx(_opCtx);
        }
    }
    // Since the opCtx must always have valid TransactionResources we reattach the original
    // resources.
    TransactionResources::attachToOpCtx(_opCtx, std::move(_originalTransactionResources));
}

void HandleTransactionResourcesFromStasher::dismissRestoredResources() {
    auto& txnResources = TransactionResources::get(_opCtx);
    txnResources.releaseAllResourcesOnCommitOrAbort();
    txnResources.state = shard_role_details::TransactionResources::State::FAILED;
    _stasher = nullptr;
}

void shard_role_details::checkLocalCatalogIsValidForUnshardedShardVersion(
    OperationContext* opCtx,
    const CollectionCatalog& stashedCatalog,
    const CollectionPtr& collectionPtr,
    const NamespaceString& nss) {
    if (opCtx->inMultiDocumentTransaction() ||
        repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()) {
        // The latest catalog.
        const auto latestCatalog = CollectionCatalog::latest(opCtx);

        const auto makeErrorMessage = [&nss]() {
            std::string errmsg = str::stream()
                << "Collection " << nss.toStringForErrorMsg()
                << " has undergone a catalog change and no longer satisfies the "
                   "requirements for the current transaction.";
            return errmsg;
        };

        if (collectionPtr) {
            // The transaction sees a collection exists.
            uassert(ErrorCodes::SnapshotUnavailable,
                    makeErrorMessage(),
                    latestCatalog->isLatestCollection(opCtx, collectionPtr.get()));
        } else if (const auto currentView = stashedCatalog.lookupView(opCtx, nss)) {
            // The transaction sees a view exists.
            uassert(ErrorCodes::SnapshotUnavailable,
                    makeErrorMessage(),
                    currentView == latestCatalog->lookupView(opCtx, nss));
        } else {
            // The transaction sees neither a collection nor a view exist. Make sure that the latest
            // catalog looks the same.
            uassert(ErrorCodes::SnapshotUnavailable,
                    makeErrorMessage(),
                    !latestCatalog->lookupCollectionByNamespace(opCtx, nss) &&
                        !latestCatalog->lookupView(opCtx, nss));
        }
    }
}

void shard_role_details::checkShardingAndLocalCatalogCollectionUUIDMatch(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardVersion& requestedShardVersion,
    const ScopedCollectionDescription& shardingCollectionDescription,
    const CollectionPtr& collectionPtr) {
    // Skip the check if the requested shard version corresponds to an untracked collection or
    // corresponds to this shard not own any chunk. Also skip the check if the router attached
    // ShardVersion::IGNORED, since in this case the router broadcasts request to shards that may
    // not even own the collection at all (so they won't have any uuid on their local catalog).
    if (requestedShardVersion == ShardVersion::UNSHARDED() ||
        !requestedShardVersion.placementVersion().isSet() ||
        ShardVersion::isPlacementVersionIgnored(requestedShardVersion)) {
        return;
    }

    // Skip checking resharding temporary collections. The reason is that resharding registers the
    // temporary collections on the sharding catalog before creating them on the shards, without
    // holding any critical section.
    // TODO: SERVER-87235 Remove this when resharding creates the temporary collections under a
    // critical section.
    if (nss.isTemporaryReshardingCollection()) {
        return;
    }

    // Check that the collection uuid in the sharding catalog and the one on the local catalog
    // match.
    if (shardingCollectionDescription.hasRoutingTable() &&
        (!collectionPtr || !shardingCollectionDescription.uuidMatches(collectionPtr->uuid()))) {
        if ((opCtx->inMultiDocumentTransaction() ||
             repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime())) {
            // If in multi-document transaction or snapshot read, throw SnapshotUnavailable so that
            // the transaction can be retried. This situation is known to be possible when a
            // collection undergoes resharding, due to the resharding commit protocol. See
            // SERVER-87061.
            // TODO: SERVER-87235: Remove this condition and leave only the tassert below also for
            // transaction and snapshot reads.
            uasserted(
                ErrorCodes::SnapshotUnavailable,
                "Sharding catalog and local catalog collection uuid do not match. Nss: '{}', sharding uuid: '{}', local uuid: '{}'"_format(
                    nss.toStringForErrorMsg(),
                    shardingCollectionDescription.getUUID().toString(),
                    collectionPtr ? collectionPtr->uuid().toString() : ""));

        } else {
            tasserted(
                8706100,
                "Sharding catalog and local catalog collection uuid do not match. Nss: '{}', sharding uuid: '{}', local uuid: '{}'"_format(
                    nss.toStringForErrorMsg(),
                    shardingCollectionDescription.getUUID().toString(),
                    collectionPtr ? collectionPtr->uuid().toString() : ""));
        }
    }
}

}  // namespace mongo
