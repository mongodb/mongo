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

#include "mongo/pch.h"

#include "mongo/db/client.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_session_external_state_d.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/db.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop-inl.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/repl/rs.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/stale_exception.h" // for SendStaleConfigException
#include "mongo/scripting/engine.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/mongoutils/checksum.h"
#include "mongo/util/mongoutils/str.h"

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#define ASAN_ENABLED __has_feature(address_sanitizer)
#define MSAN_ENABLED __has_feature(memory_sanitizer)
#define TSAN_ENABLED __has_feature(thread_sanitizer)
#define XSAN_ENABLED (ASAN_ENABLED || MSAN_ENABLED || TSAN_ENABLED)

namespace mongo {

    mongo::mutex& Client::clientsMutex = *(new mutex("clientsMutex"));
    set<Client*>& Client::clients = *(new set<Client*>); // always be in clientsMutex when manipulating this

    TSP_DEFINE(Client, currentClient)

#if defined(_DEBUG) && !XSAN_ENABLED
    struct StackChecker;
    ThreadLocalValue<StackChecker *> checker;

    struct StackChecker { 
#if defined(_WIN32)
        enum { SZ = 330 * 1024 };
#elif defined(__APPLE__) && defined(__MACH__)
        enum { SZ = 374 * 1024 };
#elif defined(__linux__)
        enum { SZ = 235 * 1024 };
#else
        enum { SZ = 235 * 1024 };   // default size, same as Linux to match old behavior
#endif
        char buf[SZ];
        StackChecker() { 
            checker.set(this);
        }
        void init() { 
            memset(buf, 42, sizeof(buf)); 
        }
        static void check(StringData tname) {
            static int max;
            StackChecker *sc = checker.get();
            const char *p = sc->buf;

            int lastStackByteModifed = 0;
            for( ; lastStackByteModifed < SZ; lastStackByteModifed++ ) { 
                if( p[lastStackByteModifed] != 42 )
                    break;
            }
            int numberBytesUsed = SZ-lastStackByteModifed;
            
            if( numberBytesUsed > max ) {
                max = numberBytesUsed;
                log() << "thread " << tname << " stack usage was " << numberBytesUsed << " bytes, " 
                      << " which is the most so far" << endl;
            }
            
            if ( numberBytesUsed > ( SZ - 16000 ) ) {
                // we are within 16000 bytes of SZ
                log() << "used " << numberBytesUsed << " bytes, max is " << (int)SZ << " exiting" << endl;
                fassertFailed( 16151 );
            }

        }
    };
#endif

    /* each thread which does db operations has a Client object in TLS.
       call this when your thread starts.
    */
    Client& Client::initThread(const char *desc, AbstractMessagingPort *mp) {
#if defined(_DEBUG) && !XSAN_ENABLED
        {
            if( sizeof(void*) == 8 ) {
                StackChecker sc;
                sc.init();
            }
        }
#endif
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
        return *c;
    }

    /* resets the client for the current thread */
    void Client::resetThread( const StringData& origThreadName ) {
        verify( currentClient.get() != 0 );

        // Detach all client info from thread
        mongo::lastError.reset(NULL);
        currentClient.get()->shutdown();
        currentClient.reset(NULL);

        setThreadName( origThreadName.rawData() );
    }


    Client::Client(const string& desc, AbstractMessagingPort *p) :
        ClientBasic(p),
        _context(0),
        _shutdown(false),
        _desc(desc),
        _god(0),
        _lastOp(0)
    {
        _hasWrittenThisPass = false;
        _pageFaultRetryableSection = 0;
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
#if defined(_DEBUG) && !XSAN_ENABLED
        {
            if( sizeof(void*) == 8 ) {
                StackChecker::check( desc() );
            }
        }
#endif
        _shutdown = true;
        if ( inShutdown() )
            return false;
        {
            scoped_lock bl(clientsMutex);
            clients.erase(this);
        }

        return false;
    }

    BSONObj CachedBSONObj::_tooBig = fromjson("{\"$msg\":\"query not recording (too large)\"}");
    Client::Context::Context(const std::string& ns , Database * db) :
        _client( currentClient.get() ), 
        _oldContext( _client->_context ),
        _path( mongo::dbpath ), // is this right? could be a different db? may need a dassert for this
        _justCreated(false),
        _doVersion( true ),
        _ns( ns ), 
        _db(db)
    {
        verify( db == 0 || db->isOk() );
        _client->_context = this;
    }

