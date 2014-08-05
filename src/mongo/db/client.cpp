// client.cpp

/**
*    Copyright (C) 2009 10gen Inc.
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

/* Client represents a connection to the database (the server-side) and corresponds
   to an open socket (or logical connection if pooling on sockets) from a client.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_session_external_state_d.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/db.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/handshake_args.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/storage_options.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/stale_exception.h" // for SendStaleConfigException
#include "mongo/scripting/engine.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/log.h"


namespace mongo {

    mongo::mutex& Client::clientsMutex = *(new mutex("clientsMutex"));
    set<Client*>& Client::clients = *(new set<Client*>); // always be in clientsMutex when manipulating this

    TSP_DEFINE(Client, currentClient)

    /* each thread which does db operations has a Client object in TLS.
       call this when your thread starts.
    */
    void Client::initThread(const char *desc, AbstractMessagingPort *mp) {
        verify( currentClient.get() == 0 );

        string fullDesc = desc;
        if ( str::equals( "conn" , desc ) && mp != NULL )
            fullDesc = str::stream() << desc << mp->connectionId();

        setThreadName( fullDesc.c_str() );

        // Create the client obj, attach to thread
        Client *c = new Client( fullDesc, mp );
        currentClient.reset(c);
        mongo::lastError.initThread();
        c->setAuthorizationSession(new AuthorizationSession(new AuthzSessionExternalStateMongod(
                getGlobalAuthorizationManager())));
    }

    Client::Client(const string& desc, AbstractMessagingPort *p) :
        ClientBasic(p),
        _shutdown(false),
        _desc(desc),
        _god(0),
        _lastOp(0)
    {
        _hasWrittenSinceCheckpoint = false;
        _connectionId = p ? p->connectionId() : 0;
        _curOp = new CurOp( this );
#ifndef _WIN32
        stringstream temp;
        temp << hex << showbase << pthread_self();
        _threadId = temp.str();
#endif
        scoped_lock bl(clientsMutex);
        clients.insert(this);
    }

    Client::~Client() {
        _god = 0;

        // Because both Client object pointers and logging infrastructure are stored in Thread
        // Specific Pointers and because we do not explicitly control the order in which TSPs are
        // deleted, it is possible for the logging infrastructure to have been deleted before
        // this code runs.  This leads to segfaults (access violations) if this code attempts
        // to log anything.  Therefore, disable logging from this destructor until this is fixed.
        // TODO(tad) Force the logging infrastructure to be the last TSP to be deleted for each
        // thread and reenable this code once that is done.
#if 0
        if ( _context )
            error() << "Client::~Client _context should be null but is not; client:" << _desc << endl;

        if ( ! _shutdown ) {
            error() << "Client::shutdown not called: " << _desc << endl;
        }
#endif

        if ( ! inShutdown() ) {
            // we can't clean up safely once we're in shutdown
            {
                scoped_lock bl(clientsMutex);
                if ( ! _shutdown )
                    clients.erase(this);
            }

            CurOp* last;
            do {
                last = _curOp;
                delete _curOp;
                // _curOp may have been reset to _curOp->_wrapped
            } while (_curOp != last);
        }
    }

    bool Client::shutdown() {
        _shutdown = true;
        if ( inShutdown() )
            return false;
        {
            scoped_lock bl(clientsMutex);
            clients.erase(this);
        }

        return false;
    }

    BSONObj CachedBSONObjBase::_tooBig = fromjson("{\"$msg\":\"query not recording (too large)\"}");

    Client::Context::Context(OperationContext* txn, const std::string& ns, Database * db)
        : _client( currentClient.get() ), 
          _justCreated(false),
          _doVersion( true ),
          _ns( ns ), 
          _db(db),
          _txn(txn) {

    }

    Client::Context::Context(OperationContext* txn,
                             const string& ns,
                             bool doVersion)
        : _client( currentClient.get() ), 
          _justCreated(false), // set for real in finishInit
          _doVersion(doVersion),
          _ns( ns ), 
          _db(NULL),
          _txn(txn) {

        _finishInit();
    }
       
    /** "read lock, and set my context, all in one operation" 
     *  This handles (if not recursively locked) opening an unopened database.
     */
    Client::ReadContext::ReadContext(
                OperationContext* txn, const string& ns, bool doVersion) {
        {
            _lk.reset(new Lock::DBRead(txn->lockState(), ns));
            Database *db = dbHolder().get(txn, ns);
            if( db ) {
                _c.reset(new Context(txn, ns, db, doVersion));
                return;
            }
        }

        // we usually don't get here, so doesn't matter how fast this part is
        {
            DEV log() << "_DEBUG ReadContext db wasn't open, will try to open " << ns << endl;
            if (txn->lockState()->isW()) {
                // write locked already
                WriteUnitOfWork wunit(txn->recoveryUnit());
                DEV RARELY log() << "write locked on ReadContext construction " << ns << endl;
                _c.reset(new Context(txn, ns, doVersion));
                wunit.commit();
            }
            else if (!txn->lockState()->isRecursive()) {
                _lk.reset(0);
                {
                    Lock::GlobalWrite w(txn->lockState());
                    WriteUnitOfWork wunit(txn->recoveryUnit());
                    Context c(txn, ns, doVersion);
                    wunit.commit();
                }

                // db could be closed at this interim point -- that is ok, we will throw, and don't mind throwing.
                _lk.reset(new Lock::DBRead(txn->lockState(), ns));
                _c.reset(new Context(txn, ns, doVersion));
            }
            else { 
                uasserted(15928, str::stream() << "can't open a database from a nested read lock " << ns);
            }
        }

        // todo: are receipts of thousands of queries for a nonexisting database a potential 
        //       cause of bad performance due to the write lock acquisition above?  let's fix that.
        //       it would be easy to first check that there is at least a .ns file, or something similar.
    }

    Client::WriteContext::WriteContext(
                OperationContext* opCtx, const std::string& ns, bool doVersion)
        : _lk(opCtx->lockState(), ns),
          _wunit(opCtx->recoveryUnit()),
          _c(opCtx, ns, doVersion) {
    }

    void Client::WriteContext::commit() {
        _wunit.commit();
    }

    void Client::Context::checkNotStale() const { 
        switch ( _client->_curOp->getOp() ) {
        case dbGetMore: // getMore's are special and should be handled else where
        case dbUpdate: // update & delete check shard version in instance.cpp, so don't check here as well
        case dbDelete:
            break;
        default: {
            string errmsg;
            ChunkVersion received;
            ChunkVersion wanted;
            if ( ! shardVersionOk( _ns , errmsg, received, wanted ) ) {
                ostringstream os;
                os << "[" << _ns << "] shard version not ok in Client::Context: " << errmsg;
                throw SendStaleConfigException( _ns, os.str(), received, wanted );
            }
        }
        }
    }

    // invoked from ReadContext
    Client::Context::Context(OperationContext* txn,
                             const string& ns,
                             Database *db,
                             bool doVersion)
        : _client( currentClient.get() ), 
          _justCreated(false),
          _doVersion( doVersion ),
          _ns( ns ), 
          _db(db),
          _txn(txn) {

        verify(_db);
        if (_doVersion) checkNotStale();
        _client->_curOp->enter( this );
    }
       
    void Client::Context::_finishInit() {
        _db = dbHolder().getOrCreate(_txn, _ns, _justCreated);
        invariant(_db);

        if( _doVersion ) checkNotStale();

        _client->_curOp->enter( this );
    }
    
    Client::Context::~Context() {
        DEV verify( _client == currentClient.get() );

        // Lock must still be held
        invariant(_txn->lockState()->isLocked());

        _client->_curOp->recordGlobalTime(_txn->lockState()->isWriteLocked(), _timer.micros());
    }

    void Client::appendLastOp( BSONObjBuilder& b ) const {
        // _lastOp is never set if replication is off
        if (repl::getGlobalReplicationCoordinator()->getReplicationMode() ==
                repl::ReplicationCoordinator::modeReplSet || !_lastOp.isNull()) {
            b.appendTimestamp( "lastOp" , _lastOp.asDate() );
        }
    }

    void Client::reportState(BSONObjBuilder& builder) {
        builder.append("desc", desc());
        if (_threadId.size()) {
            builder.append("threadId", _threadId);
        }

        if (_connectionId) {
            builder.appendNumber("connectionId", _connectionId);
        }
    }

    string Client::clientAddress(bool includePort) const {
        if( _curOp )
            return _curOp->getRemoteString(includePort);
        return "";
    }

    ClientBasic* ClientBasic::getCurrent() {
        return currentClient.get();
    }

    class HandshakeCmd : public Command {
    public:
        void help(stringstream& h) const { h << "internal"; }
        HandshakeCmd() : Command( "handshake" ) {}
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return false; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::internal);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        virtual bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            repl::HandshakeArgs handshake;
            Status status = handshake.initialize(cmdObj);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            // TODO(dannenberg) move this into actual processing for both version
            txn->getClient()->setRemoteID(handshake.getRid());

            status = repl::getGlobalReplicationCoordinator()->processHandshake(txn,
                                                                               handshake);
            return appendCommandStatus(result, status);
        }

    } handshakeCmd;



    void OpDebug::reset() {
        extra.reset();

        op = 0;
        iscommand = false;
        ns = "";
        query = BSONObj();
        updateobj = BSONObj();

        cursorid = -1;
        ntoreturn = -1;
        ntoskip = -1;
        exhaust = false;

        nscanned = -1;
        nscannedObjects = -1;
        idhack = false;
        scanAndOrder = false;
        nMatched = -1;
        nModified = -1;
        ninserted = -1;
        ndeleted = -1;
        nmoved = -1;
        fastmod = false;
        fastmodinsert = false;
        upsert = false;
        keyUpdates = 0;  // unsigned, so -1 not possible
        planSummary = "";
        execStats.reset();
        
        exceptionInfo.reset();
        
        executionTime = 0;
        nreturned = -1;
        responseLength = -1;
    }


