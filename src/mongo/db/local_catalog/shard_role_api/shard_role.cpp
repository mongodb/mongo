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

#include "mongo/db/local_catalog/shard_role_api/shard_role.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/curop.h"
#include "mongo/db/local_catalog/catalog_helper.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/shard_role_api/direct_connection_util.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_util.h"
#include "mongo/db/local_catalog/snapshot_helper.h"
#include "mongo/db/repl/intent_registry.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <iterator>

#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/utility/in_place_factory.hpp>  // IWYU pragma: keep
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

using TransactionResources = shard_role_details::TransactionResources;

namespace {
auto& killedDueToRangeDeletionCounter =
    *MetricBuilder<Counter64>("operation.killedDueToRangeDeletion");

enum class ResolutionType { kUUID, kNamespace };

struct ResolvedNamespaceOrViewAcquisitionRequest {
    ResourceId resourceId;

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

using ResolvedNamespaceOrViewAcquisitionRequests =
    absl::InlinedVector<ResolvedNamespaceOrViewAcquisitionRequest,
                        kDefaultAcquisitionContainerSize>;

template <typename List, typename T>
void removeByPtr(List& l, T* p) {
    auto it = std::find_if(l.begin(), l.end(), [&](auto& e) { return &e == p; });
    if (it != l.end())
        l.erase(it);
}

void validateResolvedCollectionByUUID(OperationContext* opCtx,
                                      const CollectionOrViewAcquisitionRequest& ar,
                                      const NamespaceString& nss) {
    invariant(ar.nssOrUUID.isUUID());
    auto shardVersion = OperationShardingState::get(opCtx).getShardVersion(nss);
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
                          << " Actual: " << nss.dbName().toStringForErrorMsg(),
            nss.dbName() == ar.nssOrUUID.dbName());
}

void validateResolvedCollectionByUUID(OperationContext* opCtx,
                                      const CollectionOrViewAcquisitionRequest& ar,
                                      const Collection* coll) {
    invariant(ar.nssOrUUID.isUUID());
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Namespace " << ar.nssOrUUID.dbName().toStringForErrorMsg() << ":"
                          << ar.nssOrUUID.uuid() << " not found",
            coll);
    validateResolvedCollectionByUUID(opCtx, ar, coll->ns());
}

/**
 * Takes the input acquisitions, populates the NSS and returns a vector sorted by the ResourceId of
 * the target collection, suitable for locking them in order. This is necessary to prevent deadlocks
 * due to ordering with strong locks. We do not care for the ordering of the databases as the
 * canonical ordering is for target collections only.
 */