    Client::Context::Context(const string& ns, const std::string& path, bool doVersion) :
        _client( currentClient.get() ), 
        _oldContext( _client->_context ),
        _path( path ), 
        _justCreated(false), // set for real in finishInit
        _doVersion(doVersion),
        _ns( ns ), 
        _db(0) 
    {
        _finishInit();
    }
       
    /** "read lock, and set my context, all in one operation" 
     *  This handles (if not recursively locked) opening an unopened database.
     */
    Client::ReadContext::ReadContext(const string& ns, const std::string& path) {
        {
            lk.reset( new Lock::DBRead(ns) );
            Database *db = dbHolder().get(ns, path);
            if( db ) {
                c.reset( new Context(path, ns, db) );
                return;
            }
        }

        // we usually don't get here, so doesn't matter how fast this part is
        {
            DEV log() << "_DEBUG ReadContext db wasn't open, will try to open " << ns << endl;
            if( Lock::isW() ) { 
                // write locked already
                DEV RARELY log() << "write locked on ReadContext construction " << ns << endl;
                c.reset(new Context(ns, path));
            }
            else if( !Lock::nested() ) { 
                lk.reset(0);
                {
                    Lock::GlobalWrite w;
                    Context c(ns, path);
                }
                // db could be closed at this interim point -- that is ok, we will throw, and don't mind throwing.
                lk.reset( new Lock::DBRead(ns) );
                c.reset(new Context(ns, path));
            }
            else { 
                uasserted(15928, str::stream() << "can't open a database from a nested read lock " << ns);
            }
        }

        // todo: are receipts of thousands of queries for a nonexisting database a potential 
        //       cause of bad performance due to the write lock acquisition above?  let's fix that.
        //       it would be easy to first check that there is at least a .ns file, or something similar.
    }

    Client::WriteContext::WriteContext(const string& ns, const std::string& path)
        : _lk( ns ) ,
          _c(ns, path) {
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
    Client::Context::Context(const string& path, const string& ns, Database *db) :
        _client( currentClient.get() ), 
        _oldContext( _client->_context ),
        _path( path ), 
        _justCreated(false),
        _doVersion( true ),
        _ns( ns ), 
        _db(db)
    {
        verify(_db);
        checkNotStale();
        _client->_context = this;
        _client->_curOp->enter( this );
    }
       
    void Client::Context::_finishInit() {
        dassert( Lock::isLocked() );
        int writeLocked = Lock::somethingWriteLocked();
        if ( writeLocked && FileAllocator::get()->hasFailed() ) {
            uassert(14031, "Can't take a write lock while out of disk space", false);
        }
        
        _db = dbHolderUnchecked().getOrCreate( _ns , _path , _justCreated );
        verify(_db);
        if( _doVersion ) checkNotStale();
        massert( 16107 , str::stream() << "Don't have a lock on: " << _ns , Lock::atLeastReadLocked( _ns ) );
        _client->_context = this;
        _client->_curOp->enter( this );
    }
    
    Client::Context::~Context() {
        DEV verify( _client == currentClient.get() );
        _client->_curOp->recordGlobalTime( _timer.micros() );
        _client->_curOp->leave( this );
        _client->_context = _oldContext; // note: _oldContext may be null
    }

    bool Client::Context::inDB( const string& db , const string& path ) const {
        if ( _path != path )
            return false;

        if ( db == _ns )
            return true;

        string::size_type idx = _ns.find( db );
        if ( idx != 0 )
            return false;

        return  _ns[db.size()] == '.';
    }

    void Client::appendLastOp( BSONObjBuilder& b ) const {
        // _lastOp is never set if replication is off
        if( theReplSet || ! _lastOp.isNull() ) {
            b.appendTimestamp( "lastOp" , _lastOp.asDate() );
        }
    }

    string Client::clientAddress(bool includePort) const {
        if( _curOp )
            return _curOp->getRemoteString(includePort);
        return "";
    }

    string Client::toString() const {
        stringstream ss;
        if ( _curOp )
            ss << _curOp->info().jsonString();
        return ss.str();
    }

    string sayClientState() {
        Client* c = currentClient.get();
        if ( !c )
            return "no client";
        return c->toString();
    }

    void Client::gotHandshake( const BSONObj& o ) {
        BSONObjIterator i(o);

        {
            BSONElement id = i.next();
            verify( id.type() );
            _remoteId = id.wrap( "_id" );
        }

        BSONObjBuilder b;
        while ( i.more() )
            b.append( i.next() );
        
        b.appendElementsUnique( _handshake );

        _handshake = b.obj();

        if (theReplSet && o.hasField("member")) {
            theReplSet->registerSlave(_remoteId, o["member"].Int());
        }
    }

    bool ClientBasic::hasCurrent() {
        return currentClient.get();
    }

    ClientBasic* ClientBasic::getCurrent() {
        return currentClient.get();
    }

    class HandshakeCmd : public Command {
    public:
        void help(stringstream& h) const { h << "internal"; }
        HandshakeCmd() : Command( "handshake" ) {}
        virtual LockType locktype() const { return NONE; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return false; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::handshake);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            Client& c = cc();
            c.gotHandshake( cmdObj );
            return 1;
        }

    } handshakeCmd;

