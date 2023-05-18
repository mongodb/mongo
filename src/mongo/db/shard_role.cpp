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

#include <boost/utility/in_place_factory.hpp>
#include <exception>
#include <map>

#include "mongo/db/catalog/catalog_helper.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/storage/capped_snapshots.h"
#include "mongo/logv2/log.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

auto getTransactionResources = OperationContext::declareDecoration<
    std::unique_ptr<shard_role_details::TransactionResources>>();

shard_role_details::TransactionResources& getOrMakeTransactionResources(OperationContext* opCtx) {
    auto& optTransactionResources = getTransactionResources(opCtx);
    if (!optTransactionResources) {
        optTransactionResources = std::make_unique<shard_role_details::TransactionResources>();
    }

    return *optTransactionResources;
}

struct ResolvedNamespaceOrViewAcquisitionRequest {
    // Populated in the first phase of collection(s) acquisition
    AcquisitionPrerequisites prerequisites;

    // Populated optionally in the second phase of collection(s) acquisition
    std::shared_ptr<Lock::DBLock> dbLock;
    boost::optional<Lock::CollectionLock> collLock;
};

using ResolvedNamespaceOrViewAcquisitionRequestsMap =
    std::map<ResourceId, ResolvedNamespaceOrViewAcquisitionRequest>;

/**
 * Takes the input acquisitions, populates the NSS and UUID parts and returns a list, sorted by NSS,
 * suitable for a defined lock acquisition order.
 */
ResolvedNamespaceOrViewAcquisitionRequestsMap resolveNamespaceOrViewAcquisitionRequests(
    OperationContext* opCtx,
    const CollectionCatalog& catalog,
    const std::vector<CollectionOrViewAcquisitionRequest>& acquisitionRequests) {

    ResolvedNamespaceOrViewAcquisitionRequestsMap sortedAcquisitionRequests;

    for (const auto& ar : acquisitionRequests) {
        if (ar.nss) {
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Namespace " << ar.nss->toStringForErrorMsg()
                                  << "is not a valid collection name",
                    ar.nss->isValid());
            auto coll = catalog.lookupCollectionByNamespace(opCtx, *ar.nss);
            checkCollectionUUIDMismatch(opCtx, *ar.nss, coll, ar.uuid);

            AcquisitionPrerequisites prerequisites(*ar.nss,
                                                   ar.uuid,
                                                   ar.readConcern,
                                                   ar.placementConcern,
                                                   ar.operationType,
                                                   ar.viewMode);

            ResolvedNamespaceOrViewAcquisitionRequest resolvedAcquisitionRequest{
                prerequisites, nullptr, boost::none};
            sortedAcquisitionRequests.emplace(ResourceId(RESOURCE_COLLECTION, *ar.nss),
                                              std::move(resolvedAcquisitionRequest));
        } else if (ar.dbname) {
            invariant(ar.uuid);
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid db name " << ar.dbname->toStringForErrorMsg(),
                    NamespaceString::validDBName(*ar.dbname,
                                                 NamespaceString::DollarInDbNameBehavior::Allow));
            auto coll = catalog.lookupCollectionByUUID(opCtx, *ar.uuid);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Namespace " << (*ar.dbname).toStringForErrorMsg() << ":"
                                  << *ar.uuid << " not found",
                    coll);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Database name mismatch for "
                                  << (*ar.dbname).toStringForErrorMsg() << ":" << *ar.uuid
                                  << ". Expected: " << (*ar.dbname).toStringForErrorMsg()
                                  << " Actual: " << coll->ns().dbName().toStringForErrorMsg(),
                    coll->ns().dbName() == *ar.dbname);

            if (ar.nss) {
                checkCollectionUUIDMismatch(opCtx, *ar.nss, coll, *ar.uuid);
            }

            AcquisitionPrerequisites prerequisites(coll->ns(),
                                                   coll->uuid(),
                                                   ar.readConcern,
                                                   ar.placementConcern,
                                                   ar.operationType,
                                                   ar.viewMode);

            ResolvedNamespaceOrViewAcquisitionRequest resolvedAcquisitionRequest{
                prerequisites, nullptr, boost::none};

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
    // actually system.views until we take the lock here. So we have this one last assertion.
    uassert(6944500,
            "Modifications to system.views must take an exclusive lock",
            !nss.isSystemDotViews() || opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));

    // Verify that we are using the latest instance if we intend to perform writes.
    if (operationType == AcquisitionPrerequisites::OperationType::kWrite) {
        auto latest = CollectionCatalog::latest(opCtx);
        if (!latest->containsCollection(opCtx, coll.get())) {
            throwWriteConflictException(str::stream() << "Unable to write to collection '"
                                                      << coll->ns().toStringForErrorMsg()
                                                      << "' due to catalog changes; please "
                                                         "retry the operation");
        }
        if (opCtx->recoveryUnit()->isActive()) {
            const auto mySnapshot = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);
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
        DatabaseShardingState::assertMatchingDbVersion(opCtx, nss.dbName(), *receivedDbVersion);
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
    // TODO SERVER-76415 fix secondary reads if shouldReadAtLastApplied

    auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);
    auto coll = CollectionPtr(catalog.establishConsistentCollection(
        opCtx, NamespaceStringOrUUID(prerequisites.nss), readTimestamp));
    checkCollectionUUIDMismatch(opCtx, nss, coll, prerequisites.uuid);

    if (coll) {
        verifyDbAndCollection(opCtx, nss, coll, prerequisites.operationType);
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
    if (stdx::holds_alternative<AcquisitionPrerequisites::PlacementConcernPlaceholder>(
            prerequisites.placementConcern)) {
        return SnapshotedServices{
            acquireLocalCollectionOrView(opCtx, catalog, prerequisites), boost::none, boost::none};
    }

    const auto& placementConcern = stdx::get<PlacementConcern>(prerequisites.placementConcern);

    auto collOrView = acquireLocalCollectionOrView(opCtx, catalog, prerequisites);
    const auto& nss = prerequisites.nss;

    const bool isPlacementConcernVersioned =
        placementConcern.dbVersion || placementConcern.shardVersion;

    const auto scopedCSS = CollectionShardingState::acquire(opCtx, nss);
    auto collectionDescription =
        scopedCSS->getCollectionDescription(opCtx, isPlacementConcernVersioned);

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
    if (std::holds_alternative<CollectionPtr>(collOrView) && collectionDescription.isSharded()) {
        std::get<CollectionPtr>(collOrView)
            .setShardKeyPattern(collectionDescription.getKeyPattern());
    }

    return SnapshotedServices{
        std::move(collOrView), std::move(collectionDescription), std::move(optOwnershipFilter)};
}

