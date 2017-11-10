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
#pragma once

#include "mongo/executor/task_executor.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/router_exec_stage.h"

namespace mongo {
/**
 * Uses a RouterStageMerge to merge results, and monitors the merged stream for special
 * sentinel documents which indicate the the set of cursors needs to be updated.  When the
 * sentinel is detected, removes it from the stream and updates the set of cursors.
 *
 * cmdToRunOnNewShards: Command to execute on the new shard to open the cursor.
 */
class RouterStageUpdateOnAddShard final : public RouterExecStage {
public:
    RouterStageUpdateOnAddShard(OperationContext* opCtx,
                                executor::TaskExecutor* executor,
                                ClusterClientCursorParams* params,
                                BSONObj cmdToRunOnNewShards);

    StatusWith<ClusterQueryResult> next(ExecContext) final;

private:
    /**
     * Establish the new cursors and tell the RouterStageMerge about them.
     * obj: The BSONObj which triggered the establishment of the new cursors
     */
    Status addNewShardCursors(BSONObj obj);

    /**
     * Open the cursors on the new shards.
     */
    StatusWith<std::vector<ClusterClientCursorParams::RemoteCursor>>
    establishShardCursorsOnNewShards(std::vector<ShardId> existingShardIds,
                                     const BSONObj& newShardDetectedObj);

    ClusterClientCursorParams* _params;
    BSONObj _cmdToRunOnNewShards;
};
}
