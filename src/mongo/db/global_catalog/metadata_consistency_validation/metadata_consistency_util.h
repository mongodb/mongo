// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_types_gen.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace metadata_consistency_util {

/**
 * Creates a MetadataInconsistencyItem object from the given parameters.
 */
template <typename MetadataDetailsType>
MetadataInconsistencyItem makeInconsistency(
    const MetadataInconsistencyTypeEnum& type,
    const MetadataDetailsType& details,
    boost::optional<MetadataInconsistencySeverityEnum> severity = boost::none) {
    MetadataInconsistencyItem item{
        type,
        std::string{idl::serialize(static_cast<MetadataInconsistencyDescriptionEnum>(type))},
        details.toBSON()};
    item.setSeverity(severity);
    return item;
}

/**
 * Returns the command level for the given namespace.
 */
MetadataConsistencyCommandLevelEnum getCommandLevel(const NamespaceString& nss);

/**
 * Construct a initial cursor reply from the given client cursor.
 * The returned reply is populated with the first batch result.
 * If `planExecutor` is not null, it uses it, and the `inconsistencies` vector is prepended to the
 * executor's output (emitted before the executor's own results).
 * If null, it builds a queued plan executor with the contents of the `inconsistencies` vector.
 */
CursorInitialReply createInitialCursorReplyMongod(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::vector<MetadataInconsistencyItem>&& inconsistencies,
    const boost::optional<mongo::SimpleCursorOptions>& requestCursorOpts,
    const BSONObj& request,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> planExecutor = nullptr);

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
    std::shared_ptr<const CollectionCatalog> localCatalogSnapshot,
    const std::vector<CollectionPtr>& localCatalogCollections,
    bool checkRangeDeletionIndexes,
    bool optionalCheckIndexes,
    bool asRSPrimaryNode = true);

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

/**
 * Checks that this shard's config database shard catalog collections match the current FCV
 * (e.g. on FCV 9.0+, the legacy config.cache.* collections should not exist).
 */
std::vector<MetadataInconsistencyItem> checkShardCatalogCollectionsConsistentWithAuthoritativeness(
    OperationContext* opCtx);

/**
 * Main check consistency metadata logic ran by the participant commands (i.e.
 * _shardsvrCheckMetadataConsistencyParticipant and
 * _shardsvrCheckMetadataConsistencySecondaryParticipant).
 * `asRSPrimaryNode` is set to true if this command is called by the shard *RS* primary (not the
 * shard DB primary). Setting it to false skips some cluster-wide checks.
 */
std::vector<MetadataInconsistencyItem> runCheckMetadataConsistencyOnParticipant(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardId& primaryShardId,
    bool checkRangeDeletionIndexes,
    bool checkIndexes,
    bool asRSPrimaryNode);

}  // namespace metadata_consistency_util
}  // namespace mongo
