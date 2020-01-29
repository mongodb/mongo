
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/s/sharding_migration_critical_section.h"

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
    MONGO_DISALLOW_COPYING(CollectionShardingState);

public:
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
     * Reports all collections which have filtering information associated.
     */
    static void report(OperationContext* opCtx, BSONObjBuilder* builder);

    /**
     * Returns the chunk filtering metadata that the current operation should be using for that
     * collection or otherwise throws if it has not been loaded yet. If the operation does not
     * require a specific shard version, returns an UNSHARDED metadata. The returned object is safe
     * to access outside of collection lock.
     *
     * If the operation context contains an 'atClusterTime' property, the returned filtering
     * metadata will be tied to a specific point in time. Otherwise it will reference the latest
     * time available.
     */
    ScopedCollectionMetadata getMetadataForOperation(OperationContext* opCtx);
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
     * Methods to control the collection's critical section. Must be called with the collection X
     * lock held.
     */
    void enterCriticalSectionCatchUpPhase(OperationContext* opCtx);
    void enterCriticalSectionCommitPhase(OperationContext* opCtx);
    void exitCriticalSection(OperationContext* opCtx);

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
    MONGO_DISALLOW_COPYING(CollectionShardingStateFactory);

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