ResolvedNamespaceOrViewAcquisitionRequests resolveNamespaceOrViewAcquisitionRequests(
    OperationContext* opCtx,
    const CollectionCatalog& catalog,
    const CollectionOrViewAcquisitionRequests& acquisitionRequests) {

    ResolvedNamespaceOrViewAcquisitionRequests resolvedAcquisitionRequests;

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
                ResourceId(RESOURCE_COLLECTION, ar.nssOrUUID.nss()),
                prerequisites,
                ResolutionType::kNamespace,
                nullptr,
                boost::none};

            resolvedAcquisitionRequests.emplace_back(std::move(resolvedAcquisitionRequest));
        } else if (ar.nssOrUUID.isUUID()) {
            // We lookup in pending commit entries as well here since this function is used for
            // UUID->NSS resolution. The usual flow for doing this in locked cases is:
            // * Getting the NSS from the UUID considering pending entries
            // * Acquiring a lock on the collection to prevent renames/drops
            // * Getting the NSS again from the UUID considering pending entries
            // In this case this method is safe to call because we will retry again if any DDL
            // operation occurred between our lock acquisition and the next mapping attempt
            // finished. Once the lock is acquired any DDL operation will have finished and
            // committed and will either NOT have changed the UUID or changed it, which forces an
            // entire retry of the operation we just described.
            auto nss = catalog.resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(
                opCtx, ar.nssOrUUID);

            validateResolvedCollectionByUUID(opCtx, ar, nss);

            AcquisitionPrerequisites prerequisites(nss,
                                                   ar.nssOrUUID.uuid(),
                                                   ar.readConcern,
                                                   ar.placementConcern,
                                                   ar.operationType,
                                                   ar.viewMode);

            ResolvedNamespaceOrViewAcquisitionRequest resolvedAcquisitionRequest{
                ResourceId(RESOURCE_COLLECTION, nss),
                prerequisites,
                ResolutionType::kUUID,
                nullptr,
                boost::none};

            resolvedAcquisitionRequests.emplace_back(std::move(resolvedAcquisitionRequest));
        } else {
            MONGO_UNREACHABLE_TASSERT(10083530);
        }
    }

    // Empirically, this early-return results in better performance than trying to sort a vector of
    // size 1.
    if (resolvedAcquisitionRequests.size() == 1) {
        return resolvedAcquisitionRequests;
    }

    // Sort them in ascending ResourceId order since that is the canonical lock ordering used across
    // the server. However always lock system.views collection in the end because concurrent
    // view-related operations always lock system.views in the end.
    std::sort(resolvedAcquisitionRequests.begin(),
              resolvedAcquisitionRequests.end(),
              [](auto& lhs, auto& rhs) {
                  return lhs.prerequisites.nss.isSystemDotViews() ==
                          rhs.prerequisites.nss.isSystemDotViews()
                      ? lhs.resourceId < rhs.resourceId
                      : rhs.prerequisites.nss.isSystemDotViews();
              });
    return resolvedAcquisitionRequests;
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
    if (operationType == AcquisitionPrerequisites::OperationType::kWrite ||
        operationType == AcquisitionPrerequisites::OperationType::kUnreplicatedWrite) {
        auto latest = CollectionCatalog::latest(opCtx);
        if (!latest->isLatestCollection(opCtx, coll.get())) {
            throwWriteConflictException(str::stream() << "Unable to write to collection '"
                                                      << coll->ns().toStringForErrorMsg()
                                                      << "' due to catalog changes; please "
                                                         "retry the operation");
        }
        if (shard_role_details::getRecoveryUnit(opCtx)->isActive()) {
            const auto mySnapshot =
                shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();
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

void logMissingPlacementConflictTime(const NamespaceString& nss) {
    if (TestingProctor::instance().isEnabled()) {
        tasserted(
            10206300,
            str::stream() << "Routed operations in multi-document transactions with readConcern != "
                             "snapshot must carry a PlacementConflictTime for nss "
                          << nss.toStringForErrorMsg());
    } else {
        static logv2::SeveritySuppressor logSeverity{
            Minutes{1}, logv2::LogSeverity::Warning(), logv2::LogSeverity::Debug(5)};
        LOGV2_DEBUG(10206301,
                    logSeverity().toInt(),
                    "Detected a missing PlacementConflictTime for an operation in a multi-document "
                    "transaction with readConcern != snapshot originating from a router.",
                    "nss"_attr = redact(nss.toStringForErrorMsg()));
    }
}

// Check the operation correctly carries the PlacementConflictTime in case of a multi-document
// transaction. The version is mandatory when all the following are true:
// 1. The operation is coming from a router
// 2. The operation carry a valid placement version that cannot be ignored.
// 3. The operation requests a read concern != snapshot
// 4. The operation runs as a part of a multi-document transaction
// 5. The operation runs on a trackable namespace
// The PlacementConflictTime is either present in the dbVersion or in the shardVersion otherwise.
void assertPlacementConflictTimePresentWhenRequired(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<DatabaseVersion>& receivedDbVersion,
    const boost::optional<ShardVersion>& receivedShardVersion) {
    bool isShardVersionIgnored =
        receivedShardVersion && ShardVersion::isPlacementVersionIgnored(*receivedShardVersion);
    bool isShardVersionUnsharded =
        receivedShardVersion && *receivedShardVersion == ShardVersion::UNSHARDED();
    bool isRoutedVersion = (receivedDbVersion && isShardVersionUnsharded) ||
        (receivedShardVersion && !isShardVersionIgnored && !isShardVersionUnsharded);

    if (isRoutedVersion && opCtx->inMultiDocumentTransaction() &&
        OperationShardingState::isComingFromRouter(opCtx) &&
        repl::ReadConcernArgs::get(opCtx).getLevel() !=
            repl::ReadConcernLevel::kSnapshotReadConcern &&
        !nss.isNamespaceAlwaysUntracked()) {
        // Get the PlacementConflictTime from the either the dbVersion or shardVersion: This is
        // inline with the protocol which will use the first available value in either of the two
        // versions.
        auto placementConflictTime = [&]() -> boost::optional<LogicalTime> {
            if (receivedDbVersion && receivedDbVersion->getPlacementConflictTime()) {
                return receivedDbVersion->getPlacementConflictTime();
            } else if (receivedShardVersion) {
                return receivedShardVersion->placementConflictTime();
            }
            return boost::none;
        }();
        if (!placementConflictTime) {
            logMissingPlacementConflictTime(nss);
        }
    }
}


void checkPlacementVersion(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const PlacementConcern& placementConcern) {
    const auto& receivedDbVersion = placementConcern.getDbVersion();
    if (receivedDbVersion) {
        const auto scopedDss = DatabaseShardingState::acquire(opCtx, nss.dbName());
        scopedDss->checkDbVersionOrThrow(opCtx, *receivedDbVersion);
    }

    const auto& receivedShardVersion = placementConcern.getShardVersion();
    if (receivedShardVersion) {
        const auto scopedCSS = CollectionShardingState::acquire(opCtx, nss);
        scopedCSS->checkShardVersionOrThrow(opCtx, *receivedShardVersion);
    }

    assertPlacementConflictTimePresentWhenRequired(
        opCtx, nss, receivedDbVersion, receivedShardVersion);
}

std::variant<CollectionPtr, std::shared_ptr<const ViewDefinition>> acquireLocalCollectionOrView(
    OperationContext* opCtx,
    const CollectionCatalog& catalog,
    const AcquisitionPrerequisites& prerequisites) {
    const auto& nss = prerequisites.nss;

    auto coll = [&]() {
        if (prerequisites.useConsistentCatalog) {
            auto readTimestamp =
                shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();
            return CollectionPtr(catalog.establishConsistentCollection(
                opCtx, NamespaceStringOrUUID(nss), readTimestamp));
        } else {
            return CollectionPtr::CollectionPtr_UNSAFE(
                catalog.lookupCollectionByNamespace(opCtx, prerequisites.nss));
        }
    }();

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
        scopedCSS->getCollectionDescription(opCtx, placementConcern.getShardVersion().has_value());

    invariant(!collectionDescription.isSharded() || placementConcern.getShardVersion());
    auto optOwnershipFilter = placementConcern.getShardVersion().has_value()
        ? boost::optional<ScopedCollectionFilter>(scopedCSS->getOwnershipFilter(
              opCtx,
              prerequisites.operationType == AcquisitionPrerequisites::OperationType::kRead
                  ? CollectionShardingState::OrphanCleanupPolicy::kDisallowOrphanCleanup
                  : CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup,
              *placementConcern.getShardVersion()))
        : boost::none;

    // TODO: This will be removed when we no longer snapshot sharding state on CollectionPtr.
    if (holds_alternative<CollectionPtr>(collOrView) && collectionDescription.isSharded()) {
        get<CollectionPtr>(collOrView).setShardKeyPattern(collectionDescription.getKeyPattern());
    }

    return SnapshotedServices{
        std::move(collOrView), std::move(collectionDescription), std::move(optOwnershipFilter)};
}

const Lock::GlobalLockOptions kLockFreeReadsGlobalLockOptions{[] {
    Lock::GlobalLockOptions options;
    options.skipRSTLLock = true;
    return options;
}()};

CollectionOrViewAcquisitions acquireResolvedCollectionsOrViewsWithoutTakingLocks(
    OperationContext* opCtx,
    const CollectionCatalog& catalog,
    ResolvedNamespaceOrViewAcquisitionRequests& sortedAcquisitionRequests) {
    CollectionOrViewAcquisitions acquisitions;

    auto& txnResources = TransactionResources::get(opCtx);
    invariant(txnResources.state != shard_role_details::TransactionResources::State::YIELDED,
              "Cannot make a new acquisition in the YIELDED state");
    invariant(txnResources.state != shard_role_details::TransactionResources::State::FAILED,
              "Cannot make a new acquisition in the FAILED state");

    // Record the catalog epoch at the first acquisition. This is necessary to detect epoch changes
    // among different catalog snapshots at every restore.
    if (!txnResources.catalogEpoch) {
        txnResources.catalogEpoch = catalog.getEpoch();
    }

    auto currentAcquireCallNum = txnResources.increaseAcquireCollectionCallCount();

    for (auto& acquisitionRequest : sortedAcquisitionRequests) {
        auto& prerequisites = acquisitionRequest.prerequisites;

        auto snapshotedServices = acquireServicesSnapshot(opCtx, catalog, prerequisites);
        const bool isCollection =
            holds_alternative<CollectionPtr>(snapshotedServices.collectionPtrOrView);

        const boost::optional<ShardVersion> placementConcernShardVersion =
            holds_alternative<PlacementConcern>(prerequisites.placementConcern)
            ? get<PlacementConcern>(prerequisites.placementConcern).getShardVersion()
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

            dassert(!prerequisites.uuid || prerequisites.uuid == collectionPtr->uuid());
            if (!prerequisites.uuid && collectionPtr) {
                // If the uuid wasn't originally set on the AcquisitionRequest, set it now on the
                // prerequisites so that on restore from yield we can check we are restoring the
                // same instance of the ns.
                prerequisites.uuid = collectionPtr->uuid();
            }

            acquisitionRequest.acquisitionLocks.hasLockFreeReadsBlock =
                bool(acquisitionRequest.lockFreeReadsResources.lockFreeReadsBlock);

            if (const auto& ptr = acquisitionRequest.lockFreeReadsResources.globalLock) {
                acquisitionRequest.acquisitionLocks.globalLock =
                    ptr->isLocked() ? MODE_IS : MODE_NONE;
                acquisitionRequest.acquisitionLocks.globalLockOptions =
                    kLockFreeReadsGlobalLockOptions;
            }

            shard_role_details::AcquiredCollection& acquiredCollection =
                txnResources.addAcquiredCollection(
                    {currentAcquireCallNum,
                     prerequisites,
                     std::move(acquisitionRequest.dbLock),
                     std::move(acquisitionRequest.collLock),
                     std::move(acquisitionRequest.lockFreeReadsResources.lockFreeReadsBlock),
                     std::move(acquisitionRequest.lockFreeReadsResources.globalLock),
                     std::move(acquisitionRequest.acquisitionLocks),
                     std::move(snapshotedServices.collectionDescription),
                     std::move(snapshotedServices.ownershipFilter),
                     std::move(get<CollectionPtr>(snapshotedServices.collectionPtrOrView))});

            CollectionAcquisition acquisition(txnResources, acquiredCollection);
            acquisitions.emplace_back(std::move(acquisition));
        } else {
            // It's a view.
            auto& acquiredView = txnResources.addAcquiredView(
                {{currentAcquireCallNum,
                  prerequisites,
                  std::move(acquisitionRequest.dbLock),
                  std::move(acquisitionRequest.collLock),
                  std::move(acquisitionRequest.lockFreeReadsResources.lockFreeReadsBlock),
                  std::move(acquisitionRequest.lockFreeReadsResources.globalLock),
                  std::move(acquisitionRequest.acquisitionLocks)},
                 std::move(get<std::shared_ptr<const ViewDefinition>>(
                     snapshotedServices.collectionPtrOrView))});

            ViewAcquisition acquisition(txnResources, acquiredView);
            acquisitions.emplace_back(std::move(acquisition));
        }
    }

    // Check if this operation is a direct connection and if it is authorized to be one.
    const auto& nss = sortedAcquisitionRequests.begin()->prerequisites.nss;
    direct_connection_util::checkDirectShardOperationAllowed(opCtx, nss);

    return acquisitions;
}

bool haveAcquiredConsistentCatalogAndSnapshot(const CollectionCatalog* catalogBeforeSnapshot,
                                              const CollectionCatalog* catalogAfterSnapshot,
                                              long long replTermBeforeSnapshot,
                                              long long replTermAfterSnapshot) {
    return catalogBeforeSnapshot == catalogAfterSnapshot &&
        replTermBeforeSnapshot == replTermAfterSnapshot;
}

std::shared_ptr<const CollectionCatalog> getConsistentCatalogAndSnapshot(
    OperationContext* opCtx,
    const NamespaceStringOrUUIDRequests& acquisitionRequests,
    bool isWriteAcquisition) {
    while (true) {
        shard_role_details::SnapshotAttempt snapshotAttempt(opCtx, acquisitionRequests);
        snapshotAttempt.snapshotInitialState();
        // Writes always need to see the latest data. Secondary reads need to read at lastApplied.
        if (!isWriteAcquisition) {
            snapshotAttempt.changeReadSourceForSecondaryReads();
        }
        snapshotAttempt.openStorageSnapshot();
        if (auto catalog = snapshotAttempt.getConsistentCatalog()) {
            return catalog;
        }
    }
}

std::pair<NamespaceStringOrUUIDRequests, bool /* isWriteAcquisition*/> toNamespaceStringOrUUIDs(
    const TransactionResources::AcquiredCollections& acquiredCollections,
    const TransactionResources::AcquiredViews& acquiredViews) {
    NamespaceStringOrUUIDRequests requests;
    bool isWriteAcquisition = false;
    for (const auto& acquiredCollection : acquiredCollections) {
        const auto& prerequisites = acquiredCollection.prerequisites;
        requests.emplace_back(prerequisites.nss);
        isWriteAcquisition |= (prerequisites.operationType != AcquisitionPrerequisites::kRead);
    }
    for (const auto& acquiredView : acquiredViews) {
        const auto& prerequisites = acquiredView.prerequisites;
        requests.emplace_back(prerequisites.nss);
    }
    return {requests, isWriteAcquisition};
}

NamespaceStringOrUUIDRequests toNamespaceStringOrUUIDs(
    const CollectionOrViewAcquisitionRequests& acquisitionRequests) {
    NamespaceStringOrUUIDRequests requests;
    for (const auto& ar : acquisitionRequests) {
        requests.emplace_back(ar.nssOrUUID);
    }
    return requests;
}

void validateRequests(OperationContext* opCtx,
                      const CollectionOrViewAcquisitionRequests& acquisitionRequests) {
    const auto& oss = OperationShardingState::get(opCtx);
    const bool isComingFromRouter = oss.isComingFromRouter(opCtx);

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
            MONGO_UNREACHABLE_TASSERT(10083531);
        }

        // Check that if the operation came from a router, all collection acquisitions declare an
        // explicit Placement Version.
        const auto checkProperPlacementVersionDeclared = [&]() {
            const auto declaresPlacementVersion = [](const CollectionOrViewAcquisitionRequest& ar) {
                return ar.placementConcern.getShardVersion() ||
                    ar.placementConcern == PlacementConcern::kPretendUnsharded;
            };

            const auto doesNotNeedPlacementVersion =
                [](const CollectionOrViewAcquisitionRequest& ar) {
                    return ar.nssOrUUID.isNamespaceString() &&
                        (ar.nssOrUUID.nss().isNamespaceAlwaysUntracked() ||
                         ar.nssOrUUID.nss().isShardLocalNamespace());
                };

            // TODO: SERVER-80719 Remove this.
            if (ar.nssOrUUID.isNamespaceString() &&
                ar.nssOrUUID.nss().isTimeseriesBucketsCollection()) {
                return true;
            }

            return !isComingFromRouter || oss.getBypassCheckAllShardRoleAcquisitionsVersioned() ||
                declaresPlacementVersion(ar) || doesNotNeedPlacementVersion(ar);
        };

        if (!checkProperPlacementVersionDeclared()) {
            if (TestingProctor::instance().isEnabled()) {
                tasserted(10317000,
                          str::stream()
                              << "ShardRole collection acquisition without a declared placement "
                                 "version detected on an operation originating from a router. Nss: "
                              << redact(ar.nssOrUUID.toStringForErrorMsg()));
            } else {
                static logv2::SeveritySuppressor logSeverity{
                    Minutes{1}, logv2::LogSeverity::Warning(), logv2::LogSeverity::Debug(5)};
                LOGV2_DEBUG(10317001,
                            logSeverity().toInt(),
                            "ShardRole collection acquisition without a declared placement version "
                            "detected on an operation originating from a router.",
                            "nss"_attr = redact(ar.nssOrUUID.toStringForErrorMsg()));
            }
        }
    }
}

