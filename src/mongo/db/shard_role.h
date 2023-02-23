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

#include "mongo/db/catalog_raii.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/s/database_version.h"

namespace mongo {

/**
 * Contains all the required properties for the acquisition of a collection. These properties are
 * taken into account in addition to the ReadConcern of the transaction, which is stored in the
 * OperationContext.
 */
struct CollectionOrViewAcquisitionRequest {
    static const AcquisitionPrerequisites::PlacementConcern kPretendUnshardedDueToDirectConnection;

    /**
     * Overload, which acquires a collection by NSS, ignoring the current UUID mapping.
     */
    CollectionOrViewAcquisitionRequest(
        NamespaceString nss,
        AcquisitionPrerequisites::PlacementConcern placementConcern,
        repl::ReadConcernArgs readConcern,
        AcquisitionPrerequisites::OperationType operationType,
        AcquisitionPrerequisites::ViewMode viewMode = AcquisitionPrerequisites::kMustBeCollection)
        : nss(nss),
          placementConcern(placementConcern),
          readConcern(readConcern),
          operationType(operationType),
          viewMode(viewMode) {}

    /**
     * Overload, which acquires a collection by NSS/UUID combination, requiring that the UUID of the
     * namespace matches exactly.
     */
    CollectionOrViewAcquisitionRequest(
        NamespaceString nss,
        UUID uuid,
        AcquisitionPrerequisites::PlacementConcern placementConcern,
        repl::ReadConcernArgs readConcern,
        AcquisitionPrerequisites::OperationType operationType,
        AcquisitionPrerequisites::ViewMode viewMode = AcquisitionPrerequisites::kMustBeCollection)
        : nss(nss),
          uuid(uuid),
          placementConcern(placementConcern),
          readConcern(readConcern),
          operationType(operationType),
          viewMode(viewMode) {}

    /**
     * Overload, which acquires a collection by NSS or DB/UUID, without imposing an expected
     * relationship between NSS and UUID.
     */
    CollectionOrViewAcquisitionRequest(
        NamespaceStringOrUUID nssOrUUID,
        AcquisitionPrerequisites::PlacementConcern placementConcern,
        repl::ReadConcernArgs readConcern,
        AcquisitionPrerequisites::OperationType operationType,
        AcquisitionPrerequisites::ViewMode viewMode = AcquisitionPrerequisites::kMustBeCollection)
        : dbname(nssOrUUID.dbName()),
          nss(nssOrUUID.nss()),
          uuid(nssOrUUID.uuid()),
          placementConcern(placementConcern),
          readConcern(readConcern),
          operationType(operationType),
          viewMode(viewMode) {}

    /**
     * Overload, which acquires a collection by NSS, ignoring the current UUID mapping. Takes the
     * placement concern from the opCtx's OperationShardingState.
     */
    CollectionOrViewAcquisitionRequest(
        OperationContext* opCtx,
        NamespaceString nss,
        repl::ReadConcernArgs readConcern,
        AcquisitionPrerequisites::OperationType operationType,
        AcquisitionPrerequisites::ViewMode viewMode = AcquisitionPrerequisites::kMustBeCollection);

    boost::optional<DatabaseName> dbname;
    boost::optional<NamespaceString> nss;
    boost::optional<UUID> uuid;

    AcquisitionPrerequisites::PlacementConcern placementConcern;
    repl::ReadConcernArgs readConcern;
    AcquisitionPrerequisites::OperationType operationType;
    AcquisitionPrerequisites::ViewMode viewMode;
};

struct CollectionAcquisitionRequest : public CollectionOrViewAcquisitionRequest {
    /**
     * Overload, which acquires a collection by NSS, ignoring the current UUID mapping.
     */
    CollectionAcquisitionRequest(NamespaceString nss,
                                 AcquisitionPrerequisites::PlacementConcern placementConcern,
                                 repl::ReadConcernArgs readConcern,
                                 AcquisitionPrerequisites::OperationType operationType)
        : CollectionOrViewAcquisitionRequest(nss,
                                             placementConcern,
                                             readConcern,
                                             operationType,
                                             AcquisitionPrerequisites::kMustBeCollection) {}

    /**
     * Overload, which acquires a collection by NSS/UUID combination, requiring that the UUID of the
     * namespace matches exactly.
     */
    CollectionAcquisitionRequest(NamespaceString nss,
                                 UUID uuid,
                                 AcquisitionPrerequisites::PlacementConcern placementConcern,
                                 repl::ReadConcernArgs readConcern,
                                 AcquisitionPrerequisites::OperationType operationType)
        : CollectionOrViewAcquisitionRequest(nss,
                                             uuid,
                                             placementConcern,
                                             readConcern,
                                             operationType,
                                             AcquisitionPrerequisites::kMustBeCollection) {}

