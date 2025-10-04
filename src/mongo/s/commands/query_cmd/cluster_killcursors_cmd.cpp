/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/killcursors_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/kill_cursors_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

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
