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

#include <exception>
#include <map>

#include "mongo/db/catalog/catalog_helper.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/logv2/log.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

auto getTransactionResources = OperationContext::declareDecoration<
    std::unique_ptr<shard_role_details::TransactionResources>>();

shard_role_details::TransactionResources& getOrMakeTransactionResources(OperationContext* opCtx) {
    auto& readConcern = repl::ReadConcernArgs::get(opCtx);
    auto& optTransactionResources = getTransactionResources(opCtx);
    if (!optTransactionResources) {
        optTransactionResources =
            std::make_unique<shard_role_details::TransactionResources>(readConcern);
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
    const std::vector<CollectionOrViewAcquisitionRequest>& acquisitionRequests) {
    auto catalog = CollectionCatalog::get(opCtx);

    ResolvedNamespaceOrViewAcquisitionRequestsMap sortedAcquisitionRequests;

    for (const auto& ar : acquisitionRequests) {
        if (ar.nss) {
            auto coll = catalog->lookupCollectionByNamespace(opCtx, *ar.nss);
            if (ar.uuid) {
                checkCollectionUUIDMismatch(opCtx, *ar.nss, coll, *ar.uuid);
            }

            AcquisitionPrerequisites prerequisites(
                *ar.nss, ar.uuid, ar.placementConcern, ar.operationType, ar.viewMode);

            ResolvedNamespaceOrViewAcquisitionRequest resolvedAcquisitionRequest{
                prerequisites, nullptr, boost::none};
            sortedAcquisitionRequests.emplace(ResourceId(RESOURCE_COLLECTION, *ar.nss),
                                              std::move(resolvedAcquisitionRequest));
        } else if (ar.dbname) {
            invariant(ar.uuid);
            auto coll = catalog->lookupCollectionByUUID(opCtx, *ar.uuid);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Namespace " << *ar.dbname << ":" << *ar.uuid << " not found",
                    coll);
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Database name mismatch for " << *ar.dbname << ":" << *ar.uuid
                                  << ". Expected: " << *ar.dbname
                                  << " Actual: " << coll->ns().dbName(),
                    coll->ns().dbName() == *ar.dbname);

            if (ar.nss) {
                checkCollectionUUIDMismatch(opCtx, *ar.nss, coll, *ar.uuid);
            }

            AcquisitionPrerequisites prerequisites(
                coll->ns(), coll->uuid(), ar.placementConcern, ar.operationType, ar.viewMode);

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
                           CollectionPtr& coll) {
    invariant(coll);

    // In most cases we expect modifications for system.views to upgrade MODE_IX to MODE_X
    // before taking the lock. One exception is a query by UUID of system.views in a
    // transaction. Usual queries of system.views (by name, not UUID) within a transaction are
    // rejected. However, if the query is by UUID we can't determine whether the namespace is
    // actually system.views until we take the lock here. So we have this one last assertion.
    uassert(6944500,
            "Modifications to system.views must take an exclusive lock",
            !nss.isSystemDotViews() || opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));

    // If we are in a transaction, we cannot yield and wait when there are pending catalog changes.
    // Instead, we must return an error in such situations. We ignore this restriction for the
    // oplog, since it never has pending catalog changes.
    if (opCtx->inMultiDocumentTransaction() && nss != NamespaceString::kRsOplogNamespace) {
        if (auto minSnapshot = coll->getMinimumVisibleSnapshot()) {
            auto mySnapshot =
                opCtx->recoveryUnit()->getPointInTimeReadTimestamp(opCtx).get_value_or(
                    opCtx->recoveryUnit()->getCatalogConflictingTimestamp());

            uassert(
                ErrorCodes::SnapshotUnavailable,
                str::stream() << "Unable to read from a snapshot due to pending collection catalog "
                                 "changes; please retry the operation. Snapshot timestamp is "
                              << mySnapshot.toString() << ". Collection minimum is "
                              << minSnapshot->toString(),
                mySnapshot.isNull() || mySnapshot >= minSnapshot.value());
        }
    }
}

void checkPlacementVersion(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const PlacementConcern& placementConcern) {
    const auto& receivedDbVersion = placementConcern.dbVersion;
    if (receivedDbVersion) {
        DatabaseShardingState::assertMatchingDbVersion(opCtx, nss.db(), *receivedDbVersion);
    }

    const auto& receivedShardVersion = placementConcern.shardVersion;
    if (receivedShardVersion) {
        const auto scopedCSS = CollectionShardingState::acquire(opCtx, nss);
        scopedCSS->checkShardVersionOrThrow(opCtx, *receivedShardVersion);
    }
}

