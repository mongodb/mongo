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
 * Each collection on a mongod instance is dynamically assigned an implementation of
 * CollectionShardingState for the duration of its lifetime, which represents the
 * shard's knowledge of its the shard version and the set of chunks that it owns, as well as
 * functions for updating and tracking this state.
 *
 * On shard servers, the implementation used is CollectionShardingRuntime.
 *
 * On embedded or non-shard servers, the implementation used is CollectionShardingStateStandalone,
 * which is a mostly empty implementation.
 *
 * This separation was required for linking reasons and the difference between sharded and not
 * sharded clusters.
 *
 * The CollectionShardingStateFactory class below is used to instantiate the correct subclass of
 * CollectionShardingState at runtime.
 *
 * Synchronization rule: In order to obtain an instance of this object, the caller must have some
 * lock on the respective collection. Different functions require different lock levels though, so
 * be sure to check the function-level comments for details.
 */
class CollectionShardingState {
public:
    CollectionShardingState() = default;
    virtual ~CollectionShardingState() = default;

    CollectionShardingState(const CollectionShardingState&) = delete;
    CollectionShardingState& operator=(const CollectionShardingState&) = delete;

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
     * Attaches info for server status.
     */
    static void appendInfoForServerStatus(OperationContext* opCtx, BSONObjBuilder* builder);

    /**
     * Returns the chunk filtering object that the current operation should be using for
     * the collection.
     *
     * If the operation context contains an 'atClusterTime', the returned filtering object will be
     * tied to a specific point in time. Otherwise, it will reference the latest time available. If
     * the operation is not associated with a shard version (refer to
     * OperationShardingState::isOperationVersioned for more info on that), returns an UNSHARDED
     * metadata object.
     * If 'kDisallowOrphanCleanup' is passed as 'OrphanCleanupPolicy', the range deleter won't
     * delete any orphan chunk associated with this ScopedCollectionFilter until the object is
     * destroyed. The intended users of this method are callers which need to perform filtering. Use
     * 'getCurrentMetadata' for other cases, like obtaining information about sharding-related
     * properties of the collection are necessary that won't change under collection IX/IS lock
     * (e.g., isSharded or the shard key).
     *
     * The returned object is safe to access even after the collection lock has been dropped.
     */

    enum class OrphanCleanupPolicy { kDisallowOrphanCleanup, kAllowOrphanCleanup };

    virtual ScopedCollectionFilter getOwnershipFilter(OperationContext* opCtx,
                                                      OrphanCleanupPolicy orphanCleanupPolicy) = 0;

    /**
     * See the comments for 'getOwnershipFilter' above for more information on this method.
     */
    virtual ScopedCollectionDescription getCollectionDescription() = 0;

    /**
     * Returns boost::none if the description for the collection is not known yet. Otherwise
     * returns the most recently refreshed from the config server metadata or shard version.
     *
     * These methods do not check for the shard version that the operation requires and should only
     * be used for cases such as checking whether a particular config server update has taken
     * effect.
     */
    virtual boost::optional<ScopedCollectionDescription> getCurrentMetadataIfKnown() = 0;
    virtual boost::optional<ChunkVersion> getCurrentShardVersionIfKnown() = 0;

    /**
     * Checks whether the shard version in the operation context is compatible with the shard
     * version of the collection and if not, throws StaleConfigException populated with the received
     * and wanted versions.
     */
    virtual void checkShardVersionOrThrow(OperationContext* opCtx) = 0;

    /**
     * Similar to checkShardVersionOrThrow but returns a status instead of throwing.
     */
    virtual Status checkShardVersionNoThrow(OperationContext* opCtx) noexcept = 0;

    /**
     * Methods to control the collection's critical section. Methods listed below must be called
     * with both the collection lock and CSRLock held in exclusive mode.
     *
     * In these methods, the CSRLock ensures concurrent access to the
     * critical section.
     */
    virtual void enterCriticalSectionCatchUpPhase(OperationContext* opCtx) = 0;
    virtual void enterCriticalSectionCommitPhase(OperationContext* opCtx) = 0;

    /**
     * Method to control the collection's critical secion. Method listed below must be called with
     * the collection lock in IX mode and the CSRLock in exclusive mode.
     *
     * In this method, the CSRLock ensures concurrent access to the
     * critical section.
     */
    virtual void exitCriticalSection(OperationContext* opCtx) = 0;

    /**
     * If the collection is currently in a critical section, returns the critical section signal to
     * be waited on. Otherwise, returns nullptr.
     */
    virtual std::shared_ptr<Notification<void>> getCriticalSectionSignal(
        ShardingMigrationCriticalSection::Operation op) const = 0;

    /**
     * BSON output of the pending metadata into a BSONArray
     * used for reporting/diagnostic purposes only
     */
    virtual void toBSONPending(BSONArrayBuilder& bb) const = 0;

    /**
     * Updates the collection's filtering metadata based on changes received from the config server
     * and also resolves the pending receives map in case some of these pending receives have
     * committed on the config server or have been abandoned by the donor shard.
     *
     * This method must be called with an exclusive collection lock and it does not acquire any
     * locks itself.
     */
    virtual void setFilteringMetadata(OperationContext* opCtx, CollectionMetadata newMetadata) = 0;

    /**
     * Append info to display in server status.
     */
    virtual void appendInfoForServerStatus(BSONArrayBuilder* builder) = 0;
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
     * Must be called prior to destruction to wait for any ongoing work to complete.
     */
    virtual void join() = 0;

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
