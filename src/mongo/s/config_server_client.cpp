/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/s/config_server_client.h"

#include "mongo/client/read_preference.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/balance_chunk_request_type.h"

namespace mongo {
namespace configsvr_client {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

}  // namespace

Status moveChunk(OperationContext* txn,
                 const ChunkType& chunk,
                 const ShardId& newShardId,
                 int64_t maxChunkSizeBytes,
                 const MigrationSecondaryThrottleOptions& secondaryThrottle,
                 bool waitForDelete) {
    auto shardRegistry = Grid::get(txn)->shardRegistry();
    auto shard = shardRegistry->getConfigShard();
    auto cmdResponseStatus = shard->runCommand(
        txn,
        kPrimaryOnlyReadPreference,
        "admin",
        BalanceChunkRequest::serializeToMoveCommandForConfig(
            chunk, newShardId, maxChunkSizeBytes, secondaryThrottle, waitForDelete),
        Shard::RetryPolicy::kIdempotent);
    if (!cmdResponseStatus.isOK()) {
        return cmdResponseStatus.getStatus();
    }

    return cmdResponseStatus.getValue().commandStatus;
}

Status rebalanceChunk(OperationContext* txn, const ChunkType& chunk) {
    auto shardRegistry = Grid::get(txn)->shardRegistry();
    auto shard = shardRegistry->getConfigShard();
    auto cmdResponseStatus =
        shard->runCommand(txn,
                          kPrimaryOnlyReadPreference,
                          "admin",
                          BalanceChunkRequest::serializeToRebalanceCommandForConfig(chunk),
                          Shard::RetryPolicy::kIdempotent);
    if (!cmdResponseStatus.isOK()) {
        return cmdResponseStatus.getStatus();
    }

    return cmdResponseStatus.getValue().commandStatus;
}

}  // namespace configsvr_client
}  // namespace mongo
