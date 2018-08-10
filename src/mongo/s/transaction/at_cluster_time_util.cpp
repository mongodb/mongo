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

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>

#include "mongo/s/transaction/at_cluster_time_util.h"

#include "mongo/db/logical_clock.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_id.h"

namespace mongo {

namespace at_cluster_time_util {

BSONObj appendAtClusterTime(BSONObj cmdObj, LogicalTime atClusterTime) {
    BSONObjBuilder cmdAtClusterTimeBob;
    for (auto el : cmdObj) {
        if (el.fieldNameStringData() == repl::ReadConcernArgs::kReadConcernFieldName) {
            BSONObjBuilder readConcernBob =
                cmdAtClusterTimeBob.subobjStart(repl::ReadConcernArgs::kReadConcernFieldName);
            for (auto&& elem : el.Obj()) {
                // afterClusterTime cannot be specified with atClusterTime.
                if (elem.fieldNameStringData() !=
                    repl::ReadConcernArgs::kAfterClusterTimeFieldName) {
                    readConcernBob.append(elem);
                }
            }

            readConcernBob.append(repl::ReadConcernArgs::kAtClusterTimeFieldName,
                                  atClusterTime.asTimestamp());
        } else {
            cmdAtClusterTimeBob.append(el);
        }
    }

    return cmdAtClusterTimeBob.obj();
}

namespace {

LogicalTime _computeAtClusterTime(OperationContext* opCtx,
                                  bool mustRunOnAll,
                                  const std::set<ShardId>& shardIds,
                                  const NamespaceString& nss,
                                  const BSONObj query,
                                  const BSONObj collation) {
    // TODO: SERVER-31767
    return LogicalClock::get(opCtx)->getClusterTime();
}
}

boost::optional<LogicalTime> computeAtClusterTime(OperationContext* opCtx,
                                                  bool mustRunOnAll,
                                                  const std::set<ShardId>& shardIds,
                                                  const NamespaceString& nss,
                                                  const BSONObj query,
                                                  const BSONObj collation) {

    // TODO SERVER-36688: Move this check to TransactionRouter::computeAtClusterTime.
    if (repl::ReadConcernArgs::get(opCtx).getLevel() !=
        repl::ReadConcernLevel::kSnapshotReadConcern) {
        return boost::none;
    }

    auto atClusterTime =
        _computeAtClusterTime(opCtx, mustRunOnAll, shardIds, nss, query, collation);

    // If the user passed afterClusterTime, atClusterTime must be greater than or equal to it.
    const auto afterClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAfterClusterTime();
    if (afterClusterTime && *afterClusterTime > atClusterTime) {
        return afterClusterTime;
    }

    return atClusterTime;
}

boost::optional<LogicalTime> computeAtClusterTimeForOneShard(OperationContext* opCtx,
                                                             const ShardId& shardId) {

    // TODO SERVER-36688: Move this check to TransactionRouter::computeAtClusterTime.
    if (repl::ReadConcernArgs::get(opCtx).getLevel() !=
        repl::ReadConcernLevel::kSnapshotReadConcern) {
        return boost::none;
    }

    auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    invariant(shardRegistry);

    auto shard = shardRegistry->getShardNoReload(shardId);
    uassert(ErrorCodes::ShardNotFound, str::stream() << "Could not find shard " << shardId, shard);

    // Return the cached last committed opTime for the shard if there is one, otherwise return the
    // lastest cluster time from the logical clock.
    auto lastCommittedOpTime = shard->getLastCommittedOpTime();
    return lastCommittedOpTime != LogicalTime::kUninitialized
        ? lastCommittedOpTime
        : LogicalClock::get(opCtx)->getClusterTime();
}

}  // namespace at_cluster_time_util
}  // namespace mongo
