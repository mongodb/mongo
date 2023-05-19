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

#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {

// TODO (SERVER-74481): Define these functions in the nested `sharding_ddl_util` namespace when the
// IDL compiler will support the use case.
void sharding_ddl_util_serializeErrorStatusToBSON(const Status& status,
                                                  StringData fieldName,
                                                  BSONObjBuilder* bsonBuilder);
Status sharding_ddl_util_deserializeErrorStatusFromBSON(const BSONElement& bsonElem);

namespace sharding_ddl_util {

/**
 * Creates a barrier after which we are guaranteed that all writes to the config server performed by
 * the previous primary have been majority commited and will be seen by the new primary.
 */
void linearizeCSRSReads(OperationContext* opCtx);

/**
 * Generic utility to send a command to a list of shards. Throws if one of the commands fails.
 */
std::vector<AsyncRequestsSender::Response> sendAuthenticatedCommandToShards(
    OperationContext* opCtx,
    StringData dbName,
    const BSONObj& command,
    const std::vector<ShardId>& shardIds,
    const std::shared_ptr<executor::TaskExecutor>& executor);

/**
 * Erase tags metadata from config server for the given namespace, using the _configsvrRemoveTags
 * command as a retryable write to ensure idempotency.
 */
void removeTagsMetadataFromConfig(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const OperationSessionInfo& osi);

/**
 * Erase tags metadata from config server for the given namespace.
 */
void removeTagsMetadataFromConfig_notIdempotent(OperationContext* opCtx,
                                                const std::shared_ptr<Shard>& configShard,
                                                const NamespaceString& nss,
                                                const WriteConcernOptions& writeConcern);


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
    bool useClusterTransaction,
    const std::shared_ptr<executor::TaskExecutor>& executor = nullptr);

/**
 * Erase collection metadata from config server and invalidate the locally cached one.
 * In particular remove the collection, chunks and index metadata associated with the given
 * namespace.
 *
 * Returns true if the collection existed before being removed.
 */
bool removeCollAndChunksMetadataFromConfig_notIdempotent(OperationContext* opCtx,
                                                         const std::shared_ptr<Shard>& configShard,
                                                         ShardingCatalogClient* catalogClient,
                                                         const NamespaceString& nss,
                                                         const WriteConcernOptions& writeConcern);

/**
 * Delete the query analyzer documents that match the given filter.
 */
void removeQueryAnalyzerMetadataFromConfig(OperationContext* opCtx, const BSONObj& filter);

/**
 * Rename sharded collection metadata as part of a renameCollection operation.
 *
 * - Update namespace associated with tags (FROM -> TO)
 * - Update FROM collection entry to TO
 *
 * This function is idempotent and can just be invoked by the CSRS.
 */
void shardedRenameMetadata(OperationContext* opCtx,
                           const std::shared_ptr<Shard>& configShard,
                           ShardingCatalogClient* catalogClient,
                           CollectionType fromCollType,
                           const NamespaceString& toNss,
                           const WriteConcernOptions& writeConcern);

/**
 * Ensure source collection uuid is consistent on every shard
 * Ensure target collection is not present on any shard when `dropTarget` is false
 */
void checkCatalogConsistencyAcrossShardsForRename(
    OperationContext* opCtx,
    const NamespaceString& fromNss,
    const NamespaceString& toNss,
    bool dropTarget,
    std::shared_ptr<executor::ScopedTaskExecutor> executor);

/**
 * Ensures rename preconditions for collections are met:
 * - Check that the namespace of the destination collection is not too long
 * - Check that `dropTarget` is true if the destination collection exists
 * - Check that no tags exist for the destination collection
 */
void checkRenamePreconditions(OperationContext* opCtx,
                              bool sourceIsSharded,
                              const NamespaceString& toNss,
                              bool dropTarget);

/**
 * Throws if the DB primary shards of the provided namespaces differs.
 *
 * Optimistically assume that no movePrimary is performed during the check: it's currently not
 * possible to ensure primary shard stability for both databases.
 */
void checkDbPrimariesOnTheSameShard(OperationContext* opCtx,
                                    const NamespaceString& fromNss,
                                    const NamespaceString& toNss);

/**
 * Throws an exception if the collection is already sharded with different options.
 *
 * If the collection is already sharded with the same options, returns the existing collection's
 * full spec, else returns boost::none.
 */
boost::optional<CreateCollectionResponse> checkIfCollectionAlreadySharded(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& key,
    const BSONObj& collation,
    bool unique);

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
void sendDropCollectionParticipantCommandToShards(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const std::vector<ShardId>& shardIds,
                                                  std::shared_ptr<executor::TaskExecutor> executor,
                                                  const OperationSessionInfo& osi,
                                                  bool fromMigrate);

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
    bool useClusterTransaction,
    const std::shared_ptr<executor::TaskExecutor>& inputExecutor = nullptr);
}  // namespace sharding_ddl_util
}  // namespace mongo