std::vector<ScopedCollectionOrViewAcquisition> acquireResolvedCollectionsOrViewsWithoutTakingLocks(
    OperationContext* opCtx,
    const CollectionCatalog& catalog,
    ResolvedNamespaceOrViewAcquisitionRequestsMap sortedAcquisitionRequests) {
    std::vector<ScopedCollectionOrViewAcquisition> acquisitions;
    for (auto& acquisitionRequest : sortedAcquisitionRequests) {
        tassert(7328900,
                "Cannot acquire for write without locks",
                acquisitionRequest.second.prerequisites.operationType ==
                        AcquisitionPrerequisites::kRead ||
                    acquisitionRequest.second.collLock);

        auto& prerequisites = acquisitionRequest.second.prerequisites;
        auto snapshotedServices = acquireServicesSnapshot(opCtx, catalog, prerequisites);
        const bool isCollection =
            std::holds_alternative<CollectionPtr>(snapshotedServices.collectionPtrOrView);

        if (isCollection) {
            shard_role_details::AcquiredCollection& acquiredCollection =
                getOrMakeTransactionResources(opCtx).addAcquiredCollection(
                    {prerequisites,
                     std::move(acquisitionRequest.second.dbLock),
                     std::move(acquisitionRequest.second.collLock),
                     std::move(snapshotedServices.collectionDescription),
                     std::move(snapshotedServices.ownershipFilter),
                     std::move(std::get<CollectionPtr>(snapshotedServices.collectionPtrOrView))});

            ScopedCollectionAcquisition scopedAcquisition(opCtx, acquiredCollection);
            acquisitions.emplace_back(std::move(scopedAcquisition));
        } else {
            // It's a view.
            const shard_role_details::AcquiredView& acquiredView =
                getOrMakeTransactionResources(opCtx).addAcquiredView(
                    {prerequisites,
                     std::move(acquisitionRequest.second.dbLock),
                     std::move(acquisitionRequest.second.collLock),
                     std::move(std::get<std::shared_ptr<const ViewDefinition>>(
                         snapshotedServices.collectionPtrOrView))});

            ScopedViewAcquisition scopedAcquisition(opCtx, acquiredView);
            acquisitions.emplace_back(std::move(scopedAcquisition));
        }
    }

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
        if (ar.nss) {
            requests.emplace_back(*ar.nss);
        } else {
            requests.emplace_back(*ar.dbname, *ar.uuid);
        }
    }
    return requests;
}

