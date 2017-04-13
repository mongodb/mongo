/**
 *    Copyright (C) 2008-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/assemble_response.h"

#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/cursor_manager.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/diag_log.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/query/find.h"
#include "mongo/db/run_commands.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/top.h"
#include "mongo/rpc/command_reply_builder.h"
#include "mongo/rpc/command_request.h"
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/rpc/legacy_request.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"

namespace mongo {

const HostAndPort kHostAndPortForDirectClient("0.0.0.0", 0);

MONGO_FP_DECLARE(rsStopGetMore);

namespace {
using logger::LogComponent;

inline void opread(Message& m) {
    if (_diaglog.getLevel() & 2) {
        _diaglog.readop(m.singleData().view2ptr(), m.header().getLen());
    }
}

inline void opwrite(Message& m) {
    if (_diaglog.getLevel() & 1) {
        _diaglog.writeop(m.singleData().view2ptr(), m.header().getLen());
    }
}

void generateLegacyQueryErrorResponse(const AssertionException* exception,
                                      const QueryMessage& queryMessage,
                                      CurOp* curop,
                                      Message* response) {
    curop->debug().exceptionInfo = exception->getInfo();

    log(LogComponent::kQuery) << "assertion " << exception->toString() << " ns:" << queryMessage.ns
                              << " query:" << (queryMessage.query.valid(BSONVersion::kLatest)
                                                   ? queryMessage.query.toString()
                                                   : "query object is corrupt");
    if (queryMessage.ntoskip || queryMessage.ntoreturn) {
        log(LogComponent::kQuery) << " ntoskip:" << queryMessage.ntoskip
                                  << " ntoreturn:" << queryMessage.ntoreturn;
    }

    const SendStaleConfigException* scex = (exception->getCode() == ErrorCodes::SendStaleConfig)
        ? static_cast<const SendStaleConfigException*>(exception)
        : NULL;

    BSONObjBuilder err;
    exception->getInfo().append(err);
    if (scex) {
        err.append("ok", 0.0);
        err.append("ns", scex->getns());
        scex->getVersionReceived().addToBSON(err, "vReceived");
        scex->getVersionWanted().addToBSON(err, "vWanted");
    }
    BSONObj errObj = err.done();

    if (scex) {
        log(LogComponent::kQuery) << "stale version detected during query over " << queryMessage.ns
                                  << " : " << errObj;
    }

    BufBuilder bb;
    bb.skip(sizeof(QueryResult::Value));
    bb.appendBuf((void*)errObj.objdata(), errObj.objsize());

    // TODO: call replyToQuery() from here instead of this!!! see dbmessage.h
    QueryResult::View msgdata = bb.buf();
    QueryResult::View qr = msgdata;
    qr.setResultFlags(ResultFlag_ErrSet);
    if (scex)
        qr.setResultFlags(qr.getResultFlags() | ResultFlag_ShardConfigStale);
    qr.msgdata().setLen(bb.len());
    qr.msgdata().setOperation(opReply);
    qr.setCursorId(0);
    qr.setStartingFrom(0);
    qr.setNReturned(1);
    response->setData(bb.release());
}

/**
 * Fills out CurOp / OpDebug with basic command info.
 */
void beginCommandOp(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& queryObj) {
    auto curop = CurOp::get(opCtx);
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    curop->setQuery_inlock(queryObj);
    curop->setNS_inlock(nss.ns());
}

void receivedCommand(OperationContext* opCtx,
                     const NamespaceString& nss,
                     Client& client,
                     DbResponse& dbResponse,
                     Message& message) {
    invariant(nss.isCommand());

    const int32_t responseToMsgId = message.header().getId();

    DbMessage dbMessage(message);
    QueryMessage queryMessage(dbMessage);

    CurOp* op = CurOp::get(opCtx);

    rpc::LegacyReplyBuilder builder{};

    try {
        // This will throw if the request is on an invalid namespace.
        rpc::LegacyRequest request{&message};
        // Auth checking for Commands happens later.
        int nToReturn = queryMessage.ntoreturn;

        beginCommandOp(opCtx, nss, request.getCommandArgs());

        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            op->markCommand_inlock();
        }

        uassert(16979,
                str::stream() << "bad numberToReturn (" << nToReturn
                              << ") for $cmd type ns - can only be 1 or -1",
                nToReturn == 1 || nToReturn == -1);

        runCommands(opCtx, request, &builder);

        op->debug().iscommand = true;
    } catch (const DBException& exception) {
        Command::generateErrorResponse(opCtx, &builder, exception);
    }

    auto response = builder.done();

    op->debug().responseLength = response.header().dataLen();

    dbResponse.response = std::move(response);
    dbResponse.responseToMsgId = responseToMsgId;
}

