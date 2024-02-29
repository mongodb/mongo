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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/collection_type.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/views/view.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/uuid.h"

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
        AcquisitionPrerequisites::ViewMode viewMode = AcquisitionPrerequisites::kCanBeView,
        Date_t lockAcquisitionDeadline = Date_t::max())
        : nssOrUUID(std::move(nssOrUUID)),
          placementConcern(std::move(placementConcern)),
          readConcern(readConcern),
          operationType(operationType),
          viewMode(viewMode),
          lockAcquisitionDeadline(lockAcquisitionDeadline) {}

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
        AcquisitionPrerequisites::ViewMode viewMode = AcquisitionPrerequisites::kCanBeView,
        Date_t lockAcquisitionDeadline = Date_t::max())
        : nssOrUUID(std::move(nss)),
          expectedUUID(std::move(uuid)),
          placementConcern(placementConcern),
          readConcern(readConcern),
          operationType(operationType),
          viewMode(viewMode),
          lockAcquisitionDeadline(lockAcquisitionDeadline) {}

    /**
     * Overload, which acquires a collection or view by NSS or DB/UUID and infers the placement and
     * read concerns from the OperationShardingState and ReadConcern values on the OperationContext.
     */
    static CollectionOrViewAcquisitionRequest fromOpCtx(
        OperationContext* opCtx,
        NamespaceStringOrUUID nssOrUUID,
        AcquisitionPrerequisites::OperationType operationType,
        AcquisitionPrerequisites::ViewMode viewMode = AcquisitionPrerequisites::kCanBeView);

    NamespaceStringOrUUID nssOrUUID;

    // When 'nssOrUUID' is in the NamespaceString form 'expectedUUID' may contain the expected
    // collection UUID for that nss. When 'nssOrUUID' is in the UUID form, then 'expectedUUID' is
    // boost::none because the 'nssOrUUID' already expresses the desired UUID.
    boost::optional<UUID> expectedUUID;

    PlacementConcern placementConcern;
    repl::ReadConcernArgs readConcern;
    AcquisitionPrerequisites::OperationType operationType;
    AcquisitionPrerequisites::ViewMode viewMode;
    Date_t lockAcquisitionDeadline;
};

struct CollectionAcquisitionRequest : public CollectionOrViewAcquisitionRequest {
    /**
     * Overload, which acquires a collection by NSS or DB/UUID, without imposing an expected
     * relationship between NSS and UUID.
     */
    CollectionAcquisitionRequest(NamespaceStringOrUUID nssOrUUID,
                                 PlacementConcern placementConcern,
                                 repl::ReadConcernArgs readConcern,
                                 AcquisitionPrerequisites::OperationType operationType,
                                 Date_t lockAcquisitionDeadline = Date_t::max())
        : CollectionOrViewAcquisitionRequest(nssOrUUID,
                                             placementConcern,
                                             readConcern,
                                             operationType,
                                             AcquisitionPrerequisites::kMustBeCollection,
                                             lockAcquisitionDeadline) {}

    /**
     * Overload, which acquires a collection by NSS/UUID combination, requiring that, if specified,
     * the UUID of the namespace matches exactly.
     */
    CollectionAcquisitionRequest(NamespaceString nss,
                                 boost::optional<UUID> uuid,
                                 PlacementConcern placementConcern,
                                 repl::ReadConcernArgs readConcern,
                                 AcquisitionPrerequisites::OperationType operationType,
                                 Date_t lockAcquisitionDeadline = Date_t::max())
        : CollectionOrViewAcquisitionRequest(nss,
                                             uuid,
                                             placementConcern,
                                             readConcern,
                                             operationType,
                                             AcquisitionPrerequisites::kMustBeCollection,
                                             lockAcquisitionDeadline) {}

    /**
     * Infers the placement and read concerns from the OperationShardingState and ReadConcern values
     * on the OperationContext.
     */
    static CollectionAcquisitionRequest fromOpCtx(
        OperationContext* opCtx,
        NamespaceString nss,
        AcquisitionPrerequisites::OperationType operationType,
        boost::optional<UUID> expectedUUID = boost::none,
        Date_t lockAcquisitionDeadline = Date_t::max());

    static CollectionAcquisitionRequest fromOpCtx(
        OperationContext* opCtx,
        NamespaceStringOrUUID nssOrUUID,
        AcquisitionPrerequisites::OperationType operationType,
        Date_t lockAcquisitionDeadline = Date_t::max());
};

