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

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"

namespace mongo {

struct RawResponsesResult {
    bool responseOK;
    std::set<ShardId> shardsWithSuccessResponses;
    std::vector<AsyncRequestsSender::Response> successResponses;
    boost::optional<Status> firstStaleConfigError;
};

/**
 * This function appends the provided WriteConcernErrorDetail to the sharded response.
 */
void appendWriteConcernErrorDetailToCmdResponse(const ShardId& shardId,
                                                WriteConcernErrorDetail wcError,
                                                BSONObjBuilder& responseBuilder);

/**
 * This function appends the provided writeConcernError BSONElement to the sharded response.
 */
void appendWriteConcernErrorToCmdResponse(const ShardId& shardID,
                                          const BSONElement& wcErrorElem,
                                          BSONObjBuilder& responseBuilder);

/**
 * Creates and returns a WriteConcernErrorDetail object from a BSONObj.
 */
std::unique_ptr<WriteConcernErrorDetail> getWriteConcernErrorDetailFromBSONObj(const BSONObj& obj);

/**
 * Makes an expression context suitable for canonicalization of queries that contain let parameters
 * and runtimeConstants on mongos.
 */
boost::intrusive_ptr<ExpressionContext> makeExpressionContextWithDefaultsForTargeter(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& collation,
    const boost::optional<ExplainOptions::Verbosity>& verbosity,
    const boost::optional<BSONObj>& letParameters,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants);

/**
 * Dispatches all the specified requests in parallel and waits until all complete, returning a
 * vector of the same size and positions as that of 'requests'.
 *
 * Throws StaleConfig if any of the remotes returns that error, regardless of what the other errors
 * are.
 */
std::vector<AsyncRequestsSender::Response> gatherResponses(
    OperationContext* opCtx,
    StringData dbName,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::vector<AsyncRequestsSender::Request>& requests);

/**
 * Dispatches all the specified requests in parallel and waits until all complete, returning a
 * vector of the same size and positions as that of 'requests'.
 */
std::vector<AsyncRequestsSender::Response> gatherResponsesNoThrowOnStaleShardVersionErrors(
    OperationContext* opCtx,
    StringData dbName,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::vector<AsyncRequestsSender::Request>& requests);

/**
 * Returns a copy of 'cmdObj' with dbVersion appended if it exists in 'dbInfo'
 */
BSONObj appendDbVersionIfPresent(BSONObj cmdObj, const CachedDatabaseInfo& dbInfo);

/**
 * Returns a copy of 'cmdObj' with 'databaseVersion' appended.
 */
BSONObj appendDbVersionIfPresent(BSONObj cmdObj, DatabaseVersion dbVersion);

/**
 * Returns a copy of 'cmdObj' with 'version' appended.
 */
BSONObj appendShardVersion(BSONObj cmdObj, ShardVersion version);

/**
 * Returns a copy of 'cmdObj' with the read/writeConcern from the OpCtx appended, unless the
 * cmdObj explicitly specifies read/writeConcern.
 */
BSONObj applyReadWriteConcern(OperationContext* opCtx,
                              bool appendRC,
                              bool appendWC,
                              const BSONObj& cmdObj);

/**
 * Convenience versions of applyReadWriteConcern() for calling from within
 * CommandInvocation, BasicCommand or BasicCommandWithRequestParser.
 */
BSONObj applyReadWriteConcern(OperationContext* opCtx,
                              CommandInvocation* invocation,
                              const BSONObj& cmdObj);

BSONObj applyReadWriteConcern(OperationContext* opCtx,
                              BasicCommandWithReplyBuilderInterface* cmd,
                              const BSONObj& cmdObj);

/**
 * Utility for dispatching unversioned commands to all shards in a cluster.
 *
 * Returns a non-OK status if a failure occurs on *this* node during execution. Otherwise, returns
 * success and a list of responses from shards (including errors from the shards or errors reaching
 * the shards).
 *
 * Note, if this mongos has not refreshed its shard list since
 * 1) a shard has been *added* through a different mongos, a request will not be sent to the added
 *    shard
 * 2) a shard has been *removed* through a different mongos, this function will return a
 *    ShardNotFound error status.
 */
std::vector<AsyncRequestsSender::Response> scatterGatherUnversionedTargetAllShards(
    OperationContext* opCtx,
    StringData dbName,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy);

/**
 * Utility for dispatching versioned commands on a namespace, deciding which shards to
 * target by applying the passed-in query and collation to the local routing table cache.
 *
 * If the command is eligible for sampling, attaches a unique sample id to one of the requests if
 * the collection has query sampling enabled and the rate-limited sampler successfully generates a
 * sample id for it.
 *
 * Does not retry on StaleConfig errors.
 */
[[nodiscard]] std::vector<AsyncRequestsSender::Response> scatterGatherVersionedTargetByRoutingTable(
    OperationContext* opCtx,
    StringData dbName,
    const NamespaceString& nss,
    const CollectionRoutingInfo& cri,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const BSONObj& query,
    const BSONObj& collation,
    const boost::optional<BSONObj>& letParameters,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants,
    bool eligibleForSampling = false);
/**
 * This overload is for callers which already have a fully initialized 'ExpressionContext' (e.g.
 * callers from the aggregation framework). Most callers should prefer the overload above.
 */
[[nodiscard]] std::vector<AsyncRequestsSender::Response> scatterGatherVersionedTargetByRoutingTable(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    StringData dbName,
    const NamespaceString& nss,
    const CollectionRoutingInfo& cri,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const BSONObj& query,
    const BSONObj& collation,
    bool eligibleForSampling = false);

/**
 * Utility for dispatching versioned commands on a namespace, deciding which shards to
 * target by applying the passed-in query and collation to the local routing table cache.
 *
 * Callers can specify shards to skip, even if these shards would be otherwise targeted.
 *
 * Allows StaleConfig errors to append to the response list.
 */
std::vector<AsyncRequestsSender::Response>
scatterGatherVersionedTargetByRoutingTableNoThrowOnStaleShardVersionErrors(
    OperationContext* opCtx,
    StringData dbName,
    const NamespaceString& nss,
    const CollectionRoutingInfo& cri,
    const std::set<ShardId>& shardsToSkip,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const BSONObj& query,
    const BSONObj& collation,
    const boost::optional<BSONObj>& letParameters,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants);

/**
 * Utility for dispatching commands against the primary of a database and attaching the appropriate
 * database version. Also attaches UNSHARDED to the command. Does not retry on stale version.
 */
AsyncRequestsSender::Response executeCommandAgainstDatabasePrimary(
    OperationContext* opCtx,
    StringData dbName,
    const CachedDatabaseInfo& dbInfo,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy);

/**
 * Utility for dispatching commands against the shard with the MinKey chunk for the namespace and
 * attaching the appropriate shard version.
 *
 * Does not retry on StaleConfig errors.
 */
AsyncRequestsSender::Response executeCommandAgainstShardWithMinKeyChunk(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionRoutingInfo& cri,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy);

/**
 * Attaches each shard's response or error status by the shard's connection string in a top-level
 * field called 'raw' in 'output'.
 *
 * If all shards that errored had the same error, writes the common error code to 'output'. Writes a
 * string representation of all errors to 'errmsg.'
 *
 * ShardNotFound responses are not treated as errors if any shard returned success. We allow
 * ShardNotFound errors to be ignored as errors since this node may not have realized that a
 * shard has been removed.
 *
 * Returns:
 * 1. A boolean indicating whether any shards reported success and only ShardNotFound errors occur.
 * 2. A set containing the list of shards that reported success or a ShardNotFound error. For shard
 *    tracking purposes, a shard with a writeConcernError is not considered to be successful.
 * 3. The list of AsyncRequestsSender::Responses that were successful.
 * 4. The first stale config error received, if such an error exists.
 */
RawResponsesResult appendRawResponses(
    OperationContext* opCtx,
    std::string* errmsg,
    BSONObjBuilder* output,
    const std::vector<AsyncRequestsSender::Response>& shardResponses);

/**
 * Extracts the query from a query-embedding command ('query' or 'q' fields). If the command does
 * not have an embedded query, returns an empty BSON object.
 */
BSONObj extractQuery(const BSONObj& cmdObj);

/**
 * Extracts the collation from a collation-embedding command ('collation' field). If the command
 * does not specify a collation, returns an empty BSON object. If the 'collation' field is of wrong
 * type, throws.
 */
BSONObj extractCollation(const BSONObj& cmdObj);

/**
 * Utility function to return an empty result set from a command.
 */
bool appendEmptyResultSet(OperationContext* opCtx,
                          BSONObjBuilder& result,
                          Status status,
                          const NamespaceString& ns);

/**
 * Returns the shards that would be targeted for the given query according to the given routing
 * info.
 */
std::set<ShardId> getTargetedShardsForQuery(boost::intrusive_ptr<ExpressionContext> expCtx,
                                            const ChunkManager& cm,
                                            const BSONObj& query,
                                            const BSONObj& collation);

/**
 * Determines the shard(s) to which the given query will be targeted, and builds a separate
 * versioned copy of the command object for each such shard.
 */
std::vector<std::pair<ShardId, BSONObj>> getVersionedRequestsForTargetedShards(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionRoutingInfo& cri,
    const BSONObj& cmdObj,
    const BSONObj& query,
    const BSONObj& collation,
    const boost::optional<BSONObj>& letParameters,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants);

/**
 * If the command is running in a transaction, returns the proper routing table to use for targeting
 * shards. If there is no active transaction or the transaction is not running with snapshot level
 * read concern, the latest routing table is returned, otherwise a historical routing table is
 * returned at the global read timestamp, which must have been selected by this point.
 *
 * Should be used by all router commands that can be run in a transaction when targeting shards.
 */
StatusWith<CollectionRoutingInfo> getCollectionRoutingInfoForTxnCmd(OperationContext* opCtx,
                                                                    const NamespaceString& nss);

/**
 * Loads all of the indexes for the given namespace from the appropriate shard. For unsharded
 * collections will read from the primary shard and for sharded collections will read from the shard
 * that owns the chunk containing the minimum key for the collection's shard key.
 *
 * Will not retry on StaleConfig or StaleDbVersion errors.
 */
StatusWith<Shard::QueryResponse> loadIndexesFromAuthoritativeShard(OperationContext* opCtx,
                                                                   const NamespaceString& nss);

}  // namespace mongo