void receivedRpc(OperationContext* opCtx,
                 Client& client,
                 DbResponse& dbResponse,
                 Message& message) {
    invariant(message.operation() == dbCommand);

    const int32_t responseToMsgId = message.header().getId();

    rpc::CommandReplyBuilder replyBuilder{};

    auto curOp = CurOp::get(opCtx);

    try {
        // database is validated here
        rpc::CommandRequest request{&message};

        // We construct a legacy $cmd namespace so we can fill in curOp using
        // the existing logic that existed for OP_QUERY commands
        NamespaceString nss(request.getDatabase(), "$cmd");
        beginCommandOp(opCtx, nss, request.getCommandArgs());
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp->markCommand_inlock();
        }

        runCommands(opCtx, request, &replyBuilder);

        curOp->debug().iscommand = true;

    } catch (const DBException& exception) {
        Command::generateErrorResponse(opCtx, &replyBuilder, exception);
    }

    auto response = replyBuilder.done();

    curOp->debug().responseLength = response.header().dataLen();

    dbResponse.response = std::move(response);
    dbResponse.responseToMsgId = responseToMsgId;
}

// In SERVER-7775 we reimplemented the pseudo-commands fsyncUnlock, inProg, and killOp
// as ordinary commands. To support old clients for another release, this helper serves
// to execute the real command from the legacy pseudo-command codepath.
// TODO: remove after MongoDB 3.2 is released
void receivedPseudoCommand(OperationContext* opCtx,
                           Client& client,
                           DbResponse& dbResponse,
                           Message& message,
                           StringData realCommandName) {
    DbMessage originalDbm(message);

    auto originalNToSkip = originalDbm.pullInt();

    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "invalid nToSkip - expected 0, but got " << originalNToSkip,
            originalNToSkip == 0);

    auto originalNToReturn = originalDbm.pullInt();

    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "invalid nToReturn - expected -1 or 1, but got " << originalNToSkip,
            originalNToReturn == -1 || originalNToReturn == 1);

    auto cmdParams = originalDbm.nextJsObj();

    Message interposed;
    // HACK:
    // legacy pseudo-commands could run on any database. The command replacements
    // can only run on 'admin'. To avoid breaking old shells and a multitude
    // of third-party tools, we rewrite the namespace. As auth is checked
    // later in Command::_checkAuthorizationImpl, we will still properly
    // reject the request if the client is not authorized.
    NamespaceString interposedNss("admin", "$cmd");

    BSONObjBuilder cmdBob;
    cmdBob.append(realCommandName, 1);
    cmdBob.appendElements(cmdParams);
    auto cmd = cmdBob.done();

    // TODO: use OP_COMMAND here instead of constructing
    // a legacy OP_QUERY style command
    BufBuilder cmdMsgBuf;

    int32_t flags = DataView(message.header().data()).read<LittleEndian<int32_t>>();
    cmdMsgBuf.appendNum(flags);

    cmdMsgBuf.appendStr(interposedNss.db(), false);  // not including null byte
    cmdMsgBuf.appendStr(".$cmd");
    cmdMsgBuf.appendNum(0);  // ntoskip
    cmdMsgBuf.appendNum(1);  // ntoreturn
    cmdMsgBuf.appendBuf(cmd.objdata(), cmd.objsize());

    interposed.setData(dbQuery, cmdMsgBuf.buf(), cmdMsgBuf.len());
    interposed.header().setId(message.header().getId());

    receivedCommand(opCtx, interposedNss, client, dbResponse, interposed);
}

