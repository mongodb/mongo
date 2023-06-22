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

#pragma once

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/transaction_resources.h"

namespace mongo {

/**
 * Structure used to declare all the prerequisites that the catalog needs to meet in order for an
 * acquisition of a namespace to succeed.
 */
struct CollectionOrViewAcquisitionRequest {
    /**
     * Overload, which acquires a collection by NSS or DB/UUID, without imposing an expected
     * relationship between NSS and UUID.
     */
    CollectionOrViewAcquisitionRequest(
        NamespaceStringOrUUID nssOrUUID,
        PlacementConcern placementConcern,
        repl::ReadConcernArgs readConcern,
        AcquisitionPrerequisites::OperationType operationType,
        AcquisitionPrerequisites::ViewMode viewMode = AcquisitionPrerequisites::kCanBeView)
        : nss(nssOrUUID.nss()),
          dbname(nssOrUUID.dbName()),
          uuid(nssOrUUID.uuid()),
          placementConcern(placementConcern),
          readConcern(readConcern),
          operationType(operationType),
          viewMode(viewMode) {}

    /**
     * Overload, which acquires a collection by NSS/UUID combination, requiring that, if specified,
     * the UUID of the namespace matches exactly.
     */
    CollectionOrViewAcquisitionRequest(
        NamespaceString nss,
        boost::optional<UUID> uuid,
        PlacementConcern placementConcern,
        repl::ReadConcernArgs readConcern,
        AcquisitionPrerequisites::OperationType operationType,
        AcquisitionPrerequisites::ViewMode viewMode = AcquisitionPrerequisites::kCanBeView)
        : nss(nss),
          uuid(uuid),
          placementConcern(placementConcern),
          readConcern(readConcern),
          operationType(operationType),
          viewMode(viewMode) {}

    /**
     * Overload, which acquires a collection or view by NSS or DB/UUID and infers the placement and
     * read concerns from the OperationShardingState and ReadConcern values on the OperationContext.
     */
    static CollectionOrViewAcquisitionRequest fromOpCtx(
        OperationContext* opCtx,
        NamespaceStringOrUUID nssOrUUID,
        AcquisitionPrerequisites::OperationType operationType,
        AcquisitionPrerequisites::ViewMode viewMode = AcquisitionPrerequisites::kCanBeView);

    boost::optional<NamespaceString> nss;

    boost::optional<DatabaseName> dbname;
    boost::optional<UUID> uuid;

    PlacementConcern placementConcern;
    repl::ReadConcernArgs readConcern;
    AcquisitionPrerequisites::OperationType operationType;
    AcquisitionPrerequisites::ViewMode viewMode;
};

struct CollectionAcquisitionRequest : public CollectionOrViewAcquisitionRequest {
    /**
     * Overload, which acquires a collection by NSS or DB/UUID, without imposing an expected
     * relationship between NSS and UUID.
     */
    CollectionAcquisitionRequest(NamespaceStringOrUUID nssOrUUID,
                                 PlacementConcern placementConcern,
                                 repl::ReadConcernArgs readConcern,
                                 AcquisitionPrerequisites::OperationType operationType)
        : CollectionOrViewAcquisitionRequest(nssOrUUID,
                                             placementConcern,
                                             readConcern,
                                             operationType,
                                             AcquisitionPrerequisites::kMustBeCollection) {}

    /**
     * Overload, which acquires a collection by NSS/UUID combination, requiring that, if specified,
     * the UUID of the namespace matches exactly.
     */
    CollectionAcquisitionRequest(NamespaceString nss,
                                 boost::optional<UUID> uuid,
                                 PlacementConcern placementConcern,
                                 repl::ReadConcernArgs readConcern,
                                 AcquisitionPrerequisites::OperationType operationType)
        : CollectionOrViewAcquisitionRequest(nss,
                                             uuid,
                                             placementConcern,
                                             readConcern,
                                             operationType,
                                             AcquisitionPrerequisites::kMustBeCollection) {}