void checkShardingPlacement(OperationContext* opCtx,
                            const CollectionOrViewAcquisitionRequests& acquisitionRequests) {
    for (const auto& ar : acquisitionRequests) {
        // We only have to check placement for collections that come from a router, which
        // will have the namespace set.
        if (ar.nssOrUUID.isNamespaceString()) {
            checkPlacementVersion(opCtx, ar.nssOrUUID.nss(), ar.placementConcern);
        }
    }
}

ResolvedNamespaceOrViewAcquisitionRequest::LockFreeReadsResources takeGlobalLock(
    OperationContext* opCtx, const CollectionOrViewAcquisitionRequests& acquisitionRequests) {
    invariant(!acquisitionRequests.empty());

    const auto deadline =
        std::min_element(acquisitionRequests.begin(),
                         acquisitionRequests.end(),
                         [](const auto& lhs, const auto& rhs) {
                             return lhs.lockAcquisitionDeadline < rhs.lockAcquisitionDeadline;
                         })
            ->lockAcquisitionDeadline;

    auto lockFreeReadsBlock = std::make_shared<LockFreeReadsBlock>(opCtx);
    auto globalLock = std::make_shared<Lock::GlobalLock>(
        opCtx, MODE_IS, deadline, Lock::InterruptBehavior::kThrow, kLockFreeReadsGlobalLockOptions);
    return {lockFreeReadsBlock, globalLock};
}

std::shared_ptr<const CollectionCatalog> stashConsistentCatalog(
    OperationContext* opCtx,
    const CollectionOrViewAcquisitionRequests& acquisitionRequests,
    bool isWriteAcquisition) {
    auto requests = toNamespaceStringOrUUIDs(acquisitionRequests);
    auto catalog = getConsistentCatalogAndSnapshot(opCtx, requests, isWriteAcquisition);
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
    return !opCtx->inMultiDocumentTransaction() &&
        !shard_role_details::getLocker(opCtx)->isWriteLocked() &&
        !(shard_role_details::getRecoveryUnit(opCtx)->isActive() && !opCtx->isLockFreeReadsOp());
}

StashedTransactionResources buildStashedTransactionResourcesAndDetachFromOpCtx(
    OperationContext* opCtx) {
    auto& transactionResources = TransactionResources::get(opCtx);

    (void)prepareForYieldingTransactionResources(opCtx);

    // TODO SERVER-77213: This should mostly go away once the Locker resides inside
    // TransactionResources and the underlying locks point to it instead of the opCtx.
    //
    // Release all locks acquired since we are going to yield externally and our opCtx is going to
    // be destroyed.
    auto resetAcquisitionLocks = [](shard_role_details::AcquiredBase& acquisition) {
        acquisition.collectionLock.reset();
        acquisition.dbLock.reset();
        acquisition.globalLock.reset();
        acquisition.lockFreeReadsBlock.reset();
    };

    for (auto& acquisition : transactionResources.acquiredCollections) {
        resetAcquisitionLocks(acquisition);
    }

    for (auto& acquisition : transactionResources.acquiredViews) {
        resetAcquisitionLocks(acquisition);
    }

    auto originalState =
        std::exchange(transactionResources.state, TransactionResources::State::STASHED);

    return StashedTransactionResources{TransactionResources::detachFromOpCtx(opCtx), originalState};
}

void stashTransactionResourcesFromOperationContextAndDontAttachNewOnes(
    OperationContext* opCtx, TransactionResourcesStasher* stasher) {
    auto stashedResources = buildStashedTransactionResourcesAndDetachFromOpCtx(opCtx);
    stasher->stashTransactionResources(std::move(stashedResources));
}

logv2::DynamicAttributes getCurOpLogAttrs(OperationContext* opCtx) {
    logv2::DynamicAttributes attr;
    const auto curop = CurOp::get(opCtx);
    curop->debug().report(opCtx,
                          nullptr,
                          curop->getOperationStorageMetrics(),
                          curop->getPrepareReadConflicts(),
                          &attr);
    return attr;
}

}  // namespace

CollectionOrViewAcquisitionRequest CollectionOrViewAcquisitionRequest::fromOpCtx(
    OperationContext* opCtx,
    NamespaceString nss,
    boost::optional<UUID> uuid,
    AcquisitionPrerequisites::OperationType operationType,
    AcquisitionPrerequisites::ViewMode viewMode) {
    auto& oss = OperationShardingState::get(opCtx);
    auto& readConcern = repl::ReadConcernArgs::get(opCtx);

    // Acquisitions by uuid cannot possibly have a corresponding ShardVersion attached.
    PlacementConcern placementConcern =
        PlacementConcern{oss.getDbVersion(nss.dbName()), oss.getShardVersion(nss)};

    return CollectionOrViewAcquisitionRequest(
        nss, uuid, placementConcern, readConcern, operationType, viewMode);
}

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

CollectionAcquisition::CollectionAcquisition(CollectionAcquisition&& other) noexcept
    : _txnResources(std::exchange(other._txnResources, {})),
      _acquiredCollection(std::exchange(other._acquiredCollection, {})) {}