void receivedQuery(OperationContext* opCtx,
                   const NamespaceString& nss,
                   Client& c,
                   DbResponse& dbResponse,
                   Message& m) {
    invariant(!nss.isCommand());
    globalOpCounters.gotQuery();

    int32_t responseToMsgId = m.header().getId();

    DbMessage d(m);
    QueryMessage q(d);

    CurOp& op = *CurOp::get(opCtx);

    try {
        Client* client = opCtx->getClient();
        Status status = AuthorizationSession::get(client)->checkAuthForFind(nss, false);
        audit::logQueryAuthzCheck(client, nss, q.query, status.code());
        uassertStatusOK(status);

        dbResponse.exhaustNS = runQuery(opCtx, q, nss, dbResponse.response);
    } catch (const AssertionException& e) {
        // If we got a stale config, wait in case the operation is stuck in a critical section
        if (!opCtx->getClient()->isInDirectClient() && e.getCode() == ErrorCodes::SendStaleConfig) {
            auto& sce = static_cast<const StaleConfigException&>(e);
            ShardingState::get(opCtx)->onStaleShardVersion(
                opCtx, NamespaceString(sce.getns()), sce.getVersionReceived());
        }

        dbResponse.response.reset();
        generateLegacyQueryErrorResponse(&e, q, &op, &dbResponse.response);
    }

    op.debug().responseLength = dbResponse.response.header().dataLen();
    dbResponse.responseToMsgId = responseToMsgId;
}

void receivedKillCursors(OperationContext* opCtx, Message& m) {
    LastError::get(opCtx->getClient()).disable();
    DbMessage dbmessage(m);
    int n = dbmessage.pullInt();

    uassert(13659, "sent 0 cursors to kill", n != 0);
    massert(13658,
            str::stream() << "bad kill cursors size: " << m.dataSize(),
            m.dataSize() == 8 + (8 * n));
    uassert(13004, str::stream() << "sent negative cursors to kill: " << n, n >= 1);

    if (n > 2000) {
        (n < 30000 ? warning() : error()) << "receivedKillCursors, n=" << n;
        verify(n < 30000);
    }

    const char* cursorArray = dbmessage.getArray(n);

    int found = CursorManager::eraseCursorGlobalIfAuthorized(opCtx, n, cursorArray);

    if (shouldLog(logger::LogSeverity::Debug(1)) || found != n) {
        LOG(found == n ? 1 : 0) << "killcursors: found " << found << " of " << n;
    }
}

void receivedInsert(OperationContext* opCtx, const NamespaceString& nsString, Message& m) {
    auto insertOp = parseLegacyInsert(m);
    invariant(insertOp.ns == nsString);
    for (const auto& obj : insertOp.documents) {
        Status status =
            AuthorizationSession::get(opCtx->getClient())->checkAuthForInsert(opCtx, nsString, obj);
        audit::logInsertAuthzCheck(opCtx->getClient(), nsString, obj, status.code());
        uassertStatusOK(status);
    }
    performInserts(opCtx, insertOp);
}

void receivedUpdate(OperationContext* opCtx, const NamespaceString& nsString, Message& m) {
    auto updateOp = parseLegacyUpdate(m);
    auto& singleUpdate = updateOp.updates[0];
    invariant(updateOp.ns == nsString);

    Status status =
        AuthorizationSession::get(opCtx->getClient())
            ->checkAuthForUpdate(
                opCtx, nsString, singleUpdate.query, singleUpdate.update, singleUpdate.upsert);
    audit::logUpdateAuthzCheck(opCtx->getClient(),
                               nsString,
                               singleUpdate.query,
                               singleUpdate.update,
                               singleUpdate.upsert,
                               singleUpdate.multi,
                               status.code());
    uassertStatusOK(status);

    performUpdates(opCtx, updateOp);
}

void receivedDelete(OperationContext* opCtx, const NamespaceString& nsString, Message& m) {
    auto deleteOp = parseLegacyDelete(m);
    auto& singleDelete = deleteOp.deletes[0];
    invariant(deleteOp.ns == nsString);

    Status status = AuthorizationSession::get(opCtx->getClient())
                        ->checkAuthForDelete(opCtx, nsString, singleDelete.query);
    audit::logDeleteAuthzCheck(opCtx->getClient(), nsString, singleDelete.query, status.code());
    uassertStatusOK(status);

    performDeletes(opCtx, deleteOp);
}

