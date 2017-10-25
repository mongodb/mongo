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
StatusWith<std::vector<AsyncRequestsSender::Response>> scatterGatherUnversionedTargetAllShards(
    OperationContext* opCtx,
    const std::string& dbName,
    boost::optional<NamespaceString> nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy);

/**
 * Utility for dispatching versioned commands on a namespace, deciding which shards to
 * target by applying the passed-in query and collation to the local routing table cache.
 *
 * Throws on seeing a StaleConfigException from any shard.
 *
 * Optionally populates a 'viewDefinition' out parameter if a shard's result contains a view
 * definition. The viewDefinition can be used to re-run the command as an aggregation.
 *
 * Return value is the same as scatterGatherUnversionedTargetAllShards().
 */
StatusWith<std::vector<AsyncRequestsSender::Response>> scatterGatherVersionedTargetByRoutingTable(
    OperationContext* opCtx,
    const std::string& dbName,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const BSONObj& query,
    const BSONObj& collation,
    BSONObj* viewDefinition);

/**
 * Utility for dispatching commands on a namespace, but with special hybrid versioning:
 * - If the namespace is unsharded, a version is attached (so this node can find out if its routing
 * table was stale, and the namespace is actually sharded), and only the primary shard is targeted.
 * - If the namespace is sharded, no version is attached, and the request is broadcast to all
 * shards.
 *
 * Throws on seeing a StaleConfigException.
 *
 * Return value is the same as scatterGatherUnversionedTargetAllShards().
 */
StatusWith<std::vector<AsyncRequestsSender::Response>> scatterGatherOnlyVersionIfUnsharded(
    OperationContext* opCtx,
    const std::string& dbName,
    const NamespaceString& nss,
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
                        std::vector<AsyncRequestsSender::Response> shardResponses,
                        std::set<ErrorCodes::Error> ignoredErrors = {});

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
