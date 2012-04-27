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
*/

/* Client represents a connection to the database (the server-side) and corresponds
   to an open socket (or logical connection if pooling on sockets) from a client.
*/

#include "pch.h"
#include "db.h"
#include "client.h"
#include "curop-inl.h"
#include "json.h"
#include "security.h"
#include "commands.h"
#include "instance.h"
#include "../s/d_logic.h"
#include "dbwebserver.h"
#include "../util/mongoutils/html.h"
#include "../util/mongoutils/checksum.h"
#include "../util/file_allocator.h"
#include "repl/rs.h"
#include "../scripting/engine.h"
#include "pagefault.h"

namespace mongo {
  
    Client* Client::syncThread;
    mongo::mutex& Client::clientsMutex = *(new mutex("clientsMutex"));
    set<Client*>& Client::clients = *(new set<Client*>); // always be in clientsMutex when manipulating this

    TSP_DEFINE(Client, currentClient)

#if defined(_DEBUG)
    struct StackChecker;
    ThreadLocalValue<StackChecker *> checker;

    struct StackChecker { 
        enum { SZ = 192 * 1024 };
        char buf[SZ];
        StackChecker() { 
            checker.set(this);
        }
        void init() { 
            memset(buf, 42, sizeof(buf)); 
        }
        static void check(const char *tname) { 
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
#if defined _DEBUG
    static unsigned long long nThreads = 0;
    void assertStartingUp() { 
        verify( nThreads <= 1 );
    }
#else
    void assertStartingUp() { }
#endif

    Client& Client::initThread(const char *desc, AbstractMessagingPort *mp) {
#if defined(_DEBUG)
        { 
            nThreads++; // never decremented.  this is for casi class asserts
            if( sizeof(void*) == 8 ) {
                StackChecker sc;
                sc.init();
            }
        }
#endif
        verify( currentClient.get() == 0 );
        Client *c = new Client(desc, mp);
        currentClient.reset(c);
        mongo::lastError.initThread();
        return *c;
    }

    Client::Client(const char *desc, AbstractMessagingPort *p) :
        _context(0),
        _shutdown(false),
        _desc(desc),
        _god(0),
        _lastOp(0),
        _mp(p),
        _sometimes(0)
    {
        _hasWrittenThisPass = false;
        _pageFaultRetryableSection = 0;
        _connectionId = setThreadName(desc);
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

        if ( _context )
            error() << "Client::~Client _context should be null but is not; client:" << _desc << endl;

        if ( ! _shutdown ) {
            error() << "Client::shutdown not called: " << _desc << endl;
        }

        if ( ! inShutdown() ) {
            // we can't clean up safely once we're in shutdown
            scoped_lock bl(clientsMutex);
            if ( ! _shutdown )
                clients.erase(this);
            delete _curOp;
        }
    }

    bool Client::shutdown() {
#if defined(_DEBUG)
        { 
            if( sizeof(void*) == 8 ) {
                StackChecker::check( desc().c_str() );
            }
        }
#endif
        _shutdown = true;
        if ( inShutdown() )
            return false;
        {
            scoped_lock bl(clientsMutex);
            clients.erase(this);
            if ( isSyncThread() ) {
                syncThread = 0;
            }
        }

        return false;
    }

    BSONObj CachedBSONObj::_tooBig = fromjson("{\"$msg\":\"query not recording (too large)\"}");
    Client::Context::Context( string ns , Database * db, bool doauth ) :
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
        checkNsAccess( doauth );
    }

    Client::Context::Context(const string& ns, string path , bool doauth, bool doVersion ) :
        _client( currentClient.get() ), 
        _oldContext( _client->_context ),
        _path( path ), 
        _justCreated(false), // set for real in finishInit
        _doVersion(doVersion),
        _ns( ns ), 
        _db(0) 
    {
        _finishInit( doauth );
    }
       
    /** "read lock, and set my context, all in one operation" 
     *  This handles (if not recursively locked) opening an unopened database.
     */
    Client::ReadContext::ReadContext(const string& ns, string path, bool doauth ) {
        {
            lk.reset( new Lock::DBRead(ns) );
            Database *db = dbHolder().get(ns, path);
            if( db ) {
                c.reset( new Context(path, ns, db, doauth) );
                return;
            }
        }

        // we usually don't get here, so doesn't matter how fast this part is
        {
            DEV log() << "_DEBUG ReadContext db wasn't open, will try to open " << ns << endl;
            if( Lock::isW() ) { 
                // write locked already
                DEV RARELY log() << "write locked on ReadContext construction " << ns << endl;
                c.reset( new Context(ns, path, doauth) );
            }
            else if( !Lock::nested() ) { 
                lk.reset(0);
                {
                    Lock::GlobalWrite w;
                    Context c(ns, path, doauth);
                }
                // db could be closed at this interim point -- that is ok, we will throw, and don't mind throwing.
                lk.reset( new Lock::DBRead(ns) );
                c.reset( new Context(ns, path, doauth) );
            }
            else { 
                uasserted(15928, str::stream() << "can't open a database from a nested read lock " << ns);
            }
        }

        // todo: are receipts of thousands of queries for a nonexisting database a potential 
        //       cause of bad performance due to the write lock acquisition above?  let's fix that.
        //       it would be easy to first check that there is at least a .ns file, or something similar.
    }

