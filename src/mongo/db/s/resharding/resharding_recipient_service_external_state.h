/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/functional.h"
#include "mongo/util/uuid.h"

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
        StringData reason) = 0;

    virtual MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        boost::optional<Timestamp> afterClusterTime,
        StringData reason,
        const ShardId& fromShardId) = 0;

    virtual MigrationDestinationManager::IndexesAndIdIndex getCollectionIndexes(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        Timestamp afterClusterTime,
        StringData reason,
        bool expandSimpleCollation = true) = 0;

    virtual void route(OperationContext* opCtx,
                       const NamespaceString& nss,
                       StringData reason,
                       unique_function<void(OperationContext* opCtx,
                                            const CollectionRoutingInfo& cri)> callback) = 0;

    virtual void updateCoordinatorDocument(OperationContext* opCtx,
                                           const BSONObj& query,
                                           const BSONObj& update) = 0;

    virtual void clearFilteringMetadataOnTempReshardingCollection(
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
        StringData reason) override;

    MigrationDestinationManager::CollectionOptionsAndUUID getCollectionOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        boost::optional<Timestamp> afterClusterTime,
        StringData reason,
        const ShardId& fromShardId) override;

    MigrationDestinationManager::IndexesAndIdIndex getCollectionIndexes(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        Timestamp afterClusterTime,
        StringData reason,
        bool expandSimpleCollation = true) override;


    void route(OperationContext* opCtx,
               const NamespaceString& nss,
               StringData reason,
               unique_function<void(OperationContext* opCtx, const CollectionRoutingInfo& cri)>
                   callback) override;

    void updateCoordinatorDocument(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update) override;

    void clearFilteringMetadataOnTempReshardingCollection(
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
