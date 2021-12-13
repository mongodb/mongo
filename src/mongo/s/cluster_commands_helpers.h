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

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/stdx/memory.h"

namespace mongo {

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
 * Dispatches all the specified requests in parallel and waits until all complete, returning a
 * vector of the same size and positions as that of 'requests'.
 *
 * Throws StaleConfigException if any remote returns a stale shardVersion error.
 */
std::vector<AsyncRequestsSender::Response> gatherResponses(
    OperationContext* opCtx,
    StringData dbName,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::vector<AsyncRequestsSender::Request>& requests,
    const std::set<ErrorCodes::Error>& ignorableErrors = {});

/**
 * Returns a copy of 'cmdObj' with 'version' appended.
 */
BSONObj appendShardVersion(BSONObj cmdObj, ChunkVersion version);

/**
 * Returns a copy of 'cmdObj' with 'allowImplicitCollectionCreation' appended.
 */
BSONObj appendAllowImplicitCreate(BSONObj cmdObj, bool allow);

/**
 * Returns a copy of 'cmdObj' with the writeConcern removed.
 */
BSONObj stripWriteConcern(const BSONObj& cmdObj);

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
 * Does not retry on StaleConfigException.
 *
 * Return value is the same as scatterGatherUnversionedTargetAllShards().
 */
std::vector<AsyncRequestsSender::Response> scatterGatherVersionedTargetByRoutingTable(
    OperationContext* opCtx,
    StringData dbName,
    const NamespaceString& nss,
    const CachedCollectionRoutingInfo& routingInfo,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const BSONObj& query,
    const BSONObj& collation);

/**
 * Utility for dispatching commands on a namespace, but with special hybrid versioning:
 * - If the namespace is unsharded, a version is attached (so this node can find out if its routing
 * table was stale, and the namespace is actually sharded), and only the primary shard is targeted.
 * - If the namespace is sharded, no version is attached, and the request is broadcast to all
 * shards.
 *
 * Does not retry on StaleConfigException.
 *
 * Return value is the same as scatterGatherUnversionedTargetAllShards().
 */
std::vector<AsyncRequestsSender::Response> scatterGatherOnlyVersionIfUnsharded(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::set<ErrorCodes::Error>& ignorableErrors = {});

/**
 * Utility for dispatching commands against the primary of a database and attach the appropriate
 * database version.
 *
 * Does not retry on StaleDbVersion.
 */
AsyncRequestsSender::Response executeCommandAgainstDatabasePrimary(
    OperationContext* opCtx,
    StringData dbName,
    const CachedDatabaseInfo& dbInfo,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy);

/**
 * Attaches each shard's response or error status by the shard's connection string in a top-level
 * field called 'raw' in 'output'.
 *
 * If all shards that errored had the same error, writes the common error code to 'output'. Writes a
 * string representation of all errors to 'errmsg.' Errors codes in 'ignoredErrors' are not treated
 * as errors if any shard returned success.
 *
 * Returns true if any shard reports success and only ignored errors occur.
 */
bool appendRawResponses(OperationContext* opCtx,
                        std::string* errmsg,
                        BSONObjBuilder* output,
                        const std::vector<AsyncRequestsSender::Response>& shardResponses,
                        std::set<ErrorCodes::Error> ignoredErrors = {});

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
 * Utility function to compute a single error code from a vector of command results.
 *
 * @return If there is an error code common to all of the error results, returns that error
 *          code; otherwise, returns 0.
 */
int getUniqueCodeFromCommandResults(const std::vector<Strategy::CommandResult>& results);

/**
 * Utility function to return an empty result set from a command.
 */
bool appendEmptyResultSet(OperationContext* opCtx,
                          BSONObjBuilder& result,
                          Status status,
                          const std::string& ns);

/**
 * If the specified database exists already, loads it in the cache (if not already there).
 * Otherwise, if it does not exist, this call will implicitly create it as non-sharded.
 */
void createShardDatabase(OperationContext* opCtx, StringData dbName);

/**
 * Returns the shards that would be targeted for the given query according to the given routing
 * info.
 */
std::set<ShardId> getTargetedShardsForQuery(OperationContext* opCtx,
                                            const CachedCollectionRoutingInfo& routingInfo,
                                            const BSONObj& query,
                                            const BSONObj& collation);

/**
 * Determines the shard(s) to which the given query will be targeted, and builds a separate
 * versioned copy of the command object for each such shard.
 */
std::vector<std::pair<ShardId, BSONObj>> getVersionedRequestsForTargetedShards(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CachedCollectionRoutingInfo& routingInfo,
    const BSONObj& cmdObj,
    const BSONObj& query,
    const BSONObj& collation);

/**
 * If the command is running in a transaction, returns the proper routing table to use for targeting
 * shards. If there is no active transaction or the transaction is not running with snapshot level
 * read concern, the latest routing table is returned, otherwise a historical routing table is
 * returned at the global read timestamp, which must have been selected by this point.
 *
 * Should be used by all router commands that can be run in a transaction when targeting shards.
 */
StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfoForTxnCmd(
    OperationContext* opCtx, const NamespaceString& nss);

/**
 * Utility for dispatching a command on a namespace that is sent to all shards. If all shards return
 * CannotImplicitlyCreateCollection, will throw. If at least one shard succeeds, will ignore
 * CannotImplicitlyCreateCollection errors.
 */
std::vector<AsyncRequestsSender::Response> dispatchCommandAssertCollectionExistsOnAtLeastOneShard(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ReadPreference readPreference,
    const BSONObj& cmdObj);

}  // namespace mongo
