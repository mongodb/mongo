// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/killcursors_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"


namespace mongo {
namespace {

struct ClusterKillCursorsCmd {
    static constexpr bool supportsReadConcern = true;
    static Status doCheckAuth(OperationContext* opCtx,
                              const NamespaceString& nss,
                              CursorId cursorId) {
        auto const authzSession = AuthorizationSession::get(opCtx->getClient());
        AuthzCheckFn authChecker = [&authzSession, &nss](AuthzCheckFnInputType userName) -> Status {
            return auth::checkAuthForKillCursors(authzSession, nss, userName);
        };

        return Grid::get(opCtx)->getCursorManager()->checkAuthCursor(opCtx, cursorId, authChecker);
    }

    static Status doKillCursor(OperationContext* opCtx,
                               const NamespaceString& nss,
                               CursorId cursorId) {
        auto const authzSession = AuthorizationSession::get(opCtx->getClient());
        AuthzCheckFn authChecker = [&authzSession, &nss](AuthzCheckFnInputType userName) -> Status {
            return auth::checkAuthForKillCursors(authzSession, nss, userName);
        };

        return Grid::get(opCtx)->getCursorManager()->killCursorWithAuthCheck(
            opCtx, cursorId, authChecker);
    }
};
MONGO_REGISTER_COMMAND(KillCursorsCmdBase<ClusterKillCursorsCmd>).forRouter();

}  // namespace
}  // namespace mongo