    /**
     * Overload, which acquires a collection by NSS or DB/UUID, without imposing an expected
     * relationship between NSS and UUID.
     */
    CollectionAcquisitionRequest(NamespaceStringOrUUID nssOrUUID,
                                 AcquisitionPrerequisites::PlacementConcern placementConcern,
                                 repl::ReadConcernArgs readConcern,
                                 AcquisitionPrerequisites::OperationType operationType)
        : CollectionOrViewAcquisitionRequest(nssOrUUID,
                                             placementConcern,
                                             readConcern,
                                             operationType,
                                             AcquisitionPrerequisites::kMustBeCollection) {}

    /**
     * Overload, which acquires a collection by NSS, ignoring the current UUID mapping. Takes the
     * placement concern from the opCtx's OperationShardingState.
     */
    CollectionAcquisitionRequest(OperationContext* opCtx,
                                 NamespaceString nss,
                                 repl::ReadConcernArgs readConcern,
                                 AcquisitionPrerequisites::OperationType operationType)
        : CollectionOrViewAcquisitionRequest(
              opCtx, nss, readConcern, operationType, AcquisitionPrerequisites::kMustBeCollection) {
    }
};

class ScopedCollectionAcquisition {
public:
    ScopedCollectionAcquisition(const mongo::ScopedCollectionAcquisition&) = delete;

    ScopedCollectionAcquisition(mongo::ScopedCollectionAcquisition&& other)
        : _opCtx(other._opCtx), _acquiredCollection(other._acquiredCollection) {
        other._opCtx = nullptr;
    }

    ~ScopedCollectionAcquisition();

    ScopedCollectionAcquisition(OperationContext* opCtx,
                                const shard_role_details::AcquiredCollection& acquiredCollection)
        : _opCtx(opCtx), _acquiredCollection(acquiredCollection) {}

    const NamespaceString& nss() const {
        return _acquiredCollection.prerequisites.nss;
    }

    // Access to services associated with the specified collection top to bottom on the hierarchical
    // stack

    // Sharding services
    const ScopedCollectionDescription& getShardingDescription() const {
        return _acquiredCollection.collectionDescription;
    }

    const boost::optional<ScopedCollectionFilter>& getCollectionFilter() const {
        return _acquiredCollection.ownershipFilter;
    }

    // StorEx services
    const CollectionPtr& getCollectionPtr() const {
        return _acquiredCollection.collectionPtr;
    }

private:
    OperationContext* _opCtx;

    // Points to the acquired resources that live on the TransactionResources opCtx decoration. The
    // lifetime of these resources is tied to the lifetime of this
    // ScopedCollectionOrViewAcquisition.
    const shard_role_details::AcquiredCollection& _acquiredCollection;
};

class ScopedViewAcquisition {
public:
    ScopedViewAcquisition(const mongo::ScopedViewAcquisition&) = delete;

    ScopedViewAcquisition(mongo::ScopedViewAcquisition&& other)
        : _opCtx(other._opCtx), _acquiredView(other._acquiredView) {
        other._opCtx = nullptr;
    }

    ~ScopedViewAcquisition();

    ScopedViewAcquisition(OperationContext* opCtx,
                          const shard_role_details::AcquiredView& acquiredView)
        : _opCtx(opCtx), _acquiredView(acquiredView) {}

    const NamespaceString& nss() const {
        return _acquiredView.prerequisites.nss;
    }

    // StorEx services
    const ViewDefinition& getViewDefinition() const {
        invariant(_acquiredView.viewDefinition);
        return *(_acquiredView.viewDefinition);
    }

private:
    OperationContext* _opCtx;

    // Points to the acquired resources that live on the TransactionResources opCtx decoration. The
    // lifetime of these resources is tied to the lifetime of this
    // ScopedCollectionOrViewAcquisition.
    const shard_role_details::AcquiredView& _acquiredView;
};

using ScopedCollectionOrViewAcquisition =
    std::variant<ScopedCollectionAcquisition, ScopedViewAcquisition>;

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
 * Serves as a temporary container for transaction resources which have been yielded via a call to
 * `yieldTransactionResources`. Must never be destroyed without having been restored and the
 * transaction resources properly committed/aborted.
 */
class YieldedTransactionResources {
public:
    ~YieldedTransactionResources();

    YieldedTransactionResources() = default;

    YieldedTransactionResources(
        std::unique_ptr<shard_role_details::TransactionResources>&& yieldedResources);

    std::unique_ptr<shard_role_details::TransactionResources> _yieldedResources;
};

YieldedTransactionResources yieldTransactionResourcesFromOperationContext(OperationContext* opCtx);

void restoreTransactionResourcesToOperationContext(OperationContext* opCtx,
                                                   YieldedTransactionResources&& yieldedResources);

}  // namespace mongo
