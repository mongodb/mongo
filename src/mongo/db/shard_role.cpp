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
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

auto getTransactionResources = OperationContext::declareDecoration<
    boost::optional<shard_role_details::TransactionResources>>();

shard_role_details::TransactionResources& getOrMakeTransactionResources(OperationContext* opCtx) {
    auto& readConcern = repl::ReadConcernArgs::get(opCtx);
    auto& optTransactionResources = getTransactionResources(opCtx);
    if (!optTransactionResources) {
        optTransactionResources.emplace(readConcern);
    }

    return *optTransactionResources;
}

NamespaceOrViewAcquisitionRequest::PlacementConcern getPlacementConcernFromOSS(
    OperationContext* opCtx, const NamespaceString& nss) {
    const auto optDbVersion = OperationShardingState::get(opCtx).getDbVersion(nss.db());
    const auto optShardVersion = OperationShardingState::get(opCtx).getShardVersion(nss);
    return {optDbVersion, optShardVersion};
}

struct ResolvedNamespaceOrViewAcquisitionRequest {
    // Populated in the first phase of collection(s) acquisition
    NamespaceString nss;

    boost::optional<UUID> uuid;

    NamespaceOrViewAcquisitionRequest::PlacementConcern tsPlacement;
    NamespaceOrViewAcquisitionRequest::ViewMode viewMode;

    // Populated optionally in the second phase of collection(s) acquisition
    boost::optional<Lock::DBLock> dbLock;
    boost::optional<Lock::CollectionLock> collLock;

    boost::optional<ScopedCollectionFilter> filter;
};

using ResolvedNamespaceOrViewAcquisitionRequestsMap =
    std::map<ResourceId, ResolvedNamespaceOrViewAcquisitionRequest>;