class CollectionOrViewAcquisition;

/**
 * A thread-unsafe ref-counted acquisition of a collection. The underlying acquisition stored inside
 * the operation's TransactionResources is managed by this class. It will be released whenever the
 * last reference to it is descoped. This class can be freely copied and moved around, each copy
 * will point to the same acquisition.
 *
 * This class cannot be transferred to other threads/OperationContext since the pointed to resources
 * lifetime would be held and manipulated by another thread.
 */
class CollectionAcquisition {
public:
    explicit CollectionAcquisition(CollectionOrViewAcquisition&& other);

    CollectionAcquisition(shard_role_details::TransactionResources& txnResources,
                          shard_role_details::AcquiredCollection& acquiredCollection);

    CollectionAcquisition(const CollectionAcquisition& other);
    CollectionAcquisition(CollectionAcquisition&& other);

    CollectionAcquisition& operator=(const CollectionAcquisition& other);
    CollectionAcquisition& operator=(CollectionAcquisition&& other);

    ~CollectionAcquisition();

    const NamespaceString& nss() const;

    /**
     * Returns whether the acquisition found a collection or the collection didn't exist.
     */
    bool exists() const;

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
    // lifetime of these resources is tied to the lifetime of this CollectionAcquisition.
    shard_role_details::TransactionResources* _txnResources;
    shard_role_details::AcquiredCollection* _acquiredCollection;
};

/**
 * A thread-unsafe ref-counted acquisition of a view. The underlying acquisition stored inside the
 * operation's TransactionResources is managed by this class. It will be released whenever the last
 * reference to it is descoped. This class can be freely copied and moved around, each copy will
 * point to the same acquisition.
 *
 * This class cannot be transferred to other threads/OperationContext since the pointed to resources
 * lifetime would be held and manipulated by another thread.
 */
class ViewAcquisition {
public:
    ViewAcquisition(shard_role_details::TransactionResources& txnResources,
                    const shard_role_details::AcquiredView& acquiredView);

    ViewAcquisition(const ViewAcquisition& other);
    ViewAcquisition(ViewAcquisition&& other);

    ViewAcquisition& operator=(const ViewAcquisition& other);
    ViewAcquisition& operator=(ViewAcquisition&& other);

    ~ViewAcquisition();

    const NamespaceString& nss() const;

    // StorEx services
    const ViewDefinition& getViewDefinition() const;

private:
    // Points to the acquired resources that live on the TransactionResources opCtx decoration. The
    // lifetime of these resources is tied to the lifetime of this ViewAcquisition.
    shard_role_details::TransactionResources* _txnResources;
    const shard_role_details::AcquiredView* _acquiredView;
};

class CollectionOrViewAcquisition {
public:
    CollectionOrViewAcquisition(CollectionAcquisition&& collection)
        : _collectionOrViewAcquisition(std::move(collection)) {}

    CollectionOrViewAcquisition(ViewAcquisition&& view)
        : _collectionOrViewAcquisition(std::move(view)) {}

    const NamespaceString& nss() const {
        if (isCollection()) {
            return getCollection().nss();
        } else {
            return getView().nss();
        }
    }

    bool isCollection() const {
        return holds_alternative<CollectionAcquisition>(_collectionOrViewAcquisition);
    }

    bool isView() const {
        return holds_alternative<ViewAcquisition>(_collectionOrViewAcquisition);
    }

    const CollectionAcquisition& getCollection() const {
        invariant(isCollection());
        return get<CollectionAcquisition>(_collectionOrViewAcquisition);
    }

    const CollectionPtr& getCollectionPtr() const {
        if (isCollection()) {
            return getCollection().getCollectionPtr();
        } else {
            return CollectionPtr::null;
        }
    }

    const ViewAcquisition& getView() const {
        invariant(isView());
        return get<ViewAcquisition>(_collectionOrViewAcquisition);
    }

    query_shape::CollectionType getCollectionType() const {
        if (isView()) {
            if (getView().getViewDefinition().timeseries())
                return query_shape::CollectionType::kTimeseries;
            return query_shape::CollectionType::kView;
        }
        const auto& collection = getCollection();
        if (!collection.exists()) {
            return query_shape::CollectionType::kNonExistent;
        }
        return query_shape::CollectionType::kCollection;
    }