    /**
     * Infers the placement and read concerns from the OperationShardingState and ReadConcern values
     * on the OperationContext.
     */
    static CollectionAcquisitionRequest fromOpCtx(
        OperationContext* opCtx,
        NamespaceString nss,
        AcquisitionPrerequisites::OperationType operationType,
        boost::optional<UUID> expectedUUID = boost::none);

    static CollectionAcquisitionRequest fromOpCtx(
        OperationContext* opCtx,
        NamespaceStringOrUUID nssOrUUID,
        AcquisitionPrerequisites::OperationType operationType);
};

class ScopedCollectionOrViewAcquisition;

class ScopedCollectionAcquisition {
public:
    ScopedCollectionAcquisition(OperationContext* opCtx,
                                shard_role_details::AcquiredCollection& acquiredCollection);

    ScopedCollectionAcquisition(ScopedCollectionAcquisition&& other)
        : _txnResources(other._txnResources), _acquiredCollection(other._acquiredCollection) {
        other._txnResources = nullptr;
    }

    explicit ScopedCollectionAcquisition(ScopedCollectionOrViewAcquisition&& other);

    ScopedCollectionAcquisition(const ScopedCollectionAcquisition&) = delete;

    ~ScopedCollectionAcquisition();

    const NamespaceString& nss() const {
        return _acquiredCollection.prerequisites.nss;
    }

    /**
     * Returns whether the acquisition found a collection or the collection didn't exist.
     */
    bool exists() const {
        return bool(_acquiredCollection.collectionPtr);
    }

    /**
     * Returns the UUID of the acquired collection, but this operation is only allowed if the
     * collection `exists()`, otherwise this method will invariant.
     */
    UUID uuid() const;

    // Access to services associated with the specified collection top to bottom on the hierarchical
    // stack

    // Sharding catalog services
    const ScopedCollectionDescription& getShardingDescription() const;
    const boost::optional<ScopedCollectionFilter>& getShardingFilter() const;

    // Local catalog services
    const CollectionPtr& getCollectionPtr() const;

private:
    friend class ScopedLocalCatalogWriteFence;

    // Points to the acquired resources that live on the TransactionResources opCtx decoration. The
    // lifetime of these resources is tied to the lifetime of this ScopedCollectionAcquisition.
    shard_role_details::TransactionResources* _txnResources;
    shard_role_details::AcquiredCollection& _acquiredCollection;
};

class ScopedViewAcquisition {
public:
    ScopedViewAcquisition(OperationContext* opCtx,
                          const shard_role_details::AcquiredView& acquiredView);

    ScopedViewAcquisition(ScopedViewAcquisition&& other)
        : _txnResources(other._txnResources), _acquiredView(other._acquiredView) {
        other._txnResources = nullptr;
    }

    ScopedViewAcquisition(const ScopedViewAcquisition&) = delete;

    ~ScopedViewAcquisition();

    const NamespaceString& nss() const {
        return _acquiredView.prerequisites.nss;
    }

    // StorEx services
    const ViewDefinition& getViewDefinition() const {
        invariant(_acquiredView.viewDefinition);
        return *(_acquiredView.viewDefinition);
    }

private:
    // Points to the acquired resources that live on the TransactionResources opCtx decoration. The
    // lifetime of these resources is tied to the lifetime of this ScopedViewAcquisition.
    shard_role_details::TransactionResources* _txnResources;
    const shard_role_details::AcquiredView& _acquiredView;
};

class ScopedCollectionOrViewAcquisition {
public:
    ScopedCollectionOrViewAcquisition(ScopedCollectionAcquisition&& collection)
        : _collectionOrViewAcquisition(std::move(collection)) {}

    ScopedCollectionOrViewAcquisition(ScopedViewAcquisition&& view)
        : _collectionOrViewAcquisition(std::move(view)) {}

    const NamespaceString& nss() const {
        if (isCollection()) {
            return getCollection().nss();
        } else {
            return getView().nss();
        }
    }

