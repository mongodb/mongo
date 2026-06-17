/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/shard_role_transaction_resources_stasher_for_pipeline.h"
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
 * 'afterClusterTime' is the clusterTime of the change event being enriched. The post-image must be
 * read from a snapshot at or after that time, never from before the change, which would return
 * pre-event (stale) state.
 */
inline void assertLocalLookupReadAtOrAfter(OperationContext* opCtx,
                                           boost::optional<Timestamp> afterClusterTime) {
    if (!afterClusterTime) {
        return;
    }
    auto readTs = shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();
    if (readTs && *readTs < *afterClusterTime) {
        tasserted(12841100,
                  str::stream() << "change-stream post-image lookup read snapshot "
                                << readTs->toString() << " precedes the event clusterTime "
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
        tassert(12841103,
                str::stream() << "PreAcquiredCollectionAcquirer: requested UUID '"
                              << (collectionUuid ? collectionUuid->toString() : "(none)")
                              << "' for nss '" << nss.toStringForErrorMsg()
                              << "' does not match held UUID '" << _collection.uuid().toString()
                              << "'",
                !collectionUuid || _collection.uuid() == *collectionUuid);

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
