// instance.cpp 

/**
*    Copyright (C) 2008 10gen Inc.
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

#include <fstream>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_manager_external_state_d.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/global_timestamp.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/process_id.h"
#include "mongo/rpc/command_reply_builder.h"
#include "mongo/rpc/command_request.h"
#include "mongo/rpc/legacy_reply.h"
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/rpc/legacy_request.h"
#include "mongo/rpc/legacy_request_builder.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/request_interface.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/d_state.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h" // for SendStaleConfigException
#include "mongo/scripting/engine.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/time_support.h"

namespace mongo {

    using std::unique_ptr;
    using logger::LogComponent;
    using std::unique_ptr;
    using std::endl;
    using std::hex;
    using std::ios;
    using std::ofstream;
    using std::string;
    using std::stringstream;
    using std::vector;

    // for diaglog
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

    void receivedKillCursors(OperationContext* txn, Message& m);

    void receivedUpdate(OperationContext* txn,
                        const NamespaceString& nsString,
                        Message& m,
                        CurOp& op);

    void receivedDelete(OperationContext* txn,
                        const NamespaceString& nsString,
                        Message& m,
                        CurOp& op);

    void receivedInsert(OperationContext* txn,
                        const NamespaceString& nsString,
                        Message& m,
                        CurOp& op);

    bool receivedGetMore(OperationContext* txn,
                         DbResponse& dbresponse,
                         Message& m,
                         CurOp& curop);

    int nloggedsome = 0;
#define LOGWITHRATELIMIT if( ++nloggedsome < 1000 || nloggedsome % 100 == 0 )

    string dbExecCommand;

    MONGO_FP_DECLARE(rsStopGetMore);

namespace {

    std::unique_ptr<AuthzManagerExternalState> createAuthzManagerExternalStateMongod() {
        return stdx::make_unique<AuthzManagerExternalStateMongod>();
    }

    MONGO_INITIALIZER(CreateAuthorizationExternalStateFactory) (InitializerContext* context) {
        AuthzManagerExternalState::create = &createAuthzManagerExternalStateMongod;
        return Status::OK();
    }

    void generateLegacyQueryErrorResponse(const AssertionException* exception,
                                          const QueryMessage& queryMessage,
                                          CurOp* curop,
                                          Message* response) {
        curop->debug().exceptionInfo = exception->getInfo();

        log(LogComponent::kQuery) << "assertion " << exception->toString()
                                  << " ns:" << queryMessage.ns << " query:"
                                  << (queryMessage.query.valid() ? queryMessage.query.toString()
                                                                 : "query object is corrupt");
        if (queryMessage.ntoskip || queryMessage.ntoreturn) {
            log(LogComponent::kQuery) << " ntoskip:" << queryMessage.ntoskip
                                      << " ntoreturn:" << queryMessage.ntoreturn;
        }

        const SendStaleConfigException* scex = (exception->getCode() == SendStaleConfigCode)
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
            log(LogComponent::kQuery) << "stale version detected during query over "
                                      << queryMessage.ns << " : " << errObj;
        }

        BufBuilder bb;
        bb.skip(sizeof(QueryResult::Value));
        bb.appendBuf((void*) errObj.objdata(), errObj.objsize());

        // TODO: call replyToQuery() from here instead of this!!! see dbmessage.h
        QueryResult::View msgdata = bb.buf();
        bb.decouple();
        QueryResult::View qr = msgdata;
        qr.setResultFlags(ResultFlag_ErrSet);
        if (scex) qr.setResultFlags(qr.getResultFlags() | ResultFlag_ShardConfigStale);
        qr.msgdata().setLen(bb.len());
        qr.msgdata().setOperation(opReply);
        qr.setCursorId(0);
        qr.setStartingFrom(0);
        qr.setNReturned(1);
        response->setData(msgdata.view2ptr(), true);
    }

} // namespace

    static void receivedCommand(OperationContext* txn,
                                const NamespaceString& nss,
                                Client& client,
                                DbResponse& dbResponse,
                                Message& message) {

        invariant(nss.isCommand());

        const MSGID responseTo = message.header().getId();

        DbMessage dbMessage(message);
        QueryMessage queryMessage(dbMessage);

        CurOp* op = CurOp::get(txn);

        rpc::LegacyReplyBuilder builder{};

        try {
            // This will throw if the request is on an invalid namespace.
            rpc::LegacyRequest request{&message};
            // Auth checking for Commands happens later.
            int nToReturn = queryMessage.ntoreturn;
            beginQueryOp(txn, nss, queryMessage.query, nToReturn, queryMessage.ntoskip);
            {
                stdx::lock_guard<Client> lk(*txn->getClient());
                op->markCommand_inlock();
            }

            uassert(16979, str::stream() << "bad numberToReturn (" << nToReturn
                                         << ") for $cmd type ns - can only be 1 or -1",
                    nToReturn == 1 || nToReturn == -1);

            runCommands(txn, request, &builder);

            op->debug().iscommand = true;
            // TODO: Does this get overwritten/do we really need to set this twice?
            op->debug().query = request.getCommandArgs();
        }
        catch (const DBException& exception) {
            Command::generateErrorResponse(txn, &builder, exception);
        }

        auto response = builder.done();

        op->debug().responseLength = response->header().dataLen();

        dbResponse.response = response.release();
        dbResponse.responseTo = responseTo;
    }

namespace {

    void receivedRpc(OperationContext* txn,
                     Client& client,
                     DbResponse& dbResponse,
                     Message& message) {

        invariant(message.operation() == dbCommand);

        const MSGID responseTo = message.header().getId();

        rpc::CommandReplyBuilder replyBuilder{};

        auto curOp = CurOp::get(txn);

        try {
            // database is validated here
            rpc::CommandRequest request{&message};

            // We construct a legacy $cmd namespace so we can fill in curOp using
            // the existing logic that existed for OP_QUERY commands
            NamespaceString nss(request.getDatabase(), "$cmd");
            beginQueryOp(txn, nss, request.getCommandArgs(), 1, 0);
            {
                stdx::lock_guard<Client> lk(*txn->getClient());
                curOp->markCommand_inlock();
            }

            runCommands(txn, request, &replyBuilder);

            curOp->debug().iscommand = true;
            curOp->debug().query = request.getCommandArgs();

        }
        catch (const DBException& exception) {
            Command::generateErrorResponse(txn, &replyBuilder, exception);
        }

        auto response = replyBuilder.done();

        curOp->debug().responseLength = response->header().dataLen();

        dbResponse.response = response.release();
        dbResponse.responseTo = responseTo;
    }

    // In SERVER-7775 we reimplemented the pseudo-commands fsyncUnlock, inProg, and killOp
    // as ordinary commands. To support old clients for another release, this helper serves
    // to execute the real command from the legacy pseudo-command codepath.
    // TODO: remove after MongoDB 3.2 is released
    void receivedPseudoCommand(OperationContext* txn,
                               Client& client,
                               DbResponse& dbResponse,
                               Message& message,
                               StringData realCommandName) {

        DbMessage originalDbm(message);
        originalDbm.pullInt(); // ntoskip
        originalDbm.pullInt(); // ntoreturn
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

        cmdMsgBuf.appendStr(interposedNss.db(), false); // not including null byte
        cmdMsgBuf.appendStr(".$cmd");
        cmdMsgBuf.appendNum(0); // ntoskip
        cmdMsgBuf.appendNum(1); // ntoreturn
        cmdMsgBuf.appendBuf(cmd.objdata(), cmd.objsize());

        interposed.setData(dbQuery, cmdMsgBuf.buf(), cmdMsgBuf.len());
        interposed.header().setId(message.header().getId());

        receivedCommand(txn, interposedNss, client, dbResponse, interposed);
    }

}  // namespace

    static void receivedQuery(OperationContext* txn,
                              const NamespaceString& nss,
                              Client& c,
                              DbResponse& dbResponse,
                              Message& m) {
        invariant(!nss.isCommand());

        MSGID responseTo = m.header().getId();

        DbMessage d(m);
        QueryMessage q(d);
        unique_ptr< Message > resp( new Message() );

        CurOp& op = *CurOp::get(txn);

        try {
            Client* client = txn->getClient();
            Status status = AuthorizationSession::get(client)->checkAuthForQuery(nss, q.query);
            audit::logQueryAuthzCheck(client, nss, q.query, status.code());
            uassertStatusOK(status);

            dbResponse.exhaustNS = runQuery(txn, q, nss, *resp);
            verify( !resp->empty() );
        }
        catch (const AssertionException& exception) {
            resp.reset(new Message());
            generateLegacyQueryErrorResponse(&exception, q, &op, resp.get());
        }

        op.debug().responseLength = resp->header().dataLen();

        dbResponse.response = resp.release();
        dbResponse.responseTo = responseTo;
    }

    // Mongod on win32 defines a value for this function. In all other executables it is NULL.
    void (*reportEventToSystem)(const char *msg) = 0;

    void mongoAbort(const char *msg) {
        if( reportEventToSystem )
            reportEventToSystem(msg);
        severe() << msg;
        ::abort();
    }

    // Returns false when request includes 'end'
    void assembleResponse( OperationContext* txn,
                           Message& m,
                           DbResponse& dbresponse,
                           const HostAndPort& remote) {
        // before we lock...
        int op = m.operation();
        bool isCommand = false;

        DbMessage dbmsg(m);

        Client& c = *txn->getClient();
        if (!c.isInDirectClient()) {
            LastError::get(c).startRequest();
            AuthorizationSession::get(c)->startRequest(txn);

            // We should not be holding any locks at this point
            invariant(!txn->lockState()->isLocked());
        }

        const char* ns = dbmsg.messageShouldHaveNs() ? dbmsg.getns() : NULL;
        const NamespaceString nsString = ns ? NamespaceString(ns) : NamespaceString();

        if ( op == dbQuery ) {
            if (nsString.isCommand()) {
                isCommand = true;
                opwrite(m);
            }
            // TODO: remove this entire code path after 3.2. Refs SERVER-7775
            else if (nsString.isSpecialCommand()) {
                opwrite(m);

                if (nsString.coll() == "$cmd.sys.inprog") {
                    receivedPseudoCommand(txn, c, dbresponse, m, "currentOp");
                    return;
                }
                if (nsString.coll() == "$cmd.sys.killop") {
                    receivedPseudoCommand(txn, c, dbresponse, m, "killOp");
                    return;
                }
                if (nsString.coll() == "$cmd.sys.unlock") {
                    receivedPseudoCommand(txn, c, dbresponse, m, "fsyncUnlock");
                    return;
                }
            }
            else {
                opread(m);
            }
        }
        else if( op == dbGetMore ) {
            opread(m);
        }
        else if ( op == dbCommand ) {
            isCommand = true;
            opwrite(m);
        }
        else {
            opwrite(m);
        }

        // Increment op counters.
        switch (op) {
        case dbQuery:
            if (!isCommand) {
                globalOpCounters.gotQuery();
            }
            else {
                // Command counting is deferred, since it is not known yet whether the command
                // needs counting.
            }
            break;
        case dbGetMore:
            globalOpCounters.gotGetMore();
            break;
        case dbInsert:
            // Insert counting is deferred, since it is not known yet whether the insert contains
            // multiple documents (each of which needs to be counted).
            break;
        case dbUpdate:
            globalOpCounters.gotUpdate();
            break;
        case dbDelete:
            globalOpCounters.gotDelete();
            break;
        }

        CurOp& currentOp = *CurOp::get(txn);
        {
            stdx::lock_guard<Client> lk(*txn->getClient());
            currentOp.setOp_inlock(op);
        }

        OpDebug& debug = currentOp.debug();
        debug.op = op;

        long long logThreshold = serverGlobalParams.slowMS;
        LogComponent responseComponent(LogComponent::kQuery);
        if (op == dbInsert ||
            op == dbDelete ||
            op == dbUpdate) {
            responseComponent = LogComponent::kWrite;
        }
        else if (isCommand) {
            responseComponent = LogComponent::kCommand;
        }

        bool shouldLog = logger::globalLogDomain()->shouldLog(responseComponent, 
                                                              logger::LogSeverity::Debug(1));

        if ( op == dbQuery ) {
            if (isCommand) {
                receivedCommand(txn, nsString, c, dbresponse, m);
            }
            else {
                receivedQuery(txn, nsString, c, dbresponse, m);
            }
        }
        else if ( op == dbCommand ) {
            receivedRpc(txn, c, dbresponse, m);
        }
        else if ( op == dbGetMore ) {
            if ( ! receivedGetMore(txn, dbresponse, m, currentOp) )
                shouldLog = true;
        }
        else if ( op == dbMsg ) {
            // deprecated - replaced by commands
            const char *p = dbmsg.getns();

            int len = strlen(p);
            if ( len > 400 )
                log(LogComponent::kQuery) << curTimeMillis64() % 10000 <<
                      " long msg received, len:" << len << endl;

            Message *resp = new Message();
            if ( strcmp( "end" , p ) == 0 )
                resp->setData( opReply , "dbMsg end no longer supported" );
            else
                resp->setData( opReply , "i am fine - dbMsg deprecated");

            dbresponse.response = resp;
            dbresponse.responseTo = m.header().getId();
        }
        else {
            try {
                // The following operations all require authorization.
                // dbInsert, dbUpdate and dbDelete can be easily pre-authorized,
                // here, but dbKillCursors cannot.
                if ( op == dbKillCursors ) {
                    currentOp.ensureStarted();
                    logThreshold = 10;
                    receivedKillCursors(txn, m);
                }
                else if (op != dbInsert && op != dbUpdate && op != dbDelete) {
                    log(LogComponent::kQuery) << "    operation isn't supported: " << op << endl;
                    currentOp.done();
                    shouldLog = true;
                }
                else {
                    if (remote != DBDirectClient::dummyHost) {
                        const ShardedConnectionInfo* connInfo =
                            ShardedConnectionInfo::get(&c, false);
                        uassert(18663,
                                str::stream() << "legacy writeOps not longer supported for "
                                              << "versioned connections, ns: " << nsString.ns()
                                              << ", op: " << opToString(op)
                                              << ", remote: " << remote.toString(),
                                connInfo == NULL);
                    }

                    if (!nsString.isValid()) {
                        uassert(16257, str::stream() << "Invalid ns [" << ns << "]", false);
                    }
                    else if (op == dbInsert) {
                        receivedInsert(txn, nsString, m, currentOp);
                    }
                    else if (op == dbUpdate) {
                        receivedUpdate(txn, nsString, m, currentOp);
                    }
                    else if (op == dbDelete) {
                        receivedDelete(txn, nsString, m, currentOp);
                    }
                    else {
                        invariant(false);
                    }
                }
             }
            catch (const UserException& ue) {
                LastError::get(c).setLastError(ue.getCode(), ue.getInfo().msg);
                MONGO_LOG_COMPONENT(3, responseComponent)
                       << " Caught Assertion in " << opToString(op) << ", continuing "
                       << ue.toString() << endl;
                debug.exceptionInfo = ue.getInfo();
            }
            catch (const AssertionException& e) {
                LastError::get(c).setLastError(e.getCode(), e.getInfo().msg);
                MONGO_LOG_COMPONENT(3, responseComponent)
                       << " Caught Assertion in " << opToString(op) << ", continuing "
                       << e.toString() << endl;
                debug.exceptionInfo = e.getInfo();
                shouldLog = true;
            }
        }
        currentOp.ensureStarted();
        currentOp.done();
        debug.executionTime = currentOp.totalTimeMillis();

        logThreshold += currentOp.getExpectedLatencyMs();

        if ( shouldLog || debug.executionTime > logThreshold ) {
            Locker::LockerInfo lockerInfo;
            txn->lockState()->getLockerInfo(&lockerInfo);

            MONGO_LOG_COMPONENT(0, responseComponent) << debug.report(currentOp, lockerInfo.stats);
        }

        if (currentOp.shouldDBProfile(debug.executionTime)) {
            // Performance profiling is on
            if (txn->lockState()->isReadLocked()) {
                MONGO_LOG_COMPONENT(1, responseComponent)
                        << "note: not profiling because recursive read lock";
            }
            else if (lockedForWriting()) {
                MONGO_LOG_COMPONENT(1, responseComponent)
                        << "note: not profiling because doing fsync+lock";
            }
            else {
                profile(txn, op);
            }
        }

        recordCurOpMetrics(txn);
        debug.reset();
    }

    void receivedKillCursors(OperationContext* txn, Message& m) {
        LastError::get(txn->getClient()).disable();
        DbMessage dbmessage(m);
        int n = dbmessage.pullInt();

        uassert( 13659 , "sent 0 cursors to kill" , n != 0 );
        massert( 13658 , str::stream() << "bad kill cursors size: " << m.dataSize() , m.dataSize() == 8 + ( 8 * n ) );
        uassert( 13004 , str::stream() << "sent negative cursors to kill: " << n  , n >= 1 );

        if ( n > 2000 ) {
            ( n < 30000 ? warning() : error() ) << "receivedKillCursors, n=" << n << endl;
            verify( n < 30000 );
        }

        const char* cursorArray = dbmessage.getArray(n);

        int found = CursorManager::eraseCursorGlobalIfAuthorized(txn, n, cursorArray);

        if ( shouldLog(logger::LogSeverity::Debug(1)) || found != n ) {
            LOG( found == n ? 1 : 0 ) << "killcursors: found " << found << " of " << n << endl;
        }

    }

    void receivedUpdate(OperationContext* txn,
                        const NamespaceString& nsString,
                        Message& m,
                        CurOp& op) {
        DbMessage d(m);
        uassertStatusOK(userAllowedWriteNS(nsString));
        op.debug().ns = nsString.ns();
        int flags = d.pullInt();
        BSONObj query = d.nextJsObj();

        verify( d.moreJSObjs() );
        verify( query.objsize() < m.header().dataLen() );
        BSONObj toupdate = d.nextJsObj();
        uassert( 10055 , "update object too large", toupdate.objsize() <= BSONObjMaxUserSize);
        verify( toupdate.objsize() < m.header().dataLen() );
        verify( query.objsize() + toupdate.objsize() < m.header().dataLen() );
        bool upsert = flags & UpdateOption_Upsert;
        bool multi = flags & UpdateOption_Multi;
        bool broadcast = flags & UpdateOption_Broadcast;

        Status status = AuthorizationSession::get(txn->getClient())->checkAuthForUpdate(nsString,
                                                                           query,
                                                                           toupdate,
                                                                           upsert);
        audit::logUpdateAuthzCheck(txn->getClient(), nsString, query, toupdate, upsert, multi,
                                   status.code());
        uassertStatusOK(status);

        op.debug().query = query;
        {
            stdx::lock_guard<Client> lk(*txn->getClient());
            op.setQuery_inlock(query);
        }

        UpdateRequest request(nsString);
        request.setUpsert(upsert);
        request.setMulti(multi);
        request.setQuery(query);
        request.setUpdates(toupdate);
        UpdateLifecycleImpl updateLifecycle(broadcast, nsString);
        request.setLifecycle(&updateLifecycle);

        request.setYieldPolicy(PlanExecutor::YIELD_AUTO);

        int attempt = 1;
        while ( 1 ) {
            try {
                ParsedUpdate parsedUpdate(txn, &request);
                uassertStatusOK(parsedUpdate.parseRequest());

                //  Tentatively take an intent lock, fix up if we need to create the collection
                ScopedTransaction transaction(txn, MODE_IX);
                Lock::DBLock dbLock(txn->lockState(), nsString.db(), MODE_IX);
                if (dbHolder().get(txn, nsString.db()) == NULL) {
                    //  If DB doesn't exist, don't implicitly create it in OldClientContext
                    break;
                }
                Lock::CollectionLock collLock(txn->lockState(),
                                              nsString.ns(),
                                              parsedUpdate.isIsolated() ? MODE_X : MODE_IX);
                OldClientContext ctx(txn, nsString);

                //  The common case: no implicit collection creation
                if (!upsert || ctx.db()->getCollection(nsString) != NULL) {
                    PlanExecutor* rawExec;
                    uassertStatusOK(getExecutorUpdate(txn,
                                                      ctx.db()->getCollection(nsString),
                                                      &parsedUpdate,
                                                      &op.debug(),
                                                      &rawExec));
                    std::unique_ptr<PlanExecutor> exec(rawExec);

                    // Run the plan and get stats out.
                    uassertStatusOK(exec->executePlan());
                    UpdateResult res = UpdateStage::makeUpdateResult(exec.get(), &op.debug());

                    // for getlasterror
                    LastError::get(txn->getClient()).recordUpdate(
                            res.existing, res.numMatched, res.upserted);
                    return;
                }
                break;
            }
            catch ( const WriteConflictException& dle ) {
                op.debug().writeConflicts++;
                if ( multi ) {
                    log(LogComponent::kWrite) << "Had WriteConflict during multi update, aborting";
                    throw;
                }
                WriteConflictException::logAndBackoff( attempt++, "update", nsString.toString() );
            }
        }

        //  This is an upsert into a non-existing database, so need an exclusive lock
        //  to avoid deadlock
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            ParsedUpdate parsedUpdate(txn, &request);
            uassertStatusOK(parsedUpdate.parseRequest());

            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock dbLock(txn->lockState(), nsString.db(), MODE_X);
            OldClientContext ctx(txn, nsString);
            uassert(ErrorCodes::NotMaster,
                    str::stream() << "Not primary while performing update on " << nsString.ns(),
                    repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(nsString));

            Database* db = ctx.db();
            if (db->getCollection(nsString)) {
                // someone else beat us to it, that's ok
                // we might race while we unlock if someone drops
                // but that's ok, we'll just do nothing and error out
            }
            else {
                WriteUnitOfWork wuow(txn);
                uassertStatusOK(userCreateNS(txn, db, nsString.ns(), BSONObj()));
                wuow.commit();
            }

            PlanExecutor* rawExec;
            uassertStatusOK(getExecutorUpdate(txn,
                                              ctx.db()->getCollection(nsString),
                                              &parsedUpdate,
                                              &op.debug(),
                                              &rawExec));
            std::unique_ptr<PlanExecutor> exec(rawExec);

            // Run the plan and get stats out.
            uassertStatusOK(exec->executePlan());
            UpdateResult res = UpdateStage::makeUpdateResult(exec.get(), &op.debug());

            LastError::get(txn->getClient()).recordUpdate(
                    res.existing, res.numMatched, res.upserted);
        } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "update", nsString.ns());
    }

    void receivedDelete(OperationContext* txn,
                        const NamespaceString& nsString,
                        Message& m,
                        CurOp& op) {
        DbMessage d(m);
        uassertStatusOK(userAllowedWriteNS(nsString));

        op.debug().ns = nsString.ns();
        int flags = d.pullInt();
        bool justOne = flags & RemoveOption_JustOne;
        verify( d.moreJSObjs() );
        BSONObj pattern = d.nextJsObj();

        Status status = AuthorizationSession::get(txn->getClient())->checkAuthForDelete(nsString,
                                                                                        pattern);
        audit::logDeleteAuthzCheck(txn->getClient(), nsString, pattern, status.code());
        uassertStatusOK(status);

        op.debug().query = pattern;
        {
            stdx::lock_guard<Client> lk(*txn->getClient());
            op.setQuery_inlock(pattern);
        }

        DeleteRequest request(nsString);
        request.setQuery(pattern);
        request.setMulti(!justOne);

        request.setYieldPolicy(PlanExecutor::YIELD_AUTO);

        int attempt = 1;
        while ( 1 ) {
            try {
                ParsedDelete parsedDelete(txn, &request);
                uassertStatusOK(parsedDelete.parseRequest());

                ScopedTransaction scopedXact(txn, MODE_IX);
                AutoGetDb autoDb(txn, nsString.db(), MODE_IX);
                if (!autoDb.getDb()) {
                    break;
                }

                Lock::CollectionLock collLock(txn->lockState(),
                                              nsString.ns(),
                                              parsedDelete.isIsolated() ? MODE_X : MODE_IX);
                OldClientContext ctx(txn, nsString);

                PlanExecutor* rawExec;
                uassertStatusOK(getExecutorDelete(txn,
                                                  ctx.db()->getCollection(nsString),
                                                  &parsedDelete,
                                                  &rawExec));
                std::unique_ptr<PlanExecutor> exec(rawExec);

                // Run the plan and get the number of docs deleted.
                uassertStatusOK(exec->executePlan());
                long long n = DeleteStage::getNumDeleted(exec.get());
                LastError::get(txn->getClient()).recordDelete(n);
                op.debug().ndeleted = n;

                break;
            }
            catch ( const WriteConflictException& dle ) {
                op.debug().writeConflicts++;
                WriteConflictException::logAndBackoff( attempt++, "delete", nsString.toString() );
            }
        }
    }

    QueryResult::View emptyMoreResult(long long);

    bool receivedGetMore(OperationContext* txn,
                         DbResponse& dbresponse,
                         Message& m,
                         CurOp& curop) {
        bool ok = true;

        DbMessage d(m);

        const char *ns = d.getns();
        int ntoreturn = d.pullInt();
        long long cursorid = d.pullInt64();

        curop.debug().ns = ns;
        curop.debug().ntoreturn = ntoreturn;
        curop.debug().cursorid = cursorid;

        unique_ptr<AssertionException> ex;
        unique_ptr<Timer> timer;
        int pass = 0;
        bool exhaust = false;
        QueryResult::View msgdata = 0;
        Timestamp last;
        while( 1 ) {
            bool isCursorAuthorized = false;
            try {
                const NamespaceString nsString( ns );
                uassert( 16258, str::stream() << "Invalid ns [" << ns << "]", nsString.isValid() );

                Status status = AuthorizationSession::get(txn->getClient())->checkAuthForGetMore(
                    nsString, cursorid);
                audit::logGetMoreAuthzCheck(txn->getClient(), nsString, cursorid, status.code());
                uassertStatusOK(status);

                if (str::startsWith(ns, "local.oplog.")){
                    while (MONGO_FAIL_POINT(rsStopGetMore)) {
                        sleepmillis(0);
                    }

                    if (pass == 0) {
                        last = getLastSetTimestamp();
                    }
                    else {
                        repl::waitUpToOneSecondForTimestampChange(last);
                    }
                }

                msgdata = getMore(txn,
                                  ns,
                                  ntoreturn,
                                  cursorid,
                                  pass,
                                  exhaust,
                                  &isCursorAuthorized);
            }
            catch ( AssertionException& e ) {
                if ( isCursorAuthorized ) {
                    // If a cursor with id 'cursorid' was authorized, it may have been advanced
                    // before an exception terminated processGetMore.  Erase the ClientCursor
                    // because it may now be out of sync with the client's iteration state.
                    // SERVER-7952
                    // TODO Temporary code, see SERVER-4563 for a cleanup overview.
                    CursorManager::eraseCursorGlobal(txn, cursorid );
                }
                ex.reset( new AssertionException( e.getInfo().msg, e.getCode() ) );
                ok = false;
                break;
            }
            
            if (msgdata.view2ptr() == 0) {
                // this should only happen with QueryOption_AwaitData
                exhaust = false;
                massert(13073, "shutting down", !inShutdown() );
                if ( ! timer ) {
                    timer.reset( new Timer() );
                }
                else {
                    if ( timer->seconds() >= 4 ) {
                        // after about 4 seconds, return. pass stops at 1000 normally.
                        // we want to return occasionally so slave can checkpoint.
                        pass = 10000;
                    }
                }
                pass++;
                if (kDebugBuild)
                    sleepmillis(20);
                else
                    sleepmillis(2);
                
                // note: the 1100 is beacuse of the waitForDifferent above
                // should eventually clean this up a bit
                curop.setExpectedLatencyMs( 1100 + timer->millis() );
                
                continue;
            }
            break;
        };

        if (ex) {
            BSONObjBuilder err;
            ex->getInfo().append( err );
            BSONObj errObj = err.done();

            curop.debug().exceptionInfo = ex->getInfo();

            replyToQuery(ResultFlag_ErrSet, m, dbresponse, errObj);
            curop.debug().responseLength = dbresponse.response->header().dataLen();
            curop.debug().nreturned = 1;
            return ok;
        }

        Message *resp = new Message();
        resp->setData(msgdata.view2ptr(), true);
        curop.debug().responseLength = resp->header().dataLen();
        curop.debug().nreturned = msgdata.getNReturned();

        dbresponse.response = resp;
        dbresponse.responseTo = m.header().getId();

        if( exhaust ) {
            curop.debug().exhaust = true;
            dbresponse.exhaustNS = ns;
        }

        return ok;
    }

    void checkAndInsert(OperationContext* txn,
                        OldClientContext& ctx,
                        const char *ns,
                        /*modifies*/BSONObj& js) {

        StatusWith<BSONObj> fixed = fixDocumentForInsert( js );
        uassertStatusOK( fixed.getStatus() );
        if ( !fixed.getValue().isEmpty() )
            js = fixed.getValue();

        int attempt = 0;
        while ( true ) {
            try {
                WriteUnitOfWork wunit(txn);
                Collection* collection = ctx.db()->getCollection( ns );
                if ( !collection ) {
                    collection = ctx.db()->createCollection( txn, ns );
                    verify( collection );
                }

                StatusWith<RecordId> status = collection->insertDocument( txn, js, true );
                uassertStatusOK( status.getStatus() );
                wunit.commit();
                break;
            }
            catch( const WriteConflictException& e ) {
                CurOp::get(txn)->debug().writeConflicts++;
                txn->recoveryUnit()->abandonSnapshot();
                WriteConflictException::logAndBackoff( attempt++, "insert", ns);
            }
        }
    }

    NOINLINE_DECL void insertMulti(OperationContext* txn,
                                   OldClientContext& ctx,
                                   bool keepGoing,
                                   const char *ns,
                                   vector<BSONObj>& objs,
                                   CurOp& op) {
        size_t i;
        for (i=0; i<objs.size(); i++){
            try {
                checkAndInsert(txn, ctx, ns, objs[i]);
            }
            catch (const UserException& ex) {
                if (!keepGoing || i == objs.size()-1){
                    globalOpCounters.incInsertInWriteLock(i);
                    throw;
                }
                LastError::get(txn->getClient()).setLastError(ex.getCode(), ex.getInfo().msg);
                // otherwise ignore and keep going
            }
        }

        globalOpCounters.incInsertInWriteLock(i);
        op.debug().ninserted = i;
    }

    static void convertSystemIndexInsertsToCommands(
            DbMessage& d,
            BSONArrayBuilder* allCmdsBuilder) {
        while (d.moreJSObjs()) {
            BSONObj spec = d.nextJsObj();
            BSONElement indexNsElement = spec["ns"];
            uassert(ErrorCodes::NoSuchKey,
                    str::stream() << "Missing \"ns\" field while inserting into " << d.getns(),
                    !indexNsElement.eoo());
            uassert(ErrorCodes::TypeMismatch,
                    str::stream() << "Expected \"ns\" field to have type String, not " <<
                    typeName(indexNsElement.type()) << " while inserting into " << d.getns(),
                    indexNsElement.type() == String);
            const StringData nsToIndex(indexNsElement.valueStringData());
            BSONObjBuilder cmdObjBuilder(allCmdsBuilder->subobjStart());
            cmdObjBuilder << "createIndexes" << nsToCollectionSubstring(nsToIndex);
            BSONArrayBuilder specArrayBuilder(cmdObjBuilder.subarrayStart("indexes"));
            while (true) {
                BSONObjBuilder specBuilder(specArrayBuilder.subobjStart());
                BSONElement specNsElement = spec["ns"];
                if ((specNsElement.type() != String) ||
                    (specNsElement.valueStringData() != nsToIndex)) {

                    break;
                }
                for (BSONObjIterator iter(spec); iter.more();) {
                    BSONElement element = iter.next();
                    if (element.fieldNameStringData() != "ns") {
                        specBuilder.append(element);
                    }
                }
                if (!d.moreJSObjs()) {
                    break;
                }
                spec = d.nextJsObj();
            }
        }
    }

    static void insertSystemIndexes(OperationContext* txn, DbMessage& d, CurOp& curOp) {
        BSONArrayBuilder allCmdsBuilder;
        try {
            convertSystemIndexInsertsToCommands(d, &allCmdsBuilder);
        }
        catch (const DBException& ex) {
            LastError::get(txn->getClient()).setLastError(ex.getCode(), ex.getInfo().msg);
            curOp.debug().exceptionInfo = ex.getInfo();
            return;
        }
        BSONArray allCmds(allCmdsBuilder.done());
        Command* createIndexesCmd = Command::findCommand("createIndexes");
        invariant(createIndexesCmd);
        const bool keepGoing = d.reservedField() & InsertOption_ContinueOnError;
        for (BSONObjIterator iter(allCmds); iter.more();) {
            try {
                BSONObj cmdObj = iter.next().Obj();

                rpc::LegacyRequestBuilder requestBuilder{};
                auto indexNs = NamespaceString(d.getns());
                auto cmdRequestMsg = requestBuilder.setDatabase(indexNs.db())
                                                   .setCommandName("createIndexes")
                                                   .setMetadata(rpc::makeEmptyMetadata())
                                                   .setCommandArgs(cmdObj).done();
                rpc::LegacyRequest cmdRequest{cmdRequestMsg.get()};
                rpc::LegacyReplyBuilder cmdReplyBuilder{};
                Command::execCommand(txn,
                                     createIndexesCmd,
                                     cmdRequest,
                                     &cmdReplyBuilder);
                auto cmdReplyMsg = cmdReplyBuilder.done();
                rpc::LegacyReply cmdReply{cmdReplyMsg.get()};
                uassertStatusOK(Command::getStatusFromCommandResult(cmdReply.getCommandReply()));
            }
            catch (const DBException& ex) {
                LastError::get(txn->getClient()).setLastError(ex.getCode(), ex.getInfo().msg);
                curOp.debug().exceptionInfo = ex.getInfo();
                if (!keepGoing) {
                    return;
                }
            }
        }
    }

    void receivedInsert(OperationContext* txn,
                        const NamespaceString& nsString,
                        Message& m,
                        CurOp& op) {
        DbMessage d(m);
        const char* ns = d.getns();
        op.debug().ns = ns;
        uassertStatusOK(userAllowedWriteNS(nsString.ns()));
        if (nsString.isSystemDotIndexes()) {
            insertSystemIndexes(txn, d, op);
            return;
        }

        if( !d.moreJSObjs() ) {
            // strange.  should we complain?
            return;
        }

        vector<BSONObj> multi;
        while (d.moreJSObjs()){
            BSONObj obj = d.nextJsObj();
            multi.push_back(obj);

            // Check auth for insert (also handles checking if this is an index build and checks
            // for the proper privileges in that case).
            Status status = AuthorizationSession::get(txn->getClient())->checkAuthForInsert(nsString, obj);
            audit::logInsertAuthzCheck(txn->getClient(), nsString, obj, status.code());
            uassertStatusOK(status);
        }

        const int notMasterCodeForInsert = 10058; // This is different from ErrorCodes::NotMaster
        {
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock dbLock(txn->lockState(), nsString.db(), MODE_IX);
            Lock::CollectionLock collLock(txn->lockState(), nsString.ns(), MODE_IX);

            // CONCURRENCY TODO: is being read locked in big log sufficient here?
            // writelock is used to synchronize stepdowns w/ writes
            uassert(notMasterCodeForInsert, "not master",
                    repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(nsString));

            // OldClientContext may implicitly create a database, so check existence
            if (dbHolder().get(txn, nsString.db()) != NULL) {
                OldClientContext ctx(txn, ns);
                if (ctx.db()->getCollection(nsString)) {
                    if (multi.size() > 1) {
                        const bool keepGoing = d.reservedField() & InsertOption_ContinueOnError;
                        insertMulti(txn, ctx, keepGoing, ns, multi, op);
                    }
                    else {
                        checkAndInsert(txn, ctx, ns, multi[0]);
                        globalOpCounters.incInsertInWriteLock(1);
                        op.debug().ninserted = 1;
                    }
                    return;
                }
            }
        }

        // Collection didn't exist so try again with MODE_X
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dbLock(txn->lockState(), nsString.db(), MODE_X);

        // CONCURRENCY TODO: is being read locked in big log sufficient here?
        // writelock is used to synchronize stepdowns w/ writes
        uassert(notMasterCodeForInsert, "not master",
                repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(nsString));

        OldClientContext ctx(txn, ns);

        if (multi.size() > 1) {
            const bool keepGoing = d.reservedField() & InsertOption_ContinueOnError;
            insertMulti(txn, ctx, keepGoing, ns, multi, op);
        } else {
            checkAndInsert(txn, ctx, ns, multi[0]);
            globalOpCounters.incInsertInWriteLock(1);
            op.debug().ninserted = 1;
        }
    }

    static AtomicUInt32 shutdownInProgress(0);

    bool inShutdown() {
        return shutdownInProgress.loadRelaxed() != 0;
    }

    bool inShutdownStrict() {
        return shutdownInProgress.load() != 0;
    }

    static void shutdownServer() {
        log(LogComponent::kNetwork) << "shutdown: going to close listening sockets..." << endl;
        ListeningSockets::get()->closeAll();

        log(LogComponent::kNetwork) << "shutdown: going to flush diaglog..." << endl;
        _diaglog.flush();

        /* must do this before unmapping mem or you may get a seg fault */
        log(LogComponent::kNetwork) << "shutdown: going to close sockets..." << endl;
        stdx::thread close_socket_thread( stdx::bind(MessagingPort::closeAllSockets, 0) );

        getGlobalServiceContext()->shutdownGlobalStorageEngineCleanly();
    }

    // shutdownLock
    //
    // Protects:
    //  Ensures shutdown is single threaded.
    // Lock Ordering:
    //  No restrictions
    stdx::mutex shutdownLock;

    void signalShutdown() {
        // Notify all threads shutdown has started
        shutdownInProgress.fetchAndAdd(1);
    }

    void exitCleanly(ExitCode code) {
        // Notify all threads shutdown has started
        shutdownInProgress.fetchAndAdd(1);

        // Grab the shutdown lock to prevent concurrent callers
        stdx::lock_guard<stdx::mutex> lockguard(shutdownLock);

        // Global storage engine may not be started in all cases before we exit
        if (getGlobalServiceContext()->getGlobalStorageEngine() == NULL) {
            dbexit(code); // returns only under a windows service
            invariant(code == EXIT_WINDOWS_SERVICE_STOP);
            return;
        }

        getGlobalServiceContext()->setKillAllOperations();

        repl::getGlobalReplicationCoordinator()->shutdown();
        auto catalogMgr = grid.catalogManager();
        if (catalogMgr) {
            catalogMgr->shutDown();
        }

        // We should always be able to acquire the global lock at shutdown.
        //
        // TODO: This call chain uses the locker directly, because we do not want to start an
        // operation context, which also instantiates a recovery unit. Also, using the
        // lockGlobalBegin/lockGlobalComplete sequence, we avoid taking the flush lock. This will
        // all go away if we start acquiring the global/flush lock as part of ScopedTransaction.
        //
        // For a Windows service, dbexit does not call exit(), so we must leak the lock outside
        // of this function to prevent any operations from running that need a lock.
        //
        DefaultLockerImpl* globalLocker = new DefaultLockerImpl();
        LockResult result = globalLocker->lockGlobalBegin(MODE_X);
        if (result == LOCK_WAITING) {
            result = globalLocker->lockGlobalComplete(UINT_MAX);
        }

        invariant(LOCK_OK == result);

        log(LogComponent::kControl) << "now exiting" << endl;

        // Execute the graceful shutdown tasks, such as flushing the outstanding journal 
        // and data files, close sockets, etc.
        try {
            shutdownServer();
        }
        catch (const DBException& ex) {
            severe() << "shutdown failed with DBException " << ex;
            std::terminate();
        }
        catch (const std::exception& ex) {
            severe() << "shutdown failed with std::exception: " << ex.what();
            std::terminate();
        }
        catch (...) {
            severe() << "shutdown failed with exception";
            std::terminate();
        }

        dbexit( code );
    }

    NOINLINE_DECL void dbexit( ExitCode rc, const char *why ) {
        audit::logShutdown(&cc());

        log(LogComponent::kControl) << "dbexit: " << why << " rc: " << rc;

#ifdef _WIN32
        // Windows Service Controller wants to be told when we are down,
        //  so don't call quickExit() yet, or say "really exiting now"
        //
        if ( rc == EXIT_WINDOWS_SERVICE_STOP ) {
            return;
        }
#endif

        quickExit(rc);
    }

    // ----- BEGIN Diaglog -----
    DiagLog::DiagLog() : f(0), level(0) {}

    void DiagLog::openFile() {
        verify( f == 0 );
        stringstream ss;
        ss << storageGlobalParams.dbpath << "/diaglog." << hex << time(0);
        string name = ss.str();
        f = new ofstream(name.c_str(), ios::out | ios::binary);
        if ( ! f->good() ) {
            log() << "diagLogging couldn't open " << name << endl;
            // todo what is this? :
            throw 1717;
        }
        else {
            log() << "diagLogging using file " << name << endl;
        }
    }

    int DiagLog::setLevel( int newLevel ) {
        stdx::lock_guard<stdx::mutex> lk(mutex);
        int old = level;
        log() << "diagLogging level=" << newLevel << endl;
        if( f == 0 ) {
            openFile();
        }
        level = newLevel; // must be done AFTER f is set
        return old;
    }

    void DiagLog::flush() {
        if ( level ) {
            log() << "flushing diag log" << endl;
            stdx::lock_guard<stdx::mutex> lk(mutex);
            f->flush();
        }
    }

    void DiagLog::writeop(char *data,int len) {
        if ( level & 1 ) {
            stdx::lock_guard<stdx::mutex> lk(mutex);
            f->write(data,len);
        }
    }

    void DiagLog::readop(char *data, int len) {
        if ( level & 2 ) {
            bool log = (level & 4) == 0;
            OCCASIONALLY log = true;
            if ( log ) {
                stdx::lock_guard<stdx::mutex> lk(mutex);
                verify( f );
                f->write(data,len);
            }
        }
    }

    DiagLog _diaglog;

    // ----- END Diaglog -----

} // namespace mongo
