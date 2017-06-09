/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/stdx/memory.h"

namespace mongo {

class CachedCollectionRoutingInfo;
class CachedDatabaseInfo;
class OperationContext;
class ShardId;

/*
 * Allows callers of routing functions to specify a preferred targeting policy. See scatterGather
 * for a usage example.
 */
enum class ShardTargetingPolicy { UseRoutingTable, BroadcastToAllShards };

/**
 * This function appends the provided writeConcernError BSONElement to the sharded response.
 */
void appendWriteConcernErrorToCmdResponse(const ShardId& shardID,
                                          const BSONElement& wcErrorElem,
                                          BSONObjBuilder& responseBuilder);
/**
 * Returns a copy of 'cmdObj' with 'version' appended.
 */
BSONObj appendShardVersion(BSONObj cmdObj, ChunkVersion version);

/**
 * Generic function for dispatching commands to the cluster.
 *
 * If 'targetPolicy' is ShardTargetingPolicy::BroadcastToAllShards, the command will be sent
 * unversioned to all shards and run on database 'dbName'. The 'query', 'collation' and
 * 'appendShardVersion' arguments, if supplied, will be ignored.
 *
 * If 'targetPolicy' is ShardTargetingPolicy::UseRoutingTable, the routing table cache will be used
 * to determine which shards the command should be dispatched to. If the namespace specified by
 * 'nss' is an unsharded collection, the command will be sent to the Primary shard for the database.
 * If 'query' is specified, only shards that own data needed by the query are targeted; otherwise,
 * all shards are targeted. By default, shardVersions are attached to the outgoing requests, and the
 * function will re-target and retry if it receives a stale shardVersion error from any shard.
 *
 * Returns a non-OK status if a failure occurs on *this* node during execution or on seeing an error
 * from a shard that means the operation as a whole should fail, such as a exceeding retries for
 * stale shardVersion errors.
 *
 * If a shard returns an error saying that the request was on a view, the shard will also return a
 * view definition. This will be stored in the BSONObj* viewDefinition argument, if non-null, so
 * that the caller can re-run the operation as an aggregation.
 *
 * Otherwise, returns success and a list of responses from shards (including errors from the shards
 * or errors reaching the shards).
 */
StatusWith<std::vector<AsyncRequestsSender::Response>> scatterGather(
    OperationContext* opCtx,
    const std::string& dbName,
    const boost::optional<NamespaceString> nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    const ShardTargetingPolicy targetPolicy = ShardTargetingPolicy::UseRoutingTable,
    const boost::optional<BSONObj> query = boost::none,
    const boost::optional<BSONObj> collation = boost::none,
    const bool appendShardVersion = true,
    BSONObj* viewDefinition = nullptr);

/**
 * Attaches each shard's response or error status by the shard's connection string in a top-level
 * field called 'raw' in 'output'.
 *
 * If all shards that errored had the same error, writes the common error code to 'output'. Writes a
 * string representation of all errors to 'errmsg.'
 *
 * Returns true if all the shards reported success.
 */
bool appendRawResponses(OperationContext* opCtx,
                        std::string* errmsg,
                        BSONObjBuilder* output,
                        std::vector<AsyncRequestsSender::Response> shardResponses);

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
bool appendEmptyResultSet(BSONObjBuilder& result, Status status, const std::string& ns);

/**
 * Returns the set of collections for the specified database, which have been marked as sharded.
 * Goes directly to the config server's metadata, without checking the local cache so it should not
 * be used in frequently called code paths.
 *
 * Throws exception on errors.
 */
std::vector<NamespaceString> getAllShardedCollectionsForDb(OperationContext* opCtx,
                                                           StringData dbName);

/**
 * Abstracts the common pattern of refreshing a collection and checking if it is sharded used across
 * multiple commands.
 */
CachedCollectionRoutingInfo getShardedCollection(OperationContext* opCtx,
                                                 const NamespaceString& nss);

/**
 * If the specified database exists already, loads it in the cache (if not already there) and
 * returns it. Otherwise, if it does not exist, this call will implicitly create it as non-sharded.
 */
StatusWith<CachedDatabaseInfo> createShardDatabase(OperationContext* opCtx, StringData dbName);

}  // namespace mongo
