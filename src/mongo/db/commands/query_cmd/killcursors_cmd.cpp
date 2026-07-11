// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/killcursors_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/stats/top.h"

#include <boost/optional/optional.hpp>

namespace mongo {

struct KillCursorsCmd {
    static constexpr bool supportsReadConcern = false;
    static Status doCheckAuth(OperationContext* opCtx, const NamespaceString& nss, CursorId id) {
        return CursorManager::get(opCtx)->checkAuthForKillCursors(opCtx, id);
    }
    static Status doKillCursor(OperationContext* opCtx, const NamespaceString& nss, CursorId id) {
        boost::optional<AutoStatsTracker> statsTracker;
        if (!nss.isCollectionlessCursorNamespace()) {
            statsTracker.emplace(opCtx,
                                 nss,
                                 Top::LockType::NotLocked,
                                 AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                 DatabaseProfileSettings::get(opCtx->getServiceContext())
                                     .getDatabaseProfileLevel(nss.dbName()));
        }

        auto authCheck = [&](const ClientCursor& cc) {
            uassertStatusOK(
                auth::checkAuthForKillCursors(AuthorizationSession::get(opCtx->getClient()),
                                              cc.nss(),
                                              cc.getAuthenticatedUser()));
        };

        auto cursorManager = CursorManager::get(opCtx);
        return cursorManager->killCursorWithAuthCheck(opCtx, id, authCheck);
    }
};
MONGO_REGISTER_COMMAND(KillCursorsCmdBase<KillCursorsCmd>).forShard();

}  // namespace mongo