std::variant<CollectionPtr, std::shared_ptr<const ViewDefinition>> acquireLocalCollectionOrView(
    OperationContext* opCtx, const AcquisitionPrerequisites& prerequisites) {
    const auto& nss = prerequisites.nss;

    const auto catalog = CollectionCatalog::get(opCtx);

    if (auto coll = CollectionPtr(catalog->lookupCollectionByNamespace(opCtx, nss))) {
        verifyDbAndCollection(opCtx, nss, coll);
        checkCollectionUUIDMismatch(opCtx, nss, coll, prerequisites.uuid);
        return coll;
    } else if (auto view = catalog->lookupView(opCtx, nss)) {
        checkCollectionUUIDMismatch(opCtx, nss, coll, prerequisites.uuid);
        uassert(ErrorCodes::CommandNotSupportedOnView,
                str::stream() << "Namespace " << nss << " is a view, not a collection",
                prerequisites.viewMode == AcquisitionPrerequisites::kCanBeView);
        return view;
    } else {
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Namespace " << nss << " does not exist",
                !prerequisites.uuid);
        return CollectionPtr();
    }
}

struct SnapshotedServices {
    std::variant<CollectionPtr, std::shared_ptr<const ViewDefinition>> collectionPtrOrView;
    boost::optional<ScopedCollectionDescription> collectionDescription;
    boost::optional<ScopedCollectionFilter> ownershipFilter;
};

SnapshotedServices acquireServicesSnapshot(OperationContext* opCtx,
                                           const AcquisitionPrerequisites& prerequisites) {
    if (stdx::holds_alternative<AcquisitionPrerequisites::PlacementConcernPlaceholder>(
            prerequisites.placementConcern)) {
        return SnapshotedServices{
            acquireLocalCollectionOrView(opCtx, prerequisites), boost::none, boost::none};
    }

    const auto& nss = prerequisites.nss;
    const auto& placementConcern = stdx::get<PlacementConcern>(prerequisites.placementConcern);

    // Check placement version before acquiring the catalog snapshot
    checkPlacementVersion(opCtx, nss, placementConcern);

    auto collOrView = acquireLocalCollectionOrView(opCtx, prerequisites);

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

    // Recheck the placement version after having acquired the catalog snapshot. If the placement
    // version still matches, then the catalog we snapshoted is consistent with the placement
    // concern too.
    checkPlacementVersion(opCtx, nss, placementConcern);

    return SnapshotedServices{
        std::move(collOrView), std::move(collectionDescription), std::move(optOwnershipFilter)};
}

