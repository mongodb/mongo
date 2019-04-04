/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/db/run_op_kill_cursors.h"

#include "merizo/base/data_cursor.h"
#include "merizo/db/audit.h"
#include "merizo/db/auth/authorization_session.h"
#include "merizo/db/cursor_id.h"
#include "merizo/db/cursor_manager.h"
#include "merizo/db/db_raii.h"
#include "merizo/util/exit.h"

namespace merizo {

namespace {

bool killCursorIfAuthorized(OperationContext* opCtx, CursorId id) {
    auto cursorManager = CursorManager::get(opCtx);

    auto pin = cursorManager->pinCursor(opCtx, id, CursorManager::kNoCheckSession);
    if (!pin.isOK()) {
        // Either the cursor doesn't exist, or it was killed during the last time it was being
        // used, and was cleaned up after this call. Either way, we cannot kill it. Write the
        // attempt to the audit log before returning.
        audit::logKillCursorsAuthzCheck(opCtx->getClient(), {}, id, pin.getStatus().code());
        return false;
    }
    auto nss = pin.getValue().getCursor()->nss();
    invariant(nss.isValid());

    boost::optional<AutoStatsTracker> statsTracker;
    if (!nss.isCollectionlessCursorNamespace()) {
        const boost::optional<int> dbProfilingLevel = boost::none;
        statsTracker.emplace(opCtx,
                             nss,
                             Top::LockType::NotLocked,
                             AutoStatsTracker::LogMode::kUpdateTopAndCurop,
                             dbProfilingLevel);
    }

    AuthorizationSession* as = AuthorizationSession::get(opCtx->getClient());
    auto cursorOwner = pin.getValue().getCursor()->getAuthenticatedUsers();
    auto authStatus = as->checkAuthForKillCursors(nss, cursorOwner);
    if (!authStatus.isOK()) {
        audit::logKillCursorsAuthzCheck(opCtx->getClient(), nss, id, authStatus.code());
        return false;
    }

    // Release the pin so that the cursor can be killed.
    pin.getValue().release();

    Status killStatus = cursorManager->killCursor(opCtx, id, true /* shouldAudit */);
    massert(28697,
            killStatus.reason(),
            killStatus.code() == ErrorCodes::OK || killStatus.code() == ErrorCodes::CursorNotFound);
    return killStatus.isOK();
}

}  // namespace

int runOpKillCursors(OperationContext* opCtx, size_t numCursorIds, const char* idsArray) {
    ConstDataCursor idsDataCursor(idsArray);
    int numKilled = 0;
    for (size_t i = 0; i < numCursorIds; i++) {
        CursorId nextCursorId = idsDataCursor.readAndAdvance<LittleEndian<int64_t>>();
        if (killCursorIfAuthorized(opCtx, nextCursorId)) {
            ++numKilled;
        }

        if (globalInShutdownDeprecated()) {
            break;
        }
    }
    return numKilled;
}

}  // namespace merizo
