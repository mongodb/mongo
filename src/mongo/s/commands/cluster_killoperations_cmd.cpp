/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/killoperations_common.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/stdx/unordered_set.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

class ClusterKillOperationsCmd : public KillOperationsCmdBase<ClusterKillOperationsCmd> {
public:
    static void killCursors(OperationContext* opCtx, const std::vector<OperationKey>& opKeys) {
        auto clusterCursorManager = Grid::get(opCtx)->getCursorManager();
        stdx::unordered_set<UUID, UUID::Hash> opKeySet(opKeys.begin(), opKeys.end());

        std::size_t numCursorsKilled = clusterCursorManager->killCursorsSatisfying(
            opCtx,
            [&opKeySet](CursorId cursorId, const ClusterCursorManager::CursorEntry& entry) -> bool {
                if (!entry.getOperationKey()) {
                    return false;
                }

                bool hasOpKey = opKeySet.find(*entry.getOperationKey()) != opKeySet.end();
                if (hasOpKey) {
                    LOGV2(4664805,
                          "Attempting to kill cursor",
                          "cursorId"_attr = cursorId,
                          "opKey"_attr = entry.getOperationKey());
                }

                return hasOpKey;
            });

        LOGV2(
            4664806, "_killOperations command killed cursors", "numKilled"_attr = numCursorsKilled);
    }
} clusterKillOperationsCmd;

}  // namespace mongo
