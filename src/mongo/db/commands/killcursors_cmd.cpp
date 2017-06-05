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

private:
    Status _killCursor(OperationContext* opCtx,
                       const NamespaceString& nss,
                       CursorId cursorId) final {
        // Cursors come in one of two flavors:
        // - Cursors owned by the collection cursor manager, such as those generated via the find
        //   command. For these cursors, we hold the appropriate collection lock for the duration of
        //   the getMore using AutoGetCollectionForRead. This will automatically update the CurOp
        //   object appropriately and record execution time via Top upon completion.
        // - Cursors owned by the global cursor manager, such as those generated via the aggregate
        //   command. These cursors either hold no collection state or manage their collection state
        //   internally, so we acquire no locks. In this case we use the AutoStatsTracker object to
        //   update the CurOp object appropriately and record execution time via Top upon
        //   completion.
        //
        // Thus, exactly one of 'readLock' and 'statsTracker' will be populated as we populate
        // 'cursorManager'.
        boost::optional<AutoGetCollectionForReadCommand> readLock;
        boost::optional<AutoStatsTracker> statsTracker;
        CursorManager* cursorManager;

        if (CursorManager::isGloballyManagedCursor(cursorId)) {
            cursorManager = CursorManager::getGlobalCursorManager();

            if (auto nssForCurOp = nss.isGloballyManagedNamespace()
                    ? nss.getTargetNSForGloballyManagedNamespace()
                    : nss) {
                const boost::optional<int> dbProfilingLevel = boost::none;
                statsTracker.emplace(
                    opCtx, *nssForCurOp, Top::LockType::NotLocked, dbProfilingLevel);
            }

            // Make sure the namespace of the cursor matches the namespace passed to the killCursors
            // command so we can be sure we checked the correct privileges.
            auto ccPin = cursorManager->pinCursor(opCtx, cursorId);
            if (ccPin.isOK()) {
                auto cursorNs = ccPin.getValue().getCursor()->nss();
                if (cursorNs != nss) {
                    return Status{ErrorCodes::Unauthorized,
                                  str::stream() << "issued killCursors on namespace '" << nss.ns()
                                                << "', but cursor with id "
                                                << cursorId
                                                << " belongs to a different namespace: "
                                                << cursorNs.ns()};
                }
            }
        } else {
            readLock.emplace(opCtx, nss);
            Collection* collection = readLock->getCollection();
            if (!collection) {
                return {ErrorCodes::CursorNotFound,
                        str::stream() << "collection does not exist: " << nss.ns()};
            }
            cursorManager = collection->getCursorManager();
        }
        invariant(cursorManager);

        return cursorManager->eraseCursor(opCtx, cursorId, true /*shouldAudit*/);
    }
} killCursorsCmd;

}  // namespace mongo
