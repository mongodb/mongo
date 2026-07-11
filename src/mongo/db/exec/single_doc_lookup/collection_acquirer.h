// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/shard_role_transaction_resources_stasher_for_pipeline.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo::exec::agg {

/**
 * Causal-consistency guard for local single-document lookups.
 *
 * 'afterClusterTime' is the clusterTime of the document being fetched. The document must be read
 * from a snapshot at or after that time, never from before, which would return stale results.
 */
inline void assertLocalLookupReadAtOrAfter(OperationContext* opCtx,
                                           boost::optional<Timestamp> afterClusterTime) {
    if (!afterClusterTime) {
        return;
    }
    auto readTs = shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();
    if (readTs && *readTs < *afterClusterTime) {
        tasserted(12841100,
                  str::stream() << "document lookup read snapshot " << readTs->toString()
                                << " precedes the requested clusterTime "
                                << afterClusterTime->toString());
    }
}

/**
 * Abstraction for acquiring a collection handle during a single-document lookup. Decouples the
 * lookup strategy from the lock acquisition mechanism:
 * - 'OnDemandCollectionAcquirer' takes a fresh acquisition per call
 * - 'PreAcquiredCollectionAcquirer' reuses an acquisition taken upfront.
 */
class CollectionAcquirer {
public:
    /**
     * RAII bundle returned by acquireCollection(). Encapsulates a CollectionAcquisition and, when
     * applicable, the HandleTransactionResourcesFromStasher that keeps the underlying
     * TransactionResources attached to the operation context for the lifetime of this object.
     */
    class Handle {
    public:
        explicit Handle(CollectionAcquisition acquisition) : _acquisition(std::move(acquisition)) {}

        // Takes ownership of an already-constructed stasher handle.
        Handle(CollectionAcquisition acquisition,
               std::unique_ptr<HandleTransactionResourcesFromStasher> handle)
            : _stasherHandle(std::move(handle)), _acquisition(std::move(acquisition)) {}

        // Move-only. Copies are forbidden, because the stasher handle is not copyable.
        Handle(Handle&&) = default;
        Handle& operator=(Handle&&) = default;
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        // Forwarding accessors so callers can use this where a CollectionAcquisition would do.
        bool exists() const {
            return _acquisition.exists();
        }
        const CollectionPtr& getCollectionPtr() const {
            return _acquisition.getCollectionPtr();
        }
        const NamespaceString& nss() const {
            return _acquisition.nss();
        }
        UUID uuid() const {
            return _acquisition.uuid();
        }
        const CollectionAcquisition& collection() const {
            return _acquisition;
        }

    private:
        // NOTE: _stasherHandle is declared first so it is destroyed *last* after _acquisition's
        // refcount has been decremented. Reversing this order would re-stash TransactionResources
        // while the CollectionAcquisition still holds a refcount into them.
        std::unique_ptr<HandleTransactionResourcesFromStasher> _stasherHandle;
        CollectionAcquisition _acquisition;
    };

    virtual ~CollectionAcquirer() = default;

    /**
     * Returns a Handle for 'nss' that the caller can use immediately. Implementations are
     * responsible for ensuring the returned acquisition reflects the current catalog state.
     *
     * The returned object owns any per-call resources (stasher handle, refcount on the underlying
     * AcquiredCollection); callers must keep it alive for the duration of any use of the contained
     * CollectionAcquisition.
     */
    virtual Handle acquireCollection(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     boost::optional<UUID> uuid) = 0;
};

/**
 * Looks up an AcquiredCollection that was placed into TransactionResources upfront and is
 * currently stashed in the pipeline's shared TransactionResourcesStasher.
 *
 * On each call the acquirer swaps the stashed resources onto the operation context, returns a
 * Handle that keeps the resources attached for the caller's lifetime, and on Handle destruction the
 * resources are automatically re-stashed for the next caller.
 *
 * The swap is a struct move pair; it does NOT acquire any new locks. The Global IS lock for the
 * upfront acquisition is held inside the stashed TransactionResources for the entire cursor
 * lifetime; the swap merely transfers ownership between the stasher and the opCtx.
 */
class PreAcquiredCollectionAcquirer : public CollectionAcquirer {
public:
    /**
     * Constructs the acquirer with:
     *   - 'stasher': the pipeline's shared TransactionResourcesStasher that holds the upfront
     * acquisitions between getMores.
     *   - 'collection': the CollectionAcquisition for the nss this acquirer will use.
     */
    PreAcquiredCollectionAcquirer(
        boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline> stasher,
        CollectionAcquisition collection)
        : _stasher(std::move(stasher)), _collection(std::move(collection)) {
        tassert(12841101, "PreAcquiredCollectionAcquirer requires a non-null stasher", _stasher);
    }

    Handle acquireCollection(OperationContext* opCtx,
                             const NamespaceString& nss,
                             boost::optional<UUID> collectionUuid) override {
        tassert(12841102,
                str::stream() << "PreAcquiredCollectionAcquirer: requested nss '"
                              << nss.toStringForErrorMsg() << "' does not match held nss '"
                              << _collection.nss().toStringForErrorMsg() << "'",
                _collection.nss() == nss);

        // The upfront-acquired collection's identity has moved on (DDL churn) since this
        // acquirer was built.
        checkCollectionUUIDMismatch(opCtx, nss, _collection.getCollectionPtr(), collectionUuid);

        // Swap the stashed TransactionResources onto opCtx for the duration of the returned Handle.
        // On scope exit, the handle's destructor re-stashes them.
        auto handle =
            std::make_unique<HandleTransactionResourcesFromStasher>(opCtx, _stasher.get());
        return Handle(_collection, std::move(handle));
    }

private:
    boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline> _stasher;
    CollectionAcquisition _collection;
};

/**
 * Acquires a collection on demand with its own lock via acquireCollectionMaybeLockFree.
 */
class OnDemandCollectionAcquirer : public CollectionAcquirer {
public:
    Handle acquireCollection(OperationContext* opCtx,
                             const NamespaceString& nss,
                             boost::optional<UUID> uuid) override {
        return Handle(
            acquireCollectionMaybeLockFree(opCtx,
                                           CollectionAcquisitionRequest::fromOpCtx(
                                               opCtx, nss, AcquisitionPrerequisites::kRead, uuid)));
    }
};

}  // namespace mongo::exec::agg