#define OPDEBUG_TOSTRING_HELP(x) if( x >= 0 ) s << " " #x ":" << (x)
#define OPDEBUG_TOSTRING_HELP_BOOL(x) if( x ) s << " " #x ":" << (x)
    string OpDebug::report( const CurOp& curop ) const {
        StringBuilder s;
        if ( iscommand )
            s << "command ";
        else
            s << opToString( op ) << ' ';
        s << ns.toString();

        if ( ! query.isEmpty() ) {
            if ( iscommand ) {
                s << " command: ";
                
                Command* curCommand = curop.getCommand();
                if (curCommand) {
                    mutablebson::Document cmdToLog(curop.query(), 
                            mutablebson::Document::kInPlaceDisabled);
                    curCommand->redactForLogging(&cmdToLog);
                    s << curCommand->name << " ";
                    s << cmdToLog.toString();
                } 
                else { // Should not happen but we need to handle curCommand == NULL gracefully
                    s << query.toString();
                }
            }
            else {
                s << " query: ";
                s << query.toString();
            }
        }

        if (!planSummary.empty()) {
            s << " planSummary: " << planSummary.toString();
        }
        
        if ( ! updateobj.isEmpty() ) {
            s << " update: ";
            updateobj.toString( s );
        }

        OPDEBUG_TOSTRING_HELP( cursorid );
        OPDEBUG_TOSTRING_HELP( ntoreturn );
        OPDEBUG_TOSTRING_HELP( ntoskip );
        OPDEBUG_TOSTRING_HELP_BOOL( exhaust );

        OPDEBUG_TOSTRING_HELP( nscanned );
        OPDEBUG_TOSTRING_HELP( nscannedObjects );
        OPDEBUG_TOSTRING_HELP_BOOL( idhack );
        OPDEBUG_TOSTRING_HELP_BOOL( scanAndOrder );
        OPDEBUG_TOSTRING_HELP( nmoved );
        OPDEBUG_TOSTRING_HELP( nMatched );
        OPDEBUG_TOSTRING_HELP( nModified );
        OPDEBUG_TOSTRING_HELP( ninserted );
        OPDEBUG_TOSTRING_HELP( ndeleted );
        OPDEBUG_TOSTRING_HELP_BOOL( fastmod );
        OPDEBUG_TOSTRING_HELP_BOOL( fastmodinsert );
        OPDEBUG_TOSTRING_HELP_BOOL( upsert );
        OPDEBUG_TOSTRING_HELP( keyUpdates );
        
        if ( extra.len() )
            s << " " << extra.str();

        if ( ! exceptionInfo.empty() ) {
            s << " exception: " << exceptionInfo.msg;
            if ( exceptionInfo.code )
                s << " code:" << exceptionInfo.code;
        }

        s << " numYields:" << curop.numYields();
        
        s << " ";
        curop.lockStat().report( s );
        
        OPDEBUG_TOSTRING_HELP( nreturned );
        if ( responseLength > 0 )
            s << " reslen:" << responseLength;
        s << " " << executionTime << "ms";
        
        return s.str();
    }

