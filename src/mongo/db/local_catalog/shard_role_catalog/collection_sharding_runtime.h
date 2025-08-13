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

#include "mongo/base/checked_cast.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/ddl/sharding_migration_critical_section.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/metadata_manager.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"
#include "mongo/util/modules_incompletely_marked_header.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * See the comments for CollectionShardingState for more information on how this class fits in the
 * sharding architecture.
 */
class MONGO_MOD_USE_REPLACEMENT(CollectionShardingState) CollectionShardingRuntime final
    : public CollectionShardingState,
      public Decorable<CollectionShardingRuntime> {
    CollectionShardingRuntime(const CollectionShardingRuntime&) = delete;
    CollectionShardingRuntime& operator=(const CollectionShardingRuntime&) = delete;

public:
    CollectionShardingRuntime(ServiceContext* service, NamespaceString nss);
    ~CollectionShardingRuntime() override;

    /**
     * Obtains the sharding runtime for the specified collection, along with a resource lock in
     * shared mode protecting it from concurrent modifications, which will be held until the object
     * goes out of scope.
     */
    class ScopedSharedCollectionShardingRuntime {
    public:
        ScopedSharedCollectionShardingRuntime(ScopedSharedCollectionShardingRuntime&&) = default;

        const CollectionShardingRuntime* operator->() const {
            return checked_cast<CollectionShardingRuntime*>(&*_scopedCss);
        }
        const CollectionShardingRuntime& operator*() const {
            return checked_cast<CollectionShardingRuntime&>(*_scopedCss);
        }

    private:
        friend class CollectionShardingRuntime;

        ScopedSharedCollectionShardingRuntime(ScopedCollectionShardingState&& scopedCss);

        ScopedCollectionShardingState _scopedCss;
    };

    /**
     * Obtains the sharding runtime for the specified collection, along with a resource lock in
     * exclusive mode protecting it from concurrent modifications, which will be held until the
     * object goes out of scope.
     */
    class ScopedExclusiveCollectionShardingRuntime {
    public:
        ScopedExclusiveCollectionShardingRuntime(ScopedExclusiveCollectionShardingRuntime&&) =
            default;

        CollectionShardingRuntime* operator->() const {
            return checked_cast<CollectionShardingRuntime*>(&*_scopedCss);
        }
        CollectionShardingRuntime& operator*() const {
            return checked_cast<CollectionShardingRuntime&>(*_scopedCss);
        }

    private:
        friend class CollectionShardingRuntime;

        ScopedExclusiveCollectionShardingRuntime(ScopedCollectionShardingState&& scopedCss);

        ScopedCollectionShardingState _scopedCss;
    };

    static ScopedSharedCollectionShardingRuntime assertCollectionLockedAndAcquireShared(
        OperationContext* opCtx, const NamespaceString& nss);
    static ScopedExclusiveCollectionShardingRuntime assertCollectionLockedAndAcquireExclusive(
        OperationContext* opCtx, const NamespaceString& nss);
    static ScopedSharedCollectionShardingRuntime acquireShared(OperationContext* opCtx,
                                                               const NamespaceString& nss);
    static ScopedExclusiveCollectionShardingRuntime acquireExclusive(OperationContext* opCtx,
                                                                     const NamespaceString& nss);

    static ScopedCollectionShardingState assertCollectionLockedAndAcquire(
        OperationContext* opCtx, const NamespaceString& nss) = delete;
    static ScopedCollectionShardingState acquire(OperationContext* opCtx,
                                                 const NamespaceString& nss) = delete;

    ScopedCollectionDescription getCollectionDescription(OperationContext* opCtx) const override;
    ScopedCollectionDescription getCollectionDescription(OperationContext* opCtx,
                                                         bool operationIsVersioned) const override;

    ScopedCollectionFilter getOwnershipFilter(OperationContext* opCtx,
                                              OrphanCleanupPolicy orphanCleanupPolicy,
                                              bool supportNonVersionedOperations) const override;
    ScopedCollectionFilter getOwnershipFilter(
        OperationContext* opCtx,
        OrphanCleanupPolicy orphanCleanupPolicy,
        const ShardVersion& receivedShardVersion) const override;

    void checkShardVersionOrThrow(OperationContext* opCtx) const override;

    void checkShardVersionOrThrow(OperationContext* opCtx,
                                  const ShardVersion& receivedShardVersion) const override;

    void appendShardVersion(BSONObjBuilder* builder) const override;

    /**
     * Returns boost::none if the description for the collection is not known yet. Otherwise
     * returns the most recently refreshed from the config server metadata.
     *
     * This method do not check for the shard version that the operation requires and should only
     * be used for cases such as checking whether a particular config server update has taken
     * effect.
     */
    boost::optional<CollectionMetadata> getCurrentMetadataIfKnown() const;

    /**
     * Updates the collection's filtering metadata based on changes received from the config server
     * and also resolves the pending receives map in case some of these pending receives have
     * committed on the config server or have been abandoned by the donor shard.
     *
     * This method must be called with an exclusive collection lock and it does not acquire any
     * locks itself.
     */
    void setFilteringMetadata(OperationContext* opCtx, CollectionMetadata newMetadata);

    /**
     * Marks the collection's filtering metadata as UNKNOWN, meaning that all attempts to check for
     * shard version match will fail with StaleConfig errors in order to trigger an update.
     *
     * Interrupts any ongoing shard metadata refresh.
     *
     * It is safe to call this method with only an intent lock on the collection (as opposed to
     * setFilteringMetadata which requires exclusive).
     */
    void clearFilteringMetadata(OperationContext* opCtx);

    /**
     * Calls to clearFilteringMetadata + clears the _metadataManager object.
     */
    void clearFilteringMetadataForDroppedCollection(OperationContext* opCtx);

    /**
     * Methods to control the collection's critical section. Methods listed below must be called
     * with both the collection lock and CSRLock held in exclusive mode.
     *
     * In these methods, the CSRLock ensures concurrent access to the critical section.
     *
     * Entering into the Critical Section interrupts any ongoing filtering metadata refresh.
     */
    void enterCriticalSectionCatchUpPhase(const BSONObj& reason);
    void enterCriticalSectionCommitPhase(const BSONObj& reason);

    /**
     * It transitions the critical section back to the catch up phase.
     */
    void rollbackCriticalSectionCommitPhaseToCatchUpPhase(const BSONObj& reason);

    /**
     * Method to control the collection's critical section. Methods listed below must be called with
     * both the collection lock and CSR acquired in exclusive mode.
     */
    void exitCriticalSection(const BSONObj& reason);

    /**
     * Same semantics than 'exitCriticalSection' but without doing error-checking. Only meant to be
     * used when recovering the critical sections in the RecoverableCriticalSectionService.
     */
    void exitCriticalSectionNoChecks();

    /**
     * If the collection is currently in a critical section, returns the critical section signal to
     * be waited on. Otherwise, returns nullptr.
     */
    boost::optional<SharedSemiFuture<void>> getCriticalSectionSignal(
        ShardingMigrationCriticalSection::Operation op) const;

    /**
     * Waits for all ranges deletion tasks with UUID 'collectionUuid' overlapping range
     * 'orphanRange' to be processed, even if the collection does not exist in the storage catalog.
     * It will block until the minimum of the operation context's timeout deadline or 'deadline' is
     * reached.
     */
    static Status waitForClean(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const UUID& collectionUuid,
                               ChunkRange orphanRange,
                               Date_t deadline);

    /**
     * Returns a future marked as ready when all the ongoing queries retaining the range complete
     */
    SharedSemiFuture<void> getOngoingQueriesCompletionFuture(const UUID& collectionUuid,
                                                             ChunkRange const& range) const;

    /**
     * Initializes the placement version recover/refresh shared semifuture for other threads to wait
     * on it.
     *
     * To invoke this method, the criticalSectionSignal must not be hold by a different thread.
     */
    void setPlacementVersionRecoverRefreshFuture(SharedSemiFuture<void> future,
                                                 CancellationSource cancellationSource);

    /**
     * If there an ongoing placement version recover/refresh, it returns the shared semifuture to be
     * waited on. Otherwise, returns boost::none.
     */
    boost::optional<SharedSemiFuture<void>> getMetadataRefreshFuture() const;

    /**
     * Resets the placement version recover/refresh shared semifuture to boost::none.
     */
    void resetPlacementVersionRecoverRefreshFuture();

    /**
     * It provides a mechanism to invalidate RangePreservers that can no longer be fulfilled because
     * of an incoming RangeDeletion for a specified shard version. It invalidates all metadata
     * trackers when shardVersion is lower than or equal to the given version. This method ensures
     * that metadata operation is performed in a thread-safe manner.
     */
    void invalidateRangePreserversOlderThanShardVersion(OperationContext* opCtx,
                                                        const ChunkVersion& shardVersion);

private:
    friend class CollectionShardingRuntimeTest;

    struct PlacementVersionRecoverOrRefresh {
    public:
        PlacementVersionRecoverOrRefresh(SharedSemiFuture<void> future,
                                         CancellationSource cancellationSource)
            : future(std::move(future)), cancellationSource(std::move(cancellationSource)) {};

        // Tracks ongoing placement version recover/refresh.
        SharedSemiFuture<void> future;

        // Cancellation source to cancel the ongoing recover/refresh placement version.
        CancellationSource cancellationSource;
    };

    /**
     * Returns the latest version of collection metadata with filtering configured for
     * atClusterTime if specified.
     */
    std::shared_ptr<ScopedCollectionDescription::Impl> _getCurrentMetadataIfKnown(
        const boost::optional<LogicalTime>& atClusterTime, bool preserveRange) const;

    /**
     * Returns the latest version of collection metadata with filtering configured for
     * atClusterTime if specified. Throws StaleConfigInfo if the shard version attached to the
     * operation context does not match the shard version on the active metadata object.
     */
    std::shared_ptr<ScopedCollectionDescription::Impl> _getMetadataWithVersionCheckAt(
        OperationContext* opCtx,
        const boost::optional<mongo::LogicalTime>& atClusterTime,
        const boost::optional<ShardVersion>& optReceivedShardVersion,
        bool preserveRange,
        bool supportNonVersionedOperations = false) const;

    /**
     * Auxiliary function used to implement the different flavours of clearFilteringMetadata.
     */
    void _clearFilteringMetadata(OperationContext* opCtx, bool collIsDropped);

    /**
     * This function cleans up some state associated with the current sharded metadata before it's
     * replaced by the new metadata.
     */
    void _cleanupBeforeInstallingNewCollectionMetadata(WithLock, OperationContext* opCtx);

    // The service context under which this instance runs
    ServiceContext* const _serviceContext;

    // Namespace this state belongs to.
    const NamespaceString _nss;

    // Tracks the migration critical section state for this collection.
    ShardingMigrationCriticalSection _critSec;

    // Protects state around the metadata manager below
    mutable stdx::mutex _metadataManagerLock;

    // Track status of filtering metadata for a specific collection
    enum class MetadataType {
        kUnknown,    // metadata is not known to this node
        kUntracked,  // no metadata found in the sharding catalog
        kTracked     // metadata for this collection is registered in the sharding catalog
    } _metadataType;

    // If the collection state is known and is untracked, this will be nullptr.
    //
    // If the collection state is known and is tracked, this will point to the metadata
    // associated with this collection.
    //
    // If the collection state is unknown:
    // - If the metadata had never been set yet, this will be nullptr.
    // - If the collection state was known and was sharded, this contains the metadata that
    // were known for the collection before the last invocation of clearFilteringMetadata().
    //
    // The following matrix enumerates the valid (Y) and invalid (X) scenarios.
    //                          __________________________________
    //                         | _metadataType (collection state) |
    //                         |__________________________________|
    //                         | UNKNOWN | UNTRACKED |  TRACKED   |
    //  _______________________|_________|___________|____________|
    // |_metadataManager unset |    Y    |     Y     |     X      |
    // |_______________________|_________|___________|____________|
    // |_metadataManager set   |    Y    |     X     |     Y      |
    // |_______________________|_________|___________|____________|
    std::shared_ptr<MetadataManager> _metadataManager;

    // Used for testing to check the number of times a new MetadataManager has been installed.
    std::uint64_t _numMetadataManagerChanges{0};

    // Tracks ongoing placement version recover/refresh. Eventually set to the semifuture to wait on
    // and a CancellationSource to cancel it
    boost::optional<PlacementVersionRecoverOrRefresh> _placementVersionInRecoverOrRefresh;
};

/**
 * RAII-style class, which obtains a reference to the critical section for the specified collection.
 *
 *
 * Placement version recovery/refresh procedures always wait for the critical section to be released
 * in order to serialise with concurrent moveChunk/shardCollection commit operations.
 *
 * Entering the critical section doesn't serialise with concurrent recovery/refresh, because
 * causally such refreshes would have happened *before* the critical section was entered.
 */
class CollectionCriticalSection {
    CollectionCriticalSection(const CollectionCriticalSection&) = delete;
    CollectionCriticalSection& operator=(const CollectionCriticalSection&) = delete;

public:
    CollectionCriticalSection(OperationContext* opCtx, NamespaceString nss, BSONObj reason);
    ~CollectionCriticalSection();

    /**
     * Enters the commit phase of the critical section and blocks reads.
     */
    void enterCommitPhase();

private:
    OperationContext* const _opCtx;

    NamespaceString _nss;
    const BSONObj _reason;
};

}  // namespace mongo
