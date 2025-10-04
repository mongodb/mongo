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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/router_role_api/collection_routing_info_targeter.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/transaction_router.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace cluster::unsplittable {

const auto kUnsplittableCollectionShardKey = BSON("_id" << 1);
const auto kUnsplittableCollectionMinKey = BSON("_id" << MINKEY);
const auto kUnsplittableCollectionMaxKey = BSON("_id" << MAXKEY);

ShardsvrReshardCollection makeMoveCollectionRequest(
    const DatabaseName& dbName,
    const NamespaceString& nss,
    const ShardId& destinationShard,
    ReshardingProvenanceEnum provenance,
    const boost::optional<bool>& performVerification = boost::none,
    const boost::optional<std::int64_t>& oplogBatchApplierTaskCount = boost::none);

ShardsvrReshardCollection makeUnshardCollectionRequest(
    const DatabaseName& dbName,
    const NamespaceString& nss,
    const boost::optional<ShardId>& destinationShard,
    const boost::optional<bool>& performVerification,
    const boost::optional<std::int64_t>& oplogBatchApplierTaskCount = boost::none);

}  // namespace cluster::unsplittable

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
    const CollectionRoutingInfo& cri,
    const BSONObj& collation,
    const boost::optional<ExplainOptions::Verbosity>& verbosity,
    const boost::optional<BSONObj>& letParameters,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants);

/**
 * Builds requests for each given shard.
 *
 * Consults the routing info to build requests for specified list of shards:
 *  - If it has a routing table, shards that own chunks for the namespace, or
 *  - If it doesn't have a routing table, the primary shard for the database.
 *
 * If the command is eligible for sampling, attaches a unique sample id to one of the requests if
 * the collection has query sampling enabled and the rate-limited sampler successfully generates a
 * sample id for it.
 */
std::vector<AsyncRequestsSender::Request> buildVersionedRequests(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const CollectionRoutingInfo& cri,
    const std::set<ShardId>& shardIds,
    const BSONObj& cmdObj,
    bool eligibleForSampling = false);

/**
 * Dispatches all the specified requests in parallel and waits until all complete, returning a
 * vector of the same size and positions as that of 'requests'.
 *
 * Throws StaleConfig if any of the remotes returns that error, regardless of what the other errors
 * are.
 */
std::vector<AsyncRequestsSender::Response> gatherResponses(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const NamespaceString& nss,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::vector<AsyncRequestsSender::Request>& requests,
    RoutingContext* routingCtx = nullptr);

/**
 * Returns a copy of 'cmdObj' with dbVersion appended if it exists in 'dbInfo'
 *
 * Use generic_argument_util::setDbVersionIfPresent() instead if the BSON is generated from an
 * IDL-command struct.
 *
 * TODO SERVER-91373: Typed commands need to set dbversion using
 * generic_argument_util::setDbVersionIfPresent() instead of appendDbVersionIfPresent().
 */
BSONObj appendDbVersionIfPresent(BSONObj cmdObj, const CachedDatabaseInfo& dbInfo);

/**
 * Returns a copy of 'cmdObj' with 'databaseVersion' appended.
 *
 * Use generic_argument_util::setDbVersionIfPresent() instead if the BSON is generated from an
 * IDL-command struct.
 *
 * TODO SERVER-91373: Typed commands need to set dbversion using
 * generic_argument_util::setDbVersionIfPresent() instead of appendDbVersionIfPresent().
 */
BSONObj appendDbVersionIfPresent(BSONObj cmdObj, DatabaseVersion dbVersion);

/**
 * Returns a copy of 'cmdObj' with 'version' appended.
 */
BSONObj appendShardVersion(BSONObj cmdObj, ShardVersion version);

/**
 * Returns a copy of 'cmdObj' with the read/writeConcern from the OpCtx appended, unless the
 * cmdObj explicitly specifies read/writeConcern.
 *
 * TODO SERVER-91373: Callers of applyReadWriteConcern that come from a basic command should use
 * setReadWriteConcern after they are converted to a typed command.
 * TODO SERVER-92350: Some callers of applyReadWriteConcern operate on a BSON that we get from a
 * Document. After the changes in 92350 they will operate on an idl command and the callsites can be
 * replaced with setReadWriteConcern.
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
 * Sets the read/write concern from the opCtx onto the idl command struct given setRc and setWc.
 */
