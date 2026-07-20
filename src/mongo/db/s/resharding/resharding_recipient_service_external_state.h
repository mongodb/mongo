// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;
class CommonReshardingMetadata;
class NamespaceString;
class OperationContext;
class ServiceContext;

/**
 * Represents the interface that RecipientStateMachine uses to interact with the rest of the
 * sharding codebase.
 *
 * In particular, RecipientStateMachine must not directly use Grid, ShardingState, or
 * ShardingCatalogClient. RecipientStateMachine must instead access those types through the
 * RecipientStateMachineExternalState interface. Having it behind an interface makes it more
 * straightforward to unit test RecipientStateMachine.
 */
class ReshardingRecipientService::RecipientStateMachineExternalState {
public:
    virtual ~RecipientStateMachineExternalState() = default;

    virtual ShardId myShardId(ServiceContext* serviceContext) const = 0;

    virtual void refreshCatalogCache(OperationContext* opCtx, const NamespaceString& nss) = 0;

    virtual CollectionRoutingInfo getTrackedCollectionRoutingInfo(OperationContext* opCtx,
                                                                  const NamespaceString& nss) = 0;

    virtual MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        boost::optional<Timestamp> afterClusterTime,
        std::string_view reason) = 0;

    virtual MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        boost::optional<Timestamp> afterClusterTime,
        std::string_view reason,
        const ShardId& fromShardId) = 0;

    virtual MigrationDestinationManager::IndexesAndIdIndex getCollectionIndexes(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        Timestamp afterClusterTime,
        std::string_view reason,
        bool expandSimpleCollation = true) = 0;

    virtual void route(OperationContext* opCtx,
                       const NamespaceString& nss,
                       std::string_view reason,
                       unique_function<void(OperationContext* opCtx,
                                            const CollectionRoutingInfo& cri)> callback) = 0;

    virtual void updateCoordinatorDocument(OperationContext* opCtx,
                                           const BSONObj& query,
                                           const BSONObj& update) = 0;

    virtual void clearCollectionMetadataOnTempReshardingCollection(
        OperationContext* opCtx, const NamespaceString& tempReshardingNss) = 0;

    virtual void ensureReshardingStashCollectionsEmpty(
        OperationContext* opCtx,
        const UUID& sourceUUID,
        const std::vector<DonorShardFetchTimestamp>& donorShards) = 0;

    /**
     * Creates the temporary resharding collection locally.
     *
     * The collection options are taken from the primary shard for the source database and the
     * collection indexes are taken from the shard which owns the global minimum chunk.
     *
     * This function won't automatically create an index on the new shard key pattern.
     */
    void ensureTempReshardingCollectionExistsWithIndexes(OperationContext* opCtx,
                                                         const CommonReshardingMetadata& metadata,
                                                         Timestamp cloneTimestamp);

    virtual std::unique_ptr<ReshardingDataReplicationInterface> makeDataReplication(
        OperationContext* opCtx,
        ReshardingMetrics* metrics,
        ReshardingApplierMetricsMap* applierMetrics,
        const CommonReshardingMetadata& metadata,
        const std::vector<DonorShardFetchTimestamp>& donorShards,
        std::size_t oplogBatchTaskCount,
        Timestamp cloneTimestamp,
        bool cloningDone,
        bool storeOplogFetcherProgress,
        bool relaxed) = 0;
};

class RecipientStateMachineExternalStateImpl
    : public ReshardingRecipientService::RecipientStateMachineExternalState {
public:
    ShardId myShardId(ServiceContext* serviceContext) const override;

    void refreshCatalogCache(OperationContext* opCtx, const NamespaceString& nss) override;

    CollectionRoutingInfo getTrackedCollectionRoutingInfo(OperationContext* opCtx,
                                                          const NamespaceString& nss) override;

    MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        boost::optional<Timestamp> afterClusterTime,
        std::string_view reason) override;

    MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        boost::optional<Timestamp> afterClusterTime,
        std::string_view reason,
        const ShardId& fromShardId) override;

    MigrationDestinationManager::IndexesAndIdIndex getCollectionIndexes(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        Timestamp afterClusterTime,
        std::string_view reason,
        bool expandSimpleCollation = true) override;


    void route(OperationContext* opCtx,
               const NamespaceString& nss,
               std::string_view reason,
               unique_function<void(OperationContext* opCtx, const CollectionRoutingInfo& cri)>
                   callback) override;

    void updateCoordinatorDocument(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update) override;

    void clearCollectionMetadataOnTempReshardingCollection(
        OperationContext* opCtx, const NamespaceString& tempReshardingNss) override;

    std::unique_ptr<ReshardingDataReplicationInterface> makeDataReplication(
        OperationContext* opCtx,
        ReshardingMetrics* metrics,
        ReshardingApplierMetricsMap* applierMetrics,
        const CommonReshardingMetadata& metadata,
        const std::vector<DonorShardFetchTimestamp>& donorShards,
        std::size_t oplogBatchTaskCount,
        Timestamp cloneTimestamp,
        bool cloningDone,
        bool storeOplogFetcherProgress,
        bool relaxed) override;

    void ensureReshardingStashCollectionsEmpty(
        OperationContext* opCtx,
        const UUID& sourceUUID,
        const std::vector<DonorShardFetchTimestamp>& donorShards) override;
};

}  // namespace mongo