    bool collectionExists() const {
        return isCollection() && getCollection().exists();
    }

private:
    friend class CollectionAcquisition;

    std::variant<CollectionAcquisition, ViewAcquisition, std::monostate>
        _collectionOrViewAcquisition;
};

using CollectionAcquisitions = stdx::unordered_map<NamespaceString, CollectionAcquisition>;

using CollectionOrViewAcquisitions =
    stdx::unordered_map<NamespaceString, CollectionOrViewAcquisition>;

/**
 * Takes into account the specified namespace acquisition requests and if they can be satisfied,
 * adds the acquired collections to the set ot TransactionResources for the current operation.
 *
 * This method will acquire and 2-phase hold all the necessary hierarchical locks (Global, DB and
 * Collection).
 */
CollectionAcquisition acquireCollection(OperationContext* opCtx,
                                        CollectionAcquisitionRequest acquisitionRequest,
                                        LockMode mode);

CollectionAcquisitions acquireCollections(
    OperationContext* opCtx,
    std::vector<CollectionAcquisitionRequest> acquisitionRequests,
    LockMode mode);

CollectionOrViewAcquisition acquireCollectionOrView(
    OperationContext* opCtx, CollectionOrViewAcquisitionRequest acquisitionRequest, LockMode mode);

CollectionOrViewAcquisitions acquireCollectionsOrViews(
    OperationContext* opCtx,
    std::vector<CollectionOrViewAcquisitionRequest> acquisitionRequests,
    LockMode mode);

/**
 * Same semantics as `acquireCollectionsOrViews` above, but will not acquire or hold any of the
 * 2-phase hierarchical locks if allowed for this operation. The conditions required for the
 * acquisition to be lock-free are:
 *    * The operation is not a multi-document transaction.
 *    * The global lock is not write locked.
 *    * No storage transaction is already open, or if it is, it has to be for a lock free operation.
 */
CollectionAcquisition acquireCollectionMaybeLockFree(
    OperationContext* opCtx, CollectionAcquisitionRequest acquisitionRequest);

CollectionAcquisitions acquireCollectionsMaybeLockFree(
    OperationContext* opCtx, std::vector<CollectionAcquisitionRequest> acquisitionRequests);

CollectionOrViewAcquisition acquireCollectionOrViewMaybeLockFree(
    OperationContext* opCtx, CollectionOrViewAcquisitionRequest acquisitionRequest);

CollectionOrViewAcquisitions acquireCollectionsOrViewsMaybeLockFree(
    OperationContext* opCtx, std::vector<CollectionOrViewAcquisitionRequest> acquisitionRequests);

/**
 * Please read the comments on AcquisitionPrerequisites::kLocalCatalogOnlyWithPotentialDataLoss for
 * more information on the semantics of this acquisition.
 */
CollectionAcquisition acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(
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
    ScopedLocalCatalogWriteFence(OperationContext* opCtx, CollectionAcquisition* acquisition);

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
    YieldedTransactionResources(YieldedTransactionResources&&) = default;
    YieldedTransactionResources& operator=(YieldedTransactionResources&&) = default;

    YieldedTransactionResources(
        std::unique_ptr<shard_role_details::TransactionResources> yieldedResources,
        shard_role_details::TransactionResources::State originalState);

    ~YieldedTransactionResources();

    /**
     * Releases the yielded TransactionResources, transitions the resources back to the opCtx and
     * marks them as FAILED.
     */
    void transitionTransactionResourcesToFailedState(OperationContext* opCtx);

    std::unique_ptr<shard_role_details::TransactionResources> _yieldedResources;
    shard_role_details::TransactionResources::State _originalState;
};

/**
 * This method puts the TransactionResources associated with the current OpCtx into the yielded
 * state and then detaches them from the OpCtx, moving their ownership to the returned object.
 *
 * The returned object must either be properly restored by a later call to
 * `restoreTransactionResourcesToOperationContext` or it must be
 * `.transitionTransactionResourcesToFailedState()`d before destruction.
 *
 * It is not always allowed to yield the transaction resources and it is the caller's responsibility
 * to verify a yield can be performed by calling Locker::canSaveLockState().
 */
YieldedTransactionResources yieldTransactionResourcesFromOperationContext(OperationContext* opCtx);

void restoreTransactionResourcesToOperationContext(
    OperationContext* opCtx, YieldedTransactionResources yieldedResourcesHolder);

