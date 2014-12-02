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

#include <boost/thread/thread.hpp>
#include <fstream>

#include "mongo/base/status.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/background.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/currentop_command.h"
#include "mongo/db/db.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/global_optime.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/ops/delete_executor.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/ops/update_executor.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/new_find.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/process_id.h"
#include "mongo/s/d_state.h"
#include "mongo/s/stale_exception.h" // for SendStaleConfigException
#include "mongo/scripting/engine.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/gcov.h"
#include "mongo/util/goodies.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/time_support.h"

namespace mongo {

    using logger::LogComponent;

namespace {
    inline LogComponent logComponentForOp(int op) {
        switch (op) {
        case dbInsert:
        case dbUpdate:
        case dbDelete:
            return LogComponent::kWrite;
        default:
            return LogComponent::kQuery;
        }
    }
}  // namespace

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
    void receivedUpdate(OperationContext* txn, Message& m, CurOp& op);
    void receivedDelete(OperationContext* txn, Message& m, CurOp& op);
    void receivedInsert(OperationContext* txn, Message& m, CurOp& op);
    bool receivedGetMore(OperationContext* txn,
                         DbResponse& dbresponse,
                         Message& m,
                         CurOp& curop,
                         bool fromDBDirectClient);

    int nloggedsome = 0;
#define LOGWITHRATELIMIT if( ++nloggedsome < 1000 || nloggedsome % 100 == 0 )

    string dbExecCommand;

    MONGO_FP_DECLARE(rsStopGetMore);