CollectionAcquisition& CollectionAcquisition::operator=(const CollectionAcquisition& other) {
    if (this != &other) {
        auto tmp{std::move(*this)};
        _txnResources = other._txnResources;
        _acquiredCollection = other._acquiredCollection;
        _txnResources->collectionAcquisitionReferences++;
        _acquiredCollection->refCount++;
    }
    return *this;
}

CollectionAcquisition& CollectionAcquisition::operator=(CollectionAcquisition&& other) {
    if (this != &other) {
        auto tmp{std::move(*this)};
        _txnResources = std::exchange(other._txnResources, {});
        _acquiredCollection = std::exchange(other._acquiredCollection, {});
    }
    return *this;
}

CollectionAcquisition::CollectionAcquisition(CollectionOrViewAcquisition&& other) {
    tassert(10566703,
            "Cannot convert a CollectionOrViewAcquisition containing a view into a "
            "CollectionAcquisition ",
            other.isCollection());
    auto& acquisition = get<CollectionAcquisition>(other._collectionOrViewAcquisition);
    _txnResources = std::exchange(acquisition._txnResources, {});
    _acquiredCollection = std::exchange(acquisition._acquiredCollection, {});
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
        if (currentRefCount == 0) {
            removeByPtr(transactionResources.acquiredCollections, _acquiredCollection);
        }
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
    tassert(10566702,
            str::stream() << "Collection " << nss().toStringForErrorMsg()
                          << " doesn't exist, so its UUID cannot be obtained",
            exists());
    return _acquiredCollection->collectionPtr->uuid();
}

const ScopedCollectionDescription& CollectionAcquisition::getShardingDescription() const {
    // The collectionDescription will only not be set if the caller as acquired the acquisition
    // using the kLocalCatalogOnlyWithPotentialDataLoss placement concern
    tassert(10566704,
            "Cannot retrieve sharding collectionDescription for kLocalCatalogOnly shard role "
            "acquisitions",
            _acquiredCollection->collectionDescription);
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

ViewAcquisition::ViewAcquisition(ViewAcquisition&& other) noexcept
    : _txnResources(std::exchange(other._txnResources, {})),
      _acquiredView(std::exchange(other._acquiredView, {})) {}


ViewAcquisition& ViewAcquisition::operator=(const ViewAcquisition& other) {
    if (this != &other) {
        auto tmp{std::move(*this)};
        _txnResources = other._txnResources;
        _acquiredView = other._acquiredView;
        _txnResources->viewAcquisitionReferences++;
        _acquiredView->refCount++;
    }
    return *this;
}

ViewAcquisition& ViewAcquisition::operator=(ViewAcquisition&& other) {
    if (this != &other) {
        auto tmp{std::move(*this)};
        _txnResources = std::exchange(other._txnResources, {});
        _acquiredView = std::exchange(other._acquiredView, {});
    }
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
            removeByPtr(transactionResources.acquiredViews, _acquiredView);
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
    tassert(10566705, "Missing view definition", _acquiredView->viewDefinition);
    return *_acquiredView->viewDefinition;
}

CollectionAcquisition acquireCollection(OperationContext* opCtx,
                                        CollectionAcquisitionRequest acquisitionRequest,
                                        LockMode mode) {
    return CollectionAcquisition(
        acquireCollectionOrView(opCtx, std::move(acquisitionRequest), mode));
}

CollectionAcquisitions acquireCollections(OperationContext* opCtx,
                                          const CollectionAcquisitionRequests& acquisitionRequests,
                                          LockMode mode) {
    // Transform the CollectionAcquisitionRequests to NamespaceOrViewAcquisitionRequests.
    CollectionOrViewAcquisitionRequests namespaceOrViewAcquisitionRequests;
    std::move(acquisitionRequests.begin(),
              acquisitionRequests.end(),
              std::back_inserter(namespaceOrViewAcquisitionRequests));

    // Acquire the collections
    auto acquisitions = acquireCollectionsOrViews(opCtx, namespaceOrViewAcquisitionRequests, mode);

    // Transform the acquisitions to CollectionAcquisitions
    CollectionAcquisitions collectionAcquisitions;
    for (auto& acquisition : acquisitions) {
        // It must be a collection, because that's what the acquisition request stated.
        invariant(acquisition.isCollection());
        collectionAcquisitions.emplace_back(std::move(acquisition));
    }
    return collectionAcquisitions;
}

CollectionAcquisitionMap makeAcquisitionMap(CollectionAcquisitions acquisitions) {
    CollectionAcquisitionMap map;
    map.reserve(acquisitions.size());
    for (auto&& a : acquisitions) {
        map.emplace(a.nss(), std::move(a));
    }
    return map;
}

CollectionOrViewAcquisitionMap makeAcquisitionMap(CollectionOrViewAcquisitions acquisitions) {
    CollectionOrViewAcquisitionMap map;
    map.reserve(acquisitions.size());
    for (auto&& a : acquisitions) {
        map.emplace(a.nss(), std::move(a));
    }
    return map;
}

CollectionOrViewAcquisition acquireCollectionOrView(
    OperationContext* opCtx, CollectionOrViewAcquisitionRequest acquisitionRequest, LockMode mode) {
    CollectionOrViewAcquisitionRequests requests{std::move(acquisitionRequest)};
    auto acquisition = acquireCollectionsOrViews(opCtx, requests, mode);
    invariant(acquisition.size() == 1);
    return std::move(acquisition.front());
}

CollectionAcquisition acquireCollectionMaybeLockFree(
    OperationContext* opCtx, CollectionAcquisitionRequest acquisitionRequest) {
    return CollectionAcquisition(
        acquireCollectionOrViewMaybeLockFree(opCtx, std::move(acquisitionRequest)));
}

CollectionAcquisitions acquireCollectionsMaybeLockFree(
    OperationContext* opCtx, const CollectionAcquisitionRequests& acquisitionRequests) {
    // Transform the CollectionAcquisitionRequests to NamespaceOrViewAcquisitionRequests.
    CollectionOrViewAcquisitionRequests namespaceOrViewAcquisitionRequests;
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
        dassert(acquisition.isCollection());
        collectionAcquisitions.emplace_back(std::move(acquisition));
    }
    return collectionAcquisitions;
}

CollectionOrViewAcquisition acquireCollectionOrViewMaybeLockFree(
    OperationContext* opCtx, CollectionOrViewAcquisitionRequest acquisitionRequest) {
    CollectionOrViewAcquisitionRequests requests{std::move(acquisitionRequest)};

    auto acquisition = acquireCollectionsOrViewsMaybeLockFree(opCtx, requests);
    dassert(acquisition.size() == 1);
    return std::move(*acquisition.begin());
}

