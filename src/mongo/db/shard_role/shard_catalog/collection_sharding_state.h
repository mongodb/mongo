/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/util/modules.h"

#include <memory>
#include <shared_mutex>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Interface for handling stale shard metadata errors.
 * Implementations perform recovery or refresh actions for sharding metadata for a given collection
 * when stale metadata exceptions are encountered.
 */
class MONGO_MOD_PRIVATE StaleShardCollectionMetadataHandler {
public:
    /**
     * Handles a StaleConfig error by recovering the sharding metadata for the specified collection.
     * Returns the newly installed ShardVersion after recovery, if any.
     */
    virtual boost::optional<ChunkVersion> handleStaleShardVersionException(
        OperationContext* opCtx, const StaleConfigInfo& sci) const = 0;
};

/**
 * Each shard node process (primary or secondary) has one instance of this object for each
 * collection residing on that shard. It sits on the second level of the hierarchy of the Shard Role
 * runtime-authoritative caches (along with DatabaseShardingState) and represents the shard's
 * knowledge of that collection's shard version and the set of chunks that it owns, as well as
 * functions for tracking this state.
 *
 * This is the only interface that non-sharding consumers should be interfacing with.
 *
 * On shard servers, the implementation used is CollectionShardingRuntime.
 *
 * The CollectionShardingStateFactory class below is used to instantiate the correct subclass of
 * CollectionShardingState at runtime.
 *
 * SYNCHRONIZATION: This class is not thread-safe by itself. It relies on external locking. For
 * this reason, it must always be accessed through ScopedCollectionShardingState  helper classes,
 * which acquire the appropriate read locks to protect against concurrent modifications.
 */
class MONGO_MOD_NEEDS_REPLACEMENT CollectionShardingState {
public:
    CollectionShardingState() = default;
    virtual ~CollectionShardingState() = default;

    CollectionShardingState(const CollectionShardingState&) = delete;
    CollectionShardingState& operator=(const CollectionShardingState&) = delete;

    /**
     * Obtains the sharding state for the specified collection, along with a lock protecting it from
     * concurrent modifications, which will be held util the object goes out of scope.
     */
    class ScopedCollectionShardingState {
    public:
        ScopedCollectionShardingState(ScopedCollectionShardingState&&);

        ~ScopedCollectionShardingState();

        const CollectionShardingState* operator->() const {
            return _css;
        }
        const CollectionShardingState& operator*() const {
            return *_css;
        }

        ScopedCollectionShardingState(std::shared_lock<std::shared_mutex> lock,
                                      CollectionShardingState* css);

    private:
        friend class CollectionShardingState;
        friend class CollectionShardingRuntime;

        // Constructor without the lock.
        // Important: Only for use in non-shard servers!
        ScopedCollectionShardingState(CollectionShardingState* css);

        static ScopedCollectionShardingState acquire(OperationContext* opCtx,
                                                     const NamespaceString& nss);

        boost::optional<std::shared_lock<std::shared_mutex>> _lock;  //  NOLINT
        CollectionShardingState* _css;
    };

    /**
     * Obtains the sharding state for the specified collection, along with a lock in exclusive mode
     * protecting it from concurrent modifications, which will be held until the object goes out of
     * scope.
     *
     * Exclusive access is only used by catalog operations that modify the collection sharding
     * state.
     */
    class ScopedExclusiveCollectionShardingState {
    public:
        ScopedExclusiveCollectionShardingState(ScopedExclusiveCollectionShardingState&&) = default;
        ~ScopedExclusiveCollectionShardingState() = default;

        CollectionShardingState* operator->() const {
            return _css;
        }
        CollectionShardingState& operator*() const {
            return *_css;
        }

        ScopedExclusiveCollectionShardingState(std::unique_lock<std::shared_mutex> lock,
                                               CollectionShardingState* css)
            : _lock(std::move(lock)), _css(css) {}

    private:
        friend class CollectionShardingRuntime;

        // Constructor without the lock.
        // Important: Only for use in non-shard servers!
        ScopedExclusiveCollectionShardingState(CollectionShardingState* css);

        static ScopedExclusiveCollectionShardingState acquire(OperationContext* opCtx,
                                                              const NamespaceString& nss);

        boost::optional<std::unique_lock<std::shared_mutex>> _lock;  //  NOLINT
        CollectionShardingState* _css;
    };