bool receivedGetMore(OperationContext* opCtx, DbResponse& dbresponse, Message& m, CurOp& curop) {
    globalOpCounters.gotGetMore();
    DbMessage d(m);

    const char* ns = d.getns();
    int ntoreturn = d.pullInt();
    uassert(
        34419, str::stream() << "Invalid ntoreturn for OP_GET_MORE: " << ntoreturn, ntoreturn >= 0);
    long long cursorid = d.pullInt64();

    curop.debug().ntoreturn = ntoreturn;
    curop.debug().cursorid = cursorid;

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setNS_inlock(ns);
    }

    bool exhaust = false;
    bool isCursorAuthorized = false;

    try {
        const NamespaceString nsString(ns);
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid ns [" << ns << "]",
                nsString.isValid());

        Status status = AuthorizationSession::get(opCtx->getClient())
                            ->checkAuthForGetMore(nsString, cursorid, false);
        audit::logGetMoreAuthzCheck(opCtx->getClient(), nsString, cursorid, status.code());
        uassertStatusOK(status);

        while (MONGO_FAIL_POINT(rsStopGetMore)) {
            sleepmillis(0);
        }

        dbresponse.response =
            getMore(opCtx, ns, ntoreturn, cursorid, &exhaust, &isCursorAuthorized);
    } catch (AssertionException& e) {
        if (isCursorAuthorized) {
            // If a cursor with id 'cursorid' was authorized, it may have been advanced
            // before an exception terminated processGetMore.  Erase the ClientCursor
            // because it may now be out of sync with the client's iteration state.
            // SERVER-7952
            // TODO Temporary code, see SERVER-4563 for a cleanup overview.
            CursorManager::eraseCursorGlobal(opCtx, cursorid);
        }

        BSONObjBuilder err;
        e.getInfo().append(err);
        BSONObj errObj = err.done();

        curop.debug().exceptionInfo = e.getInfo();

        replyToQuery(ResultFlag_ErrSet, m, dbresponse, errObj);
        curop.debug().responseLength = dbresponse.response.header().dataLen();
        curop.debug().nreturned = 1;
        return false;
    }

    curop.debug().responseLength = dbresponse.response.header().dataLen();
    auto queryResult = QueryResult::ConstView(dbresponse.response.buf());
    curop.debug().nreturned = queryResult.getNReturned();

    dbresponse.responseToMsgId = m.header().getId();

    if (exhaust) {
        curop.debug().exhaust = true;
        dbresponse.exhaustNS = ns;
    }

    return true;
}

}  // namespace

