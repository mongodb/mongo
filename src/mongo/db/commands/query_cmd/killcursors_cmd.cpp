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
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/killcursors_common.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/client_cursor/kill_cursors_gen.h"
#include "mongo/db/stats/top.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
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