    /**
     * Obtains the collection sharding state for the requested 'nss', along with the necessary
     * shared CSS locks that guarantee its stability. The locks will be held until the returned
     * object goes out of scope.
     *
     * It must be called with the collection lock held in at least MODE_IS.
     */
    static ScopedCollectionShardingState assertCollectionLockedAndAcquire(
        OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Obtains the collection sharding state for the requested 'nss', along with the necessary
     * shared CSS locks that guarantee its stability. The locks will be held until the returned
     * object goes out of scope.
     */
    static ScopedCollectionShardingState acquire(OperationContext* opCtx,
                                                 const NamespaceString& nss);

    /**
     * Returns the names of the collections that have a CollectionShardingState.
     */
    static std::vector<NamespaceString> getCollectionNames(OperationContext* opCtx);

    /**
     * Reports all collections which have filtering information associated.
     */
    static void appendInfoForShardingStateCommand(OperationContext* opCtx, BSONObjBuilder* builder);

    /**
     * Returns StaleShardCollectionMetadataHandler object that can be used to react to Stale Shard
     * exceptions.
     */
    static const StaleShardCollectionMetadataHandler& getStaleShardExceptionHandler(
        OperationContext* opCtx);

    /**
     * If the shard currently doesn't know whether the collection is sharded or not, it will throw a
     * StaleConfig error.
     *
     * If the request doesn't have a shard version all collections will be treated as UNTRACKED.
     */
    virtual ScopedCollectionDescription getCollectionDescription(OperationContext* opCtx) const = 0;

    virtual ScopedCollectionDescription getCollectionDescription(
        OperationContext* opCtx, bool operationIsVersioned) const = 0;

    /**
     * This method must be called with an OperationShardingState, which specifies an expected shard
     * version for the collection and it will invariant otherwise.
     *
     * If the shard currently doesn't know whether the collection is sharded or not, or if the
     * expected shard version doesn't match with the one in the OperationShardingState, it will
     * throw a StaleConfig error.
     *
     * If the operation context contains an 'atClusterTime', the returned filtering object will be
     * tied to a specific point in time. Otherwise, it will reference the latest cluster time
     * available.
     *
     * If 'kDisallowOrphanCleanup' is passed as 'OrphanCleanupPolicy', the range deleter won't
     * delete any orphan chunk associated with this ScopedCollectionFilter until the object is
     * destroyed. The intended users of this mode are read operations, which need to yield the
     * collection lock, but still perform filtering.
     *
     * The 'supportNonVersionedOperations' parameter states whether this function should consider
     * operations that don't have a shard version.
     * If the request doesn't have a shard version:
     *    - this function will invariant if !supportNonVersionedOperations (default value)
     *    - the collection will be treated as UNTRACKED otherwise.
     *
     * Use 'getCollectionDescription' for other cases, like obtaining information about
     * sharding-related properties of the collection are necessary that won't change under
     * collection IX/IS lock (e.g., isSharded or the shard key).
     */
    enum class OrphanCleanupPolicy { kDisallowOrphanCleanup, kAllowOrphanCleanup };
    virtual ScopedCollectionFilter getOwnershipFilter(
        OperationContext* opCtx,
        OrphanCleanupPolicy orphanCleanupPolicy,
        bool supportNonVersionedOperations = false) const = 0;

    virtual ScopedCollectionFilter getOwnershipFilter(
        OperationContext* opCtx,
        OrphanCleanupPolicy orphanCleanupPolicy,
        const ShardVersion& receivedShardVersion) const = 0;

    /**
     * Checks whether the shard version in the operation context is compatible with the shard
     * version of the collection and if not, throws StaleConfig error populated with the received
     * and wanted versions.
     *
     * If the request is not versioned all collections will be treated as UNTRACKED.
     */
    virtual void checkShardVersionOrThrow(OperationContext* opCtx) const = 0;

    virtual void checkShardVersionOrThrow(OperationContext* opCtx,
                                          const ShardVersion& receivedShardVersion) const = 0;

    /**
     * Appends information about the shard version of the collection.
     */
    virtual void appendShardVersion(BSONObjBuilder* builder) const = 0;
};

/**
 * Singleton factory to instantiate CollectionShardingState objects specific to the type of instance
 * which is running.
 */
class MONGO_MOD_NEEDS_REPLACEMENT CollectionShardingStateFactory {
    CollectionShardingStateFactory(const CollectionShardingStateFactory&) = delete;
    CollectionShardingStateFactory& operator=(const CollectionShardingStateFactory&) = delete;

public:
    static void set(ServiceContext* service,
                    std::unique_ptr<CollectionShardingStateFactory> factory);
    static void clear(ServiceContext* service);

    virtual ~CollectionShardingStateFactory() = default;

    /**
     * Called by the CollectionShardingState::acquire method once per newly cached namespace. It is
     * invoked under a mutex and must not acquire any locks or do blocking work.
     *
     * Implementations must be thread-safe when called from multiple threads.
     */
    virtual std::unique_ptr<CollectionShardingState> make(const NamespaceString& nss) = 0;

    /**
     * Called by the CollectionShardingState::getStaleShardExceptionHandler. Constructs a
     * StaleShardExceptionHandler object that can be used to react to Stale Shard exceptions.
     */
    virtual const StaleShardCollectionMetadataHandler& getStaleShardExceptionHandler() const = 0;

protected:
    CollectionShardingStateFactory() = default;
};

}  // namespace mongo