void checkShardingPlacement(
    OperationContext* opCtx,
    const std::vector<CollectionOrViewAcquisitionRequest>& acquisitionRequests) {
    for (const auto& ar : acquisitionRequests) {
        // We only have to check placement for collections that come from a router, which
        // will have the namespace set.
        if (ar.nss) {
            checkPlacementVersion(opCtx, *ar.nss, ar.placementConcern);
        }
    }
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

}  // namespace

CollectionOrViewAcquisitionRequest CollectionOrViewAcquisitionRequest::fromOpCtx(
    OperationContext* opCtx,
    NamespaceString nss,
    AcquisitionPrerequisites::OperationType operationType,
    AcquisitionPrerequisites::ViewMode viewMode,
    boost::optional<UUID> expectedUUID) {
    auto& oss = OperationShardingState::get(opCtx);
    auto& readConcern = repl::ReadConcernArgs::get(opCtx);

    return CollectionOrViewAcquisitionRequest(
        nss,
        expectedUUID,
        {oss.getDbVersion(nss.db()), oss.getShardVersion(nss)},
        readConcern,
        operationType,
        viewMode);
}

CollectionAcquisitionRequest CollectionAcquisitionRequest::fromOpCtx(
    OperationContext* opCtx,
    NamespaceString nss,
    AcquisitionPrerequisites::OperationType operationType,
    boost::optional<UUID> expectedUUID) {
    auto& oss = OperationShardingState::get(opCtx);
    auto& readConcern = repl::ReadConcernArgs::get(opCtx);

    return CollectionAcquisitionRequest(nss,
                                        expectedUUID,
                                        {oss.getDbVersion(nss.db()), oss.getShardVersion(nss)},
                                        readConcern,
                                        operationType);
}

CollectionAcquisitionRequest CollectionAcquisitionRequest::fromOpCtx(
    OperationContext* opCtx,
    NamespaceStringOrUUID nssOrUUID,
    AcquisitionPrerequisites::OperationType operationType) {
    auto& oss = OperationShardingState::get(opCtx);
    auto& readConcern = repl::ReadConcernArgs::get(opCtx);

    // Acquisitions by uuid cannot possibly have a corresponding ShardVersion attached.
    PlacementConcern placementConcern = nssOrUUID.nss()
        ? PlacementConcern{oss.getDbVersion(nssOrUUID.dbName().db()),
                           oss.getShardVersion(*nssOrUUID.nss())}
        : PlacementConcern{oss.getDbVersion(nssOrUUID.dbName().db()), {}};

    return CollectionAcquisitionRequest(nssOrUUID, placementConcern, readConcern, operationType);
}

UUID ScopedCollectionAcquisition::uuid() const {
    invariant(exists(),
              str::stream() << "Collection " << nss().toStringForErrorMsg()
                            << " doesn't exist, so its UUID cannot be obtained");
    return _acquiredCollection.collectionPtr->uuid();
}

const ScopedCollectionDescription& ScopedCollectionAcquisition::getShardingDescription() const {
    // The collectionDescription will only not be set if the caller as acquired the acquisition
    // using the kLocalCatalogOnlyWithPotentialDataLoss placement concern
    invariant(_acquiredCollection.collectionDescription);
    return *_acquiredCollection.collectionDescription;
}

const boost::optional<ScopedCollectionFilter>& ScopedCollectionAcquisition::getShardingFilter()
    const {
    // The collectionDescription will only not be set if the caller as acquired the acquisition
    // using the kLocalCatalogOnlyWithPotentialDataLoss placement concern
    invariant(_acquiredCollection.collectionDescription);
    return _acquiredCollection.ownershipFilter;
}