ResolvedNamespaceOrViewAcquisitionRequestsMap resolveNamespaceOrViewAcquisitionRequests(
    OperationContext* opCtx,
    std::initializer_list<NamespaceOrViewAcquisitionRequest> acquisitionRequests) {
    auto catalog = CollectionCatalog::get(opCtx);

    ResolvedNamespaceOrViewAcquisitionRequestsMap sortedAcquisitionRequests;

    for (const auto& ar : acquisitionRequests) {
        if (ar.nss) {
            auto coll = catalog->lookupCollectionByNamespace(opCtx, *ar.nss);
            if (ar.uuid) {
                checkCollectionUUIDMismatch(opCtx, *ar.nss, coll, *ar.uuid);
            }

            ResolvedNamespaceOrViewAcquisitionRequest resolvedAcquisitionRequest{
                *ar.nss,
                ar.uuid,
                ar.placementConcern,
                ar.viewMode,
                boost::none,
                boost::none,
                boost::none};
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

            ResolvedNamespaceOrViewAcquisitionRequest resolvedAcquisitionRequest{
                coll->ns(),
                coll->uuid(),
                ar.placementConcern,
                ar.viewMode,
                boost::none,
                boost::none,
                boost::none};

            sortedAcquisitionRequests.emplace(ResourceId(RESOURCE_COLLECTION, coll->ns()),
                                              std::move(resolvedAcquisitionRequest));
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

void checkPlacementVersion(
    OperationContext* opCtx,
    const ResolvedNamespaceOrViewAcquisitionRequest& resolvedAcquisitionRequest) {
    const auto& nss = resolvedAcquisitionRequest.nss;

    const auto& receivedDbVersion = resolvedAcquisitionRequest.tsPlacement.dbVersion;
    if (receivedDbVersion) {
        DatabaseShardingState::assertMatchingDbVersion(opCtx, nss.db(), *receivedDbVersion);
    }

    const auto& receivedShardVersion = resolvedAcquisitionRequest.tsPlacement.shardVersion;
    if (receivedShardVersion) {
        const auto scopedCSS = CollectionShardingState::acquire(opCtx, nss);
        scopedCSS->checkShardVersionOrThrow(opCtx, *receivedShardVersion);
    }
}

ScopedCollectionOrViewAcquisition acquireResolvedCollectionOrView(
    OperationContext* opCtx,
    ResolvedNamespaceOrViewAcquisitionRequest& resolvedAcquisitionRequest) {
    const auto& nss = resolvedAcquisitionRequest.nss;

    // Check placement version before acquiring the catalog snapshot
    checkPlacementVersion(opCtx, resolvedAcquisitionRequest);

    const auto catalog = CollectionCatalog::get(opCtx);
    auto coll = catalog->lookupCollectionByNamespace(opCtx, nss);

    // Checks after having established the storage catalog snapshot
    if (resolvedAcquisitionRequest.uuid) {
        checkCollectionUUIDMismatch(opCtx, nss, coll, resolvedAcquisitionRequest.uuid);
    }

    if (coll) {
        verifyDbAndCollection(opCtx, nss, coll);
    } else if (catalog->lookupView(opCtx, nss)) {
        uassert(ErrorCodes::CommandNotSupportedOnView,
                str::stream() << "Namespace " << nss << " is a view, not a collection",
                resolvedAcquisitionRequest.viewMode ==
                    NamespaceOrViewAcquisitionRequest::ViewMode::kCanBeView);
    } else {
        uasserted(ErrorCodes::NamespaceNotFound,
                  str::stream() << "Namespace " << nss << "does not exist.");
    }

    const bool isPlacementConcernVersioned = resolvedAcquisitionRequest.tsPlacement.dbVersion ||
        resolvedAcquisitionRequest.tsPlacement.shardVersion;

    const auto scopedCSS = CollectionShardingState::acquire(opCtx, nss);
    auto collectionDescription =
        scopedCSS->getCollectionDescription(opCtx, isPlacementConcernVersioned);

    // Recheck the placement version after having acquired the catalog snapshot. If the placement
    // version still matches, then the catalog we snapshoted is consistent with the placement
    // concern too.
    checkPlacementVersion(opCtx, resolvedAcquisitionRequest);

    return ScopedCollectionOrViewAcquisition::make(opCtx,
                                                   std::move(coll),
                                                   std::move(collectionDescription),
                                                   std::move(resolvedAcquisitionRequest.dbLock),
                                                   std::move(resolvedAcquisitionRequest.collLock));
}

std::vector<ScopedCollectionOrViewAcquisition> acquireResolvedCollectionsOrViewsWithoutTakingLocks(
    OperationContext* opCtx,
    ResolvedNamespaceOrViewAcquisitionRequestsMap sortedAcquisitionRequests) {
    std::vector<ScopedCollectionOrViewAcquisition> acquisitions;
    for (auto& acquisitionRequest : sortedAcquisitionRequests) {
        auto acquisition = acquireResolvedCollectionOrView(opCtx, acquisitionRequest.second);
        acquisitions.emplace_back(std::move(acquisition));
    }

    return acquisitions;
}

}  // namespace

const NamespaceOrViewAcquisitionRequest::PlacementConcern
    NamespaceOrViewAcquisitionRequest::kPretendUnshardedDueToDirectConnection{boost::none,
                                                                              boost::none};

NamespaceOrViewAcquisitionRequest::NamespaceOrViewAcquisitionRequest(
    OperationContext* opCtx,
    NamespaceString nss,
    repl::ReadConcernArgs readConcern,
    ViewMode viewMode)
    : nss(nss),
      placementConcern(getPlacementConcernFromOSS(opCtx, nss)),
      readConcern(readConcern),
      viewMode(viewMode) {}

ScopedCollectionOrViewAcquisition::~ScopedCollectionOrViewAcquisition() {
    if (_opCtx) {
        auto& transactionResources = getOrMakeTransactionResources(_opCtx);
        transactionResources.acquiredCollections.remove_if(
            [& acquiredCollection = _acquiredCollection](
                const shard_role_details::TransactionResources::AcquiredCollection&
                    txnResourceAcquiredColl) {
                return &txnResourceAcquiredColl == &acquiredCollection;
            });
    }
}

ScopedCollectionOrViewAcquisition ScopedCollectionOrViewAcquisition::make(
    OperationContext* opCtx,
    CollectionPtr&& collectionPtr,
    ScopedCollectionDescription&& collectionDescription,
    boost::optional<Lock::DBLock>&& dbLock,
    boost::optional<Lock::CollectionLock>&& collectionLock) {
    invariant(collectionPtr);

    const auto nss = collectionPtr->ns();
    const auto uuid = collectionPtr->uuid();
    const shard_role_details::TransactionResources::AcquiredCollection& acquiredCollection =
        getOrMakeTransactionResources(opCtx).addAcquiredCollection({nss,
                                                                    uuid,
                                                                    std::move(collectionPtr),
                                                                    collectionDescription,
                                                                    std::move(dbLock),
                                                                    std::move(collectionLock)});

    return ScopedCollectionOrViewAcquisition(opCtx, std::move(acquiredCollection));
}

std::vector<ScopedCollectionOrViewAcquisition> acquireCollectionsOrViews(
    OperationContext* opCtx,
    std::initializer_list<NamespaceOrViewAcquisitionRequest> acquisitionRequests,
    LockMode mode) {
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
        for (auto& ar : sortedAcquisitionRequests) {
            // TODO: SERVER-73004 When acquiring multiple collections, avoid recursively locking the
            // dbLock because that causes recursive locking of the globalLock which prevents
            // yielding.
            ar.second.dbLock.emplace(
                opCtx, ar.second.nss.db(), isSharedLockMode(mode) ? MODE_IS : MODE_IX);
            ar.second.collLock.emplace(opCtx, ar.second.nss, mode);
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
    std::initializer_list<NamespaceOrViewAcquisitionRequest> acquisitionRequests) {
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

YieldedTransactionResources yieldTransactionResources(OperationContext* opCtx);

void restoreTransactionResources(OperationContext* opCtx,
                                 YieldedTransactionResources&& yieldedResources);

}  // namespace mongo
