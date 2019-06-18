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

#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/s/sharding_migration_critical_section.h"
#include "mongo/db/s/sharding_state_lock.h"

namespace mongo {

/**
 * Each collection on a mongod instance is dynamically assigned two pieces of information for the
 * duration of its lifetime:
 *  CollectionShardingState - this is a passive data-only state, which represents what is the
 * shard's knowledge of its the shard version and the set of chunks that it owns.
 *  CollectionShardingRuntime (missing from the embedded mongod) - this is the heavyweight machinery
 * which implements the sharding protocol functions and is what controls the data-only state.
 *
 * The CollectionShardingStateFactory class below is used in order to allow for the collection
 * runtime to be instantiated separately from the sharding state.
 *
 * Synchronization rule: In order to obtain an instance of this object, the caller must have some
 * lock on the respective collection.
 */
class CollectionShardingState {
    CollectionShardingState(const CollectionShardingState&) = delete;
    CollectionShardingState& operator=(const CollectionShardingState&) = delete;

public:
    using CSRLock = ShardingStateLock<CollectionShardingState>;

    virtual ~CollectionShardingState() = default;

    /**
     * Obtains the sharding state for the specified collection. If it does not exist, it will be
     * created and will remain in memory until the collection is dropped.
     *
     * Must be called with some lock held on the specific collection being looked up and the
     * returned pointer must not be stored.
     */
    static CollectionShardingState* get(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * It is the caller's responsibility to ensure that the collection locks for this namespace are
     * held when this is called. The returned pointer should never be stored.
     */
    static CollectionShardingState* get_UNSAFE(ServiceContext* svcCtx, const NamespaceString& nss);

    /**
     * Reports all collections which have filtering information associated.
     */
    static void report(OperationContext* opCtx, BSONObjBuilder* builder);

    /**
     * Returns the orphan chunk filtering metadata that the current operation should be using for
     * the collection.
     *
     * If the operation context contains an 'atClusterTime', the returned filtering metadata will be
     * tied to a specific point in time. Otherwise, it will reference the latest time available. If
     * the operation is not associated with a shard version (refer to
     * OperationShardingState::isOperationVersioned for more info on that), returns an UNSHARDED
     * metadata object.
     *
     * The intended users of this method are callers which need to perform orphan filtering. Use
     * 'getCurrentMetadata' for other cases, like obtaining information about sharding-related
     * properties of the collection are necessary that won't change under collection IX/IS lock
     * (e.g., isSharded or the shard key).
     *
     * The returned object is safe to access even after the collection lock has been dropped.
     */
    ScopedCollectionMetadata getOrphansFilter(OperationContext* opCtx);

    /**
     * See the comments for 'getOrphansFilter' above for more information on this method.
     */
    ScopedCollectionMetadata getCurrentMetadata();

    /**
     * Returns boost::none if the filtering metadata for the collection is not known yet. Otherwise
     * returns the most recently refreshed from the config server metadata or shard version.
     *
     * These methods do not check for the shard version that the operation requires and should only
     * be used for cases such as checking whether a particular config server update has taken
     * effect.
     */
    boost::optional<ScopedCollectionMetadata> getCurrentMetadataIfKnown();
    boost::optional<ChunkVersion> getCurrentShardVersionIfKnown();

    /**
     * Checks whether the shard version in the operation context is compatible with the shard
     * version of the collection and if not, throws StaleConfigException populated with the received
     * and wanted versions.
     */
    void checkShardVersionOrThrow(OperationContext* opCtx);

    /**
     * Methods to control the collection's critical section. Methods listed below must be called
     * with both the collection lock and CollectionShardingRuntimeLock held in exclusive mode.
     *
     * In these methods, the CollectionShardingRuntimeLock ensures concurrent access to the
     * critical section.
     */
    void enterCriticalSectionCatchUpPhase(OperationContext* opCtx, CSRLock&);
    void enterCriticalSectionCommitPhase(OperationContext* opCtx, CSRLock&);


    /**
     * Method to control the collection's critical secion. Method listed below must be called with
     * the collection lock in IX mode and the CollectionShardingRuntimeLock in exclusive mode.
     *
     * In this method, the CollectionShardingRuntimeLock ensures concurrent access to the
     * critical section.
     */
    void exitCriticalSection(OperationContext* opCtx, CSRLock&);

    /**
     * If the collection is currently in a critical section, returns the critical section signal to
     * be waited on. Otherwise, returns nullptr.
     */
    auto getCriticalSectionSignal(ShardingMigrationCriticalSection::Operation op) const {
        return _critSec.getSignal(op);
    }

protected:
    CollectionShardingState(NamespaceString nss);

private:
    friend CSRLock;

    /**
     * Returns the latest version of collection metadata with filtering configured for
     * atClusterTime if specified.
     */
    boost::optional<ScopedCollectionMetadata> _getMetadataWithVersionCheckAt(
        OperationContext* opCtx, const boost::optional<mongo::LogicalTime>& atClusterTime);

    // Object-wide ResourceMutex to protect changes to the CollectionShardingRuntime or objects
    // held within. Use only the CollectionShardingRuntimeLock to lock this mutex.
    Lock::ResourceMutex _stateChangeMutex;

    // Namespace this state belongs to.
    const NamespaceString _nss;

    // Tracks the migration critical section state for this collection.
    ShardingMigrationCriticalSection _critSec;

    /**
     * Obtains the current metadata for the collection or boost::none if the metadata is not yet
     * known
     */
    virtual boost::optional<ScopedCollectionMetadata> _getMetadata(
        const boost::optional<mongo::LogicalTime>& atClusterTime) = 0;
};

/**
 * Singleton factory to instantiate CollectionShardingState objects specific to the type of instance
 * which is running.
 */
class CollectionShardingStateFactory {
    CollectionShardingStateFactory(const CollectionShardingStateFactory&) = delete;
    CollectionShardingStateFactory& operator=(const CollectionShardingStateFactory&) = delete;

public:
    static void set(ServiceContext* service,
                    std::unique_ptr<CollectionShardingStateFactory> factory);
    static void clear(ServiceContext* service);

    virtual ~CollectionShardingStateFactory() = default;

    /**
     * Called by the CollectionShardingState::get method once per newly cached namespace. It is
     * invoked under a mutex and must not acquire any locks or do blocking work.
     *
     * Implementations must be thread-safe when called from multiple threads.
     */
    virtual std::unique_ptr<CollectionShardingState> make(const NamespaceString& nss) = 0;

protected:
    CollectionShardingStateFactory(ServiceContext* serviceContext)
        : _serviceContext(serviceContext) {}

    // The service context which owns this factory
    ServiceContext* const _serviceContext;
};

}  // namespace mongo