const CollectionPtr& ScopedCollectionAcquisition::getCollectionPtr() const {
    tassert(ErrorCodes::InternalError,
            "Collection acquisition has been invalidated",
            !_acquiredCollection.invalidated);
    return _acquiredCollection.collectionPtr;
}

ScopedCollectionAcquisition::~ScopedCollectionAcquisition() {
    if (_opCtx) {
        const auto& transactionResources = getTransactionResources(_opCtx);
        if (transactionResources) {
            transactionResources->acquiredCollections.remove_if(
                [this](const shard_role_details::AcquiredCollection& txnResourceAcquiredColl) {
                    return &txnResourceAcquiredColl == &(this->_acquiredCollection);
                });
            if (transactionResources->acquiredCollections.empty() &&
                transactionResources->acquiredViews.empty()) {
                transactionResources->releaseAllResourcesOnCommitOrAbort();
            }
        }
    }
}

ScopedViewAcquisition::~ScopedViewAcquisition() {
    if (_opCtx) {
        const auto& transactionResources = getTransactionResources(_opCtx);
        if (transactionResources) {
            transactionResources->acquiredViews.remove_if(
                [this](const shard_role_details::AcquiredView& txnResourceAcquiredView) {
                    return &txnResourceAcquiredView == &(this->_acquiredView);
                });
            if (transactionResources->acquiredCollections.empty() &&
                transactionResources->acquiredViews.empty()) {
                transactionResources->releaseAllResourcesOnCommitOrAbort();
            }
        }
    }
}

ScopedCollectionAcquisition acquireCollection(OperationContext* opCtx,
                                              CollectionAcquisitionRequest acquisitionRequest,
                                              LockMode mode) {
    return std::get<ScopedCollectionAcquisition>(
        acquireCollectionOrView(opCtx, acquisitionRequest, mode));
}

std::vector<ScopedCollectionAcquisition> acquireCollections(
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

    // Transform the acquisitions to ScopedCollectionAcquisitions
    std::vector<ScopedCollectionAcquisition> collectionAcquisitions;
    for (auto& acquisition : acquisitions) {
        // It must be a collection, because that's what the acquisition request stated.
        invariant(std::holds_alternative<ScopedCollectionAcquisition>(acquisition));

        collectionAcquisitions.emplace_back(
            std::move(std::get<ScopedCollectionAcquisition>(acquisition)));
    }
    return collectionAcquisitions;
}

ScopedCollectionOrViewAcquisition acquireCollectionOrView(
    OperationContext* opCtx, CollectionOrViewAcquisitionRequest acquisitionRequest, LockMode mode) {
    auto acquisition = acquireCollectionsOrViews(opCtx, {std::move(acquisitionRequest)}, mode);
    invariant(acquisition.size() == 1);
    return std::move(acquisition.front());
}
namespace shard_role_details {

void SnapshotAttempt::snapshotInitialState() {
    // The read source used can change depending on replication state, so we must fetch the repl
    // state beforehand, to compare with afterwards.
    _replTermBeforeSnapshot = repl::ReplicationCoordinator::get(_opCtx)->getTerm();

    _catalogBeforeSnapshot = CollectionCatalog::get(_opCtx);
}

void SnapshotAttempt::openStorageSnapshot() {
    invariant(_catalogBeforeSnapshot && _replTermBeforeSnapshot);
    if (_opCtx->recoveryUnit()->isActive()) {
        // We already have an open storage snapshot, nothing to do.
        return;
    }

    for (auto& nssOrUUID : _acquisitionRequests) {
        establishCappedSnapshotIfNeeded(_opCtx, *_catalogBeforeSnapshot, nssOrUUID);
    }

    // TODO SERVER-76415 call preallocateSnapshotForOplogRead() when reading from the oplog.
    _opCtx->recoveryUnit()->preallocateSnapshot();
    _openedSnapshot = true;
}

std::shared_ptr<const CollectionCatalog> SnapshotAttempt::getConsistentCatalog() {
    invariant(_catalogBeforeSnapshot && _replTermBeforeSnapshot &&
              _opCtx->recoveryUnit()->isActive());
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

    // We can't abandon the snapshot if we are in a WUOW.
    if (_openedSnapshot && !_opCtx->lockState()->inAWriteUnitOfWork()) {
        _opCtx->recoveryUnit()->abandonSnapshot();
    }
    CurOp::get(_opCtx)->yielded();
}

ResolvedNamespaceOrViewAcquisitionRequestsMap generateSortedAcquisitionRequests(
    OperationContext* opCtx,
    const CollectionCatalog& catalog,
    const std::vector<CollectionOrViewAcquisitionRequest>& acquisitionRequests) {
    ResolvedNamespaceOrViewAcquisitionRequestsMap sortedAcquisitionRequests;

    auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx);

