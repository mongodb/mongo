// instance.cpp : Global state variables and functions.
//

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
*/

#include "pch.h"
#include "db.h"
#include "query.h"
#include "introspect.h"
#include "repl.h"
#include "dbmessage.h"
#include "instance.h"
#include "lasterror.h"
#include "security.h"
#include "json.h"
//#include "reccache.h"
#include "replpair.h"
#include "../s/d_logic.h"
#include "../util/file_allocator.h"
#include "../util/goodies.h"
#include "cmdline.h"
#if !defined(_WIN32)
#include <sys/file.h>
#endif
#include "stats/counters.h"
#include "background.h"

namespace mongo {

    inline void opread(Message& m) { if( _diaglog.level & 2 ) _diaglog.readop((char *) m.singleData(), m.header()->len); }
    inline void opwrite(Message& m) { if( _diaglog.level & 1 ) _diaglog.write((char *) m.singleData(), m.header()->len); }

    void receivedKillCursors(Message& m);
    void receivedUpdate(Message& m, CurOp& op);
    void receivedDelete(Message& m, CurOp& op);
    void receivedInsert(Message& m, CurOp& op);
    bool receivedGetMore(DbResponse& dbresponse, Message& m, CurOp& curop );

    int nloggedsome = 0;
#define LOGSOME if( ++nloggedsome < 1000 || nloggedsome % 100 == 0 )

    string dbExecCommand;

    char *appsrvPath = NULL;

    DiagLog _diaglog;

    bool useCursors = true;
    bool useHints = true;
    
    void flushOpLog( stringstream &ss ) {
        if( _diaglog.f && _diaglog.f->is_open() ) {
            ss << "flushing op log and files\n";
            _diaglog.flush();
        }
    }

    int ctr = 0;

    KillCurrentOp killCurrentOp;
    
    int lockFile = 0;

    // see FSyncCommand:
    unsigned lockedForWriting; 
    mongo::mutex lockedForWritingMutex("lockedForWriting");
    bool unlockRequested = false;

    void inProgCmd( Message &m, DbResponse &dbresponse ) {
        BSONObjBuilder b;

        if( ! cc().isAdmin() ){
            BSONObjBuilder b;
            b.append("err", "unauthorized");
        }
        else {
            DbMessage d(m);
            QueryMessage q(d);
            bool all = q.query["$all"].trueValue();
            vector<BSONObj> vals;
            {
                Client& me = cc();
                scoped_lock bl(Client::clientsMutex);
                for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) { 
                    Client *c = *i;
                    assert( c );
                    if ( c == &me )
                        continue;
                    CurOp* co = c->curop();
                    assert( co );
                    if( all || co->active() )
                        vals.push_back( co->infoNoauth() );
                }
            }
            b.append("inprog", vals);
            unsigned x = lockedForWriting;
            if( x ) {
                b.append("fsyncLock", x);
                b.append("info", "use db.$cmd.sys.unlock.findOne() to terminate the fsync write/snapshot lock");
            }
        }
        