/**
 * An opaque class meant for containing TransactionResources that are stashed for subsequent
 * getMores.
 *
 * Usage of this class must be done via the RAII type HandleTransactionResourcesFromStasher. It will
 * take care of restoring the TransactionResources onto the operation and stash them back once it
 * goes out of scope.
 */
class StashedTransactionResources {
public:
    StashedTransactionResources() = default;

    StashedTransactionResources(
        std::unique_ptr<shard_role_details::TransactionResources> yieldedResources,
        shard_role_details::TransactionResources::State originalState)
        : _yieldedResources(std::move(yieldedResources)), _originalState(originalState) {}

    StashedTransactionResources(const StashedTransactionResources&) = delete;
    StashedTransactionResources(StashedTransactionResources&&) = default;

    StashedTransactionResources& operator=(const StashedTransactionResources&) = delete;
    StashedTransactionResources& operator=(StashedTransactionResources&&) = default;

    ~StashedTransactionResources() {
        invariant(!_yieldedResources,
                  "Resources must be disposed or passed on to an opCtx before destroying the "
                  "StashedTransactionResources");
    }

    /**
     * Releases the yielded TransactionResources without transitioning them back to an opCtx. This
     * releases all locks and acquisitions held.
     */
    void dispose();

private:
    friend class HandleTransactionResourcesFromStasher;

    std::unique_ptr<shard_role_details::TransactionResources> _yieldedResources;
    shard_role_details::TransactionResources::State _originalState;
};

/**
 * Interface for supporting storing/releasing of stashed transaction resources.
 * See ClientCursor for example implementation.
 */
class TransactionResourcesStasher {
public:
    TransactionResourcesStasher() = default;
    virtual ~TransactionResourcesStasher() = default;

    /**
     *  Releases the stashed TransactionResources to the caller.
     */
    virtual StashedTransactionResources releaseStashedTransactionResources() = 0;

    /**
     * Stashes the provided TransactionResources.
     */
    virtual void stashTransactionResources(StashedTransactionResources resources) = 0;
};

class StashTransactionResourcesForDBDirect {
public:
    StashTransactionResourcesForDBDirect(OperationContext* opCtx);
    ~StashTransactionResourcesForDBDirect();

private:
    OperationContext* _opCtx;
    std::unique_ptr<shard_role_details::TransactionResources> _originalTransactionResources;
};

/**
 * This method puts the TransactionResources associated with the current OpCtx into the stashed
 * state and then detaches them from the OpCtx, moving their ownership to the given cursor.
 */
void stashTransactionResourcesFromOperationContext(OperationContext* opCtx,
                                                   TransactionResourcesStasher* stasher);

/**
 * An RAII class that handles restoration of the TransactionResources onto the OperationContext from
 * a TransactionResourcesStasher.
 *
 * This class automatically handles stashing and unstashing the resources onto the
 * TransactionResourcesStasher as long as the TransactionResources aren't in the FAILED
 * state. If the operation has failed and the resources have to be released the user must
 * dismissRestoredResources() in order to release them and not stash them into the stasher.
 */
class HandleTransactionResourcesFromStasher {
public:
    HandleTransactionResourcesFromStasher(OperationContext* opCtx,
                                          TransactionResourcesStasher* stasher);
    ~HandleTransactionResourcesFromStasher();

    /**
     * Marks the current TransactionResources as FAILED and releases all resources. After calling
     * this method the transactions won't be stashed back into the ClientCursor.
     */
    void dismissRestoredResources();

private:
    OperationContext* _opCtx;
    TransactionResourcesStasher* _stasher;
    std::unique_ptr<shard_role_details::TransactionResources> _originalTransactionResources;
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

/*
 * Checks that, when in multi-document transaction, local catalog stashed by the transaction and the
 * CollectionPtr it obtained are valid to be used for a request that attached
 */
void checkLocalCatalogIsValidForUnshardedShardVersion(OperationContext* opCtx,
                                                      const CollectionCatalog& stashedCatalog,
                                                      const CollectionPtr& collectionPtr,
                                                      const NamespaceString& nss);

/*
 * Check that the collection uuid on the sharding catalog and the local catalog match.
 */
void checkShardingAndLocalCatalogCollectionUUIDMatch(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardVersion& requestedShardVersion,
    const ScopedCollectionDescription& shardingCollectionDescription,
    const CollectionPtr& collectionPtr);

}  // namespace shard_role_details
}  // namespace mongo