    int Client::recommendedYieldMicros( int * writers , int * readers, bool needExact ) {
        int num = 0;
        int w = 0;
        int r = 0;
        {
            scoped_lock bl(clientsMutex);
            for ( set<Client*>::iterator i=clients.begin(); i!=clients.end(); ++i ) {
                Client* c = *i;
                if ( c->lockState().hasLockPending() ) {
                    num++;
                    if ( c->lockState().hasAnyWriteLock() )
                        w++;
                    else
                        r++;
                }
                if (num > 100 && !needExact)
                    break;
            }
        }

        if ( writers )
            *writers = w;
        if ( readers )
            *readers = r;

        int time = r * 10; // we have to be nice to readers since they don't have priority
        time += w; // writers are greedy, so we can be mean tot hem

        time = min( time , 1000000 );

        // if there has been a kill request for this op - we should yield to allow the op to stop
        // This function returns empty string if we aren't interrupted
        if ( *killCurrentOp.checkForInterruptNoAssert() ) {
            return 100;
        }

        return time;
    }

    int Client::getActiveClientCount( int& writers, int& readers ) {
        writers = 0;
        readers = 0;

        scoped_lock bl(clientsMutex);
        for ( set<Client*>::iterator i=clients.begin(); i!=clients.end(); ++i ) {
            Client* c = *i;
            if ( ! c->curop()->active() )
                continue;

            if ( c->lockState().hasAnyWriteLock() )
                writers++;
            if ( c->lockState().hasAnyReadLock() )
                readers++;
        }

        return writers + readers;
    }

    bool Client::allowedToThrowPageFaultException() const {
        if ( _hasWrittenThisPass )
            return false;
        
        if ( ! _pageFaultRetryableSection )
            return false;

        if ( _pageFaultRetryableSection->laps() >= 100 )
            return false;
        
        // if we've done a normal yield, it means we're in a ClientCursor or something similar
        // in that case, that code should be handling yielding, not us
        if ( _curOp && _curOp->numYields() > 0 ) 
            return false;

        return true;
    }

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
        idhack = false;
        scanAndOrder = false;
        nupdated = -1;
        ninserted = -1;
        ndeleted = -1;
        nmoved = -1;
        fastmod = false;
        fastmodinsert = false;
        upsert = false;
        keyUpdates = 0;  // unsigned, so -1 not possible
        
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
            if ( iscommand )
                s << " command: ";
            else
                s << " query: ";
            s << query.toString();
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
        OPDEBUG_TOSTRING_HELP_BOOL( idhack );
        OPDEBUG_TOSTRING_HELP_BOOL( scanAndOrder );
        OPDEBUG_TOSTRING_HELP( nmoved );
        OPDEBUG_TOSTRING_HELP( nupdated );
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

        if ( curop.numYields() )
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
        OPDEBUG_APPEND_BOOL( idhack );
        OPDEBUG_APPEND_BOOL( scanAndOrder );
        OPDEBUG_APPEND_BOOL( moved );
        OPDEBUG_APPEND_NUMBER( nmoved );
        OPDEBUG_APPEND_NUMBER( nupdated );
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

        return true;
    }

}
