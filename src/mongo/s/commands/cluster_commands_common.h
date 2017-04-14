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
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/stdx/memory.h"

namespace mongo {

class CachedCollectionRoutingInfo;
class CachedDatabaseInfo;
class OperationContext;

/**
 * Utility function to target all shards for a request that does not have a specific namespace.
 */
std::vector<AsyncRequestsSender::Request> buildRequestsForAllShards(OperationContext* opCtx,
                                                                    const BSONObj& cmdObj);

/**
 * Utility function to target all shards that own data for a collection.
 *
 * Selects shards to target based on 'routingInfo', and constructs a vector of requests, one per
 * targeted shard, where the cmdObj to send to each shard has been modified to include the shard's
 * shardVersion.
 */
std::vector<AsyncRequestsSender::Request> buildRequestsForShardsThatHaveCollection(
    OperationContext* opCtx, const CachedCollectionRoutingInfo& routingInfo, const BSONObj& cmdObj);

/**
 * Utility function to target all shards that own chunks that match a query on a collection.
 *
 * Selects shards to target based on the ChunkManager in 'routingInfo', and constructs a vector of
 * requests, one per targeted shard, where the cmdObj to send to each shard has been modified to
 * include the shard's shardVersion.
 */
std::vector<AsyncRequestsSender::Request> buildRequestsForShardsForQuery(
    OperationContext* opCtx,
    const CachedCollectionRoutingInfo& routingInfo,
    const BSONObj& cmdObj,
    const BSONObj& filter,
    const BSONObj& collation);

/**
 * Utility function to scatter 'requests' to shards and gather the responses.
 *
 * Returns an error status if any shard returns a stale shardVersion error or if a shard is not
 * found.
 *
 * @output: if non-null:
 * -- places the raw responses from the shards into a field called 'raw' in 'output'
 * -- appends the writeConcern element for the first writeConcern error encountered to 'output'
 * -- appends an error code and message to 'output'. If all shards had the same error, the error
 *    code is the common error code, otherwise '0'
 * -- *Warning* resets 'output' to empty if an error status is returned.
 *
 * @viewDefinition: if non-null and a shard returns an error saying that the command was on a view,
 * the view definition is stored in 'viewDefinition'.
 */
StatusWith<std::vector<AsyncRequestsSender::Response>> gatherResponsesFromShards(
    OperationContext* opCtx,
    const std::string& dbName,
    const BSONObj& cmdObj,
    const std::vector<AsyncRequestsSender::Request>& requests,
    BSONObjBuilder* output,
    BSONObj* viewDefinition);

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
