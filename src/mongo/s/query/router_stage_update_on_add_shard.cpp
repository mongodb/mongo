/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery
#include "mongo/s/query/router_stage_update_on_add_shard.h"

#include <algorithm>

#include "mongo/base/checked_cast.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/router_stage_merge.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

// Returns true if the change stream document has an 'operationType' of 'newShardDetected'.
bool needsUpdate(const StatusWith<ClusterQueryResult>& childResult) {
    if (!childResult.isOK() || childResult.getValue().isEOF()) {
        return false;
    }
    return ((*childResult.getValue().getResult())[DocumentSourceChangeStream::kOperationTypeField]
                .str() == DocumentSourceChangeStream::kNewShardDetectedOpType);
}
}

RouterStageUpdateOnAddShard::RouterStageUpdateOnAddShard(OperationContext* opCtx,
                                                         executor::TaskExecutor* executor,
                                                         ClusterClientCursorParams* params,
                                                         std::vector<ShardId> shardIds,
                                                         BSONObj cmdToRunOnNewShards)
    : RouterExecStage(opCtx, stdx::make_unique<RouterStageMerge>(opCtx, executor, params)),
      _params(params),
      _shardIds(std::move(shardIds)),
      _cmdToRunOnNewShards(cmdToRunOnNewShards) {}

StatusWith<ClusterQueryResult> RouterStageUpdateOnAddShard::next(
    RouterExecStage::ExecContext execContext) {
    auto childStage = getChildStage();
    auto childResult = childStage->next(execContext);
    while (needsUpdate(childResult)) {
        addNewShardCursors(*childResult.getValue().getResult());
        childResult = childStage->next(execContext);
    }
    return childResult;
}

void RouterStageUpdateOnAddShard::addNewShardCursors(BSONObj newShardDetectedObj) {
    checked_cast<RouterStageMerge*>(getChildStage())
        ->addNewShardCursors(establishShardCursorsOnNewShards(newShardDetectedObj));
}

std::vector<RemoteCursor> RouterStageUpdateOnAddShard::establishShardCursorsOnNewShards(
    const BSONObj& newShardDetectedObj) {
    auto* opCtx = getOpCtx();
    // Reload the shard registry.  We need to ensure a reload initiated after calling this method
    // caused the reload, otherwise we aren't guaranteed to get all the new shards.
    auto* shardRegistry = Grid::get(opCtx)->shardRegistry();
    if (!shardRegistry->reload(opCtx)) {
        // A 'false' return from shardRegistry.reload() means a reload was already in progress and
        // it completed before reload() returned.  So another reload(), regardless of return
        // value, will ensure a reload started after the first call to reload().
        shardRegistry->reload(opCtx);
    }

    std::vector<ShardId> shardIds, newShardIds;
    shardRegistry->getAllShardIdsNoReload(&shardIds);
    std::sort(_shardIds.begin(), _shardIds.end());
    std::sort(shardIds.begin(), shardIds.end());
    std::set_difference(shardIds.begin(),
                        shardIds.end(),
                        _shardIds.begin(),
                        _shardIds.end(),
                        std::back_inserter(newShardIds));

    auto cmdObj = DocumentSourceChangeStream::replaceResumeTokenInCommand(
        _cmdToRunOnNewShards,
        newShardDetectedObj[DocumentSourceChangeStream::kIdField].embeddedObject());
    std::vector<std::pair<ShardId, BSONObj>> requests;
    for (const auto& shardId : newShardIds) {
        requests.emplace_back(shardId, cmdObj);
        _shardIds.push_back(shardId);
    }
    const bool allowPartialResults = false;  // partial results are not allowed
    return establishCursors(opCtx,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            _params->nsString,
                            _params->readPreference.value_or(ReadPreferenceSetting()),
                            requests,
                            allowPartialResults);
}

}  // namespace mongo