    int counter = 0;
    for (const auto& ar : acquisitionRequests) {
        NamespaceStringOrUUID nssOrUUID =
            ar.nss ? NamespaceStringOrUUID(*ar.nss) : NamespaceStringOrUUID(*ar.dbname, *ar.uuid);
        uassertStatusOK(nssOrUUID.isNssValid());
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid db name " << ar.dbname->toStringForErrorMsg(),
                NamespaceString::validDBName(nssOrUUID.dbName(),
                                             NamespaceString::DollarInDbNameBehavior::Allow));

        auto coll =
            CollectionPtr(catalog.establishConsistentCollection(opCtx, nssOrUUID, readTimestamp));

        // If we are acquiring by UUID and no collection matches, raise NamespaceNotFound.
        if (!ar.nss) {
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Namespace " << (*ar.dbname).toStringForErrorMsg() << ":"
                                  << *ar.uuid << " not found",
                    coll);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Database name mismatch for "
                                  << (*ar.dbname).toStringForErrorMsg() << ":" << *ar.uuid
                                  << ". Expected: " << (*ar.dbname).toStringForErrorMsg()
                                  << " Actual: " << coll->ns().dbName().toStringForErrorMsg(),
                    coll->ns().dbName() == *ar.dbname);
        }

        const auto& nss = ar.nss ? *ar.nss : coll->ns();
        AcquisitionPrerequisites prerequisites(
            nss, ar.uuid, ar.readConcern, ar.placementConcern, ar.operationType, ar.viewMode);

        ResolvedNamespaceOrViewAcquisitionRequest resolvedAcquisitionRequest{
            prerequisites, nullptr, boost::none};
        // We don't care about ordering in this case, use a mock ResourceId as the key.
        sortedAcquisitionRequests.emplace(ResourceId(RESOURCE_COLLECTION, counter++),
                                          std::move(resolvedAcquisitionRequest));
    }
    return sortedAcquisitionRequests;
}
}  // namespace shard_role_details

