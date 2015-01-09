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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

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
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/repl/handshake_args.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/storage_options.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/d_state.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exit.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/log.h"

namespace mongo {

    using std::string;
    using std::stringstream;

    using logger::LogComponent;

    boost::mutex Client::clientsMutex;
    ClientSet Client::clients;

    TSP_DEFINE(Client, currentClient)

    /* each thread which does db operations has a Client object in TLS.
       call this when your thread starts.
    */
    void Client::initThread(const char *desc, AbstractMessagingPort *mp) {
        invariant(currentClient.get() == 0);

        string fullDesc;
        if (mp != NULL) {
            fullDesc = str::stream() << desc << mp->connectionId();
        }
        else {
            fullDesc = desc;
        }

        setThreadName(fullDesc.c_str());
        mongo::lastError.initThread();

        // Create the client obj, attach to thread
        Client* client = new Client(fullDesc, mp);
        client->setAuthorizationSession(
            new AuthorizationSession(
                new AuthzSessionExternalStateMongod(getGlobalAuthorizationManager())));

        currentClient.reset(client);

        // This makes the client visible to maintenance threads
        boost::mutex::scoped_lock clientLock(clientsMutex);
        clients.insert(client);
    }

namespace {
    //  Create an appropriate new locker for the storage engine in use. Caller owns.
    Locker* newLocker() {
        if (isMMAPV1()) {
            return new MMAPV1LockerImpl();
        }

        return new LockerImpl<false>();
    }
}

    Client::Client(const string& desc, AbstractMessagingPort *p)
        : ClientBasic(p),
          _desc(desc),
          _threadId(boost::this_thread::get_id()),
          _connectionId(p ? p->connectionId() : 0),
          _god(0),
          _txn(NULL),
          _locker(newLocker()),
          _lastOp(0),
          _shutdown(false) {

        _curOp = new CurOp( this );
    }