namespace shard_role_details {
void SnapshotAttempt::snapshotInitialState() {
    // The read source used can change depending on replication state, so we must fetch the repl
    // state beforehand, to compare with afterwards.
    _replTermBeforeSnapshot = repl::ReplicationCoordinator::get(_opCtx)->getTerm();

    _catalogBeforeSnapshot = CollectionCatalog::get(_opCtx);
}

void SnapshotAttempt::changeReadSourceForSecondaryReads() {
    dassert(_replTermBeforeSnapshot && _catalogBeforeSnapshot);
    auto catalog = *_catalogBeforeSnapshot;

    for (auto& nsOrUUID : _acquisitionRequests) {
        NamespaceString nss;
        try {
            // This can lookup into the commit pending entries without establishing a consistent
            // collection. This is safe because we only use this resolved namespace to check if the
            // collection is replicated or not. As we do not allow changing this setting by the user
            // this is independent of the consistent collection namespace. Note that a later check
            // in the Acquisition API will verify that it is not uncommitted in the WT snapshot. We
            // do not allow lookups on uncommitted collections since they still do not exist.
            nss = catalog->resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(_opCtx,
                                                                                       nsOrUUID);
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
        if (_shouldReadAtLastApplied)
            return;
    }
}

void SnapshotAttempt::openStorageSnapshot() {
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

ResolvedNamespaceOrViewAcquisitionRequests generateSortedAcquisitionRequests(
    OperationContext* opCtx,
    const CollectionCatalog& catalog,
    const CollectionOrViewAcquisitionRequests& acquisitionRequests,
    const ResolvedNamespaceOrViewAcquisitionRequest::LockFreeReadsResources&
        lockFreeReadsResources) {
    ResolvedNamespaceOrViewAcquisitionRequests resolvedAcquisitionRequests;

    auto readTimestamp = shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();

    int counter = 0;
    for (const auto& ar : acquisitionRequests) {
        const auto resolvedBy =
            ar.nssOrUUID.isNamespaceString() ? ResolutionType::kNamespace : ResolutionType::kUUID;

        const auto& nss = [&] {
            if (ar.nssOrUUID.isUUID()) {
                auto coll =
                    catalog.establishConsistentCollection(opCtx, ar.nssOrUUID, readTimestamp);
                validateResolvedCollectionByUUID(opCtx, ar, coll.get());
                return coll->ns();
            } else {
                return ar.nssOrUUID.nss();
            }
        }();
        const auto& prerequisiteUUID =
            ar.nssOrUUID.isUUID() ? ar.nssOrUUID.uuid() : ar.expectedUUID;
        AcquisitionPrerequisites prerequisites(nss,
                                               prerequisiteUUID,
                                               ar.readConcern,
                                               ar.placementConcern,
                                               ar.operationType,
                                               ar.viewMode);

        ResolvedNamespaceOrViewAcquisitionRequest resolvedAcquisitionRequest{
            ResourceId(RESOURCE_COLLECTION, counter++),
            prerequisites,
            resolvedBy,
            nullptr,
            boost::none,
            lockFreeReadsResources};
        // We don't care about ordering in this case, use a mock ResourceId as the key.
        resolvedAcquisitionRequests.emplace_back(std::move(resolvedAcquisitionRequest));
    }

    // Empirically, this early-return results in better performance than trying to sort a vector of
    // size 1.
    if (resolvedAcquisitionRequests.size() == 1) {
        return resolvedAcquisitionRequests;
    }

    // Sort them in ascending ResourceId order since that is the canonical lock ordering used across
    // the server.
    std::sort(resolvedAcquisitionRequests.begin(),
              resolvedAcquisitionRequests.end(),
              [](auto& lhs, auto& rhs) { return lhs.resourceId < rhs.resourceId; });
    return resolvedAcquisitionRequests;
}

CollectionOrViewAcquisitions acquireCollectionsOrViewsLockFree(
    OperationContext* opCtx, const CollectionOrViewAcquisitionRequests& acquisitionRequests) {
    if (acquisitionRequests.empty()) {
        return {};
    }

    validateRequests(opCtx, acquisitionRequests);

    // We shouldn't have an open snapshot unless a previous lock-free acquisition opened and
    // stashed it already.
    dassert(!shard_role_details::getRecoveryUnit(opCtx)->isActive() || opCtx->isLockFreeReadsOp());

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
    auto catalog = openSnapshot
        ? stashConsistentCatalog(opCtx, acquisitionRequests, false /* isWriteAcquisition */)
        : CollectionCatalog::get(opCtx);

    try {
        // Second sharding placement check.
        checkShardingPlacement(opCtx, acquisitionRequests);

        auto sortedAcquisitionRequests = shard_role_details::generateSortedAcquisitionRequests(
            opCtx, *catalog, acquisitionRequests, lockFreeReadsResources);
        return acquireResolvedCollectionsOrViewsWithoutTakingLocks(
            opCtx, *catalog, sortedAcquisitionRequests);
    } catch (...) {
        if (openSnapshot && !shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork())
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
        throw;
    }
}
}  // namespace shard_role_details

CollectionOrViewAcquisitions acquireCollectionsOrViews(
    OperationContext* opCtx,
    const CollectionOrViewAcquisitionRequests& acquisitionRequests,
    LockMode mode) {
    if (acquisitionRequests.empty()) {
        return {};
    }

    validateRequests(opCtx, acquisitionRequests);

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
        const auto& dbName = sortedAcquisitionRequests.begin()->prerequisites.nss.dbName();
        Lock::DBLockSkipOptions dbLockOptions = [&]() {
            Lock::DBLockSkipOptions dbLockOptions;
            dbLockOptions.skipRSTLLock = std::all_of(
                sortedAcquisitionRequests.begin(),
                sortedAcquisitionRequests.end(),
                [](const auto& ar) { return AutoGetDb::canSkipRSTLLock(ar.prerequisites.nss); });
            dbLockOptions.skipFlowControlTicket =
                std::all_of(sortedAcquisitionRequests.begin(),
                            sortedAcquisitionRequests.end(),
                            [](const auto& ar) {
                                return AutoGetDb::canSkipFlowControlTicket(ar.prerequisites.nss);
                            });
            auto mostRestrictiveIntent = rss::consensus::IntentRegistry::Intent::Read;
            for (const auto& acquisition : sortedAcquisitionRequests) {
                if (acquisition.prerequisites.operationType == AcquisitionPrerequisites::kWrite) {
                    mostRestrictiveIntent = rss::consensus::IntentRegistry::Intent::Write;
                    // Cannot get more restricted than Write intent so break out early.
                    break;
                }
                if (acquisition.prerequisites.operationType ==
                    AcquisitionPrerequisites::kUnreplicatedWrite) {
                    mostRestrictiveIntent = rss::consensus::IntentRegistry::Intent::LocalWrite;
                }
            }
            dbLockOptions.explicitIntent = mostRestrictiveIntent;
            return dbLockOptions;
        }();

        const bool isWriteAcquisition =
            dbLockOptions.explicitIntent != rss::consensus::IntentRegistry::Intent::Read;

        const auto lockAcquisitionDeadline =
            sortedAcquisitionRequests.begin()->prerequisites.lockAcquisitionDeadline;
        const auto dbLockMode = isSharedLockMode(mode) ? MODE_IS : MODE_IX;
        const auto dbLock = std::make_shared<Lock::DBLock>(
            opCtx, dbName, dbLockMode, lockAcquisitionDeadline, dbLockOptions);

        for (auto& ar : sortedAcquisitionRequests) {
            const auto& nss = ar.prerequisites.nss;
            tassert(7300400,
                    str::stream()
                        << "Cannot acquire locks for collections across different databases ('"
                        << dbName.toStringForErrorMsg() << "' vs '"
                        << nss.dbName().toStringForErrorMsg() << "'",
                    dbName == nss.dbName());
            const auto& lockAcquisitionDeadline = ar.prerequisites.lockAcquisitionDeadline;

            ar.dbLock = dbLock;
            ar.acquisitionLocks.dbLock = dbLockMode;
            ar.acquisitionLocks.dbLockOptions = dbLockOptions;
            ar.collLock.emplace(opCtx, nss, mode, lockAcquisitionDeadline);
            ar.acquisitionLocks.collLock = mode;
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
        bool hasOptimisticResolutionFailed = std::any_of(
            sortedAcquisitionRequests.begin(),
            sortedAcquisitionRequests.end(),
            [&](const auto& ar) {
                const auto& prerequisites = ar.prerequisites;
                if (ar.resolvedBy != ResolutionType::kUUID) {
                    return false;
                }
                const auto& currentCatalog = CollectionCatalog::get(opCtx);
                const auto nss =
                    currentCatalog->resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(
                        opCtx,
                        NamespaceStringOrUUID{prerequisites.nss.dbName(), *prerequisites.uuid});
                return prerequisites.nss != nss;
            });

        if (MONGO_unlikely(hasOptimisticResolutionFailed)) {
            // Retry optimistic resolution.
            continue;
        }

        // Open a consistent catalog snapshot if needed.
        bool openSnapshot = !shard_role_details::getRecoveryUnit(opCtx)->isActive();
        auto catalog = openSnapshot
            ? stashConsistentCatalog(opCtx, acquisitionRequests, isWriteAcquisition)
            : CollectionCatalog::get(opCtx);

        try {
            return acquireResolvedCollectionsOrViewsWithoutTakingLocks(
                opCtx, *catalog, sortedAcquisitionRequests);
        } catch (const DBException& ex) {
            if (openSnapshot && !shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork())
                shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

            if (ex.code() == ErrorCodes::CollectionUUIDMismatch) {
                const auto info = ex.extraInfo<CollectionUUIDMismatchInfo>();
                // A UUID mismatch error here for a collection we requested by UUID should be
                // transformed into a NamespaceNotFound error to avoid confusing the user if it
                // doesn't exist in our snapshot. This error must be treated here since we do commit
                // pending lookups for namespace resolution before we've established a consistent
                // collection.
                //
                // This case could be reached by having correctly looked up a commit pending UUID
                // twice for a collection that has just been created but not yet visible in our WT
                // snapshot. In this case we have to reconvert the CollectionUUIDMismatch error.
                auto isUUIDMismatchDueToFailedResolution =
                    std::any_of(sortedAcquisitionRequests.begin(),
                                sortedAcquisitionRequests.end(),
                                [&](const ResolvedNamespaceOrViewAcquisitionRequest& ar) {
                                    return ar.resolvedBy == ResolutionType::kUUID &&
                                        ar.prerequisites.uuid == info->collectionUUID();
                                });
                uassert(ErrorCodes::NamespaceNotFound,
                        str::stream() << "Namespace " << info->dbName().toStringForErrorMsg() << ":"
                                      << info->collectionUUID() << " not found",
                        !(isUUIDMismatchDueToFailedResolution && !info->actualCollection()));
            }
            throw;
        }
    }
}

CollectionOrViewAcquisitions acquireCollectionsOrViewsMaybeLockFree(
    OperationContext* opCtx, const CollectionOrViewAcquisitionRequests& acquisitionRequests) {
    const bool allAcquisitionsForRead =
        std::all_of(acquisitionRequests.begin(), acquisitionRequests.end(), [](const auto& ar) {
            return ar.operationType == AcquisitionPrerequisites::kRead;
        });
    tassert(7740500, "Cannot acquire for write without locks", allAcquisitionsForRead);

    if (supportsLockFreeRead(opCtx)) {
        return shard_role_details::acquireCollectionsOrViewsLockFree(opCtx, acquisitionRequests);
    } else {
        const auto lockMode = opCtx->inMultiDocumentTransaction() ? MODE_IX : MODE_IS;
        return acquireCollectionsOrViews(opCtx, acquisitionRequests, lockMode);
    }
}

CollectionAcquisition acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(
    OperationContext* opCtx, const NamespaceString& nss, LockMode mode) {
    tassert(10566706,
            "Cannot use acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss on "
            "sharding-aware operations",
            !OperationShardingState::isComingFromRouter(opCtx));

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

CollectionAcquisition acquireLocalCollectionNoConsistentCatalog(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    AcquisitionPrerequisites::OperationType operationType,
    LockMode lockMode) {
    const auto replIntent = [operationType]() {
        switch (operationType) {
            case AcquisitionPrerequisites::OperationType::kRead:
                return rss::consensus::IntentRegistry::Intent::Read;
            case AcquisitionPrerequisites::OperationType::kWrite:
                return rss::consensus::IntentRegistry::Intent::Write;
            case AcquisitionPrerequisites::OperationType::kUnreplicatedWrite:
                return rss::consensus::IntentRegistry::Intent::LocalWrite;
        }
        MONGO_UNREACHABLE_TASSERT(7683107);
    }();

    tassert(7683108,
            "Cannot use acquireCollectionNoConsistentCatalog on sharding-aware operations",
            !OperationShardingState::isComingFromRouter(opCtx));

    auto& txnResources = TransactionResources::get(opCtx);

    auto currentAcquireCallNum = txnResources.increaseAcquireCollectionCallCount();

    const auto dbLockMode = isSharedLockMode(lockMode) ? MODE_IS : MODE_IX;
    auto dbLock =
        std::make_shared<Lock::DBLock>(opCtx,
                                       nsOrUUID.dbName(),
                                       dbLockMode,
                                       Date_t::max(),
                                       Lock::DBLockSkipOptions{.explicitIntent = replIntent});

    // Takes the collection lock. If nsOrUUID is of type uuid, then it optimistically resolves the
    // uuid to nss using the latest catalog, locks the nss and then checks the resolution was
    // correct.
    Lock::CollectionLock collLock =
        CollectionNamespaceOrUUIDLock::resolveAndLockCollectionByNssOrUUID(
            opCtx, nsOrUUID, lockMode);

    const auto catalog = CollectionCatalog::get(opCtx);
    const auto nss = catalog->resolveNamespaceStringOrUUID(opCtx, nsOrUUID);

    auto prerequisites =
        AcquisitionPrerequisites(nss,
                                 boost::none,
                                 repl::ReadConcernArgs::get(opCtx),
                                 PlacementConcern::kPretendUnsharded,
                                 operationType,
                                 AcquisitionPrerequisites::ViewMode::kMustBeCollection);
    prerequisites.useConsistentCatalog = false;

    auto collOrView = acquireLocalCollectionOrView(opCtx, *catalog, prerequisites);
    invariant(holds_alternative<CollectionPtr>(collOrView));

    auto& coll = get<CollectionPtr>(collOrView);
    if (coll)
        prerequisites.uuid = boost::optional<UUID>(coll->uuid());

    shard_role_details::AcquisitionLocks lockRequirements;
    lockRequirements.dbLock = dbLockMode;
    lockRequirements.collLock = lockMode;

    shard_role_details::AcquiredCollection& acquiredCollection =
        txnResources.addAcquiredCollection({currentAcquireCallNum,
                                            prerequisites,
                                            std::move(dbLock),
                                            std::move(collLock),
                                            std::move(lockRequirements),
                                            std::move(coll)});

    // Record the catalog epoch at the first acquisition. This is necessary to detect epoch changes
    // among different catalog snapshots at every restore.
    if (!txnResources.catalogEpoch) {
        txnResources.catalogEpoch = catalog->getEpoch();
    }

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
        // This establish consistent collection is safe to use because it will happen after the
        // collection writer has fully committed the changes. The resulted Collection object will be
        // fetched from the in-memory catalog and not from the durable catalog and no temporary
        // objects will be stored in the snapshot.
        auto collection = catalog->establishConsistentCollection(
            opCtx, acquiredCollection->prerequisites.nss, boost::none /*readTimestamp*/);
        checkCollectionUUIDMismatch(
            opCtx, nss, collection.get(), acquiredCollection->prerequisites.uuid);
        if (!acquiredCollection->collectionPtr && collection.get()) {
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

PreparedForYieldToken prepareForYieldingTransactionResources(OperationContext* opCtx) {
    auto& transactionResources = TransactionResources::get(opCtx);
    invariant(
        !(transactionResources.yielded ||
          transactionResources.state == shard_role_details::TransactionResources::State::YIELDED));
    invariant(
        transactionResources.state == shard_role_details::TransactionResources::State::ACTIVE ||
        transactionResources.state == shard_role_details::TransactionResources::State::EMPTY ||
        transactionResources.state == shard_role_details::TransactionResources::State::FAILED);
    for (auto& acquisition : transactionResources.acquiredCollections) {
        // Yielding kLocalCatalogOnlyWithPotentialDataLoss acquisitions is not allowed.
        invariant(
            !holds_alternative<AcquisitionPrerequisites::PlacementConcernPlaceholder>(
                acquisition.prerequisites.placementConcern),
            str::stream() << "Collection " << acquisition.prerequisites.nss.toStringForErrorMsg()
                          << " acquired with special placement concern and cannot be yielded");
        // Detach the CollectionPtr from the snapshot and re-assign it. This is safe to do since we
        // do not dereference it while yielded. This will only be used by the restore to check if
        // the collection has appeared suddenly.
        acquisition.collectionPtr =
            CollectionPtr::CollectionPtr_UNSAFE(acquisition.collectionPtr.get());
    }

    return PreparedForYieldToken{};
}

YieldedTransactionResources yieldTransactionResourcesFromOperationContext(OperationContext* opCtx) {
    auto token = prepareForYieldingTransactionResources(opCtx);
    tassert(10778400,
            "Cannot yield inside a write unit of work",
            !shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
    return yieldTransactionResourcesFromOperationContext(opCtx, token);
}

YieldedTransactionResources yieldTransactionResourcesFromOperationContext(OperationContext* opCtx,
                                                                          PreparedForYieldToken) {
    auto& transactionResources = TransactionResources::get(opCtx);

    // Stash the locker state, only if we have any active acquisition. Don't do it when we don't, as
    // it is illegal to call saveLockStateAndUnlock without actually holding any locks.
    if (transactionResources.state == shard_role_details::TransactionResources::State::ACTIVE) {
        Locker::LockSnapshot lockSnapshot;
        shard_role_details::getLocker(opCtx)->saveLockStateAndUnlock(&lockSnapshot);
        transactionResources.yielded.emplace(
            TransactionResources::YieldedStateHolder{std::move(lockSnapshot)});
    }

    auto originalState = std::exchange(transactionResources.state,
                                       shard_role_details::TransactionResources::State::YIELDED);

    return YieldedTransactionResources(TransactionResources::detachFromOpCtx(opCtx), originalState);
}

void stashTransactionResourcesFromOperationContext(OperationContext* opCtx,
                                                   TransactionResourcesStasher* stasher) {
    stashTransactionResourcesFromOperationContextAndDontAttachNewOnes(opCtx, stasher);
    shard_role_details::TransactionResources::attachToOpCtx(
        opCtx, std::make_unique<shard_role_details::TransactionResources>());
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

        ScopeGuard removeStaleCollectionPtrReferences([&] {
            // Detach the CollectionPtr from the snapshot in case of failure since we could retry
            // the whole operation again by abandoning the snapshot and reacquiring the locks. Note
            // this is safe to do since the collections will have passed a appearance after restore
            // check.
            for (auto& acquiredCollection : transactionResources.acquiredCollections) {
                acquiredCollection.collectionPtr =
                    CollectionPtr::CollectionPtr_UNSAFE(acquiredCollection.collectionPtr.get());
            }
        });

        // Reestablish a consistent catalog snapshot (multi document transactions don't yield).
        auto [requests, isWriteAcquisition] = toNamespaceStringOrUUIDs(
            transactionResources.acquiredCollections, transactionResources.acquiredViews);
        auto catalog = getConsistentCatalogAndSnapshot(opCtx, requests, isWriteAcquisition);

        // The catalog epoch changes every time a replication rollback is performed. If a rollback
        // occurs while the query is yielded, the query might be resumed on a earlier point in
        // time which can lead to anomalies. To avoid potential issues, the query must be
        // terminated.
        tassert(9935000,
                "Found a collection acquisition without catalog epoch",
                transactionResources.catalogEpoch.has_value());
        int64_t catalogEpochAtRestoreTime = catalog->getEpoch();
        int64_t catalogEpochAtAcquisitionTime = *transactionResources.catalogEpoch;
        uassert(ErrorCodes::QueryPlanKilled,
                "The catalog was closed and reopened",
                catalogEpochAtRestoreTime == catalogEpochAtAcquisitionTime);
        // Reacquire service snapshots. Will throw if placement concern can no longer be met.
        for (auto& acquiredCollection : transactionResources.acquiredCollections) {
            const auto& prerequisites = acquiredCollection.prerequisites;

            auto uassertCollectionAppearedAfterRestore = [&] {
                uasserted(ErrorCodes::QueryPlanKilled,
                          str::stream()
                              << "Collection " << prerequisites.nss.toStringForErrorMsg()
                              << " appeared after a restore, which violates the semantics of "
                                 "restore");
            };

            auto uassertedCollectionIsAViewAfterRestore = [&] {
                uasserted(ErrorCodes::QueryPlanKilled,
                          str::stream() << "Collection " << prerequisites.nss.toStringForErrorMsg()
                                        << " is now a view after restore from yield");
            };

            auto checkOrphanRangePreserverIsStillValid = [&] {
                // The query will get killed, throwing a QueryPlanKilled error, when all these
                // conditions are met:
                // * The Range Preserver associated with this query has been invalidated.
                // * The Read Concern of the operation is other than ‘snapshot’ or ‘available’.
                // * The parameter terminateSecondaryReadsOnOrphanCleanup is enabled.
                // * The feature flag featureFlagTerminateSecondaryReadsUponRangeDeletion is
                // enabled.
                // (Ignore FCV check): It will always check if the feature flag
                // TerminateSecondaryReadsUponRangeDeletion is enabled on the current binary to
                // terminate the read query.
                if (acquiredCollection.ownershipFilter &&
                    feature_flags::gTerminateSecondaryReadsUponRangeDeletion
                        .isEnabledAndIgnoreFCVUnsafe() &&
                    terminateSecondaryReadsOnOrphanCleanup.load() &&
                    (prerequisites.readConcern.getLevel() !=
                         repl::ReadConcernLevel::kSnapshotReadConcern &&
                     prerequisites.readConcern.getLevel() !=
                         repl::ReadConcernLevel::kAvailableReadConcern) &&
                    !acquiredCollection.ownershipFilter->isRangePreserverStillValid()) {
                    static constexpr char errMsg[] =
                        "Read has been terminated due to orphan range cleanup";
                    if (enableQueryKilledByRangeDeletionLog.load()) {
                        LOGV2(10016300,
                              errMsg,
                              logv2::DynamicAttributes{
                                  getCurOpLogAttrs(opCtx),
                                  "orphanCleanupDelaySecs"_attr = orphanCleanupDelaySecs.load(),
                              });
                    }
                    killedDueToRangeDeletionCounter.increment();
                    uasserted(ErrorCodes::QueryPlanKilled, errMsg);
                }
            };

            if (prerequisites.operationType == AcquisitionPrerequisites::OperationType::kRead) {
                checkOrphanRangePreserverIsStillValid();

                // Just reacquire the CollectionPtr. Reads don't care about placement changes
                // because they have already established a ScopedCollectionFilter that acts as
                // RangePreserver.
                auto collOrView = acquireLocalCollectionOrView(opCtx, *catalog, prerequisites);

                if (!holds_alternative<CollectionPtr>(collOrView)) {
                    uassertedCollectionIsAViewAfterRestore();
                }
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

                if (!holds_alternative<CollectionPtr>(
                        reacquiredServicesSnapshot.collectionPtrOrView)) {
                    uassertedCollectionIsAViewAfterRestore();
                }
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
                opCtx, transactionResources.acquiredCollections.front().prerequisites.nss);

            // TODO: This will be removed when we no longer snapshot sharding state on CollectionPtr
            invariant(acquiredCollection.collectionDescription);
            if (acquiredCollection.collectionDescription->isSharded()) {
                acquiredCollection.collectionPtr.setShardKeyPattern(
                    acquiredCollection.collectionDescription->getKeyPattern());
            }
        }

        for (auto& acquiredView : transactionResources.acquiredViews) {
            const auto& prerequisites = acquiredView.prerequisites;
            auto collOrView = acquireLocalCollectionOrView(opCtx, *catalog, prerequisites);

            uassert(ErrorCodes::QueryPlanKilled,
                    str::stream() << "Namespace '" << prerequisites.nss.toStringForErrorMsg()
                                  << "' is no longer a view.",
                    std::holds_alternative<std::shared_ptr<const ViewDefinition>>(collOrView));

            const auto postRestoreViewDefinition =
                get<std::shared_ptr<const ViewDefinition>>(collOrView);
            uassert(ErrorCodes::QueryPlanKilled,
                    str::stream() << "View definition for '"
                                  << prerequisites.nss.toStringForErrorMsg() << "' has changed.",
                    acquiredView.viewDefinition == postRestoreViewDefinition);
        }

        removeStaleCollectionPtrReferences.dismiss();
        return catalog;
    };

    // If the yielded TransactionResources do not have any acquired collection/view, then trivially
    // restore to an EMPTY state.
    if (transactionResources.acquiredCollections.empty() &&
        transactionResources.acquiredViews.empty()) {
        transactionResources.state = shard_role_details::TransactionResources::State::EMPTY;
        scopeGuard.dismiss();
        return;
    }

    auto catalog = [&]() -> std::shared_ptr<const CollectionCatalog> {
        size_t attempts = 0;
        while (true) {
            attempts++;
            try {
                return restoreFn();
            } catch (const ExceptionFor<ErrorCodes::StaleConfig>& ex) {
                if (ex->getCriticalSectionSignal() && !opCtx->isRetryableWrite()) {
                    // If we encountered a critical section, then yield, wait for the critical
                    // section to finish and then we'll resume the write from the point we had left.
                    // We do this to prevent large multi-writes from repeatedly failing due to
                    // StaleConfig and exhausting the mongos retry attempts. Yield the locks.
                    //
                    // We don't do this in case we are executing a retryable write, since doing that
                    // means holding the session checked in while we wait, which can lead to
                    // deadlocks. Also retryable writes will safely be retried by a higher layer in
                    // case of StaleConfig.
                    Locker::LockSnapshot lockSnapshot;
                    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
                    shard_role_details::getLocker(opCtx)->saveLockStateAndUnlock(&lockSnapshot);
                    transactionResources.yielded.emplace(
                        TransactionResources::YieldedStateHolder{std::move(lockSnapshot)});
                    // Wait for the critical section to finish.
                    refresh_util::waitForCriticalSectionToComplete(opCtx,
                                                                   *ex->getCriticalSectionSignal())
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
            } catch (const StorageUnavailableException& ex) {
                Locker::LockSnapshot lockSnapshot;
                shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
                shard_role_details::getLocker(opCtx)->saveLockStateAndUnlock(&lockSnapshot);
                transactionResources.yielded.emplace(
                    TransactionResources::YieldedStateHolder{std::move(lockSnapshot)});

                logAndRecordWriteConflictAndBackoff(opCtx,
                                                    attempts,
                                                    "shard role yield",
                                                    ex.reason(),
                                                    NamespaceStringOrUUID(NamespaceString::kEmpty));
                // Try again to restore.
                continue;
            }
        }
    }();

    transactionResources.state = yieldedResourcesHolder._originalState;

    if (!opCtx->inMultiDocumentTransaction()) {
        CollectionCatalog::stash(opCtx, catalog);
    }

    scopeGuard.dismiss();
}

std::unique_ptr<shard_role_details::TransactionResources>
restoreStashedTransactionResourcesToOperationContext(
    OperationContext* opCtx, StashedTransactionResources& stashedResources) {
    std::unique_ptr<TransactionResources> outOriginalTransactionResources;
    if (TransactionResources::isPresent(opCtx)) {
        outOriginalTransactionResources = TransactionResources::detachFromOpCtx(opCtx);
    }

    invariant(stashedResources._yieldedResources);

    ScopeGuard restoreFailedGuard([&] {
        if (TransactionResources::isPresent(opCtx)) {
            // We have attempted to restore the resources but failed, the transaction resources have
            // been moved to the opCtx, so we must move them back to the stashed object.
            invariant(!stashedResources._yieldedResources);
            stashedResources._yieldedResources = TransactionResources::detachFromOpCtx(opCtx);
        }
        stashedResources._yieldedResources->releaseAllResourcesOnCommitOrAbort();
        stashedResources._yieldedResources->state = TransactionResources::State::FAILED;
        TransactionResources::attachToOpCtx(opCtx, std::move(outOriginalTransactionResources));
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
    auto restoreLocksFn = [&](shard_role_details::AcquiredBase& acquisition) {
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
    };

    auto& acquiredCollections = stashedResources._yieldedResources->acquiredCollections;
    auto& acquiredViews = stashedResources._yieldedResources->acquiredViews;

    auto itAcquiredCollections = acquiredCollections.begin();
    auto itAcquiredViews = acquiredViews.begin();

    // Two-pointers approach to iterate acquiredCollections and acquiredViews in
    // acquireCollectionCallNum order.
    while (itAcquiredCollections != acquiredCollections.end() ||
           itAcquiredViews != acquiredViews.end()) {
        if (itAcquiredCollections != acquiredCollections.end() &&
            (itAcquiredViews == acquiredViews.end() ||
             itAcquiredCollections->acquireCollectionCallNum <=
                 itAcquiredViews->acquireCollectionCallNum)) {
            restoreLocksFn(*itAcquiredCollections);
            itAcquiredCollections++;
        } else {
            restoreLocksFn(*itAcquiredViews);
            itAcquiredViews++;
        }
    }

    // Wait for a configured amount of time after acquiring locks if the failpoint is enabled
    catalog_helper::setAutoGetCollectionWaitFailpointExecute(
        [&](const BSONObj& data) { sleepFor(Milliseconds(data["waitForMillis"].numberInt())); });

    restoreTransactionResourcesToOperationContext(
        opCtx,
        YieldedTransactionResources{std::move(stashedResources._yieldedResources),
                                    stashedResources._originalState});

    restoreFailedGuard.dismiss();
    return outOriginalTransactionResources;
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

StashTransactionResourcesForMultiDocumentTransaction::
    StashTransactionResourcesForMultiDocumentTransaction(OperationContext* opCtx)
    : _opCtx(opCtx), _restored(false) {
    if (TransactionResources::isPresent(_opCtx)) {
        _stashedResources = buildStashedTransactionResourcesAndDetachFromOpCtx(opCtx);
        // Re-attach an empty TransactionResources to the opCtx to prepare for the multi-document
        // transaction for new acquisitions.
        shard_role_details::TransactionResources::attachToOpCtx(
            opCtx, std::make_unique<shard_role_details::TransactionResources>());
        _stashed = true;
    } else {
        _stashed = false;
    }
}

StashTransactionResourcesForMultiDocumentTransaction::
    ~StashTransactionResourcesForMultiDocumentTransaction() {
    if (!_restored) {
        restoreOnAbort();
    }
}

void StashTransactionResourcesForMultiDocumentTransaction::restoreOnCommit() {
    // Nothing to do if we never stashed.
    if (!_stashed || _restored)
        return;
    std::unique_ptr<shard_role_details::TransactionResources> originalTransactionResources =
        restoreStashedTransactionResourcesToOperationContext(_opCtx, _stashedResources);
    _restored = true;
    // The original TransactionResources must be empty, as we are restoring from a successful
    // multi-document transaction and we expect no active acquisitions to be left in scope at this
    // point.
    originalTransactionResources.reset();
}

void StashTransactionResourcesForMultiDocumentTransaction::restoreOnAbort() {
    // Nothing to do if we never stashed.
    if (!_stashed || _restored)
        return;
    // Free and destroy whatever transaction resources are active. Notice that this can throw if
    // the attached resources are not empty. This would cause a crash (std::terminate, you can't
    // throw in a destructor). We never expect this case to happen because those resorces are
    // acquired by the multi-document transaction. Since at this stage the transaction participant
    // has aborted and failed we expect no acquisitions to be still in scope.
    TransactionResources::detachFromOpCtx(_opCtx);
    // Reaching this point means that the transaction participant has aborted and threw an
    // exception. We are therefore failing the entire operation and restoring the resources would
    // not be logically sound. Additionally, attempting to restore would be problematic as acquiring
    // locks (like GlobalLock) would likely fail or deadlock. We therefore re-attach the resources
    // as failed, which indicates no locks are actually taken by acquisitions.
    _stashedResources._yieldedResources->releaseAllResourcesOnCommitOrAbort();
    _stashedResources._yieldedResources->state = TransactionResources::State::FAILED;
    TransactionResources::attachToOpCtx(_opCtx, std::move(_stashedResources._yieldedResources));
}

HandleTransactionResourcesFromStasher::HandleTransactionResourcesFromStasher(
    OperationContext* opCtx, TransactionResourcesStasher* stasher)
    : _opCtx(opCtx), _stasher(stasher) {
    auto stashedResources = stasher->releaseStashedTransactionResources();
    try {
        _originalTransactionResources =
            restoreStashedTransactionResourcesToOperationContext(_opCtx, stashedResources);
    } catch (const DBException&) {
        // If we failed to restore the resources, we stash them back to the stasher.
        stasher->stashTransactionResources(std::move(stashedResources));
        throw;
    }
}

HandleTransactionResourcesFromStasher::~HandleTransactionResourcesFromStasher() {
    if (TransactionResources::isPresent(_opCtx)) {
        if (_stasher) {
            // If we haven't dismissed the resources, we yield and stash them.
            stashTransactionResourcesFromOperationContextAndDontAttachNewOnes(_opCtx, _stasher);
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
                    latestCatalog->checkIfUUIDExistsAtLatest(opCtx, collectionPtr->uuid()));
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
    if (MONGO_unlikely(requestedShardVersion.getIgnoreShardingCatalogUuidMismatch())) {
        return;
    }

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
            // TODO: SERVER-87235: Remove this condition and leave only the tassert that will be
            // introduced in SERVER-88476 below also for transaction and snapshot reads.
            uasserted(ErrorCodes::SnapshotUnavailable,
                      fmt::format("Sharding catalog and local catalog collection uuid do not "
                                  "match. Nss: '{}', sharding uuid: '{}', local uuid: '{}'",
                                  nss.toStringForErrorMsg(),
                                  shardingCollectionDescription.getUUID().toString(),
                                  collectionPtr ? collectionPtr->uuid().toString() : ""));
        } else {
            // TODO: SERVER-88476: reintroduce tassert here similar to the uassert above.
            static logv2::SeveritySuppressor logSeverity{
                Minutes{1}, logv2::LogSeverity::Warning(), logv2::LogSeverity::Debug(5)};
            auto clusterUuidString = shardingCollectionDescription.getUUID().toString();
            auto localUuidString = collectionPtr ? collectionPtr->uuid().toString() : "";
            LOGV2_DEBUG(9087200,
                        logSeverity().toInt(),
                        "Sharding catalog and local catalog collection uuid do not match",
                        "nss"_attr = nss.toStringForErrorMsg(),
                        "sharding uuid"_attr = clusterUuidString,
                        "local uuid"_attr = localUuidString);
        }
    }
}

NamespaceString shard_role_nocheck::resolveNssWithoutAcquisition(OperationContext* opCtx,
                                                                 const DatabaseName& dbName,
                                                                 const UUID& uuid) {
    return CollectionCatalog::get(opCtx)->resolveNamespaceStringFromDBNameAndUUID(
        opCtx, dbName, uuid);
}

boost::optional<NamespaceString> shard_role_nocheck::lookupNssWithoutAcquisition(
    OperationContext* opCtx, const UUID& uuid) {
    return CollectionCatalog::get(opCtx)->lookupNSSByUUID(opCtx, uuid);
}

}  // namespace mongo