std::vector<ScopedCollectionOrViewAcquisition> acquireCollectionsOrViews(
    OperationContext* opCtx,
    std::vector<CollectionOrViewAcquisitionRequest> acquisitionRequests,
    LockMode mode) {
    if (acquisitionRequests.size() == 0) {
        return {};
    }

    // We shouldn't have an open snapshot unless we are in a multi document transaction or a
    // previous acquisition opened and stashed it already.
    // TODO enable invariant when everything uses acquisitions.

    bool inMultiDocumentTransaction = opCtx->inMultiDocumentTransaction();

    while (true) {
        // Optimistically populate the nss and uuid parts of the resolved acquisition requests and
        // sort them
        auto sortedAcquisitionRequests = resolveNamespaceOrViewAcquisitionRequests(
            opCtx, *CollectionCatalog::get(opCtx), acquisitionRequests);

        // At this point, sortedAcquisitionRequests contains fully resolved (both nss and uuid)
        // namespace or view requests in sorted order. However, there is still no guarantee that the
        // nss <-> uuid mapping won't change from underneath.
        //
        // Lock the collection locks in the sorted order and recheck the UUIDS. If it throws
        // CollectionUUIDMismatch, we need to start over.
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

        const auto dbLock =
            std::make_shared<Lock::DBLock>(opCtx,
                                           dbName,
                                           isSharedLockMode(mode) ? MODE_IS : MODE_IX,
                                           Date_t::max(),
                                           dbLockOptions);

        for (auto& ar : sortedAcquisitionRequests) {
            const auto& nss = ar.second.prerequisites.nss;
            tassert(7300400,
                    str::stream()
                        << "Cannot acquire locks for collections across different databases ('"
                        << dbName.toStringForErrorMsg() << "' vs '"
                        << nss.dbName().toStringForErrorMsg() << "'",
                    dbName == nss.dbName());

            ar.second.dbLock = dbLock;
            ar.second.collLock.emplace(opCtx, nss, mode);
        }

        // Wait for a configured amount of time after acquiring locks if the failpoint is
        // enabled
        catalog_helper::setAutoGetCollectionWaitFailpointExecute([&](const BSONObj& data) {
            sleepFor(Milliseconds(data["waitForMillis"].numberInt()));
        });

        // Recheck UUIDs. For writes in multi document transactions we need to use the latest
        // catalog and throw a WW conflict if UUIDs don't match. In all other cases we use the
        // current catalog and retry the loop.
        const auto& currentCatalog = CollectionCatalog::get(opCtx);
        const auto& latestCatalog = CollectionCatalog::latest(opCtx);
        for (auto& ar : sortedAcquisitionRequests) {
            const auto& prerequisites = ar.second.prerequisites;
            auto changedUUID = [&](auto& catalog) {
                const auto coll = catalog->lookupCollectionByNamespace(opCtx, prerequisites.nss);
                if (prerequisites.uuid && (!coll || coll->uuid() != prerequisites.uuid)) {
                    return true;
                }
                return false;
            };
            bool writeInMultiDocumentTransaction = inMultiDocumentTransaction &&
                prerequisites.operationType == AcquisitionPrerequisites::OperationType::kWrite;
            if (changedUUID(writeInMultiDocumentTransaction ? latestCatalog : currentCatalog)) {
                if (writeInMultiDocumentTransaction) {
                    throwWriteConflictException(
                        str::stream() << "Unable to write to collection '" << prerequisites.nss
                                      << "' due to catalog changes; please retry the operation");
                } else {
                    // Retry optimistic resolution.
                    continue;
                }
            }
        }

        checkShardingPlacement(opCtx, acquisitionRequests);

        // TODO SERVER-76901 open consistent storage snapshot.
        auto catalog = CollectionCatalog::get(opCtx);
        return acquireResolvedCollectionsOrViewsWithoutTakingLocks(
            opCtx, *catalog, std::move(sortedAcquisitionRequests));
    }
}

std::vector<ScopedCollectionOrViewAcquisition> acquireCollectionsOrViewsWithoutTakingLocks(
    OperationContext* opCtx, std::vector<CollectionOrViewAcquisitionRequest> acquisitionRequests) {
    if (acquisitionRequests.size() == 0) {
        return {};
    }

    // We shouldn't have an open snapshot unless a previous lock-free acquisition opened and stashed
    // it already.
    invariant(!opCtx->recoveryUnit()->isActive() || opCtx->isLockFreeReadsOp());

    // TODO SERVER-76415 take global lock.

    // Make sure the sharding placement is correct before opening the storage snapshot, we will
    // check it again after opening it to make sure it is consistent. This is specially important in
    // secondaries since they can be lagging and might not be aware of the latests routing changes.
    checkShardingPlacement(opCtx, acquisitionRequests);

    // Open a consistent catalog snapshot if needed.
    bool openSnapshot = !opCtx->recoveryUnit()->isActive();
    auto catalog = openSnapshot ? stashConsistentCatalog(opCtx, acquisitionRequests)
                                : CollectionCatalog::get(opCtx);

    try {
        // Second sharding placement check.
        checkShardingPlacement(opCtx, acquisitionRequests);

        auto sortedAcquisitionRequests = shard_role_details::generateSortedAcquisitionRequests(
            opCtx, *catalog, acquisitionRequests);
        return acquireResolvedCollectionsOrViewsWithoutTakingLocks(
            opCtx, *catalog, std::move(sortedAcquisitionRequests));
    } catch (...) {
        if (openSnapshot && !opCtx->lockState()->inAWriteUnitOfWork())
            opCtx->recoveryUnit()->abandonSnapshot();
        throw;
    }
}

