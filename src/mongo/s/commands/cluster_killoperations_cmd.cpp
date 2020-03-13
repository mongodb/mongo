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
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"

namespace mongo {

class ClusterKillOperationsCmd : public KillOperationsCmdBase<ClusterKillOperationsCmd> {
public:
    static void killCursors(OperationContext* opCtx, const std::vector<OperationKey>& opKeys) {
        auto clusterCursorManager = Grid::get(opCtx)->getCursorManager();
        for (auto& cursorId : clusterCursorManager->getCursorsForOpKeys(opKeys)) {
            LOGV2(4664805, "Attempting to kill cursor", "cursorId"_attr = cursorId);
            auto cursorNss = clusterCursorManager->getNamespaceForCursorId(cursorId);
            if (!cursorNss) {
                // The cursor must have already been killed.
                continue;
            }

            auto status = clusterCursorManager->killCursor(opCtx, cursorNss.get(), cursorId);

            if (!status.isOK()) {
                LOGV2(4664806,
                      "Failed to kill the cursor ",
                      "status"_attr = redact(status.toString()));
            } else {
                LOGV2(4664807, "Killed cursor", "cursorId"_attr = cursorId);
            }
        }
    }
} ClusterKillOperationsCmd;

}  // namespace mongo