    bool isCollection() const {
        return std::holds_alternative<ScopedCollectionAcquisition>(_collectionOrViewAcquisition);
    }

    bool isView() const {
        return std::holds_alternative<ScopedViewAcquisition>(_collectionOrViewAcquisition);
    }

    const ScopedCollectionAcquisition& getCollection() const {
        invariant(isCollection());
        return std::get<ScopedCollectionAcquisition>(_collectionOrViewAcquisition);
    }

    const CollectionPtr& getCollectionPtr() const {
        if (isCollection()) {
            return getCollection().getCollectionPtr();
        } else {
            return CollectionPtr::null;
        }
    }

    const ScopedViewAcquisition& getView() const {
        invariant(isView());
        return std::get<ScopedViewAcquisition>(_collectionOrViewAcquisition);
    }

private:
    friend class ScopedCollectionAcquisition;

    std::variant<ScopedCollectionAcquisition, ScopedViewAcquisition, std::monostate>
        _collectionOrViewAcquisition;
};

/**
 * Takes into account the specified namespace acquisition requests and if they can be satisfied,
 * adds the acquired collections to the set ot TransactionResources for the current operation.
 *
 * This method will acquire and 2-phase hold all the necessary hierarchical locks (Global, DB and
 * Collection).
 */
ScopedCollectionAcquisition acquireCollection(OperationContext* opCtx,
                                              CollectionAcquisitionRequest acquisitionRequest,
                                              LockMode mode);

std::vector<ScopedCollectionAcquisition> acquireCollections(
    OperationContext* opCtx,
    std::vector<CollectionAcquisitionRequest> acquisitionRequests,
    LockMode mode);

ScopedCollectionOrViewAcquisition acquireCollectionOrView(
    OperationContext* opCtx, CollectionOrViewAcquisitionRequest acquisitionRequest, LockMode mode);

// TODO SERVER-77405 Rename and make it delegate to locked version when lock-free is not possible.
ScopedCollectionOrViewAcquisition acquireCollectionOrViewWithoutTakingLocks(
    OperationContext* opCtx, CollectionOrViewAcquisitionRequest acquisitionRequest);

std::vector<ScopedCollectionOrViewAcquisition> acquireCollectionsOrViews(
    OperationContext* opCtx,
    std::vector<CollectionOrViewAcquisitionRequest> acquisitionRequests,
    LockMode mode);

/**
 * Same semantics as `acquireCollectionsOrViews` above, but will not acquire or hold any of the
 * 2-phase hierarchical locks.
 */
std::vector<ScopedCollectionOrViewAcquisition> acquireCollectionsOrViewsWithoutTakingLocks(
    OperationContext* opCtx, std::vector<CollectionOrViewAcquisitionRequest> acquisitionRequests);

/**
 * Please read the comments on AcquisitionPrerequisites::kLocalCatalogOnlyWithPotentialDataLoss for
 * more information on the semantics of this acquisition.
 */
ScopedCollectionAcquisition acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(
    OperationContext* opCtx, const NamespaceString& nss, LockMode mode);

/**
 * This utility is what allows modifications to the local catalog part of an acquisition for a
 * specific collection to become visible on a previously established acquisition for that
 * collection, before or after the end of a WUOW.
 *
 * The presence of ScopedLocalCatalogWriteFence on the stack renders the collection for which it was
 * instantiated unusable within its scope. Once it goes out of scope, any changes performed to the
 * catalog collection will be visible to:
 *  - The transaction only, if the WUOW has not yet committed
 *  - Any subsequent collection acquisitions, when the WUOW commits
 *
 * NOTE: This utility by itself does not ensure that catalog modifications which are subordinate to
 * the placement concern (create collection is subordinate to the location of the DB primary, for
 * example) do not conflict with placement changes (e.g. movePrimary). This is currently implemented
 * at a higher level through the usage of DB/Collection X-locks.
 */
class ScopedLocalCatalogWriteFence {
public:
    ScopedLocalCatalogWriteFence(OperationContext* opCtx, ScopedCollectionAcquisition* acquisition);