ScopedCollectionAcquisition acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(
    OperationContext* opCtx, const NamespaceString& nss, LockMode mode) {
    invariant(!OperationShardingState::isComingFromRouter(opCtx));

    auto& txnResources = getOrMakeTransactionResources(opCtx);
    txnResources.assertNoAcquiredCollections();

    auto dbLock = std::make_shared<Lock::DBLock>(
        opCtx, nss.dbName(), isSharedLockMode(mode) ? MODE_IS : MODE_IX);
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
    invariant(std::holds_alternative<CollectionPtr>(collOrView));

    auto& coll = std::get<CollectionPtr>(collOrView);
    if (coll)
        prerequisites.uuid = boost::optional<UUID>(coll->uuid());

    shard_role_details::AcquiredCollection& acquiredCollection =
        txnResources.addAcquiredCollection({prerequisites,
                                            std::move(dbLock),
                                            std::move(collLock),
                                            boost::none,
                                            boost::none,
                                            std::move(coll)});

    return ScopedCollectionAcquisition(opCtx, acquiredCollection);
}

ScopedLocalCatalogWriteFence::ScopedLocalCatalogWriteFence(OperationContext* opCtx,
                                                           ScopedCollectionAcquisition* acquisition)
    : _opCtx(opCtx), _acquiredCollection(&acquisition->_acquiredCollection) {
    // Clear the collectionPtr from the acquisition to indicate that it should not be used until the
    // caller is done with the DDL modifications
    _acquiredCollection->collectionPtr = CollectionPtr();

    // OnCommit, there is nothing to do because the caller is not allowed to use the collection in
    // the scope of the ScopedLocalCatalogWriteFence and the destructor will take care of updating
    // the acquisition to point to the latest changed value.
    std::weak_ptr<shard_role_details::AcquiredCollection::SharedImpl> sharedImplWeakPtr =
        _acquiredCollection->sharedImpl;
    opCtx->recoveryUnit()->onRollback(
        [acquiredCollection = _acquiredCollection,
         sharedImplWeakPtr = sharedImplWeakPtr](OperationContext* opCtx) mutable {
            // OnRollback, the acquired collection must be set to reference the previously
            // established catalog snapshot
            if (!sharedImplWeakPtr.expired()) {
                _updateAcquiredLocalCollection(opCtx, acquiredCollection);
            }
        });
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
    std::unique_ptr<shard_role_details::TransactionResources>&& yieldedResources)
    : _yieldedResources(std::move(yieldedResources)) {}

void YieldedTransactionResources::dispose() {
    if (_yieldedResources) {
        _yieldedResources->releaseAllResourcesOnCommitOrAbort();
        _yieldedResources.reset();
    }
}

boost::optional<YieldedTransactionResources> yieldTransactionResourcesFromOperationContext(
    OperationContext* opCtx) {
    auto& transactionResources = getTransactionResources(opCtx);
    if (!transactionResources) {
        return YieldedTransactionResources();
    }

    invariant(!transactionResources->yielded);


    for (auto& acquisition : transactionResources->acquiredCollections) {
        // Yielding kLocalCatalogOnlyWithPotentialDataLoss acquisitions is not allowed.
        invariant(
            !stdx::holds_alternative<AcquisitionPrerequisites::PlacementConcernPlaceholder>(
                acquisition.prerequisites.placementConcern),
            str::stream() << "Collection " << acquisition.prerequisites.nss.toStringForErrorMsg()
                          << " acquired with special placement concern and cannot be yielded");

        // If the uuid wasn't originally set on the prerequisites, set it now so that on restore
        // from yield we can check we are restoring the same instance of the ns.
        if (!acquisition.prerequisites.uuid && acquisition.collectionPtr) {
            acquisition.prerequisites.uuid = acquisition.collectionPtr->uuid();
        }
    }

    // Yielding view acquisitions is not supported.
    tassert(7300502,
            "Yielding view acquisitions is forbidden",
            transactionResources->acquiredViews.empty());

    invariant(!transactionResources->yieldedLocker);
    Locker::LockSnapshot lockSnapshot;
    opCtx->lockState()->saveLockStateAndUnlock(&lockSnapshot);

    transactionResources->yieldedLocker.emplace(std::move(lockSnapshot));
    transactionResources->yielded = true;

    return YieldedTransactionResources(std::move(transactionResources));
}

