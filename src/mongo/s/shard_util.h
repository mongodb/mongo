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

#include "mongo/s/client/shard.h"

namespace mongo {

class NamespaceString;
class OperationContext;
class ShardKeyPattern;
class ShardRegistry;
template <typename T>
class StatusWith;

/**
 * Set of methods used to introspect the state of individual shards.
 */
namespace shardutil {

/**
 * Executes the listDatabases command against the specified shard and obtains the total data
 * size across all databases in bytes (essentially, the totalSize field).
 *
 * Returns OK with the total size or an error. Known errors are:
 *  ShardNotFound if shard by that id is not available on the registry
 *  NoSuchKey if the total shard size could not be retrieved
 */
StatusWith<long long> retrieveTotalShardSize(OperationContext* txn, const ShardId& shardId);

/**
 * Asks the mongod holding this chunk to find a key that approximately divides the specified chunk
 * in two.
 */
StatusWith<BSONObj> selectMedianKey(OperationContext* txn,
                                    const ShardId& shardId,
                                    const NamespaceString& nss,
                                    const ShardKeyPattern& shardKeyPattern,
                                    const BSONObj& minKey,
                                    const BSONObj& maxKey);

/**
 * Ask the specified shard to figure out the split points for a given chunk.
 *
 * @param shardId The shard id to query.
 * @param nss Namespace, which owns the chunk.
 * @param shardKeyPattern The shard key which corresponds to this sharded namespace.
 * @param minKey/maxKey Bounds of the chunk.
 * @param chunkSize Chunk size to target in bytes.
 * @param maxPoints Limits the number of split points that are needed. Zero means max.
 * @param maxObjs Limits the number of objects in each chunk. Zero means max.
 */
StatusWith<std::vector<BSONObj>> selectChunkSplitPoints(OperationContext* txn,
                                                        const ShardId& shardId,
                                                        const NamespaceString& nss,
                                                        const ShardKeyPattern& shardKeyPattern,
                                                        const BSONObj& minKey,
                                                        const BSONObj& maxKey,
                                                        long long chunkSizeBytes,
                                                        int maxPoints,
                                                        int maxObjs);

}  // namespace shardutil
}  // namespace mongo