// Returns false when request includes 'end'
void assembleResponse(OperationContext* opCtx,
                      Message& m,
                      DbResponse& dbresponse,
                      const HostAndPort& remote) {
    // before we lock...
    NetworkOp op = m.operation();
    bool isCommand = false;

    DbMessage dbmsg(m);

    Client& c = *opCtx->getClient();
    if (c.isInDirectClient()) {
        invariant(!opCtx->lockState()->inAWriteUnitOfWork());
    } else {
        LastError::get(c).startRequest();
        AuthorizationSession::get(c)->startRequest(opCtx);

        // We should not be holding any locks at this point
        invariant(!opCtx->lockState()->isLocked());
    }

    const char* ns = dbmsg.messageShouldHaveNs() ? dbmsg.getns() : NULL;
    const NamespaceString nsString = ns ? NamespaceString(ns) : NamespaceString();

    if (op == dbQuery) {
        if (nsString.isCommand()) {
            isCommand = true;
            opwrite(m);
        }
        // TODO: remove this entire code path after 3.2. Refs SERVER-7775
        else if (nsString.isSpecialCommand()) {
            opwrite(m);

            if (nsString.coll() == "$cmd.sys.inprog") {
                receivedPseudoCommand(opCtx, c, dbresponse, m, "currentOp");
                return;
            }
            if (nsString.coll() == "$cmd.sys.killop") {
                receivedPseudoCommand(opCtx, c, dbresponse, m, "killOp");
                return;
            }
            if (nsString.coll() == "$cmd.sys.unlock") {
                receivedPseudoCommand(opCtx, c, dbresponse, m, "fsyncUnlock");
                return;
            }
        } else {
            opread(m);
        }
    } else if (op == dbGetMore) {
        opread(m);
    } else if (op == dbCommand) {
        isCommand = true;
        opwrite(m);
    } else {
        opwrite(m);
    }

    CurOp& currentOp = *CurOp::get(opCtx);
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        // Commands handling code will reset this if the operation is a command
        // which is logically a basic CRUD operation like query, insert, etc.
        currentOp.setNetworkOp_inlock(op);
        currentOp.setLogicalOp_inlock(networkOpToLogicalOp(op));
    }

    OpDebug& debug = currentOp.debug();

    long long logThresholdMs = serverGlobalParams.slowMS;
    bool shouldLogOpDebug = shouldLog(logger::LogSeverity::Debug(1));

    if (op == dbQuery) {
        if (isCommand) {
            receivedCommand(opCtx, nsString, c, dbresponse, m);
        } else {
            receivedQuery(opCtx, nsString, c, dbresponse, m);
        }
    } else if (op == dbCommand) {
        receivedRpc(opCtx, c, dbresponse, m);
    } else if (op == dbGetMore) {
        if (!receivedGetMore(opCtx, dbresponse, m, currentOp))
            shouldLogOpDebug = true;
    } else {
        // The remaining operations do not return any response. They are fire-and-forget.
        try {
            if (op == dbKillCursors) {
                currentOp.ensureStarted();
                logThresholdMs = 10;
                receivedKillCursors(opCtx, m);
            } else if (op != dbInsert && op != dbUpdate && op != dbDelete) {
                log() << "    operation isn't supported: " << static_cast<int>(op);
                currentOp.done();
                shouldLogOpDebug = true;
            } else {
                if (remote != kHostAndPortForDirectClient) {
                    const ShardedConnectionInfo* connInfo = ShardedConnectionInfo::get(&c, false);
                    uassert(18663,
                            str::stream() << "legacy writeOps not longer supported for "
                                          << "versioned connections, ns: "
                                          << nsString.ns()
                                          << ", op: "
                                          << networkOpToString(op)
                                          << ", remote: "
                                          << remote.toString(),
                            connInfo == NULL);
                }

                if (!nsString.isValid()) {
                    uassert(16257, str::stream() << "Invalid ns [" << ns << "]", false);
                } else if (op == dbInsert) {
                    receivedInsert(opCtx, nsString, m);
                } else if (op == dbUpdate) {
                    receivedUpdate(opCtx, nsString, m);
                } else if (op == dbDelete) {
                    receivedDelete(opCtx, nsString, m);
                } else {
                    invariant(false);
                }
            }
        } catch (const UserException& ue) {
            LastError::get(c).setLastError(ue.getCode(), ue.getInfo().msg);
            LOG(3) << " Caught Assertion in " << networkOpToString(op) << ", continuing "
                   << redact(ue);
            debug.exceptionInfo = ue.getInfo();
        } catch (const AssertionException& e) {
            LastError::get(c).setLastError(e.getCode(), e.getInfo().msg);
            LOG(3) << " Caught Assertion in " << networkOpToString(op) << ", continuing "
                   << redact(e);
            debug.exceptionInfo = e.getInfo();
            shouldLogOpDebug = true;
        }
    }
    currentOp.ensureStarted();
    currentOp.done();
    debug.executionTimeMicros = currentOp.totalTimeMicros();

    logThresholdMs += currentOp.getExpectedLatencyMs();
    Top::get(opCtx->getServiceContext())
        .incrementGlobalLatencyStats(
            opCtx, currentOp.totalTimeMicros(), currentOp.getReadWriteType());

    const bool shouldSample = serverGlobalParams.sampleRate == 1.0
        ? true
        : c.getPrng().nextCanonicalDouble() < serverGlobalParams.sampleRate;

    if (shouldLogOpDebug || (shouldSample && debug.executionTimeMicros > logThresholdMs * 1000LL)) {
        Locker::LockerInfo lockerInfo;
        opCtx->lockState()->getLockerInfo(&lockerInfo);
        log() << debug.report(&c, currentOp, lockerInfo.stats);
    }

    if (currentOp.shouldDBProfile(shouldSample)) {
        // Performance profiling is on
        if (opCtx->lockState()->isReadLocked()) {
            LOG(1) << "note: not profiling because recursive read lock";
        } else if (lockedForWriting()) {
            // TODO SERVER-26825: Fix race condition where fsyncLock is acquired post
            // lockedForWriting() call but prior to profile collection lock acquisition.
            LOG(1) << "note: not profiling because doing fsync+lock";
        } else if (storageGlobalParams.readOnly) {
            LOG(1) << "note: not profiling because server is read-only";
        } else {
            profile(opCtx, op);
        }
    }

    recordCurOpMetrics(opCtx);
}

}  // namespace mongo