    ScopedLocalCatalogWriteFence(ScopedLocalCatalogWriteFence&) = delete;
    ScopedLocalCatalogWriteFence(ScopedLocalCatalogWriteFence&&) = delete;

    ~ScopedLocalCatalogWriteFence();

private:
    static void _updateAcquiredLocalCollection(
        OperationContext* opCtx, shard_role_details::AcquiredCollection* acquiredCollection);

    OperationContext* _opCtx;
    shard_role_details::AcquiredCollection* _acquiredCollection;
};

/**
 * Serves as a temporary container for transaction resources which have been yielded via a call to
 * `yieldTransactionResources`. Must never be destroyed without having been restored and the
 * transaction resources properly committed/aborted, or disposed of.
 */
struct YieldedTransactionResources {
    YieldedTransactionResources(
        std::unique_ptr<shard_role_details::TransactionResources> yieldedResources);

    YieldedTransactionResources(YieldedTransactionResources&&) = default;

    ~YieldedTransactionResources();

    /**
     * Releases the yielded TransactionResources.
     */
    void dispose();

    std::unique_ptr<shard_role_details::TransactionResources> _yieldedResources;
};

/**
 * This method puts the TransactionResources associated with the current OpCtx into the yielded
 * state and then detaches them from the OpCtx, moving their ownership to the returned object.
 *
 * The returned object must either be properly restored by a later call to
 * `restoreTransactionResourcesToOperationContext` or it must be `.dispose()`d of before
 * destruction.
 *
 * It is not always allowed to yield the transaction resources and it is the caller's responsibility
 * to verify a yield can be performed by calling Locker::canSaveLockState().
 */
YieldedTransactionResources yieldTransactionResourcesFromOperationContext(OperationContext* opCtx);

void restoreTransactionResourcesToOperationContext(
    OperationContext* opCtx, YieldedTransactionResources yieldedResourcesHolder);

/**
 * TODO SERVER-77405 remove, make internal only.
 *
 * Performs some checks to determine whether the operation is compatible with a lock-free read.
 * Multi-doc transactions are not supported, nor are operations performing a write.
 */
bool supportsLockFreeRead(OperationContext* opCtx);

/**
 * TODO (SERVER-69813): Get rid of this when ShardServerCatalogCacheLoader will be removed.
 * RAII type for letting secondary reads to block behind the PBW lock.
 * Note: Do not add additional usage. This is only temporary for ease of backport.
 */
struct BlockAcquisitionsSecondaryReadsDuringBatchApplication_DONT_USE {
public:
    BlockAcquisitionsSecondaryReadsDuringBatchApplication_DONT_USE(OperationContext* opCtx);
    ~BlockAcquisitionsSecondaryReadsDuringBatchApplication_DONT_USE();

private:
    OperationContext* _opCtx{nullptr};
    boost::optional<bool> _originalSettings;
};

namespace shard_role_details {
class SnapshotAttempt {
public:
    SnapshotAttempt(OperationContext* opCtx,
                    const std::vector<NamespaceStringOrUUID>& acquisitionRequests)
        : _opCtx{opCtx}, _acquisitionRequests(acquisitionRequests) {}

    ~SnapshotAttempt();

    void snapshotInitialState();

    void changeReadSourceForSecondaryReads();

    void openStorageSnapshot();

    [[nodiscard]] std::shared_ptr<const CollectionCatalog> getConsistentCatalog();

private:
    OperationContext* _opCtx;
    const std::vector<NamespaceStringOrUUID>& _acquisitionRequests;
    bool _openedSnapshot = false;
    bool _successful = false;
    boost::optional<long long> _replTermBeforeSnapshot;
    boost::optional<std::shared_ptr<const CollectionCatalog>> _catalogBeforeSnapshot;
    boost::optional<bool> _shouldReadAtLastApplied;
};
}  // namespace shard_role_details
}  // namespace mongo
