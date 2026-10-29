// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands.h"
#include "mongo/db/commands/killoperations_common.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/logv2/log.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <memory>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
};
MONGO_REGISTER_COMMAND(ClusterKillOperationsCmd).forRouter();

}  // namespace mongo