#define OPDEBUG_APPEND_NUMBER(x) if( x != -1 ) b.appendNumber( #x , (x) )
#define OPDEBUG_APPEND_BOOL(x) if( x ) b.appendBool( #x , (x) )
    bool OpDebug::append(const CurOp& curop, BSONObjBuilder& b, size_t maxSize) const {
        b.append( "op" , iscommand ? "command" : opToString( op ) );
        b.append( "ns" , ns.toString() );
        
        int queryUpdateObjSize = 0;
        if (!query.isEmpty()) {
            queryUpdateObjSize += query.objsize();
        }
        else if (!iscommand && curop.haveQuery()) {
            queryUpdateObjSize += curop.query()["query"].size();
        }

        if (!updateobj.isEmpty()) {
            queryUpdateObjSize += updateobj.objsize();
        }

        if (static_cast<size_t>(queryUpdateObjSize) > maxSize) {
            if (!query.isEmpty()) {
                // Use 60 since BSONObj::toString can truncate strings into 150 chars
                // and we want to have enough room for both query and updateobj when
                // the entire document is going to be serialized into a string
                const string abbreviated(query.toString(false, false), 0, 60);
                b.append(iscommand ? "command" : "query", abbreviated + "...");
            }
            else if (!iscommand && curop.haveQuery()) {
                const string abbreviated(curop.query()["query"].toString(false, false), 0, 60);
                b.append("query", abbreviated + "...");
            }

            if (!updateobj.isEmpty()) {
                const string abbreviated(updateobj.toString(false, false), 0, 60);
                b.append("updateobj", abbreviated + "...");
            }

            return false;
        }

        if (!query.isEmpty()) {
            b.append(iscommand ? "command" : "query", query);
        }
        else if (!iscommand && curop.haveQuery()) {
            curop.appendQuery(b, "query");
        }

        if (!updateobj.isEmpty()) {
            b.append("updateobj", updateobj);
        }

        const bool moved = (nmoved >= 1);

        OPDEBUG_APPEND_NUMBER( cursorid );
        OPDEBUG_APPEND_NUMBER( ntoreturn );
        OPDEBUG_APPEND_NUMBER( ntoskip );
        OPDEBUG_APPEND_BOOL( exhaust );

        OPDEBUG_APPEND_NUMBER( nscanned );
        OPDEBUG_APPEND_NUMBER( nscannedObjects );
        OPDEBUG_APPEND_BOOL( idhack );
        OPDEBUG_APPEND_BOOL( scanAndOrder );
        OPDEBUG_APPEND_BOOL( moved );
        OPDEBUG_APPEND_NUMBER( nmoved );
        OPDEBUG_APPEND_NUMBER( nMatched );
        OPDEBUG_APPEND_NUMBER( nModified );
        OPDEBUG_APPEND_NUMBER( ninserted );
        OPDEBUG_APPEND_NUMBER( ndeleted );
        OPDEBUG_APPEND_BOOL( fastmod );
        OPDEBUG_APPEND_BOOL( fastmodinsert );
        OPDEBUG_APPEND_BOOL( upsert );
        OPDEBUG_APPEND_NUMBER( keyUpdates );

        b.appendNumber( "numYield" , curop.numYields() );
        b.append( "lockStats" , curop.lockStat().report() );

        if ( ! exceptionInfo.empty() )
            exceptionInfo.append( b , "exception" , "exceptionCode" );

        OPDEBUG_APPEND_NUMBER( nreturned );
        OPDEBUG_APPEND_NUMBER( responseLength );
        b.append( "millis" , executionTime );

        execStats.append(b, "execStats");

        return true;
    }

    void saveGLEStats(const BSONObj& result, const std::string& conn) {
        // This can be called in mongod, which is unfortunate.  To fix this,
        // we can redesign how connection pooling works on mongod for sharded operations.
    }
}