void restoreTransactionResourcesToOperationContext(OperationContext* opCtx,
                                                   YieldedTransactionResources&& yieldedResources) {
    if (!yieldedResources._yieldedResources) {
        // Nothing to restore.
        return;
    }

    // On failure to restore, release the yielded resources.
    ScopeGuard scopeGuard([&] {
        yieldedResources._yieldedResources->releaseAllResourcesOnCommitOrAbort();
        yieldedResources._yieldedResources.reset();
    });

    auto restoreFn = [&]() {
        // Reacquire locks.
        if (yieldedResources._yieldedResources->yieldedLocker) {
            opCtx->lockState()->restoreLockState(
                opCtx, *yieldedResources._yieldedResources->yieldedLocker);
            yieldedResources._yieldedResources->yieldedLocker.reset();
        }

        // Reestablish a consistent catalog snapshot (multi document transactions don't yield).
        auto requests =
            toNamespaceStringOrUUIDs(yieldedResources._yieldedResources->acquiredCollections);
        auto catalog = getConsistentCatalogAndSnapshot(opCtx, requests);

        // Reacquire service snapshots. Will throw if placement concern can no longer be met.
        for (auto& acquiredCollection : yieldedResources._yieldedResources->acquiredCollections) {
            const auto& prerequisites = acquiredCollection.prerequisites;

            auto uassertCollectionAppearedAfterRestore = [&] {
                uasserted(
                    743870,
                    str::stream()
                        << "Collection " << prerequisites.nss.toStringForErrorMsg()
                        << " appeared after a restore, which violates the semantics of restore");
            };

            if (prerequisites.operationType == AcquisitionPrerequisites::OperationType::kRead) {
                // Just reacquire the CollectionPtr. Reads don't care about placement changes
                // because they have already established a ScopedCollectionFilter that acts as
                // RangePreserver.
                auto collOrView = acquireLocalCollectionOrView(opCtx, *catalog, prerequisites);

                // We do not support yielding view acquisitions. Therefore it is not possible that
                // upon restore 'acquireLocalCollectionOrView' snapshoted a view -- it would not
                // have met the prerequisite that the collection instance is still the same as the
                // one before yielding.
                invariant(std::holds_alternative<CollectionPtr>(collOrView));
                if (!acquiredCollection.collectionPtr != !std::get<CollectionPtr>(collOrView)) {
                    uassertCollectionAppearedAfterRestore();
                }

                // Update the services snapshot on TransactionResources
                acquiredCollection.collectionPtr = std::move(std::get<CollectionPtr>(collOrView));
            } else {
                auto reacquiredServicesSnapshot =
                    acquireServicesSnapshot(opCtx, *catalog, prerequisites);

                // We do not support yielding view acquisitions. Therefore it is not possible that
                // upon restore 'acquireLocalCollectionOrView' snapshoted a view -- it would not
                // have met the prerequisite that the collection instance is still the same as the
                // one before yielding.
                invariant(std::holds_alternative<CollectionPtr>(
                    reacquiredServicesSnapshot.collectionPtrOrView));
                if (!acquiredCollection.collectionPtr !=
                    !std::get<CollectionPtr>(reacquiredServicesSnapshot.collectionPtrOrView)) {
                    uassertCollectionAppearedAfterRestore();
                }

                // Update the services snapshot on TransactionResources
                acquiredCollection.collectionPtr = std::move(
                    std::get<CollectionPtr>(reacquiredServicesSnapshot.collectionPtrOrView));
                acquiredCollection.collectionDescription =
                    std::move(reacquiredServicesSnapshot.collectionDescription);
                acquiredCollection.ownershipFilter =
                    std::move(reacquiredServicesSnapshot.ownershipFilter);
            }
            // TODO: This will be removed when we no longer snapshot sharding state on
            // CollectionPtr.
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
                    yieldedResources._yieldedResources->yieldedLocker.emplace();
                    opCtx->recoveryUnit()->abandonSnapshot();
                    opCtx->lockState()->saveLockStateAndUnlock(
                        yieldedResources._yieldedResources->yieldedLocker.get_ptr());
                    // Wait for the critical section to finish.
                    OperationShardingState::waitForCriticalSectionToComplete(
                        opCtx, *ex->getCriticalSectionSignal())
                        .ignore();
                    // Try again to restore.
                    continue;
                }
                throw;
            }
        }
    }();

    // Restore TransactionsResource on opCtx.
    yieldedResources._yieldedResources->yielded = false;
    getTransactionResources(opCtx) = std::move(yieldedResources)._yieldedResources;
    if (!opCtx->inMultiDocumentTransaction()) {
        CollectionCatalog::stash(opCtx, catalog);
    }
    scopeGuard.dismiss();
}

}  // namespace mongo