        replyToQuery(0, m, dbresponse, b.obj());
    }
    
    void killOp( Message &m, DbResponse &dbresponse ) {
        BSONObj obj;
        if( ! cc().isAdmin() ){
            obj = fromjson("{\"err\":\"unauthorized\"}");
        }
        /*else if( !dbMutexInfo.isLocked() ) 
            obj = fromjson("{\"info\":\"no op in progress/not locked\"}");
            */
        else {
            DbMessage d(m);
            QueryMessage q(d);
            BSONElement e = q.query.getField("op");
            if( !e.isNumber() ) { 
                obj = fromjson("{\"err\":\"no op number field specified?\"}");
            }
            else { 
                log() << "going to kill op: " << e << endl;
                obj = fromjson("{\"info\":\"attempting to kill op\"}");
                killCurrentOp.kill( (unsigned) e.number() );
            }
        }
        replyToQuery(0, m, dbresponse, obj);
    }

    void unlockFsync(const char *ns, Message& m, DbResponse &dbresponse) {
        BSONObj obj;
        if( ! cc().isAdmin() || strncmp(ns, "admin.", 6) != 0 ) { 
            obj = fromjson("{\"err\":\"unauthorized\"}");
        }
        else {
            if( lockedForWriting ) { 
				log() << "command: unlock requested" << endl;
                obj = fromjson("{ok:1,\"info\":\"unlock requested\"}");
                unlockRequested = true;
            }
            else { 
                obj = fromjson("{ok:0,\"errmsg\":\"not locked\"}");
            }
        }
        replyToQuery(0, m, dbresponse, obj);
    }

    static bool receivedQuery(Client& c, DbResponse& dbresponse, Message& m ){
        bool ok = true;
        MSGID responseTo = m.header()->id;

        DbMessage d(m);
        QueryMessage q(d);
        auto_ptr< Message > resp( new Message() );

        CurOp& op = *(c.curop());
        
        try {
            dbresponse.exhaust = runQuery(m, q, op, *resp);
            assert( !resp->empty() );
        }
        catch ( AssertionException& e ) {
            ok = false;
            op.debug().str << " exception ";
            LOGSOME { 
                log() << "assertion " << e.toString() << " ns:" << q.ns << " query:" <<
                    (q.query.valid() ? q.query.toString() : "query object is corrupt") << endl;
                if( q.ntoskip || q.ntoreturn )
                    log() << " ntoskip:" << q.ntoskip << " ntoreturn:" << q.ntoreturn << endl;
            }

            BSONObjBuilder err;
            e.getInfo().append( err );
            BSONObj errObj = err.done();

            BufBuilder b;
            b.skip(sizeof(QueryResult));
            b.appendBuf((void*) errObj.objdata(), errObj.objsize());

            // todo: call replyToQuery() from here instead of this!!! see dbmessage.h
            QueryResult * msgdata = (QueryResult *) b.buf();
            b.decouple();
            QueryResult *qr = msgdata;
            qr->_resultFlags() = ResultFlag_ErrSet;
            if ( e.getCode() == StaleConfigInContextCode )
                qr->_resultFlags() |= ResultFlag_ShardConfigStale;
            qr->len = b.len();
            qr->setOperation(opReply);
            qr->cursorId = 0;
            qr->startingFrom = 0;
            qr->nReturned = 1;
            resp.reset( new Message() );
            resp->setData( msgdata, true );
        }

        if ( op.shouldDBProfile( 0 ) ){
            op.debug().str << " bytes:" << resp->header()->dataLen();
        }
        
        dbresponse.response = resp.release();
        dbresponse.responseTo = responseTo;
        
        return ok;
    }

    // Returns false when request includes 'end'
    bool assembleResponse( Message &m, DbResponse &dbresponse, const SockAddr &client ) {

        // before we lock...
        int op = m.operation();
        bool isCommand = false;
        const char *ns = m.singleData()->_data + 4;
        if ( op == dbQuery ) {
            if( strstr(ns, ".$cmd") ) {
                isCommand = true;
                opwrite(m);
                if( strstr(ns, ".$cmd.sys.") ) { 
                    if( strstr(ns, "$cmd.sys.inprog") ) {
                        inProgCmd(m, dbresponse);
                        return true;
                    }
                    if( strstr(ns, "$cmd.sys.killop") ) { 
                        killOp(m, dbresponse);
                        return true;
                    }
                    if( strstr(ns, "$cmd.sys.unlock") ) { 
                        unlockFsync(ns, m, dbresponse);
                        return true;
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
        
        globalOpCounters.gotOp( op , isCommand );
        
        Client& c = cc();
        
        auto_ptr<CurOp> nestedOp;
        CurOp* currentOpP = c.curop();
        if ( currentOpP->active() ){
            nestedOp.reset( new CurOp( &c , currentOpP ) );
            currentOpP = nestedOp.get();
        }
        CurOp& currentOp = *currentOpP;
        currentOp.reset(client,op);
        
        OpDebug& debug = currentOp.debug();
        StringBuilder& ss = debug.str;
        ss << opToString( op ) << " ";

        int logThreshold = cmdLine.slowMS;
        bool log = logLevel >= 1;
        
        if ( op == dbQuery ) {
            if ( handlePossibleShardedMessage( m , &dbresponse ) )
                return true;
            receivedQuery(c , dbresponse, m );
        }
        else if ( op == dbGetMore ) {
            if ( ! receivedGetMore(dbresponse, m, currentOp) )
                log = true;
        }
        else if ( op == dbMsg ) {
            // deprecated - replaced by commands
            char *p = m.singleData()->_data;
            int len = strlen(p);
            if ( len > 400 )
                out() << curTimeMillis() % 10000 <<
                    " long msg received, len:" << len << endl;

            Message *resp = new Message();
            if ( strcmp( "end" , p ) == 0 )
                resp->setData( opReply , "dbMsg end no longer supported" );
            else
                resp->setData( opReply , "i am fine - dbMsg deprecated");

            dbresponse.response = resp;
            dbresponse.responseTo = m.header()->id;
        }
        else {
            const char *ns = m.singleData()->_data + 4;
            char cl[256];
            nsToDatabase(ns, cl);
            if( ! c.getAuthenticationInfo()->isAuthorized(cl) ) { 
                uassert_nothrow("unauthorized");
            }
            else {
                try {
                    if ( op == dbInsert ) {
                        receivedInsert(m, currentOp);
                    }
                    else if ( op == dbUpdate ) {
                        receivedUpdate(m, currentOp);
                    }
                    else if ( op == dbDelete ) {
                        receivedDelete(m, currentOp);
                    }
                    else if ( op == dbKillCursors ) {
                        currentOp.ensureStarted();
                        logThreshold = 10;
                        ss << "killcursors ";
                        receivedKillCursors(m);
                    }
                    else {
                        mongo::log() << "    operation isn't supported: " << op << endl;
                        currentOp.done();
                        log = true;
                    }
                }
                catch ( AssertionException& e ) {
                    static int n;
                    tlog(3) << " Caught Assertion in " << opToString(op) << ", continuing" << endl;
                    ss << " exception " + e.toString();
                    log = ++n < 10;
                }
            }
        }
        currentOp.ensureStarted();
        currentOp.done();
        int ms = currentOp.totalTimeMillis();
        
        log = log || (logLevel >= 2 && ++ctr % 512 == 0);
        //DEV log = true; 
        if ( log || ms > logThreshold ) {
            if( logLevel < 3 && op == dbGetMore && strstr(ns, ".oplog.") && ms < 3000 && !log ) {
                /* it's normal for getMore on the oplog to be slow because of use of awaitdata flag. */
            } else {
                ss << ' ' << ms << "ms";
                mongo::tlog() << ss.str() << endl;
            }
        }
        
        if ( currentOp.shouldDBProfile( ms ) ){
            // performance profiling is on
            if ( dbMutex.getState() < 0 ){
                mongo::log(1) << "note: not profiling because recursive read lock" << endl;
            }
            else {
                mongolock lk(true);
                if ( dbHolder.isLoaded( nsToDatabase( currentOp.getNS() ) , dbpath ) ){
                    Client::Context c( currentOp.getNS() );
                    profile(ss.str().c_str(), ms);
                }
                else {
                    mongo::log() << "note: not profiling because db went away - probably a close on: " << currentOp.getNS() << endl;
                }
            }
        }

        return true;
    } /* assembleResponse() */

    void killCursors(int n, long long *ids);
    void receivedKillCursors(Message& m) {
        int *x = (int *) m.singleData()->_data;
        x++; // reserved
        int n = *x++;
        uassert( 13004 , "sent 0 cursors to kill" , n >= 1 );
        if ( n > 2000 ) {
            log( n < 30000 ? LL_WARNING : LL_ERROR ) << "receivedKillCursors, n=" << n << endl;
            assert( n < 30000 );
        }
        killCursors(n, (long long *) x);
    }

    /* db - database name
       path - db directory
    */
    /*static*/ void Database::closeDatabase( const char *db, const string& path ) {
        assertInWriteLock();
        
        Client::Context * ctx = cc().getContext();
        assert( ctx );
        assert( ctx->inDB( db , path ) );
        Database *database = ctx->db();
        assert( database->name == db );
        
        oplogCheckCloseDatabase( database ); // oplog caches some things, dirty its caches

        if( BackgroundOperation::inProgForDb(db) ) { 
            log() << "warning: bg op in prog during close db? " << db << endl;
        }

        /* important: kill all open cursors on the database */
        string prefix(db);
        prefix += '.';
        ClientCursor::invalidate(prefix.c_str());

        NamespaceDetailsTransient::clearForPrefix( prefix.c_str() );

        dbHolder.erase( db, path );
        ctx->clear();
        delete database; // closes files
    }

    void receivedUpdate(Message& m, CurOp& op) {
        DbMessage d(m);
        const char *ns = d.getns();
        assert(*ns);
        uassert( 10054 ,  "not master", isMasterNs( ns ) );
        op.debug().str << ns << ' ';
        int flags = d.pullInt();
        BSONObj query = d.nextJsObj();

        assert( d.moreJSObjs() );
        assert( query.objsize() < m.header()->dataLen() );
        BSONObj toupdate = d.nextJsObj();
        uassert( 10055 , "update object too large", toupdate.objsize() <= MaxBSONObjectSize);
        assert( toupdate.objsize() < m.header()->dataLen() );
        assert( query.objsize() + toupdate.objsize() < m.header()->dataLen() );
        bool upsert = flags & UpdateOption_Upsert;
        bool multi = flags & UpdateOption_Multi;
        bool broadcast = flags & UpdateOption_Broadcast;
        {
            string s = query.toString();
            /* todo: we shouldn't do all this ss stuff when we don't need it, it will slow us down. 
               instead, let's just story the query BSON in the debug object, and it can toString() 
               lazily
            */
            op.debug().str << " query: " << s;
            op.setQuery(query);
        }        

        mongolock lk(1);

        if ( ! broadcast && handlePossibleShardedMessage( m , 0 ) )
            return;

        Client::Context ctx( ns );

        UpdateResult res = updateObjects(ns, toupdate, query, upsert, multi, true, op.debug() );
        lastError.getSafe()->recordUpdate( res.existing , res.num , res.upserted ); // for getlasterror
    }

    void receivedDelete(Message& m, CurOp& op) {
        DbMessage d(m);
        const char *ns = d.getns();
        assert(*ns);
        uassert( 10056 ,  "not master", isMasterNs( ns ) );
        op.debug().str << ns << ' ';
        int flags = d.pullInt();
        bool justOne = flags & RemoveOption_JustOne;
        bool broadcast = flags & RemoveOption_Broadcast;
        assert( d.moreJSObjs() );
        BSONObj pattern = d.nextJsObj();
        {
            string s = pattern.toString();
            op.debug().str << " query: " << s;
            op.setQuery(pattern);
        }        

        writelock lk(ns);
        if ( ! broadcast & handlePossibleShardedMessage( m , 0 ) )
            return;
        Client::Context ctx(ns);
        
        long long n = deleteObjects(ns, pattern, justOne, true);
        lastError.getSafe()->recordDelete( n );
    }
    
    QueryResult* emptyMoreResult(long long);

    bool receivedGetMore(DbResponse& dbresponse, Message& m, CurOp& curop ) {
        StringBuilder& ss = curop.debug().str;
        bool ok = true;
        
        DbMessage d(m);

        const char *ns = d.getns();
        int ntoreturn = d.pullInt();
        long long cursorid = d.pullInt64();
        
        ss << ns << " cid:" << cursorid;
        if( ntoreturn ) 
            ss << " ntoreturn:" << ntoreturn;

		time_t start = 0;
        int pass = 0;        
        bool exhaust = false;
        QueryResult* msgdata;
        while( 1 ) {
            try {
                mongolock lk(false);
                Client::Context ctx(ns);
                msgdata = processGetMore(ns, ntoreturn, cursorid, curop, pass, exhaust);
            }
            catch ( GetMoreWaitException& ) { 
                exhaust = false;
                massert(13073, "shutting down", !inShutdown() );
				if( pass == 0 ) { 
  				    start = time(0);
				}
				else { 
				  if( time(0) - start >= 4 ) {
					// after about 4 seconds, return.  this is a sanity check.  pass stops at 1000 normally 
					// for DEV this helps and also if sleep is highly inaccurate on a platform.  we want to 
					// return occasionally so slave can checkpoint.
					pass = 10000;
				  }
				}
                pass++;
                DEV 
                    sleepmillis(20);
                else 
                    sleepmillis(2);
                continue;
            }
            catch ( AssertionException& e ) {
                exhaust = false;
                ss << " exception " << e.toString();
                msgdata = emptyMoreResult(cursorid);
                ok = false;
            }
            break;
        };

        Message *resp = new Message();
        resp->setData(msgdata, true);
        ss << " bytes:" << resp->header()->dataLen();
        ss << " nreturned:" << msgdata->nReturned;
        dbresponse.response = resp;
        dbresponse.responseTo = m.header()->id;
        if( exhaust ) { 
            ss << " exhaust "; 
            dbresponse.exhaust = ns;
        }
        return ok;
    }

    void receivedInsert(Message& m, CurOp& op) {
        DbMessage d(m);
		const char *ns = d.getns();
		assert(*ns);
        uassert( 10058 ,  "not master", isMasterNs( ns ) );
        op.debug().str << ns;

        writelock lk(ns);

        if ( handlePossibleShardedMessage( m , 0 ) )
            return;

        Client::Context ctx(ns);		
        while ( d.moreJSObjs() ) {
            BSONObj js = d.nextJsObj();
            uassert( 10059 , "object to insert too large", js.objsize() <= MaxBSONObjectSize);
            theDataFileMgr.insertWithObjMod(ns, js, false);
            logOp("i", ns, js);
            globalOpCounters.gotInsert();
        }
    }

    class JniMessagingPort : public AbstractMessagingPort {
    public:
        JniMessagingPort(Message& _container) : container(_container) { }
        void reply(Message& received, Message& response, MSGID) {
            container = response;
        }
        void reply(Message& received, Message& response) {
            container = response;
        }
        unsigned remotePort(){
            return 1;
        }
        Message & container;
    };
    
    void getDatabaseNames( vector< string > &names , const string& usePath ) {
        boost::filesystem::path path( usePath );
        for ( boost::filesystem::directory_iterator i( path );
                i != boost::filesystem::directory_iterator(); ++i ) {
            if ( directoryperdb ) {
                boost::filesystem::path p = *i;
                string dbName = p.leaf();
                p /= ( dbName + ".ns" );
                if ( MMF::exists( p ) )
                    names.push_back( dbName );
            } else {
                string fileName = boost::filesystem::path(*i).leaf();
                if ( fileName.length() > 3 && fileName.substr( fileName.length() - 3, 3 ) == ".ns" )
                    names.push_back( fileName.substr( 0, fileName.length() - 3 ) );
            }
        }
    }

    /* returns true if there is data on this server.  useful when starting replication. 
       local database does NOT count except for rsoplog collection.
    */
    bool replHasDatabases() { 
        vector<string> names;
        getDatabaseNames(names);
        if( names.size() >= 2 ) return true;
        if( names.size() == 1 ){
            if( names[0] != "local" )
                return true;
            // we have a local database.  return true if oplog isn't empty
            {
                readlock lk(rsoplog);
                BSONObj o;
                if( Helpers::getFirst(rsoplog, o) )
                    return true;
            }
        }
        return false;
    }

    bool DBDirectClient::call( Message &toSend, Message &response, bool assertOk ) {
        if ( lastError._get() )
            lastError.startRequest( toSend, lastError._get() );
        DbResponse dbResponse;
        assembleResponse( toSend, dbResponse );
        assert( dbResponse.response );
        dbResponse.response->concat(); // can get rid of this if we make response handling smarter
        response = *dbResponse.response;
        return true;
    }

    void DBDirectClient::say( Message &toSend ) {
        if ( lastError._get() )
            lastError.startRequest( toSend, lastError._get() );
        DbResponse dbResponse;
        assembleResponse( toSend, dbResponse );
    }

    auto_ptr<DBClientCursor> DBDirectClient::query(const string &ns, Query query, int nToReturn , int nToSkip ,
                                                   const BSONObj *fieldsToReturn , int queryOptions ){
        
        //if ( ! query.obj.isEmpty() || nToReturn != 0 || nToSkip != 0 || fieldsToReturn || queryOptions )
        return DBClientBase::query( ns , query , nToReturn , nToSkip , fieldsToReturn , queryOptions );
        //
        //assert( query.obj.isEmpty() );
        //throw UserException( (string)"yay:" + ns );
    }

    void DBDirectClient::killCursor( long long id ){
        ClientCursor::erase( id );
    }

    DBClientBase * createDirectClient(){
        return new DBDirectClient();
    }

    //void recCacheCloseAll();

    mongo::mutex exitMutex("exit");
    int numExitCalls = 0;
    void shutdown();

    bool inShutdown(){
        return numExitCalls > 0;
    }

    void tryToOutputFatal( const string& s ){
        try {
            rawOut( s );
            return;
        }
        catch ( ... ){}

        try {
            cerr << s << endl;
            return;
        }
        catch ( ... ){}
        
        // uh - oh, not sure there is anything else we can do...
    }

    /* not using log() herein in case we are already locked */
    void dbexit( ExitCode rc, const char *why) {        
        Client * c = currentClient.get();
        {
            scoped_lock lk( exitMutex );
            if ( numExitCalls++ > 0 ) {
                if ( numExitCalls > 5 ){
                    // this means something horrible has happened
                    ::_exit( rc );
                }
                stringstream ss;
                ss << "dbexit: " << why << "; exiting immediately";
                tryToOutputFatal( ss.str() );
                if ( c ) c->shutdown();
                ::exit( rc );                
            }
        }
        
        {
            stringstream ss;
            ss << "dbexit: " << why;
            tryToOutputFatal( ss.str() );
        }
        
        try {
            shutdown(); // gracefully shutdown instance
        }
        catch ( ... ){
            tryToOutputFatal( "shutdown failed with exception" );
        }

        try { 
            mutexDebugger.programEnding();
        }
        catch (...) { }
        
        tryToOutputFatal( "dbexit: really exiting now\n" );
        if ( c ) c->shutdown();
        ::exit(rc);
    }
    
    void shutdown() {

        log() << "shutdown: going to close listening sockets..." << endl;        
        ListeningSockets::get()->closeAll();

        log() << "shutdown: going to flush oplog..." << endl;
        stringstream ss2;
        flushOpLog( ss2 );
        rawOut( ss2.str() );

        /* must do this before unmapping mem or you may get a seg fault */
        log() << "shutdown: going to close sockets..." << endl;
        boost::thread close_socket_thread( boost::bind(MessagingPort::closeAllSockets, 0) );

        // wait until file preallocation finishes
        // we would only hang here if the file_allocator code generates a
        // synchronous signal, which we don't expect
        log() << "shutdown: waiting for fs preallocator..." << endl;
        theFileAllocator().waitUntilFinished();
        
        log() << "shutdown: closing all files..." << endl;
        stringstream ss3;
        MemoryMappedFile::closeAllFiles( ss3 );
        rawOut( ss3.str() );

        // should we be locked here?  we aren't. might be ok as-is.
        //recCacheCloseAll();
        
#if !defined(_WIN32) && !defined(__sunos__)
        if ( lockFile ){
            log() << "shutdown: removing fs lock..." << endl;
            if( ftruncate( lockFile , 0 ) ) 
                log() << "couldn't remove fs lock " << errnoWithDescription() << endl;
            flock( lockFile, LOCK_UN );
        }
#endif
    }

#if !defined(_WIN32) && !defined(__sunos__)
    void writePid(int fd) {
        stringstream ss;
        ss << getpid() << endl;
        string s = ss.str();
        const char * data = s.c_str();
        assert ( write( fd, data, strlen( data ) ) );
    }

    void acquirePathLock() {
      string name = ( boost::filesystem::path( dbpath ) / "mongod.lock" ).native_file_string();

        bool oldFile = false;

        if ( boost::filesystem::exists( name ) && boost::filesystem::file_size( name ) > 0 ) {
            oldFile = true;
        }

        lockFile = open( name.c_str(), O_RDWR | O_CREAT , S_IRWXU | S_IRWXG | S_IRWXO );
		if( lockFile <= 0 ) {
		    uasserted( 10309 , str::stream() << "Unable to create / open lock file for lockfilepath: " << name << ' ' << errnoWithDescription());
        }
        if (flock( lockFile, LOCK_EX | LOCK_NB ) != 0) {
            close ( lockFile );
            lockFile = 0;
            uassert( 10310 ,  "Unable to acquire lock for lockfilepath: " + name,  0 );
        }

        if ( oldFile ){
            // we check this here because we want to see if we can get the lock
            // if we can't, then its probably just another mongod running
            cout << "************** \n" 
                 << "old lock file: " << name << ".  probably means unclean shutdown\n"
                 << "recommend removing file and running --repair\n" 
                 << "see: http://dochub.mongodb.org/core/repair for more information\n"
                 << "*************" << endl;
            close ( lockFile );
            lockFile = 0;
            uassert( 12596 , "old lock file" , 0 );
        }

        uassert( 13342, "Unable to truncate lock file", ftruncate(lockFile, 0) == 0);
        writePid( lockFile );
        fsync( lockFile );
    }
#else
    void acquirePathLock() {
        // TODO - this is very bad
    }
#endif    
    
} // namespace mongo