template <typename CommandType>
void setReadWriteConcern(OperationContext* opCtx, CommandType& cmd, bool setRC, bool setWC) {
    if (TransactionRouter::get(opCtx)) {
        // When running in a transaction, the rules are:
        // - Apply readConcern only if this is the first operation in the transaction.
        // - Never apply writeConcern. This is done directly by the TransactionRouter.
        if (!opCtx->isStartingMultiDocumentTransaction() || !setRC)
            return;
        setWC = false;
    }

    const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    if (readConcernArgs.wasAtClusterTimeSelected() || (setRC && !cmd.getReadConcern()))
        cmd.setReadConcern(readConcernArgs);
    if (setWC && !cmd.getWriteConcern())
        cmd.setWriteConcern(opCtx->getWriteConcern());
}

template <typename CommandType>
void setReadWriteConcern(OperationContext* opCtx, CommandType& cmd, CommandInvocation* invocation) {
    const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    const auto readConcernSupport = invocation->supportsReadConcern(
        readConcernArgs.getLevel(), readConcernArgs.isImplicitDefault());
    setReadWriteConcern(opCtx,
                        cmd,
                        readConcernSupport.readConcernSupport.isOK(),
                        invocation->supportsWriteConcern());
}

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
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy);

/**
 * Utility for dispatching unversioned commands to a dedicated config server if it exists and all
 * shards in a cluster.
 *
 * Returns a non-OK status if a failure occurs on *this* node during execution. Otherwise, returns
 * success and a list of responses from the config server and shards (including errors from the
 * config server or shards or errors reaching the config server or shards).
 *
 * Note, if this mongos has not refreshed its shard list since
 * 1) a shard has been *added* through a different mongos, a request will not be sent to the added
 *    shard
 * 2) a shard has been *removed* through a different mongos, this function will return a
 *    ShardNotFound error status.
 */
std::vector<AsyncRequestsSender::Response> scatterGatherUnversionedTargetConfigServerAndShards(
    OperationContext* opCtx,
    const DatabaseName& dbName,
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
    RoutingContext& routingCtx,
    const NamespaceString& nss,
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
    RoutingContext& routingCtx,
    const NamespaceString& nss,
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
    RoutingContext& routingCtx,
    const NamespaceString& nss,
    const std::set<ShardId>& shardsToSkip,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const BSONObj& query,
    const BSONObj& collation,
    const boost::optional<BSONObj>& letParameters,
    const boost::optional<LegacyRuntimeConstants>& runtimeConstants);

/**
 * Utility for dispatching commands against the primary of a database and attaching
 * the appropriate database version.
 * NOTE: It only attaches the database version and no shard version.
 */
AsyncRequestsSender::Response executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const CachedDatabaseInfo& dbInfo,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy);

/**
 * Utility for dispatching commands against the shard with the MinKey chunk for the namespace
 * and attaching the appropriate shard version.
 *
 * Does not retry on StaleConfig errors.
 */
AsyncRequestsSender::Response executeCommandAgainstShardWithMinKeyChunk(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const NamespaceString& nss,
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
    const std::vector<AsyncRequestsSender::Response>& shardResponses,
    bool appendWriteConcernError = true);

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
                                            const CollectionRoutingInfo& cri,
                                            const BSONObj& query,
                                            const BSONObj& collation);
/**
 * Returns the shards that would be targeted for the given query according to the given routing
 * info.
 */
std::set<ShardId> getTargetedShardsForCanonicalQuery(const CanonicalQuery& query,
                                                     const CollectionRoutingInfo& cri);

/**
 * Determines the shard(s) to which the given query will be targeted, and builds a separate
 * versioned copy of the command object for each such shard.
 */