    Client::WriteContext::WriteContext(const string& ns, string path , bool doauth ) 
        : _lk( ns ) ,
          _c( ns , path , doauth ) {
    }


    void Client::Context::checkNotStale() const { 
        switch ( _client->_curOp->getOp() ) {
        case dbGetMore: // getMore's are special and should be handled else where
        case dbUpdate: // update & delete check shard version in instance.cpp, so don't check here as well
        case dbDelete:
            break;
        default: {
            string errmsg;
            ShardChunkVersion received;
            ShardChunkVersion wanted;
            if ( ! shardVersionOk( _ns , errmsg, received, wanted ) ) {
                ostringstream os;
                os << "[" << _ns << "] shard version not ok in Client::Context: " << errmsg;
                throw SendStaleConfigException( _ns, os.str(), received, wanted );
            }
        }
        }
    }

    // invoked from ReadContext
    Client::Context::Context(const string& path, const string& ns, Database *db , bool doauth) :
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
        checkNsAccess( doauth );
    }
       
    void Client::Context::_finishInit( bool doauth ) {
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
        checkNsAccess( doauth, writeLocked ? 1 : 0 );
    }

    void Client::Context::_auth( int lockState ) {
        if (lockState <= 0 && str::endsWith(_ns, ".system.users"))
            lockState = 1; // we don't want read-only users to be able to read system.users SERVER-4692

        if ( _client->_ai.isAuthorizedForLock( _db->name , lockState ) )
            return;

        // before we assert, do a little cleanup
        _client->_context = _oldContext; // note: _oldContext may be null

        stringstream ss;
        ss << "unauthorized db:" << _db->name << " ns:" << _ns << " lock type:" << lockState << " client:" << _client->clientAddress();
        uasserted( 10057 , ss.str() );
    }
    
    Client::Context::~Context() {
        DEV verify( _client == currentClient.get() );
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
    
    void Client::Context::checkNsAccess( bool doauth, int lockState ) {
        if ( 0 ) { // SERVER-4276
            uassert( 15929, "client access to index backing namespace prohibited", NamespaceString::normal( _ns.c_str() ) );
        }
        if ( doauth ) {
            _auth( lockState );
        }
    }
    void Client::Context::checkNsAccess( bool doauth ) {
        checkNsAccess( doauth, Lock::somethingWriteLocked() ? 1 : 0 );
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
            ss << _curOp->infoNoauth().jsonString();
        return ss.str();
    }

    string sayClientState() {
        Client* c = currentClient.get();
        if ( !c )
            return "no client";
        return c->toString();
    }

    Client* curopWaitingForLock( char type ) {
        Client * c = currentClient.get();
        verify( c );
        CurOp * co = c->curop();
        if ( co ) {
            co->waitingForLock( type );
        }
        return c;
    }

    void curopGotLock(Client *c) {
        verify(c);
        CurOp * co = c->curop();
        if ( co )
            co->gotLock();
    }

    void KillCurrentOp::interruptJs( AtomicUInt *op ) {
        if ( !globalScriptEngine )
            return;
        if ( !op ) {
            globalScriptEngine->interruptAll();
        }
        else {
            globalScriptEngine->interrupt( *op );
        }
    }

    void KillCurrentOp::killAll() {
        _globalKill = true;
        interruptJs( 0 );
    }

    void KillCurrentOp::kill(AtomicUInt i) {
        bool found = false;
        {
            scoped_lock l( Client::clientsMutex );
            for( set< Client* >::const_iterator j = Client::clients.begin(); !found && j != Client::clients.end(); ++j ) {
                for( CurOp *k = ( *j )->curop(); !found && k; k = k->parent() ) {
                    if ( k->opNum() == i ) {
                        k->kill();
                        for( CurOp *l = ( *j )->curop(); l != k; l = l->parent() ) {
                            l->kill();
                        }
                        found = true;
                    }
                }
            }
        }
        if ( found ) {
            interruptJs( &i );
        }
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
            theReplSet->ghost->associateSlave(_remoteId, o["member"].Int());
        }
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
        virtual bool run(const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            Client& c = cc();
            c.gotHandshake( cmdObj );
            return 1;
        }

    } handshakeCmd;

