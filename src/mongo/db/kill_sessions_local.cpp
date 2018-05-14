/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/kill_sessions_local.h"

#include "mongo/db/client.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/kill_sessions_common.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session.h"
#include "mongo/db/session_catalog.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {
void killSessionsLocalKillCursors(OperationContext* opCtx, const SessionKiller::Matcher& matcher) {

    auto res = CursorManager::killCursorsWithMatchingSessions(opCtx, matcher);
    uassertStatusOK(res.first);
}
}  // namespace

void killSessionsLocalKillTransactions(OperationContext* opCtx,
                                       const SessionKiller::Matcher& matcher,
                                       bool shouldKillClientCursors) {
    SessionCatalog::get(opCtx)->scanSessions(
        opCtx, matcher, [shouldKillClientCursors](OperationContext* opCtx, Session* session) {
            session->abortArbitraryTransaction(opCtx, shouldKillClientCursors);
        });
}

void killSessionsLocalKillTransactionCursors(OperationContext* opCtx,
                                             const SessionKiller::Matcher& matcher) {
    SessionCatalog::get(opCtx)->scanSessions(
        opCtx, matcher, [](OperationContext* opCtx, Session* session) {
            session->killTransactionCursors(opCtx);
        });
}

SessionKiller::Result killSessionsLocal(OperationContext* opCtx,
                                        const SessionKiller::Matcher& matcher,
                                        SessionKiller::UniformRandomBitGenerator* urbg) {
    killSessionsLocalKillTransactions(opCtx, matcher);
    uassertStatusOK(killSessionsLocalKillOps(opCtx, matcher));
    killSessionsLocalKillCursors(opCtx, matcher);
    return {std::vector<HostAndPort>{}};
}

void killAllExpiredTransactions(OperationContext* opCtx) {
    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    SessionCatalog::get(opCtx)->scanSessions(
        opCtx, matcherAllSessions, [](OperationContext* opCtx, Session* session) {
            try {
                session->abortArbitraryTransactionIfExpired(opCtx);
            } catch (const DBException& ex) {
                Status status = ex.toStatus();
                std::string errmsg = str::stream()
                    << "May have failed to abort expired transaction with session id (lsid) '"
                    << session->getSessionId() << "'."
                    << " Caused by: " << status;

                // LockTimeout errors are expected if we are unable to acquire an IS lock to clean
                // up transaction cursors. The transaction abort (and lock resource release) should
                // have succeeded despite failing to clean up cursors. The cursors will eventually
                // be cleaned up by the cursor manager. We'll log such errors at a higher log level
                // for diagnostic purposes in case something gets stuck.
                if (ErrorCodes::isShutdownError(status.code()) ||
                    status == ErrorCodes::LockTimeout) {
                    LOG(1) << errmsg;
                } else {
                    warning() << errmsg;
                }
            }
        });
}

}  // namespace mongo