std::vector<AsyncRequestsSender::Request> getVersionedRequestsForTargetedShards(
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
 * Note that this will be deprecated (SERVER-102925) in favor of getRoutingContextForTxnCmd().
 */
StatusWith<CollectionRoutingInfo> getCollectionRoutingInfoForTxnCmd_DEPRECATED(
    OperationContext* opCtx, const NamespaceString& nss);

/**
 * Helper used by getRoutingContextForTxnCmd(). Defaults to allowLocks = false.
 */
StatusWith<std::unique_ptr<RoutingContext>> getRoutingContext(
    OperationContext* opCtx, const std::vector<NamespaceString>& nss, bool allowLocks = false);

/**
 * If the command is running in a transaction, returns the RoutingContext to use for targeting
 * shards. If there is no active transaction or the transaction is not running with snapshot level
 * read concern, the RoutingContext acquires the latest routing table, otherwise it acquires a
 * historical routing table at the global read timestamp, which must have been selected by this
 * point. A vector of namespaces is passed in as it is possible to acquire a RoutingContext for an
 * empty nssList (for collectionless aggregates, which don't operate on a real collection), or
 * multiple namespaces.
 *
 * Should be used by all router commands that can be run in a transaction when targeting shards.
 */
StatusWith<std::unique_ptr<RoutingContext>> getRoutingContextForTxnCmd(
    OperationContext* opCtx, const std::vector<NamespaceString>& nss);

/**
 * Force a refresh of the routing cache and fetch the routing info for the given collection.
 *
 * Throw NamespaceNotSharded exception in case the collection is unsharded.
 *
 * WARNING: This function is DEPRECATED.
 * Forcing a catalog refresh will cause subsequent cache lookups to wait until the refresh is
 * completed. Moreover, the returned routing information could be stale. This is because the refresh
 * is not causal-consistent with other DDL operations.
 */
CollectionRoutingInfo getRefreshedCollectionRoutingInfoAssertSharded_DEPRECATED(
    OperationContext* opCtx, const NamespaceString& nss);
/**
 * Loads all of the indexes for the given namespace from the appropriate shard. For unsharded
 * collections will read from the primary shard and for sharded collections will read from the shard
 * that owns the chunk containing the minimum key for the collection's shard key.
 *
 * Will not retry on StaleConfig or StaleDbVersion errors.
 */
StatusWith<Shard::QueryResponse> loadIndexesFromAuthoritativeShard(OperationContext* opCtx,
                                                                   RoutingContext& routingCtx,
                                                                   const NamespaceString& nss);

/**
 * Utility that safely calculates the sum of a limit and skip so it can be forwarded to the shards.
 * In a sharded cluster, each shard will apply the sum individually, and the final limit and skip
 * will be performed router-side.
 */
StatusWith<boost::optional<int64_t>> addLimitAndSkipForShards(boost::optional<int64_t> limit,
                                                              boost::optional<int64_t> skip);


/**
 * This function abstracts the complexity of using the appropriate routingContext when the operation
 * is of type rawData.
 *
 * If `rawData` is enabled for the current operation, and the collection is a tracked viewful
 * timeseries, and the buckets nss is present on the router cache:
 *   1. Executes `translateNssFunc` using the appropriate buckets namespace.
 *   2. Returns the routing context held by the CollectionRoutingInfoTargeter.
 * Otherwise, it will just return the `originalRoutingCtx`.
 *
 * Important notes:
 *   - This function must be invoked within a CollectionRouter wrapper.
 *   - The caller must retain ownership of the `CollectionRoutingInfoTargeter` for the entire
 *     lifetime of the RoutingContext object returned by this function.
 *
 * TODO (SERVER-108351) Remove all usages of this function once all timeseries collections become
 * viewless.
 */
RoutingContext& translateNssForRawDataAccordingToRoutingInfo(
    OperationContext* opCtx,
    const NamespaceString& originalNss,
    const CollectionRoutingInfoTargeter& targeter,
    RoutingContext& originalRoutingCtx,
    std::function<void(const NamespaceString& translatedNss)> translateNssFunc);

}  // namespace mongo