    Client::~Client() {
        _god = 0;

        if ( ! inShutdown() ) {
            // we can't clean up safely once we're in shutdown
            {
                boost::mutex::scoped_lock clientLock(clientsMutex);
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
            boost::mutex::scoped_lock clientLock(clientsMutex);
            clients.erase(this);
        }

        return false;
    }

    BSONObj CachedBSONObjBase::_tooBig = fromjson("{\"$msg\":\"query not recording (too large)\"}");

    Client::Context::Context(OperationContext* txn, const std::string& ns, Database * db)
        : _client(currentClient.get()),
          _justCreated(false),
          _doVersion(true),
          _ns(ns),
          _db(db),
          _txn(txn) {
    }

    Client::Context::Context(OperationContext* txn,
                             const std::string& ns,
                             Database* db,
                             bool justCreated)
        : _client(currentClient.get()),
          _justCreated(justCreated),
          _doVersion(true),
          _ns(ns),
          _db(db),
          _txn(txn) {
        _finishInit();
    }

    Client::Context::Context(OperationContext* txn,
                             const string& ns,
                             bool doVersion)
        : _client(currentClient.get()),
          _justCreated(false), // set for real in finishInit
          _doVersion(doVersion),
          _ns(ns),
          _db(NULL),
          _txn(txn) {

        _finishInit();
    }


    AutoGetDb::AutoGetDb(OperationContext* txn, const StringData& ns, LockMode mode)
            : _dbLock(txn->lockState(), ns, mode),
              _db(dbHolder().get(txn, ns)) {

    }

    AutoGetOrCreateDb::AutoGetOrCreateDb(OperationContext* txn,
                                         const StringData& ns,
                                         LockMode mode)
            :  _transaction(txn, MODE_IX),
               _dbLock(txn->lockState(), ns, mode),
              _db(dbHolder().get(txn, ns)) {
        invariant(mode == MODE_IX || mode == MODE_X);
        _justCreated = false;
        // If the database didn't exist, relock in MODE_X
        if (_db == NULL) {
            if (mode != MODE_X) {
                _dbLock.relockWithMode(MODE_X);
            }
            _db = dbHolder().openDb(txn, ns);
            _justCreated = true;
        }
    }

    AutoGetCollectionForRead::AutoGetCollectionForRead(OperationContext* txn,
                                                       const std::string& ns)
            : _txn(txn),
              _transaction(txn, MODE_IS),
              _db(_txn, nsToDatabaseSubstring(ns), MODE_IS),
              _collLock(_txn->lockState(), ns, MODE_IS),
              _coll(NULL) {

        _init(ns, nsToCollectionSubstring(ns));
    }

    AutoGetCollectionForRead::AutoGetCollectionForRead(OperationContext* txn,
                                                       const NamespaceString& nss)
            : _txn(txn),
              _transaction(txn, MODE_IS),
              _db(_txn, nss.db(), MODE_IS),
              _collLock(_txn->lockState(), nss.toString(), MODE_IS),
              _coll(NULL) {

        _init(nss.toString(), nss.coll());
    }

    void AutoGetCollectionForRead::_init(const std::string& ns, const StringData& coll) {
        massert(28535, "need a non-empty collection name", !coll.empty());

        // TODO: Client::Context legacy, needs to be removed
        _txn->getCurOp()->ensureStarted();
        _txn->getCurOp()->setNS(ns);

        // We have both the DB and collection locked, which the prerequisite to do a stable shard
        // version check.
        ensureShardVersionOKOrThrow(ns);

        // At this point, we are locked in shared mode for the database by the DB lock in the
        // constructor, so it is safe to load the DB pointer.
        if (_db.getDb()) {
            // TODO: Client::Context legacy, needs to be removed
            _txn->getCurOp()->enter(ns.c_str(), _db.getDb()->getProfilingLevel());

            _coll = _db.getDb()->getCollection(ns);
        }
    }

    AutoGetCollectionForRead::~AutoGetCollectionForRead() {
        // Report time spent in read lock
        _txn->getCurOp()->recordGlobalTime(false, _timer.micros());
    }

    Client::WriteContext::WriteContext(OperationContext* opCtx, const std::string& ns)
        : _txn(opCtx),
          _nss(ns),
          _autodb(opCtx, _nss.db(), MODE_IX),
          _collk(opCtx->lockState(), ns, MODE_IX),
          _c(opCtx, ns, _autodb.getDb(), _autodb.justCreated()) {
        _collection = _c.db()->getCollection( ns );
        if ( !_collection && !_autodb.justCreated() ) {
            // relock in MODE_X
            _collk.relockWithMode( MODE_X, _autodb.lock() );
            Database* db = dbHolder().get(_txn, ns );
            invariant( db == _c.db() );
        }
    }

    void Client::Context::checkNotStale() const { 
        switch ( _client->_curOp->getOp() ) {
        case dbGetMore: // getMore's are special and should be handled else where
        case dbUpdate: // update & delete check shard version in instance.cpp, so don't check here as well
        case dbDelete:
            break;
        default: {
            ensureShardVersionOKOrThrow(_ns);
        }
        }
    }
       
    void Client::Context::_finishInit() {
        _db = dbHolder().get(_txn, _ns);
        if (_db) {
            _justCreated = false;
        }
        else {
            invariant(_txn->lockState()->isDbLockedForMode(nsToDatabaseSubstring(_ns), MODE_X));
            _db = dbHolder().openDb(_txn, _ns, &_justCreated);
            invariant(_db);
        }

        if( _doVersion ) checkNotStale();

        _client->_curOp->enter(_ns.c_str(), _db->getProfilingLevel());
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

        std::stringstream ss;
        ss << _threadId;
        builder.append("threadId", ss.str());

        if (_connectionId) {
            builder.appendNumber("connectionId", _connectionId);
        }
    }

    void Client::setOperationContext(OperationContext* txn) {
        // We can only set the OperationContext once before resetting it.
        invariant(txn != NULL && _txn == NULL);

        boost::unique_lock<SpinLock> uniqueLock(_lock);
        _txn = txn;
    }

    void Client::resetOperationContext() {
        invariant(_txn != NULL);
        boost::unique_lock<SpinLock> uniqueLock(_lock);
        _txn = NULL;
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
        writeConflicts = 0;
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
                    mutablebson::Document cmdToLog(query, mutablebson::Document::kInPlaceDisabled);
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
        OPDEBUG_TOSTRING_HELP( writeConflicts );
        
        if ( extra.len() )
            s << " " << extra.str();

        if ( ! exceptionInfo.empty() ) {
            s << " exception: " << exceptionInfo.msg;
            if ( exceptionInfo.code )
                s << " code:" << exceptionInfo.code;
        }

        s << " numYields:" << curop.numYields();
        
        OPDEBUG_TOSTRING_HELP( nreturned );
        if ( responseLength > 0 )
            s << " reslen:" << responseLength;
        s << " " << executionTime << "ms";
        
        return s.str();
    }

    namespace {
        /**
         * Appends {name: obj} to the provided builder.  If obj is greater than maxSize, appends a
         * string summary of obj instead of the object itself.
         */
        void appendAsObjOrString(const StringData& name,
                                 const BSONObj& obj,
                                 size_t maxSize,
                                 BSONObjBuilder* builder) {
            if (static_cast<size_t>(obj.objsize()) <= maxSize) {
                builder->append(name, obj);
            }
            else {
                // Generate an abbreviated serialization for the object, by passing false as the
                // "full" argument to obj.toString().
                const bool isArray = false;
                const bool full = false;
                std::string objToString = obj.toString(isArray, full);
                if (objToString.size() <= maxSize) {
                    builder->append(name, objToString);
                }
                else {
                    // objToString is still too long, so we append to the builder a truncated form
                    // of objToString concatenated with "...".  Instead of creating a new string
                    // temporary, mutate objToString to do this (we know that we can mutate
                    // characters in objToString up to and including objToString[maxSize]).
                    objToString[maxSize - 3] = '.';
                    objToString[maxSize - 2] = '.';
                    objToString[maxSize - 1] = '.';
                    builder->append(name, StringData(objToString).substr(0, maxSize));
                }
            }
        }
    }

#define OPDEBUG_APPEND_NUMBER(x) if( x != -1 ) b.appendNumber( #x , (x) )
#define OPDEBUG_APPEND_BOOL(x) if( x ) b.appendBool( #x , (x) )
    void OpDebug::append(const CurOp& curop, BSONObjBuilder& b) const {
        const size_t maxElementSize = 50 * 1024;

        b.append( "op" , iscommand ? "command" : opToString( op ) );
        b.append( "ns" , ns.toString() );

        if (!query.isEmpty()) {
            appendAsObjOrString(iscommand ? "command" : "query", query, maxElementSize, &b);
        }
        else if (!iscommand && curop.haveQuery()) {
            appendAsObjOrString("query", curop.query(), maxElementSize, &b);
        }

        if (!updateobj.isEmpty()) {
            appendAsObjOrString("updateobj", updateobj, maxElementSize, &b);
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
        OPDEBUG_APPEND_NUMBER( writeConflicts );

        b.appendNumber("numYield", curop.numYields());

        if ( ! exceptionInfo.empty() )
            exceptionInfo.append( b , "exception" , "exceptionCode" );

        OPDEBUG_APPEND_NUMBER( nreturned );
        OPDEBUG_APPEND_NUMBER( responseLength );
        b.append( "millis" , executionTime );

        execStats.append(b, "execStats");
    }

    void saveGLEStats(const BSONObj& result, const std::string& conn) {
        // This can be called in mongod, which is unfortunate.  To fix this,
        // we can redesign how connection pooling works on mongod for sharded operations.
    }
}