    void killOp( OperationContext* txn, Message &m, DbResponse &dbresponse ) {
        DbMessage d(m);
        QueryMessage q(d);
        BSONObj obj;

        const bool isAuthorized = txn->getClient()->getAuthorizationSession()->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::killop);
        audit::logKillOpAuthzCheck(txn->getClient(),
                                   q.query,
                                   isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);
        if (!isAuthorized) {
            obj = fromjson("{\"err\":\"unauthorized\"}");
        }
        /*else if( !dbMutexInfo.isLocked() )
            obj = fromjson("{\"info\":\"no op in progress/not locked\"}");
            */
        else {
            BSONElement e = q.query.getField("op");
            if( !e.isNumber() ) {
                obj = fromjson("{\"err\":\"no op number field specified?\"}");
            }
            else {
                log() << "going to kill op: " << e << endl;
                obj = fromjson("{\"info\":\"attempting to kill op\"}");
                getGlobalEnvironment()->killOperation( (unsigned) e.number() );
            }
        }
        replyToQuery(0, m, dbresponse, obj);
    }

    bool _unlockFsync();
    static void unlockFsync(OperationContext* txn, const char *ns, Message& m, DbResponse &dbresponse) {
        BSONObj obj;

        const bool isAuthorized = txn->getClient()->getAuthorizationSession()->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::unlock);
        audit::logFsyncUnlockAuthzCheck(
                txn->getClient(), isAuthorized ? ErrorCodes::OK : ErrorCodes::Unauthorized);
        if (!isAuthorized) {
            obj = fromjson("{\"err\":\"unauthorized\"}");
        }
        else if (strncmp(ns, "admin.", 6) != 0 ) {
            obj = fromjson("{\"err\":\"unauthorized - this command must be run against the admin DB\"}");
        }
        else {
            log() << "command: unlock requested" << endl;
            if( _unlockFsync() ) {
                obj = fromjson("{ok:1,\"info\":\"unlock completed\"}");
            }
            else {
                obj = fromjson("{ok:0,\"errmsg\":\"not locked\"}");
            }
        }
        replyToQuery(0, m, dbresponse, obj);
    }

    static bool receivedQuery(OperationContext* txn,
                              Client& c,
                              DbResponse& dbresponse,
                              Message& m,
                              bool fromDBDirectClient) {
        bool ok = true;
        MSGID responseTo = m.header().getId();

        DbMessage d(m);
        QueryMessage q(d);
        auto_ptr< Message > resp( new Message() );

        CurOp& op = *(c.curop());

        scoped_ptr<AssertionException> ex;

        try {
            NamespaceString ns(d.getns());
            if (!ns.isCommand()) {
                // Auth checking for Commands happens later.
                Client* client = txn->getClient();
                Status status = client->getAuthorizationSession()->checkAuthForQuery(ns, q.query);
                audit::logQueryAuthzCheck(client, ns, q.query, status.code());
                uassertStatusOK(status);
            }
            dbresponse.exhaustNS = newRunQuery(txn, m, q, op, *resp, fromDBDirectClient);
            verify( !resp->empty() );
        }
        catch ( SendStaleConfigException& e ){
            ex.reset( new SendStaleConfigException( e.getns(), e.getInfo().msg, e.getVersionReceived(), e.getVersionWanted() ) );
            ok = false;
        }
        catch ( AssertionException& e ) {
            ex.reset( new AssertionException( e.getInfo().msg, e.getCode() ) );
            ok = false;
        }

        if( ex ){

            op.debug().exceptionInfo = ex->getInfo();
            log() << "assertion " << ex->toString() << " ns:" << q.ns << " query:" <<
                (q.query.valid() ? q.query.toString() : "query object is corrupt") << endl;
            if( q.ntoskip || q.ntoreturn )
                log() << " ntoskip:" << q.ntoskip << " ntoreturn:" << q.ntoreturn << endl;

            SendStaleConfigException* scex = NULL;
            if ( ex->getCode() == SendStaleConfigCode ) scex = static_cast<SendStaleConfigException*>( ex.get() );

            BSONObjBuilder err;
            ex->getInfo().append( err );
            if( scex ){
                err.append( "ns", scex->getns() );
                scex->getVersionReceived().addToBSON( err, "vReceived" );
                scex->getVersionWanted().addToBSON( err, "vWanted" );
            }
            BSONObj errObj = err.done();

            if( scex ){
                log() << "stale version detected during query over "
                      << q.ns << " : " << errObj << endl;
            }

            BufBuilder b;
            b.skip(sizeof(QueryResult::Value));
            b.appendBuf((void*) errObj.objdata(), errObj.objsize());

            // todo: call replyToQuery() from here instead of this!!! see dbmessage.h
            QueryResult::View msgdata = b.buf();
            b.decouple();
            QueryResult::View qr = msgdata;
            qr.setResultFlags(ResultFlag_ErrSet);
            if( scex ) qr.setResultFlags(qr.getResultFlags() | ResultFlag_ShardConfigStale);
            qr.msgdata().setLen(b.len());
            qr.msgdata().setOperation(opReply);
            qr.setCursorId(0);
            qr.setStartingFrom(0);
            qr.setNReturned(1);
            resp.reset( new Message() );
            resp->setData( msgdata.view2ptr(), true );

        }

        op.debug().responseLength = resp->header().dataLen();

        dbresponse.response = resp.release();
        dbresponse.responseTo = responseTo;

        return ok;
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
                           const HostAndPort& remote,
                           bool fromDBDirectClient ) {
        // before we lock...
        int op = m.operation();
        bool isCommand = false;

        DbMessage dbmsg(m);

        Client& c = *txn->getClient();
        if (!txn->isGod()) {
            c.getAuthorizationSession()->startRequest(txn);

            // We should not be holding any locks at this point
            invariant(!txn->lockState()->isLocked());
        }

        if ( op == dbQuery ) {
            const char *ns = dbmsg.getns();

            if (strstr(ns, ".$cmd")) {
                isCommand = true;
                opwrite(m);
                if( strstr(ns, ".$cmd.sys.") ) {
                    if( strstr(ns, "$cmd.sys.inprog") ) {
                        inProgCmd(txn, m, dbresponse);
                        return;
                    }
                    if( strstr(ns, "$cmd.sys.killop") ) {
                        killOp(txn, m, dbresponse);
                        return;
                    }
                    if( strstr(ns, "$cmd.sys.unlock") ) {
                        unlockFsync(txn, ns, m, dbresponse);
                        return;
                    }
                }
            }
            else {
                opread(m);
            }
        }
        else if( op == dbGetMore ) {
            opread(m);
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
        
        scoped_ptr<CurOp> nestedOp;
        CurOp* currentOpP = c.curop();
        if ( currentOpP->active() ) {
            nestedOp.reset( new CurOp( &c , currentOpP ) );
            currentOpP = nestedOp.get();
        }

        CurOp& currentOp = *currentOpP;
        currentOp.reset(remote,op);

        OpDebug& debug = currentOp.debug();
        debug.op = op;

        long long logThreshold = serverGlobalParams.slowMS;
        bool shouldLog = logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1));

        if ( op == dbQuery ) {
            receivedQuery(txn, c , dbresponse, m, fromDBDirectClient );
        }
        else if ( op == dbGetMore ) {
            if ( ! receivedGetMore(txn, dbresponse, m, currentOp, fromDBDirectClient) )
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
                    const char* ns = dbmsg.getns();
                    const NamespaceString nsString(ns);

                    if (remote != DBDirectClient::dummyHost) {
                        const ShardedConnectionInfo* connInfo = ShardedConnectionInfo::get(false);
                        uassert(18663,
                                str::stream() << "legacy writeOps not longer supported for "
                                              << "versioned connections, ns: " << string(ns)
                                              << ", op: " << opToString(op)
                                              << ", remote: " << remote.toString(),
                                connInfo == NULL);
                    }

                    if (!nsString.isValid()) {
                        uassert(16257, str::stream() << "Invalid ns [" << ns << "]", false);
                    }
                    else if (op == dbInsert) {
                        receivedInsert(txn, m, currentOp);
                    }
                    else if (op == dbUpdate) {
                        receivedUpdate(txn, m, currentOp);
                    }
                    else if (op == dbDelete) {
                        receivedDelete(txn, m, currentOp);
                    }
                    else {
                        invariant(false);
                    }
                }
             }
            catch (const UserException& ue) {
                setLastError(ue.getCode(), ue.getInfo().msg.c_str());
                MONGO_LOG_COMPONENT(3, logComponentForOp(op))
                       << " Caught Assertion in " << opToString(op) << ", continuing "
                       << ue.toString() << endl;
                debug.exceptionInfo = ue.getInfo();
            }
            catch (const AssertionException& e) {
                setLastError(e.getCode(), e.getInfo().msg.c_str());
                MONGO_LOG_COMPONENT(3, logComponentForOp(op))
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
            MONGO_LOG_COMPONENT(0, logComponentForOp(op))
                    << debug.report( currentOp ) << endl;
        }

        if ( currentOp.shouldDBProfile( debug.executionTime ) ) {
            // performance profiling is on
            if (txn->lockState()->hasAnyReadLock()) {
                MONGO_LOG_COMPONENT(1, logComponentForOp(op))
                        << "note: not profiling because recursive read lock" << endl;
            }
            else if ( lockedForWriting() ) {
                MONGO_LOG_COMPONENT(1, logComponentForOp(op))
                        << "note: not profiling because doing fsync+lock" << endl;
            }
            else {
                profile(txn, c, op, currentOp);
            }
        }

        debug.recordStats();
        debug.reset();
    } /* assembleResponse() */

    void receivedKillCursors(OperationContext* txn, Message& m) {
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

        int found = CollectionCursorCache::eraseCursorGlobalIfAuthorized(txn, n, cursorArray);

        if ( logger::globalLogDomain()->shouldLog(logger::LogSeverity::Debug(1)) || found != n ) {
            LOG( found == n ? 1 : 0 ) << "killcursors: found " << found << " of " << n << endl;
        }

    }

    void receivedUpdate(OperationContext* txn, Message& m, CurOp& op) {
        DbMessage d(m);
        NamespaceString ns(d.getns());
        uassertStatusOK( userAllowedWriteNS( ns ) );
        op.debug().ns = ns.ns();
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

        Status status = txn->getClient()->getAuthorizationSession()->checkAuthForUpdate(ns,
                                                                           query,
                                                                           toupdate,
                                                                           upsert);
        audit::logUpdateAuthzCheck(txn->getClient(), ns, query, toupdate, upsert, multi, status.code());
        uassertStatusOK(status);

        op.debug().query = query;
        op.setQuery(query);

        UpdateRequest request(ns);

        request.setUpsert(upsert);
        request.setMulti(multi);
        request.setQuery(query);
        request.setUpdates(toupdate);
        request.setUpdateOpLog(); // TODO: This is wasteful if repl is not active.
        UpdateLifecycleImpl updateLifecycle(broadcast, ns);
        request.setLifecycle(&updateLifecycle);

        request.setYieldPolicy(PlanExecutor::YIELD_AUTO);

        int attempt = 1;
        while ( 1 ) {
            try {
                UpdateExecutor executor(txn, &request, &op.debug());
                uassertStatusOK(executor.prepare());

                //  Tentatively take an intent lock, fix up if we need to create the collection
                ScopedTransaction transaction(txn, MODE_IX);
                Lock::DBLock dbLock(txn->lockState(), ns.db(), MODE_IX);
                if (dbHolder().get(txn, ns.db()) == NULL) {
                    //  If DB doesn't exist, don't implicitly create it in Client::Context
                    break;
                }
                Lock::CollectionLock colLock(txn->lockState(), ns.ns(), MODE_IX);
                Client::Context ctx(txn, ns);

                //  The common case: no implicit collection creation
                if (!upsert || ctx.db()->getCollection(txn, ns) != NULL) {
                    UpdateResult res = executor.execute(ctx.db());

                    // for getlasterror
                    lastError.getSafe()->recordUpdate( res.existing , res.numMatched , res.upserted );
                    return;
                }
                break;
            }
            catch ( const WriteConflictException& dle ) {
                if ( multi ) {
                    log(LogComponent::kWrite) << "Had WriteConflict during multi update, aborting";
                    throw;
                }
                WriteConflictException::logAndBackoff( attempt++, "update", ns.toString() );
            }
        }

        //  This is an upsert into a non-existing database, so need an exclusive lock
        //  to avoid deadlock
        {
            UpdateExecutor executor(txn, &request, &op.debug());
            uassertStatusOK(executor.prepare());

            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock dbLock(txn->lockState(), ns.db(), MODE_X);
            Client::Context ctx(txn, ns);
            Database* db = ctx.db();
            if ( db->getCollection( txn, ns ) ) {
                // someone else beat us to it, that's ok
                // we might race while we unlock if someone drops
                // but that's ok, we'll just do nothing and error out
            }
            else {
                WriteUnitOfWork wuow(txn);
                uassertStatusOK( userCreateNS( txn, db,
                                               ns.ns(), BSONObj(),
                                               true ) );
                wuow.commit();
            }

            UpdateResult res = executor.execute(db);
            lastError.getSafe()->recordUpdate( res.existing , res.numMatched , res.upserted );
        }
    }

    void receivedDelete(OperationContext* txn, Message& m, CurOp& op) {
        DbMessage d(m);
        NamespaceString ns(d.getns());
        uassertStatusOK( userAllowedWriteNS( ns ) );

        op.debug().ns = ns.ns();
        int flags = d.pullInt();
        bool justOne = flags & RemoveOption_JustOne;
        verify( d.moreJSObjs() );
        BSONObj pattern = d.nextJsObj();

        Status status = txn->getClient()->getAuthorizationSession()->checkAuthForDelete(ns, pattern);
        audit::logDeleteAuthzCheck(txn->getClient(), ns, pattern, status.code());
        uassertStatusOK(status);

        op.debug().query = pattern;
        op.setQuery(pattern);

        DeleteRequest request(ns);
        request.setQuery(pattern);
        request.setMulti(!justOne);
        request.setUpdateOpLog(true);

        request.setYieldPolicy(PlanExecutor::YIELD_AUTO);

        int attempt = 1;
        while ( 1 ) {
            try {
                DeleteExecutor executor(txn, &request);
                uassertStatusOK(executor.prepare());

                AutoGetDb autoDb(txn, ns.db(), MODE_IX);
                if (!autoDb.getDb()) break;

                Lock::CollectionLock colLock(txn->lockState(), ns.ns(), MODE_IX);
                Client::Context ctx(txn, ns);

                long long n = executor.execute(ctx.db());
                lastError.getSafe()->recordDelete( n );
                op.debug().ndeleted = n;

                break;
            }
            catch ( const WriteConflictException& dle ) {
                WriteConflictException::logAndBackoff( attempt++, "delete", ns.toString() );
            }
        }
    }

    QueryResult::View emptyMoreResult(long long);

    bool receivedGetMore(OperationContext* txn,
                         DbResponse& dbresponse,
                         Message& m,
                         CurOp& curop,
                         bool fromDBDirectClient) {
        bool ok = true;

        DbMessage d(m);

        const char *ns = d.getns();
        int ntoreturn = d.pullInt();
        long long cursorid = d.pullInt64();

        curop.debug().ns = ns;
        curop.debug().ntoreturn = ntoreturn;
        curop.debug().cursorid = cursorid;

        scoped_ptr<AssertionException> ex;
        scoped_ptr<Timer> timer;
        int pass = 0;
        bool exhaust = false;
        QueryResult::View msgdata = 0;
        OpTime last;
        while( 1 ) {
            bool isCursorAuthorized = false;
            try {
                const NamespaceString nsString( ns );
                uassert( 16258, str::stream() << "Invalid ns [" << ns << "]", nsString.isValid() );

                Status status = txn->getClient()->getAuthorizationSession()->checkAuthForGetMore(
                        nsString, cursorid);
                audit::logGetMoreAuthzCheck(txn->getClient(), nsString, cursorid, status.code());
                uassertStatusOK(status);

                if (str::startsWith(ns, "local.oplog.")){
                    while (MONGO_FAIL_POINT(rsStopGetMore)) {
                        sleepmillis(0);
                    }

                    if (pass == 0) {
                        last = getLastSetOptime();
                    }
                    else {
                        repl::waitUpToOneSecondForOptimeChange(last);
                    }
                }

                msgdata = newGetMore(txn,
                                     ns,
                                     ntoreturn,
                                     cursorid,
                                     curop,
                                     pass,
                                     exhaust,
                                     &isCursorAuthorized,
                                     fromDBDirectClient);
            }
            catch ( AssertionException& e ) {
                if ( isCursorAuthorized ) {
                    // If a cursor with id 'cursorid' was authorized, it may have been advanced
                    // before an exception terminated processGetMore.  Erase the ClientCursor
                    // because it may now be out of sync with the client's iteration state.
                    // SERVER-7952
                    // TODO Temporary code, see SERVER-4563 for a cleanup overview.
                    CollectionCursorCache::eraseCursorGlobal(txn, cursorid );
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
                if (debug)
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
                        Client::Context& ctx,
                        const char *ns,
                        /*modifies*/BSONObj& js) {

        StatusWith<BSONObj> fixed = fixDocumentForInsert( js );
        uassertStatusOK( fixed.getStatus() );
        if ( !fixed.getValue().isEmpty() )
            js = fixed.getValue();

        WriteUnitOfWork wunit(txn);
        Collection* collection = ctx.db()->getCollection( txn, ns );
        if ( !collection ) {
            collection = ctx.db()->createCollection( txn, ns );
            verify( collection );
            repl::logOp(txn,
                        "c",
                        (ctx.db()->name() + ".$cmd").c_str(),
                        BSON("create" << nsToCollectionSubstring(ns)));
        }

        StatusWith<RecordId> status = collection->insertDocument( txn, js, true );
        uassertStatusOK( status.getStatus() );
        repl::logOp(txn, "i", ns, js);
        wunit.commit();
    }

    NOINLINE_DECL void insertMulti(OperationContext* txn,
                                   Client::Context& ctx,
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
                setLastError(ex.getCode(), ex.getInfo().msg.c_str());
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
            setLastError(ex.getCode(), ex.getInfo().msg.c_str());
            curOp.debug().exceptionInfo = ex.getInfo();
            return;
        }
        BSONArray allCmds(allCmdsBuilder.done());
        Command* createIndexesCmd = Command::findCommand("createIndexes");
        invariant(createIndexesCmd);
        const bool keepGoing = d.reservedField() & InsertOption_ContinueOnError;
        for (BSONObjIterator iter(allCmds); iter.more();) {
            try {
                BSONObjBuilder resultBuilder;
                BSONObj cmdObj = iter.next().Obj();
                Command::execCommand(
                        txn,
                        createIndexesCmd,
                        0, /* what should I use for query option? */
                        d.getns(),
                        cmdObj,
                        resultBuilder,
                        false /* fromRepl */);
                uassertStatusOK(Command::getStatusFromCommandResult(resultBuilder.done()));
            }
            catch (const DBException& ex) {
                setLastError(ex.getCode(), ex.getInfo().msg.c_str());
                curOp.debug().exceptionInfo = ex.getInfo();
                if (!keepGoing) {
                    return;
                }
            }
        }
    }

    void receivedInsert(OperationContext* txn, Message& m, CurOp& op) {
        DbMessage d(m);
        const char *ns = d.getns();
        op.debug().ns = ns;
        uassertStatusOK( userAllowedWriteNS( ns ) );
        if (nsToCollectionSubstring(ns) == "system.indexes") {
            insertSystemIndexes(txn, d, op);
            return;
        }
        const NamespaceString nsString(ns);

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
            Status status = txn->getClient()->getAuthorizationSession()->checkAuthForInsert(nsString, obj);
            audit::logInsertAuthzCheck(txn->getClient(), nsString, obj, status.code());
            uassertStatusOK(status);
        }

        const int notMasterCodeForInsert = 10058; // This is different from ErrorCodes::NotMaster
        {
            const bool isIndexBuild = (nsToCollectionSubstring(ns) == "system.indexes");
            const LockMode mode = isIndexBuild ? MODE_X : MODE_IX;
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock dbLock(txn->lockState(), nsString.db(), mode);
            Lock::CollectionLock collLock(txn->lockState(), nsString.ns(), mode);

            // CONCURRENCY TODO: is being read locked in big log sufficient here?
            // writelock is used to synchronize stepdowns w/ writes
            uassert(notMasterCodeForInsert, "not master",
                    repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(nsString.db()));

            // Client::Context may implicitly create a database, so check existence
            if (dbHolder().get(txn, nsString.db()) != NULL) {
                Client::Context ctx(txn, ns);
                if (mode == MODE_X || ctx.db()->getCollection(txn, nsString)) {
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
                repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(nsString.db()));

        Client::Context ctx(txn, ns);

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

    static void shutdownServer() {
        log(LogComponent::kNetwork) << "shutdown: going to close listening sockets..." << endl;
        ListeningSockets::get()->closeAll();

        log(LogComponent::kNetwork) << "shutdown: going to flush diaglog..." << endl;
        _diaglog.flush();

        /* must do this before unmapping mem or you may get a seg fault */
        log(LogComponent::kNetwork) << "shutdown: going to close sockets..." << endl;
        boost::thread close_socket_thread( stdx::bind(MessagingPort::closeAllSockets, 0) );

        StorageEngine* storageEngine = getGlobalEnvironment()->getGlobalStorageEngine();
        storageEngine->cleanShutdown();
    }

    void exitCleanly(ExitCode code) {
        if (shutdownInProgress.fetchAndAdd(1) != 0) {
            while (true) {
                sleepsecs(1000);
            }
        }

        // Global storage engine may not be started in all cases before we exit
        if (getGlobalEnvironment()->getGlobalStorageEngine() == NULL) {
            dbexit(code); // never returns
            invariant(false);
        }

        getGlobalEnvironment()->setKillAllOperations();

        repl::getGlobalReplicationCoordinator()->shutdown();

        OperationContextImpl txn;
        Lock::GlobalWrite lk(txn.lockState());

        log() << "now exiting" << endl;

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
        flushForGcov();

        audit::logShutdown(currentClient.get());

        log() << "dbexit: " << why << " rc: " << rc;

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
    DiagLog::DiagLog() : f(0) , level(0), mutex("DiagLog") { 
    }

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
        scoped_lock lk(mutex);
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
            scoped_lock lk(mutex);
            f->flush();
        }
    }
    
    void DiagLog::writeop(char *data,int len) {
        if ( level & 1 ) {
            scoped_lock lk(mutex);
            f->write(data,len);
        }
    }
    
    void DiagLog::readop(char *data, int len) {
        if ( level & 2 ) {
            bool log = (level & 4) == 0;
            OCCASIONALLY log = true;
            if ( log ) {
                scoped_lock lk(mutex);
                verify( f );
                f->write(data,len);
            }
        }
    }

    DiagLog _diaglog;

    // ----- END Diaglog -----

} // namespace mongo