    class ClientListPlugin : public WebStatusPlugin {
    public:
        ClientListPlugin() : WebStatusPlugin( "clients" , 20 ) {}
        virtual void init() {}

        virtual void run( stringstream& ss ) {
            using namespace mongoutils::html;

            ss << "\n<table border=1 cellpadding=2 cellspacing=0>";
            ss << "<tr align='left'>"
               << th( a("", "Connections to the database, both internal and external.", "Client") )
               << th( a("http://www.mongodb.org/display/DOCS/Viewing+and+Terminating+Current+Operation", "", "OpId") )
               << "<th>Active</th>"
               << "<th>LockType</th>"
               << "<th>Waiting</th>"
               << "<th>SecsRunning</th>"
               << "<th>Op</th>"
               << th( a("http://www.mongodb.org/display/DOCS/Developer+FAQ#DeveloperFAQ-What%27sa%22namespace%22%3F", "", "Namespace") )
               << "<th>Query</th>"
               << "<th>client</th>"
               << "<th>msg</th>"
               << "<th>progress</th>"

               << "</tr>\n";
            {
                scoped_lock bl(Client::clientsMutex);
                for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) {
                    Client *c = *i;
                    CurOp& co = *(c->curop());
                    ss << "<tr><td>" << c->desc() << "</td>";

                    tablecell( ss , co.opNum() );
                    tablecell( ss , co.active() );
                    {
                        char lt = co.lockType();
                        tablecell(ss, lt ? lt : ' ');
                    }
                    tablecell( ss , co.isWaitingForLock() );
                    if ( co.active() )
                        tablecell( ss , co.elapsedSeconds() );
                    else
                        tablecell( ss , "" );
                    tablecell( ss , co.getOp() );
                    tablecell( ss , co.getNS() );
                    if ( co.haveQuery() ) {
                        tablecell( ss , co.query() );
                    }
                    else
                        tablecell( ss , "" );
                    tablecell( ss , co.getRemoteString() );

                    tablecell( ss , co.getMessage() );
                    tablecell( ss , co.getProgressMeter().toString() );


                    ss << "</tr>\n";
                }
            }
            ss << "</table>\n";

        }

    } clientListPlugin;

    int Client::recommendedYieldMicros( int * writers , int * readers ) {
        int num = 0;
        int w = 0;
        int r = 0;
        {
            scoped_lock bl(clientsMutex);
            for ( set<Client*>::iterator i=clients.begin(); i!=clients.end(); ++i ) {
                Client* c = *i;
                if ( c->curop()->isWaitingForLock() ) {
                    num++;
                    char lt = c->curop()->lockType();
                    if( lt == 'w' || lt == 'W' )
                        w++;
                    else
                        r++;
                }
            }
        }

        if ( writers )
            *writers = w;
        if ( readers )
            *readers = r;

        int time = r * 100;
        time += w * 500;

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

            char lt = c->curop()->lockType();
            if ( lt == 'w' || lt == 'W' )
                writers++;
            else if ( lt == 'r' || lt == 'R' )
                readers++;
        }

        return writers + readers;
    }

    bool Client::allowedToThrowPageFaultException() const {
        return 
            _hasWrittenThisPass == false && 
            _pageFaultRetryableSection != 0 && 
            _pageFaultRetryableSection->laps() < 100; 
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
    string OpDebug::toString() const {
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
        
        OPDEBUG_TOSTRING_HELP( nreturned );
        if ( responseLength > 0 )
            s << " reslen:" << responseLength;
        s << " " << executionTime << "ms";

        return s.str();
    }

#define OPDEBUG_APPEND_NUMBER(x) if( x != -1 ) b.appendNumber( #x , (x) )
#define OPDEBUG_APPEND_BOOL(x) if( x ) b.appendBool( #x , (x) )
    void OpDebug::append( const CurOp& curop, BSONObjBuilder& b ) const {
        b.append( "op" , iscommand ? "command" : opToString( op ) );
        b.append( "ns" , ns.toString() );
        if ( ! query.isEmpty() )
            b.append( iscommand ? "command" : "query" , query );
        else if ( ! iscommand && curop.haveQuery() )
            curop.appendQuery( b , "query" );

        if ( ! updateobj.isEmpty() )
            b.append( "updateobj" , updateobj );
        
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
        OPDEBUG_APPEND_BOOL( fastmod );
        OPDEBUG_APPEND_BOOL( fastmodinsert );
        OPDEBUG_APPEND_BOOL( upsert );
        OPDEBUG_APPEND_NUMBER( keyUpdates );

        if ( ! exceptionInfo.empty() ) 
            exceptionInfo.append( b , "exception" , "exceptionCode" );
        
        OPDEBUG_APPEND_NUMBER( nreturned );
        OPDEBUG_APPEND_NUMBER( responseLength );
        b.append( "millis" , executionTime );
        
    }

}
