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

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/write_block_bypass.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/async_rpc_error_info.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/executor/async_rpc_util.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/impersonated_user_metadata.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/async_rpc_shard_retry_policy.h"
#include "mongo/s/async_rpc_shard_targeter.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"


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
template <typename CommandType>
std::vector<AsyncRequestsSender::Response> sendAuthenticatedCommandToShards(
    OperationContext* opCtx,
    std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> originalOpts,
    const std::vector<ShardId>& shardIds,
    bool ignoreResponses = false) {
    if (shardIds.size() == 0) {
        return {};
    }

    // AsyncRPC ignores impersonation metadata so we need to manually attach them to
    // the command
    if (auto meta = rpc::getAuthDataToImpersonatedUserMetadata(opCtx)) {
        originalOpts->genericArgs.unstable.setDollarAudit(*meta);
    }
    originalOpts->genericArgs.unstable.setMayBypassWriteBlocking(
        WriteBlockBypass::get(opCtx).isWriteBlockBypassEnabled());

    std::vector<ExecutorFuture<async_rpc::AsyncRPCResponse<typename CommandType::Reply>>> futures;
    auto indexToShardId = std::make_shared<stdx::unordered_map<int, ShardId>>();

    CancellationSource cancelSource(originalOpts->token);

    for (size_t i = 0; i < shardIds.size(); ++i) {
        ReadPreferenceSetting readPref(ReadPreference::PrimaryOnly);
        std::unique_ptr<async_rpc::Targeter> targeter =
            std::make_unique<async_rpc::ShardIdTargeter>(
                originalOpts->exec, opCtx, shardIds[i], readPref);
        bool startTransaction = originalOpts->genericArgs.stable.getStartTransaction()
            ? *originalOpts->genericArgs.stable.getStartTransaction()
            : false;
        auto retryPolicy = std::make_shared<async_rpc::ShardRetryPolicyWithIsStartingTransaction>(
            Shard::RetryPolicy::kIdempotentOrCursorInvalidated, startTransaction);
        auto opts =
            std::make_shared<async_rpc::AsyncRPCOptions<CommandType>>(originalOpts->exec,
                                                                      cancelSource.token(),
                                                                      originalOpts->cmd,
                                                                      originalOpts->genericArgs,
                                                                      retryPolicy);
        futures.push_back(async_rpc::sendCommand<CommandType>(opts, opCtx, std::move(targeter)));
        (*indexToShardId)[i] = shardIds[i];
    }

    auto responses =
        async_rpc::getAllResponsesOrFirstErrorWithCancellation<
            AsyncRequestsSender::Response,
            async_rpc::AsyncRPCResponse<typename CommandType::Reply>>(
            std::move(futures),
            cancelSource,
            [indexToShardId](async_rpc::AsyncRPCResponse<typename CommandType::Reply> reply,
                             size_t index) -> AsyncRequestsSender::Response {
                BSONObjBuilder replyBob;
                reply.response.serialize(&replyBob);
                reply.genericReplyFields.stable.serialize(&replyBob);
                reply.genericReplyFields.unstable.serialize(&replyBob);
                return AsyncRequestsSender::Response{
                    (*indexToShardId)[index],
                    executor::RemoteCommandOnAnyResponse(
                        reply.targetUsed, replyBob.obj(), reply.elapsed)};
            })
            .getNoThrow();

    if (ignoreResponses) {
        return {};
    }

    if (auto status = responses.getStatus(); status != Status::OK()) {
        uassertStatusOK(async_rpc::unpackRPCStatus(status));
    }

    return responses.getValue();
}

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
    bool useClusterTransaction,
    const std::shared_ptr<executor::TaskExecutor>& executor = nullptr);

/**
 * Delete the query analyzer documents that match the given filter.
 */
void removeQueryAnalyzerMetadataFromConfig(OperationContext* opCtx, const BSONObj& filter);

/**
 * Ensures rename preconditions for collections are met:
 * - Check that the namespace of the destination collection is not too long
 * - Check that `dropTarget` is true if the destination collection exists
 * - Check that no tags exist for the destination collection
 */
void checkRenamePreconditions(OperationContext* opCtx,
                              const NamespaceString& toNss,
                              const boost::optional<CollectionType>& optTargetCollType,
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
    const boost::optional<UUID>& collectionUUID = boost::none);

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
 * Same as `getOperationsToCreateOrShardCollectionOnShardingCatalog`, with the difference that it
 * generates the collection and chunk entries for an unsplittable collection.
 */
std::vector<BatchedCommandRequest> getOperationsToCreateUnsplittableCollectionOnShardingCatalog(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& collectionUuid,
    const ShardId& shardId);

}  // namespace sharding_ddl_util
}  // namespace mongo
