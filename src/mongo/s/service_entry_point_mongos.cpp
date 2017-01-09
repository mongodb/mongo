/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/s/service_entry_point_mongos.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/transport/service_entry_point_utils.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/thread_idle_callback.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using transport::TransportLayer;

namespace {

BSONObj buildErrReply(const DBException& ex) {
    BSONObjBuilder errB;
    errB.append("$err", ex.what());
    errB.append("code", ex.getCode());
    if (!ex._shard.empty()) {
        errB.append("shard", ex._shard);
    }
    return errB.obj();
}

}  // namespace

ServiceEntryPointMongos::ServiceEntryPointMongos(TransportLayer* tl) : _tl(tl) {}

void ServiceEntryPointMongos::startSession(transport::SessionHandle session) {
    launchWrappedServiceEntryWorkerThread(
        std::move(session),
        [this](const transport::SessionHandle& session) { _sessionLoop(session); });
}

void ServiceEntryPointMongos::_sessionLoop(const transport::SessionHandle& session) {
    int64_t counter = 0;

    while (true) {
        // Release any cached egress connections for client back to pool before destroying
        auto guard = MakeGuard(ShardConnection::releaseMyConnections);

        Message message;

        // Source a Message from the client
        {
            auto status = session->sourceMessage(&message).wait();

            if (ErrorCodes::isInterruption(status.code()) ||
                ErrorCodes::isNetworkError(status.code())) {
                break;
            }

            // Our session may have been closed internally.
            if (status == TransportLayer::TicketSessionClosedStatus) {
                break;
            }

            uassertStatusOK(status);
        }

        auto txn = cc().makeOperationContext();

        const int32_t msgId = message.header().getId();

        const NetworkOp op = message.operation();

        // This exception will not be returned to the caller, but will be logged and will close the
        // connection
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "Message type " << op << " is not supported.",
                op > dbMsg);

        // Start a new LastError session. Any exceptions thrown from here onwards will be returned
        // to the caller (if the type of the message permits it).
        ClusterLastErrorInfo::get(txn->getClient()).newRequest();
        LastError::get(txn->getClient()).startRequest();

        DbMessage dbm(message);

        NamespaceString nss;

        try {

            if (dbm.messageShouldHaveNs()) {
                nss = NamespaceString(StringData(dbm.getns()));

                uassert(ErrorCodes::InvalidNamespace,
                        str::stream() << "Invalid ns [" << nss.ns() << "]",
                        nss.isValid());

                uassert(ErrorCodes::IllegalOperation,
                        "Can't use 'local' database through mongos",
                        nss.db() != NamespaceString::kLocalDb);
            }

            AuthorizationSession::get(txn->getClient())->startRequest(txn.get());

            LOG(3) << "Request::process begin ns: " << nss << " msg id: " << msgId
                   << " op: " << networkOpToString(op);

            switch (op) {
                case dbQuery:
                    if (nss.isCommand() || nss.isSpecialCommand()) {
                        Strategy::clientCommandOp(txn.get(), nss, &dbm);
                    } else {
                        Strategy::queryOp(txn.get(), nss, &dbm);
                    }
                    break;
                case dbGetMore:
                    Strategy::getMore(txn.get(), nss, &dbm);
                    break;
                case dbKillCursors:
                    Strategy::killCursors(txn.get(), &dbm);
                    break;
                default:
                    Strategy::writeOp(txn.get(), &dbm);
                    break;
            }

            LOG(3) << "Request::process end ns: " << nss << " msg id: " << msgId
                   << " op: " << networkOpToString(op);

        } catch (const DBException& ex) {
            LOG(1) << "Exception thrown"
                   << " while processing " << networkOpToString(op) << " op"
                   << " for " << nss.ns() << causedBy(ex);

            if (op == dbQuery || op == dbGetMore) {
                replyToQuery(ResultFlag_ErrSet, session, message, buildErrReply(ex));
            }

            // We *always* populate the last error for now
            LastError::get(txn->getClient()).setLastError(ex.getCode(), ex.what());
        }

        if ((counter++ & 0xf) == 0) {
            markThreadIdle();
        }
    }
}

}  // namespace mongo