std::vector<ScopedCollectionOrViewAcquisition> acquireResolvedCollectionsOrViewsWithoutTakingLocks(
    OperationContext* opCtx,
    ResolvedNamespaceOrViewAcquisitionRequestsMap sortedAcquisitionRequests) {
    std::vector<ScopedCollectionOrViewAcquisition> acquisitions;
    for (auto& acquisitionRequest : sortedAcquisitionRequests) {
        tassert(7328900,
                "Cannot acquire for write without locks",
                acquisitionRequest.second.prerequisites.operationType ==
                        AcquisitionPrerequisites::kRead ||
                    acquisitionRequest.second.collLock);

        auto& prerequisites = acquisitionRequest.second.prerequisites;
        auto snapshotedServices = acquireServicesSnapshot(opCtx, prerequisites);
        const bool isCollection =
            std::holds_alternative<CollectionPtr>(snapshotedServices.collectionPtrOrView);

        if (isCollection) {
            const auto& collectionPtr =
                std::get<CollectionPtr>(snapshotedServices.collectionPtrOrView);
            invariant(!prerequisites.uuid || prerequisites.uuid == collectionPtr->uuid());
            if (!prerequisites.uuid && collectionPtr) {
                // If the uuid wasn't originally set on the AcquisitionRequest, set it now on the
                // prerequisites so that on restore from yield we can check we are restoring the
                // same instance of the ns.
                prerequisites.uuid = collectionPtr->uuid();
            }

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

}  // namespace

CollectionOrViewAcquisitionRequest CollectionOrViewAcquisitionRequest::fromOpCtx(
    OperationContext* opCtx,
    NamespaceString nss,
    AcquisitionPrerequisites::OperationType operationType,
    AcquisitionPrerequisites::ViewMode viewMode) {
    auto& oss = OperationShardingState::get(opCtx);
    auto& readConcern = repl::ReadConcernArgs::get(opCtx);

    return CollectionOrViewAcquisitionRequest(
        nss,
        {oss.getDbVersion(nss.db()), oss.getShardVersion(nss)},
        readConcern,
        operationType,
        viewMode);
}

CollectionAcquisitionRequest CollectionAcquisitionRequest::fromOpCtx(
    OperationContext* opCtx,
    NamespaceString nss,
    AcquisitionPrerequisites::OperationType operationType) {
    auto& oss = OperationShardingState::get(opCtx);
    auto& readConcern = repl::ReadConcernArgs::get(opCtx);

    return CollectionAcquisitionRequest(
        nss, {oss.getDbVersion(nss.db()), oss.getShardVersion(nss)}, readConcern, operationType);
}

const UUID& ScopedCollectionAcquisition::uuid() const {
    invariant(exists(),
              str::stream() << "Collection " << nss()
                            << " doesn't exist, so its UUID cannot be obtained");
    return *_acquiredCollection.prerequisites.uuid;
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

ScopedCollectionAcquisition::~ScopedCollectionAcquisition() {
    if (_opCtx) {
        const auto& transactionResources = getTransactionResources(_opCtx);
        if (transactionResources) {
            transactionResources->acquiredCollections.remove_if(
                [this](const shard_role_details::AcquiredCollection& txnResourceAcquiredColl) {
                    return &txnResourceAcquiredColl == &(this->_acquiredCollection);
                });
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

std::vector<ScopedCollectionOrViewAcquisition> acquireCollectionsOrViews(
    OperationContext* opCtx,
    std::vector<CollectionOrViewAcquisitionRequest> acquisitionRequests,
    LockMode mode) {
    if (acquisitionRequests.size() == 0) {
        return {};
    }

    // Optimistically populate the nss and uuid parts of the resolved acquisition requests and sort
    // them
    while (true) {
        auto sortedAcquisitionRequests =
            resolveNamespaceOrViewAcquisitionRequests(opCtx, acquisitionRequests);

        // At this point, sortedAcquisitionRequests contains fully resolved (both nss and uuid)
        // namespace or view requests in sorted order. However, there is still no guarantee that the
        // nss <-> uuid mapping won't change from underneath.
        //
        // Lock the collection locks in the sorted order and pass the resolved namespaces to
        // acquireCollectionsOrViewsWithoutTakingLocks. If it throws CollectionUUIDMismatch, we
        // need to start over.
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
                        << dbName << "' vs '" << nss.dbName() << "'",
                    dbName == nss.dbName());

            ar.second.dbLock = dbLock;
            ar.second.collLock.emplace(opCtx, nss, mode);
        }

        try {
            return acquireResolvedCollectionsOrViewsWithoutTakingLocks(
                opCtx, std::move(sortedAcquisitionRequests));
        } catch (const ExceptionFor<ErrorCodes::CollectionUUIDMismatch>&) {
            continue;
        }
    }
}

std::vector<ScopedCollectionOrViewAcquisition> acquireCollectionsOrViewsWithoutTakingLocks(
    OperationContext* opCtx,
    std::initializer_list<CollectionOrViewAcquisitionRequest> acquisitionRequests) {
    while (true) {
        auto sortedAcquisitionRequests =
            resolveNamespaceOrViewAcquisitionRequests(opCtx, acquisitionRequests);

        try {
            return acquireResolvedCollectionsOrViewsWithoutTakingLocks(
                opCtx, std::move(sortedAcquisitionRequests));
        } catch (const ExceptionFor<ErrorCodes::CollectionUUIDMismatch>&) {
            continue;
        }
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

    auto collOrView = acquireLocalCollectionOrView(
        opCtx,
        AcquisitionPrerequisites(nss,
                                 boost::none,
                                 AcquisitionPrerequisites::kLocalCatalogOnlyWithPotentialDataLoss,
                                 AcquisitionPrerequisites::OperationType::kWrite,
                                 AcquisitionPrerequisites::ViewMode::kMustBeCollection));
    invariant(std::holds_alternative<CollectionPtr>(collOrView));

    auto& coll = std::get<CollectionPtr>(collOrView);

    shard_role_details::AcquiredCollection& acquiredCollection = txnResources.addAcquiredCollection(
        {AcquisitionPrerequisites(nss,
                                  coll ? boost::optional<UUID>(coll->uuid()) : boost::none,
                                  AcquisitionPrerequisites::kLocalCatalogOnlyWithPotentialDataLoss,
                                  AcquisitionPrerequisites::OperationType::kWrite,
                                  AcquisitionPrerequisites::ViewMode::kMustBeCollection),
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
    opCtx->recoveryUnit()->onRollback(
        [acquiredCollection = _acquiredCollection](OperationContext* opCtx) mutable {
            // OnRollback, the acquired collection must be set to reference the previously
            // established catalog snapshot
            _updateAcquiredLocalCollection(opCtx, acquiredCollection);
        });
}

ScopedLocalCatalogWriteFence::~ScopedLocalCatalogWriteFence() {
    _updateAcquiredLocalCollection(_opCtx, _acquiredCollection);
}

void ScopedLocalCatalogWriteFence::_updateAcquiredLocalCollection(
    OperationContext* opCtx, shard_role_details::AcquiredCollection* acquiredCollection) {
    try {
        auto collectionOrView =
            acquireLocalCollectionOrView(opCtx, acquiredCollection->prerequisites);
        invariant(std::holds_alternative<CollectionPtr>(collectionOrView));

        acquiredCollection->collectionPtr = std::move(std::get<CollectionPtr>(collectionOrView));
    } catch (...) {
        fassertFailedWithStatus(737661, exceptionToStatus());
    }
}

YieldedTransactionResources::~YieldedTransactionResources() {
    invariant(!_yieldedResources);
}

YieldedTransactionResources::YieldedTransactionResources(
    std::unique_ptr<shard_role_details::TransactionResources>&& yieldedResources)
    : _yieldedResources(std::move(yieldedResources)) {}

YieldedTransactionResources yieldTransactionResourcesFromOperationContext(OperationContext* opCtx) {
    auto& transactionResources = getTransactionResources(opCtx);
    if (!transactionResources) {
        return YieldedTransactionResources();
    }

    invariant(!transactionResources->yielded);

    // Yielding kLocalCatalogOnlyWithPotentialDataLoss acquisitions is not allowed.
    for (auto& acquisition : transactionResources->acquiredCollections) {
        invariant(
            !stdx::holds_alternative<AcquisitionPrerequisites::PlacementConcernPlaceholder>(
                acquisition.prerequisites.placementConcern),
            str::stream() << "Collection " << acquisition.prerequisites.nss
                          << " acquired with special placement concern and cannot be yielded");
    }

    // Yielding view acquisitions is not supported.
    tassert(7300502,
            "Yielding view acquisitions is forbidden",
            transactionResources->acquiredViews.empty());

    invariant(!transactionResources->lockSnapshot);
    transactionResources->lockSnapshot.emplace();
    opCtx->lockState()->saveLockStateAndUnlock(&(*transactionResources->lockSnapshot));

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

    // Reacquire locks.
    if (yieldedResources._yieldedResources->lockSnapshot) {
        opCtx->lockState()->restoreLockState(opCtx,
                                             *yieldedResources._yieldedResources->lockSnapshot);
        yieldedResources._yieldedResources->lockSnapshot.reset();
    }

    // Reacquire service snapshots. Will throw if placement concern can no longer be met.
    for (auto& acquiredCollection : yieldedResources._yieldedResources->acquiredCollections) {
        const auto& prerequisites = acquiredCollection.prerequisites;

        auto uassertCollectionAppearedAfterRestore = [&] {
            uasserted(743870,
                      str::stream()
                          << "Collection " << prerequisites.nss
                          << " appeared after a restore, which violates the semantics of restore");
        };

        if (prerequisites.operationType == AcquisitionPrerequisites::OperationType::kRead) {
            // Just reacquire the CollectionPtr. Reads don't care about placement changes because
            // they have already established a ScopedCollectionFilter that acts as RangePreserver.
            auto collOrView = acquireLocalCollectionOrView(opCtx, prerequisites);

            // We do not support yielding view acquisitions. Therefore it is not possible that upon
            // restore 'acquireLocalCollectionOrView' snapshoted a view -- it would not have met the
            // prerequisite that the collection instance is still the same as the one before
            // yielding.
            invariant(std::holds_alternative<CollectionPtr>(collOrView));
            if (!acquiredCollection.collectionPtr != !std::get<CollectionPtr>(collOrView))
                uassertCollectionAppearedAfterRestore();

            // Update the services snapshot on TransactionResources
            acquiredCollection.collectionPtr = std::move(std::get<CollectionPtr>(collOrView));
        } else {
            auto reacquiredServicesSnapshot = acquireServicesSnapshot(opCtx, prerequisites);

            // We do not support yielding view acquisitions. Therefore it is not possible that upon
            // restore 'acquireLocalCollectionOrView' snapshoted a view -- it would not have met the
            // prerequisite that the collection instance is still the same as the one before
            // yielding.
            invariant(std::holds_alternative<CollectionPtr>(
                reacquiredServicesSnapshot.collectionPtrOrView));
            if (!acquiredCollection.collectionPtr !=
                !std::get<CollectionPtr>(reacquiredServicesSnapshot.collectionPtrOrView))
                uassertCollectionAppearedAfterRestore();

            // Update the services snapshot on TransactionResources
            acquiredCollection.collectionPtr =
                std::move(std::get<CollectionPtr>(reacquiredServicesSnapshot.collectionPtrOrView));
            acquiredCollection.collectionDescription =
                std::move(reacquiredServicesSnapshot.collectionDescription);
            acquiredCollection.ownershipFilter =
                std::move(reacquiredServicesSnapshot.ownershipFilter);
        }
    }

    // Restore TransactionsResource on opCtx.
    yieldedResources._yieldedResources->yielded = false;
    getTransactionResources(opCtx) = std::move(yieldedResources)._yieldedResources;
    scopeGuard.dismiss();
}

}  // namespace mongo
