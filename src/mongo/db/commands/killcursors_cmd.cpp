/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/disallow_copying.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/commands/killcursors_common.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/query/killcursors_request.h"
#include "mongo/db/stats/top.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

class KillCursorsCmd final : public KillCursorsCmdBase {
    MONGO_DISALLOW_COPYING(KillCursorsCmd);

public:
    KillCursorsCmd() = default;

    bool supportsReadConcern(const std::string& dbName,
                             const BSONObj& cmdObj,
                             repl::ReadConcernLevel level) const final {
        // killCursors must support snapshot read concern in order to be run in transactions.
        return level == repl::ReadConcernLevel::kLocalReadConcern ||
            level == repl::ReadConcernLevel::kSnapshotReadConcern;
    }

private:
    Status _checkAuth(Client* client, const NamespaceString& nss, CursorId id) const final {
        auto opCtx = client->getOperationContext();
        const auto check = [opCtx, id](CursorManager* manager) {
            return manager->checkAuthForKillCursors(opCtx, id);
        };

        return CursorManager::withCursorManager(opCtx, id, nss, check);
    }

    Status _killCursor(OperationContext* opCtx,
                       const NamespaceString& nss,
                       CursorId id) const final {
        boost::optional<AutoStatsTracker> statsTracker;
        if (CursorManager::isGloballyManagedCursor(id)) {
            if (auto nssForCurOp = nss.isGloballyManagedNamespace()
                    ? nss.getTargetNSForGloballyManagedNamespace()
                    : nss) {
                const boost::optional<int> dbProfilingLevel = boost::none;
                statsTracker.emplace(
                    opCtx, *nssForCurOp, Top::LockType::NotLocked, dbProfilingLevel);
            }
        }

        return CursorManager::withCursorManager(
            opCtx, id, nss, [opCtx, id](CursorManager* manager) {
                return manager->killCursor(opCtx, id, true /* shouldAudit */);
            });
    }
} killCursorsCmd;

}  // namespace mongo
