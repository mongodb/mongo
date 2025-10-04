/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util_detail.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/local_catalog/drop_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>


namespace mongo {

// Forward declarations
enum class AuthoritativeMetadataAccessLevelEnum : std::int32_t;
class NamespacePlacementChanged;

namespace sharding_ddl_util {

template <typename CommandType>
std::vector<AsyncRequestsSender::Response> sendAuthenticatedCommandToShards(
    OperationContext* opCtx,
    std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> originalOpts,
    const std::map<ShardId, ShardVersion>& shardIdsToShardVersions,
    const ReadPreferenceSetting readPref,
    bool throwOnError) {
    std::vector<ShardId> shardIds;
    std::vector<ShardVersion> shardVersions;
    for (auto const& [shardId, shardVersion] : shardIdsToShardVersions) {
        shardIds.push_back(shardId);
        shardVersions.push_back(shardVersion);
    }
    return sharding_ddl_util_detail::sendAuthenticatedCommandToShards(
        opCtx, originalOpts, shardIds, shardVersions, readPref, throwOnError);
}

template <typename CommandType>
std::vector<AsyncRequestsSender::Response> sendAuthenticatedCommandToShards(
    OperationContext* opCtx,
    std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> originalOpts,
    const std::vector<ShardId>& shardIds,
    bool throwOnError = true) {
    return sharding_ddl_util_detail::sendAuthenticatedCommandToShards(
        opCtx,
        originalOpts,
        shardIds,
        boost::none /* shardVersions */,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        throwOnError);
}

/**
 * Given a Status, returns a new truncated version of the Status or a copy of the Status.
 */
Status possiblyTruncateErrorStatus(const Status& status);

/**
 * Creates a barrier after which we are guaranteed that all writes to the config server performed by
 * the previous primary have been majority commited and will be seen by the new primary.
 */
void linearizeCSRSReads(OperationContext* opCtx);

/**
 * Erase tags metadata from config server for the given namespace, using the _configsvrRemoveTags
 * command as a retryable write to ensure idempotency.
 */
void removeTagsMetadataFromConfig(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const OperationSessionInfo& osi);

/**
 * Erase collection metadata from config server and invalidate the locally cached one.
 * In particular remove the collection and chunks metadata associated with the given namespace.
 */
void removeCollAndChunksMetadataFromConfig(
    OperationContext* opCtx,
    const std::shared_ptr<Shard>& configShard,
    ShardingCatalogClient* catalogClient,
    const CollectionType& coll,
    const WriteConcernOptions& writeConcern,
    const OperationSessionInfo& osi,
    const std::shared_ptr<executor::TaskExecutor>& executor = nullptr,
    bool logCommitOnConfigPlacementHistory = true);

/**
 * Log the effects of a dropCollection commit by inserting a new document in config.placementHistory
 * (if not already present).
 */
void logDropCollectionCommitOnConfigPlacementHistory(
    OperationContext* opCtx,
    const NamespacePlacementType& committedPlacementChange,
    const OperationSessionInfo& osi,
    const std::shared_ptr<executor::TaskExecutor>& executor);

/**
 * Delete the query analyzer document associated to the passed in namespace.
 */
void removeQueryAnalyzerMetadata(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const OperationSessionInfo& osi);

/**
 * Delete the query analyzer documents associated to the passed in collection UUIDs
 * (note: using such a type instead of NamespaceString guarantees replay protection in step down
 * scenarios).
 */
void removeQueryAnalyzerMetadata(OperationContext* opCtx, const std::vector<UUID>& collectionUUIDs);

/**
 * Ensures rename preconditions for collections are met:
 * - Check that the namespace of the destination collection is not too long
 * - Check that `dropTarget` is true if the destination collection exists
 * - Check that no tags exist for the destination collection
 */
void checkRenamePreconditions(OperationContext* opCtx,
                              const NamespaceString& toNss,
                              const boost::optional<CollectionType>& optTargetCollType,
                              bool isSourceUnsharded,
                              bool dropTarget);

/**
 * Throws an exception if the collection is already tracked with different options.
 *
 * If the collection is already tracked with the same options, returns the existing collection's
 * full spec, else returns boost::none.
 *
 * If the collection is tracked as unsplittable and the request is for a splittable collection,
 * returns boost::none.
 */
boost::optional<CreateCollectionResponse> checkIfCollectionAlreadyTrackedWithOptions(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& key,
    const BSONObj& collation,
    bool unique,
    bool unsplittable);

/**
 * Stops ongoing migrations and prevents future ones to start for the given nss.
 * If expectedCollectionUUID is set and doesn't match that of that collection, then this is a no-op.
 * If expectedCollectionUUID is not set, no UUID check will be performed before stopping migrations.
 */
void stopMigrations(OperationContext* opCtx,
                    const NamespaceString& nss,
                    const boost::optional<UUID>& expectedCollectionUUID,
                    const boost::optional<OperationSessionInfo>& osi = boost::none);

/**
 * Resume migrations and balancing rounds for the given nss.
 * If expectedCollectionUUID is set and doesn't match that of the collection, then this is a no-op.
 * If expectedCollectionUUID is not set, no UUID check will be performed before resuming migrations.
 */
void resumeMigrations(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const boost::optional<UUID>& expectedCollectionUUID,
                      const boost::optional<OperationSessionInfo>& osi = boost::none);

/**
 * Calls to the config server primary to get the collection document for the given nss.
 * Returns the value of the allowMigrations flag on the collection document.
 */
bool checkAllowMigrations(OperationContext* opCtx, const NamespaceString& nss);

/*
 * Returns the UUID of the collection (if exists) using the catalog. It does not provide any locking
 * guarantees after the call.
 **/
boost::optional<UUID> getCollectionUUID(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        bool allowViews = false);

/*
 * Performs a noop retryable write on the given shards using the session and txNumber specified in
 * 'osi'
 */
void performNoopRetryableWriteOnShards(OperationContext* opCtx,
                                       const std::vector<ShardId>& shardIds,
                                       const OperationSessionInfo& osi,
                                       const std::shared_ptr<executor::TaskExecutor>& executor);


/*
 * Performs a noop write locally with majority write concern.
 */
void performNoopMajorityWriteLocally(OperationContext* opCtx);

/**
 * Sends the _shardsvrDropCollectionParticipant command to the specified shards.
 */
void sendDropCollectionParticipantCommandToShards(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::vector<ShardId>& shardIds,
    std::shared_ptr<executor::TaskExecutor> executor,
    const OperationSessionInfo& osi,
    bool fromMigrate,
    bool dropSystemCollections,
    const boost::optional<UUID>& collectionUUID = boost::none,
    bool requireCollectionEmpty = false);

BSONObj getCriticalSectionReasonForRename(const NamespaceString& from, const NamespaceString& to);

/**
 * Runs the given transaction chain on the catalog. Transaction will be remote if called by a shard.
 * Important: StmtsIds must be set in the transactionChain if the OperationSessionId is not empty
 * since we are spawning a transaction on behalf of a retryable operation.
 */
void runTransactionOnShardingCatalog(
    OperationContext* opCtx,
    txn_api::Callback&& transactionChain,
    const WriteConcernOptions& writeConcern,
    const OperationSessionInfo& osi,
    const std::shared_ptr<executor::TaskExecutor>& inputExecutor = nullptr);

/*
 * Same as `runTransactionOnShardingCatalog` but automatically adding StmtsIds to passed in
 * operations
 */
void runTransactionWithStmtIdsOnShardingCatalog(
    OperationContext* opCtx,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const OperationSessionInfo& osi,
    const std::vector<BatchedCommandRequest>&& ops);

/**
 * Returns the default key pattern value for unsplittable collections.
 */
const KeyPattern& unsplittableCollectionShardKey();

boost::optional<CollectionType> getCollectionFromConfigServer(OperationContext* opCtx,
                                                              const NamespaceString& nss);

/*
 * The returned operations to execute on the sharding catalog are the following:
 * 1. Delete any existing chunk entries (there can be 0 or 1 depending on whether we are
 * creating a new collection or sharding a pre-existing unsplittable collection).
 * 2. Insert new chunk entries.
 * 3. Upsert the collection entry (update in case of pre-existing unspittable collection or insert
 * if the collection did not exist).
 * 4. Insert the placement information.
 */
std::vector<BatchedCommandRequest> getOperationsToCreateOrShardCollectionOnShardingCatalog(
    const CollectionType& coll,
    const std::vector<ChunkType>& chunks,
    const ChunkVersion& placementVersion,
    const std::set<ShardId>& shardIds);

/*
 * Compose the needed metadata to request the creation of an unsplittable collection (intended to be
 * used in combination with getOperationsToCreateOrShardCollectionOnShardingCatalog()).
 */
std::pair<CollectionType, std::vector<ChunkType>> generateMetadataForUnsplittableCollectionCreation(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& collectionUuid,
    const BSONObj& defaultCollation,
    const ShardId& shardId);

/*
 * Throws IllegalOperation if the cluster is not yet blocking direct shard operations. This ensures
 * that data cannot be migrated to a new shard before all direct shard operations have been blocked.
 */
void assertDataMovementAllowed();

/*
 * Throws InvalidNamespace if the namespace length for the collection exceeds the maximum namespace
 * character limit.
 */
void assertNamespaceLengthLimit(const NamespaceString& nss, bool isUnsharded);

/**
 *  Commits a create of the database metadata to the shard catalog by sending the command
 * `_shardsvrCommitCreateDatabaseMetadata` to the appropiate shard. This command can be
 * used to update the database metadata of the shard catalog of any shard.
 */
void commitCreateDatabaseMetadataToShardCatalog(
    OperationContext* opCtx,
    const DatabaseType& db,
    const OperationSessionInfo& osi,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token);

/**
 *  Commits a drop of the database metadata to the shard catalog by sending the command
 * `_shardsvrCommitDropDatabaseMetadata` to the appropiate shard. This command can be
 * used to update the database metadata of the shard catalog of any shard.
 */
void commitDropDatabaseMetadataToShardCatalog(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const ShardId& shardId,
    const OperationSessionInfo& osi,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token);

/**
 *  Sends the `_shardsvrFetchCollMetadata` command to specified target shards.
 * Each shard will fetch the authoritative collection and chunk metadata for the given namespace
 * `nss` from the config server and persist it locally in its own shard catalog.
 */
void sendFetchCollMetadataToShards(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const std::vector<ShardId>& shardIds,
                                   const OperationSessionInfo& osi,
                                   const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                                   const CancellationToken& token);

/**
 * Based on the FCV, get the where the DDL needs to act accordingly to the database
 * authoritativeness.
 */
AuthoritativeMetadataAccessLevelEnum getGrantedAuthoritativeMetadataAccessLevel(
    const VersionContext& vCtx, const ServerGlobalParams::FCVSnapshot& snapshot);

/*
 * Provided a collection UUID, returns the ID of one of the shards that are currently owning its
 * chunks (or boost:node when the collection is untracked or non-existing).
 * The method assumes that the caller is currently holding a Critical Section for the namespace
 * requested and ensures a stable value across calls as long as the queried routing table isn't
 * modified.
 */
boost::optional<ShardId> pickShardOwningCollectionChunks(OperationContext* opCtx,
                                                         const UUID& collUuid);

/**
 * Request to the specified shard the generation of a 'namespacePlacementChange' notification
 * matching the commit of a sharding DDL operation, meant to drive the behavior of change stream
 * readers.
 */
void generatePlacementChangeNotificationOnShard(
    OperationContext* opCtx,
    const NamespacePlacementChanged& placementChangeNotification,
    const ShardId& shard,
    std::function<OperationSessionInfo(OperationContext*)> buildNewSessionFn,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token);

}  // namespace sharding_ddl_util
}  // namespace mongo
