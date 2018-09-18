/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/shard_id.h"

namespace mongo {

// Contains a collection of utility functions relating to atClusterTime.
// TODO SERVER-36688: Move the atClusterTime helpers into TransactionRouter.
namespace at_cluster_time_util {

/**
 * Returns a copy of 'cmdObj' with atClusterTime appended to a readConcern.
 */
BSONObj appendAtClusterTime(BSONObj cmdObj, LogicalTime atClusterTime);

/**
 * Returns the latest known lastCommittedOpTime for the targeted shard, or the latest in-memory
 * cluster time if there is none.
 *
 * A null logical time is returned if the readConcern on the OperationContext is not snapshot.
 */
boost::optional<LogicalTime> computeAtClusterTimeForOneShard(OperationContext* opCtx,
                                                             const ShardId& shardId);
/**
 * Returns the atClusterTime to use for the given query. This will be the latest known
 * lastCommittedOpTime for the targeted shards if the same set of shards would be targeted at
 * that time, otherwise the latest in-memory cluster time.
 *
 * A null logical time is returned if the readConcern on the OperationContext is not snapshot.
 */
boost::optional<LogicalTime> computeAtClusterTime(OperationContext* opCtx,
                                                  bool mustRunOnAll,
                                                  const std::set<ShardId>& shardIds,
                                                  const NamespaceString& nss,
                                                  const BSONObj query,
                                                  const BSONObj collation);

}  // namespace at_cluster_time_util
}  // namespace mongo
