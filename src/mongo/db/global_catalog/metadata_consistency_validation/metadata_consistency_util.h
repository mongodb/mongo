/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_types_gen.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/sharding_environment/shard_id.h"

#include <memory>
#include <vector>

namespace mongo {
namespace metadata_consistency_util {

/**
 * Creates a MetadataInconsistencyItem object from the given parameters.
 */
template <typename MetadataDetailsType>
MetadataInconsistencyItem makeInconsistency(const MetadataInconsistencyTypeEnum& type,
                                            const MetadataDetailsType& details) {
    return {type,
            std::string{MetadataInconsistencyDescription_serializer(
                static_cast<MetadataInconsistencyDescriptionEnum>(type))},
            details.toBSON()};
}

/**
 * Returns the command level for the given namespace.
 */
MetadataConsistencyCommandLevelEnum getCommandLevel(const NamespaceString& nss);

/**
 * Creates a queued data plan executor for the given list of inconsistencies
 */
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeQueuedPlanExecutor(
    OperationContext* opCtx,
    std::vector<MetadataInconsistencyItem>&& inconsistencies,
    const NamespaceString& nss);

/**
 * Construct a initial cursor reply from the given client cursor.
 * The returned reply is populated with the first batch result.
 */
CursorInitialReply createInitialCursorReplyMongod(OperationContext* opCtx,
                                                  ClientCursorParams&& cursorParams,
                                                  long long batchSize);
/**
 * Returns a list of inconsistencies between the collections' metadata on the shard and the
 * collections' metadata in the config server. Setting the parameter checkRangeDeletionIndexes
 * to true activates an optional check to identify inconsistencies when a collection has an
 * outstanding range deletion without a supporting shard key index.
 *
 * The list of inconsistencies is returned as a vector of MetadataInconsistencies objects. If
 * there is no inconsistency, it is returned an empty vector.
 */
std::vector<MetadataInconsistencyItem> checkCollectionMetadataConsistency(
    OperationContext* opCtx,
    const ShardId& shardId,
    const ShardId& primaryShardId,
    const std::vector<CollectionType>& shardingCatalogCollections,
    const std::vector<CollectionPtr>& localCatalogCollections,
    bool checkRangeDeletionIndexes);

/**
 * For every collection, check that all the shards currently owning chunks for that collection have
 * exactly the same indexes.
 * It is only safe to call this function under the database/collection DDL lock in 'S' mode.
 *
 * The list of inconsistencies is returned as a vector of MetadataInconsistencies objects. If
 * there is no inconsistency, it is returned an empty vector.
 */
std::vector<MetadataInconsistencyItem> checkIndexesConsistencyAcrossShards(
    OperationContext* opCtx, const std::vector<CollectionType>& collections);

/**
 * For every collection, check that all the shards currently owning chunks and the DBPrimary shard
 * for that collection have exactly the same collection metadata (excluding indexes).
 * It is only safe to call this function under the database/collection DDL lock in 'S' mode.
 *
 * The list of inconsistencies is returned as a vector of MetadataInconsistencies objects. If
 * there is no inconsistency, it is returned an empty vector.
 */
std::vector<MetadataInconsistencyItem> checkCollectionMetadataConsistencyAcrossShards(
    OperationContext* opCtx, const std::vector<CollectionType>& collections);

/**
 * Check different types of inconsistencies from the chunks persisted in 'config.chunks' of the
 * given collection.
 *
 * The list of inconsistencies is returned as a vector of MetadataInconsistencies objects. If
 * there is no inconsistency, it is returned an empty vector.
 *
 * This method can only be called from the config server.
 */
std::vector<MetadataInconsistencyItem> checkChunksConsistency(OperationContext* opCtx,
                                                              const CollectionType& collection);

/**
 * Check different types of inconsistencies from a given set of zones owned by a collection.
 *
 * The list of inconsistencies is returned as a vector of MetadataInconsistencies objects. If
 * there is no inconsistency, it is returned an empty vector.
 */
std::vector<MetadataInconsistencyItem> checkZonesConsistency(OperationContext* opCtx,
                                                             const CollectionType& collection,
                                                             const std::vector<TagsType>& zones);

/*
 * Return a list of inconsistencies within the sharding catalog collection metadata
 *
 * The list of inconsistencies is returned as a vector of MetadataInconsistencies objects. If
 * there is no inconsistency, it is returned an empty vector.
 */
std::vector<MetadataInconsistencyItem> checkCollectionShardingMetadataConsistency(
    OperationContext* opCtx, const CollectionType& collection);

/**
 * Checks for inconsistencies in the database's metadata between the global catalog and the
 * shard catalog.
 *
 * The list of inconsistencies is returned as a vector of MetadataInconsistencies objects. If
 * there is no inconsistency, it returns an empty vector.
 */
std::vector<MetadataInconsistencyItem> checkDatabaseMetadataConsistency(
    OperationContext* opCtx, const DatabaseType& dbInGlobalCatalog);

}  // namespace metadata_consistency_util
}  // namespace mongo
