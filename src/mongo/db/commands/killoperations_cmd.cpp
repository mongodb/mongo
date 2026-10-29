// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/killoperations_common.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/unordered_set.h"

#include <memory>
#include <vector>

#include <absl/container/node_hash_set.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

class KillOperationsCmd : public KillOperationsCmdBase<KillOperationsCmd> {
public:
    static void killCursors(OperationContext* opCtx, const std::vector<OperationKey>& opKeys) {
        auto cursorManager = CursorManager::get(opCtx);
        for (auto& cursorId : cursorManager->getCursorsForOpKeys(opKeys)) {
            LOGV2(4664802, "Attempting to kill cursor", "cursorId"_attr = cursorId);
            auto status = cursorManager->killCursor(opCtx, cursorId);

            if (!status.isOK()) {
                LOGV2(
                    4664803, "Failed to kill the cursor", "error"_attr = redact(status.toString()));
            } else {
                LOGV2(4664804, "Killed cursor", "cursorId"_attr = cursorId);
            }
        }
    }
};
MONGO_REGISTER_COMMAND(KillOperationsCmd).forShard();

}  // namespace mongo
