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

#include "stdafx.h"
#include "db.h"
#include "query.h"
#include "introspect.h"
#include "repl.h"
#include "dbmessage.h"
#include "instance.h"
#include "lasterror.h"
#include "security.h"
#include "json.h"
#include "reccache.h"
#include "replset.h"
#include "../s/d_logic.h"
#include "../util/file_allocator.h"
#include "cmdline.h"
#if !defined(_WIN32)
#include <sys/file.h>
#endif

namespace mongo {

    CmdLine cmdLine;

    int nloggedsome = 0;
#define LOGSOME if( ++nloggedsome < 1000 || nloggedsome % 100 == 0 )

    SlaveTypes slave = NotSlave;
    bool master = false; // true means keep an op log
//    extern int curOp;
    bool autoresync = false;
    
    /* we use new here so we don't have to worry about destructor orders at program shutdown */
    MongoMutex &dbMutex( *(new MongoMutex) );
    MutexInfo dbMutexInfo;

    string dbExecCommand;

    string bind_ip = "";

    char *appsrvPath = null;

    DiagLog _diaglog;

    int opIdMem = 100000000;

    bool useCursors = true;
    bool useHints = true;
    
    void closeAllSockets();
    void flushOpLog( stringstream &ss ) {
        if( _diaglog.f && _diaglog.f->is_open() ) {
            ss << "flushing op log and files\n";
            _diaglog.flush();
        }
    }

    int ctr = 0;

    /* 0 = ok
       1 = kill current operation and reset this to 0
       future: maybe use this as a "going away" thing on process termination with a higher flag value 
    */
    int killCurrentOp = 0;
    
    int lockFile = 0;

    void inProgCmd( Message &m, DbResponse &dbresponse ) {
        BSONObjBuilder b;

        AuthenticationInfo *ai = cc().ai;
        if( !ai->isAuthorized("admin") ) { 
            BSONObjBuilder b;
            b.append("err", "unauthorized");
        }
        else {
            vector<BSONObj> vals;
            {
                boostlock bl(Client::clientsMutex);
                for( set<Client*>::iterator i = Client::clients.begin(); i != Client::clients.end(); i++ ) { 
                    Client *c = *i;
                    CurOp& co = *(c->curop());
                    if( co.active )
                        vals.push_back( co.infoNoauth() );
                }
            }
            b.append("inprog", vals);
        }

        replyToQuery(0, m, dbresponse, b.obj());
    }
    
    void killOp( Message &m, DbResponse &dbresponse ) {
        BSONObj obj;
        AuthenticationInfo *ai = currentClient.get()->ai;
        if( !ai->isAuthorized("admin") ) { 
            obj = fromjson("{\"err\":\"unauthorized\"}");
        }
        else if( !dbMutexInfo.isLocked() ) 
            obj = fromjson("{\"info\":\"no op in progress/not locked\"}");
        else {
            killCurrentOp = 1;
            obj = fromjson("{\"info\":\"attempting to kill op\"}");
        }
        replyToQuery(0, m, dbresponse, obj);
    }
    
    // Returns false when request includes 'end'
    bool assembleResponse( Message &m, DbResponse &dbresponse, const sockaddr_in &client ) {
        bool writeLock = true;

        // before we lock...
        int op = m.data->operation();
        if ( op == dbQuery ) {
            const char *ns = m.data->_data + 4;
            if( strstr(ns, "$cmd.sys.") ) { 
                if( strstr(ns, "$cmd.sys.inprog") ) {
                    inProgCmd(m, dbresponse);
                    return true;
                }
                if( strstr(ns, "$cmd.sys.killop") ) { 
                    killOp(m, dbresponse);
                    return true;
                }
            }
        }
        else if( op == dbGetMore ) {
            writeLock = false;
        }
        
        if ( handlePossibleShardedMessage( m , dbresponse ) ){
            /* important to do this before we lock
               so if a message has to be forwarded, doesn't block for that
            */
            return true;
        }

        mongolock lk(writeLock);
        
        stringstream ss;
        char buf[64];
        time_t now = time(0);
        CurOp& currentOp = *cc().curop();
        currentOp.reset(now, client);

        time_t_to_String(now, buf);
        buf[20] = 0; // don't want the year
        ss << buf;

        Timer t;
        Client& c = cc();
        c.clearns();

        int logThreshold = 100;
        int ms;
        bool log = logLevel >= 1;
        c.curop()->op = op;

#if 0
        /* use this if you only want to process operations for a particular namespace.
         maybe add to cmd line parms or something fancier.
         */
        DbMessage ddd(m);
        if ( strncmp(ddd.getns(), "clusterstock", 12) != 0 ) {
            static int q;
            if ( ++q < 20 )
                out() << "TEMP skip " << ddd.getns() << endl;
            goto skip;
        }
#endif

        if ( op == dbQuery ) {
            // receivedQuery() does its own authorization processing.
            if ( ! receivedQuery(dbresponse, m, ss, true) )
                log = true;
        }
        else if ( op == dbGetMore ) {
            // receivedQuery() does its own authorization processing.
            OPREAD;
            DEV log = true;
            ss << "getmore ";
            if ( ! receivedGetMore(dbresponse, m, ss) )
                log = true;
        }
        else if ( op == dbMsg ) {
			/* deprecated / rarely used.  intended for connection diagnostics. */
            ss << "msg ";
            char *p = m.data->_data;
            int len = strlen(p);
            if ( len > 400 )
                out() << curTimeMillis() % 10000 <<
                     " long msg received, len:" << len <<
                     " ends with: " << p + len - 10 << endl;
            bool end = false; //strcmp("end", p) == 0;
            Message *resp = new Message();
            resp->setData(opReply, "i am fine");
            dbresponse.response = resp;
            dbresponse.responseTo = m.data->id;
            //dbMsgPort.reply(m, resp);
            if ( end )
                return false;
        }
        else {
            const char *ns = m.data->_data + 4;
            char cl[256];
            nsToClient(ns, cl);
            strncpy(currentOp.ns, ns, Namespace::MaxNsLen);
            AuthenticationInfo *ai = currentClient.get()->ai;
            if( !ai->isAuthorized(cl) ) { 
                uassert_nothrow("unauthorized");
            }
            else if ( op == dbInsert ) {
                OPWRITE;
                try {
                    ss << "insert ";
                    receivedInsert(m, ss);
                }
                catch ( AssertionException& e ) {
                    LOGSOME problem() << " Caught Assertion insert, continuing\n";
                    ss << " exception " + e.toString();
                    log = true;
                }
            }
            else if ( op == dbUpdate ) {
                OPWRITE;
                try {
                    ss << "update ";
                    receivedUpdate(m, ss);
                }
                catch ( AssertionException& e ) {
                    LOGSOME problem() << " Caught Assertion update, continuing" << endl;
                    ss << " exception " + e.toString();
                    log = true;
                }
            }
            else if ( op == dbDelete ) {
                OPWRITE;
                try {
                    ss << "remove ";
                    receivedDelete(m, ss);
                }
                catch ( AssertionException& e ) {
                    LOGSOME problem() << " Caught Assertion receivedDelete, continuing" << endl;
                    ss << " exception " + e.toString();
                    log = true;
                }
            }
            else if ( op == dbKillCursors ) {
                OPREAD;
                try {
                    logThreshold = 10;
                    ss << "killcursors ";
                    receivedKillCursors(m);
                }
                catch ( AssertionException& e ) {
                    problem() << " Caught Assertion in kill cursors, continuing" << endl;
                    ss << " exception " + e.toString();
                    log = true;
                }
            }
            else {
                out() << "    operation isn't supported: " << op << endl;
                currentOp.active = false;
                assert(false);
            }
        }
        ms = t.millis();
        log = log || (logLevel >= 2 && ++ctr % 512 == 0);
        DEV log = true;
        if ( log || ms > logThreshold ) {
            ss << ' ' << t.millis() << "ms";
            out() << ss.str().c_str() << endl;
        }
        Database *database = cc().database();
        if ( database && database->profile >= 1 ) {
            if ( database->profile >= 2 || ms >= 100 ) {
                // performance profiling is on
                lk.releaseAndWriteLock();
                profile(ss.str().c_str()+20/*skip ts*/, ms);
            }
        }

        currentOp.active = false;
        return true;
    }

    void killCursors(int n, long long *ids);
    void receivedKillCursors(Message& m) {
        int *x = (int *) m.data->_data;
        x++; // reserved
        int n = *x++;
        assert( n >= 1 );
        if ( n > 2000 ) {
            problem() << "Assertion failure, receivedKillCursors, n=" << n << endl;
            assert( n < 30000 );
        }
        killCursors(n, (long long *) x);
    }

    /* cl - database name
       path - db directory
    */
    void closeClient( const char *cl, const string& path ) {
        Database *database = cc().database();
        assert( database );
        assert( database->name == cl );
        if ( string("local") != cl ) {
            DBInfo i(cl);
            i.dbDropped();
        }

        /* important: kill all open cursors on the database */
        string prefix(cl);
        prefix += '.';
        ClientCursor::invalidate(prefix.c_str());

        NamespaceDetailsTransient::drop( prefix.c_str() );

        eraseDatabase( cl, path );
        delete database; // closes files
        cc().clearns();
    }

    void receivedUpdate(Message& m, stringstream& ss) {
        DbMessage d(m);
        const char *ns = d.getns();
        assert(*ns);
        uassert( "not master", isMasterNs( ns ) );
        setClient(ns);
        Client& client = cc();
        client.top.setWrite();
        ss << ns << ' ';
        int flags = d.pullInt();
        BSONObj query = d.nextJsObj();

        assert( d.moreJSObjs() );
        assert( query.objsize() < m.data->dataLen() );
        BSONObj toupdate = d.nextJsObj();
        uassert("update object too large", toupdate.objsize() <= MaxBSONObjectSize);
        assert( toupdate.objsize() < m.data->dataLen() );
        assert( query.objsize() + toupdate.objsize() < m.data->dataLen() );
        bool upsert = flags & Option_Upsert;
        bool multi = flags & Option_Multi;
        {
            string s = query.toString();
            /* todo: we shouldn't do all this ss stuff when we don't need it, it will slow us down. */
            ss << " query: " << s;
            CurOp& currentOp = *client.curop();
            strncpy(currentOp.query, s.c_str(), sizeof(currentOp.query)-2);
        }        
        UpdateResult res = updateObjects(ns, toupdate, query, upsert, multi, ss, true);
        /* TODO FIX: recordUpdate should take a long int for parm #2 */
        recordUpdate( res.existing , (int) res.num ); // for getlasterror
    }

    void receivedDelete(Message& m, stringstream &ss) {
        DbMessage d(m);
        const char *ns = d.getns();
        assert(*ns);
        uassert( "not master", isMasterNs( ns ) );
        setClient(ns);
        Client& client = cc();
        client.top.setWrite();
        int flags = d.pullInt();
        bool justOne = flags & 1;
        assert( d.moreJSObjs() );
        BSONObj pattern = d.nextJsObj();
        {
            string s = pattern.toString();
            ss << " query: " << s;
            CurOp& currentOp = *client.curop();
            strncpy(currentOp.query, s.c_str(), sizeof(currentOp.query)-2);
        }        
        int n = deleteObjects(ns, pattern, justOne, true);
        recordDelete( n );
    }

    /**
     * @return if this was successful
     */
    bool receivedQuery(DbResponse& dbresponse, /*AbstractMessagingPort& dbMsgPort, */Message& m, stringstream& ss, bool logit) {
        bool ok = true;
        MSGID responseTo = m.data->id;

        DbMessage d(m);
        QueryMessage q(d);
        QueryResult* msgdata;
        
        try {
            if (q.fields.get() && q.fields->errmsg)
                uassert(q.fields->errmsg, false);

            /* note these are logged BEFORE authentication -- which is sort of ok */
            if ( _diaglog.level && logit ) {
                if ( strstr(q.ns, ".$cmd") ) {
                    /* $cmd queries are "commands" and usually best treated as write operations */
                    OPWRITE;
                }
                else {
                    OPREAD;
                }
            }

            setClient( q.ns );
            Client& client = cc();
            client.top.setRead();
            strncpy(client.curop()->ns, q.ns, Namespace::MaxNsLen);
            msgdata = runQuery(m, ss ).release();
        }
        catch ( AssertionException& e ) {
            ok = false;
            ss << " exception ";
            LOGSOME problem() << " Caught Assertion in runQuery ns:" << q.ns << ' ' << e.toString() << '\n';
            log() << "  ntoskip:" << q.ntoskip << " ntoreturn:" << q.ntoreturn << '\n';
            if ( q.query.valid() )
                log() << "  query:" << q.query.toString() << endl;
            else
                log() << "  query object is not valid!" << endl;

            BSONObjBuilder err;
            err.append("$err", e.msg.empty() ? "assertion during query" : e.msg);
            BSONObj errObj = err.done();

            BufBuilder b;
            b.skip(sizeof(QueryResult));
            b.append((void*) errObj.objdata(), errObj.objsize());

            // todo: call replyToQuery() from here instead of this!!! see dbmessage.h
            msgdata = (QueryResult *) b.buf();
            b.decouple();
            QueryResult *qr = msgdata;
            qr->resultFlags() = QueryResult::ResultFlag_ErrSet;
            qr->len = b.len();
            qr->setOperation(opReply);
            qr->cursorId = 0;
            qr->startingFrom = 0;
            qr->nReturned = 1;

        }
        Message *resp = new Message();
        resp->setData(msgdata, true); // transport will free
        dbresponse.response = resp;
        dbresponse.responseTo = responseTo;
        Database *database = cc().database();
        if ( database ) {
            if ( database->profile )
                ss << " bytes:" << resp->data->dataLen();
        }
        else {
            if ( strstr(q.ns, "$cmd") == 0 ) // (this condition is normal for $cmd dropDatabase)
                log() << "ERROR: receiveQuery: database is null; ns=" << q.ns << endl;
        }
        
        return ok;
    }
    
    QueryResult* emptyMoreResult(long long);

    bool receivedGetMore(DbResponse& dbresponse, /*AbstractMessagingPort& dbMsgPort, */Message& m, stringstream& ss) {
        bool ok = true;
        DbMessage d(m);
        const char *ns = d.getns();
        ss << ns;
        setClient(ns);
        cc().top.setRead();
        int ntoreturn = d.pullInt();
        long long cursorid = d.pullInt64();
        ss << " cid:" << cursorid;
        ss << " ntoreturn:" << ntoreturn;
        QueryResult* msgdata;
        try {
            AuthenticationInfo *ai = currentClient.get()->ai;
            uassert("unauthorized", ai->isAuthorized(cc().database()->name.c_str()));
            msgdata = getMore(ns, ntoreturn, cursorid, ss);
        }
        catch ( AssertionException& e ) {
            ss << " exception " + e.toString();
            msgdata = emptyMoreResult(cursorid);
            ok = false;
        }
        Message *resp = new Message();
        resp->setData(msgdata, true);
        ss << " bytes:" << resp->data->dataLen();
        ss << " nreturned:" << msgdata->nReturned;
        dbresponse.response = resp;
        dbresponse.responseTo = m.data->id;
        //dbMsgPort.reply(m, resp);
        return ok;
    }

    void receivedInsert(Message& m, stringstream& ss) {
        DbMessage d(m);
		const char *ns = d.getns();
		assert(*ns);
        uassert( "not master", isMasterNs( ns ) );
		setClient(ns);
        cc().top.setWrite();
		ss << ns;
		
        while ( d.moreJSObjs() ) {
            BSONObj js = d.nextJsObj();
            uassert("object to insert too large", js.objsize() <= MaxBSONObjectSize);
            theDataFileMgr.insert(ns, js, false);
            logOp("i", ns, js);
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
    
    /* a call from jscript to the database locally.

         m - inbound message
         out - outbound message, if there is any, will be set here.
               if there is one, out.data will be non-null on return.
    		   The out.data buffer will automatically clean up when out
    		   goes out of scope (out.freeIt==true)

       note we should already be in the mutex lock from connThread() at this point.
    */
    /*
    void jniCallbackDeprecated(Message& m, Message& out)
    {
		//
        AuthenticationInfo *ai = currentClient.get()->ai;
        
        Database *clientOld = cc().database();

        JniMessagingPort jmp(out);
        callDepth++;
        int curOpOld = curOp;

        try {

            stringstream ss;
            char buf[64];
            time_t_to_String(time(0), buf);
            buf[20] = 0; // don't want the year
            ss << buf << " dbjs ";

            {
                Timer t;

                bool log = logLevel >= 1;
                curOp = m.data->operation();

                if ( m.data->operation() == dbQuery ) {
                    // on a query, the Message must have m.freeIt true so that the buffer data can be
                    // retained by cursors.  As freeIt is false, we make a copy here.
                    assert( m.data->len > 0 && m.data->len < 32000000 );
                    Message copy(malloc(m.data->len), true);
                    memcpy(copy.data, m.data, m.data->len);
                    DbResponse dbr;
                    receivedQuery(dbr, copy, ss, false);
                    jmp.reply(m, *dbr.response, dbr.responseTo);
                }
                else if ( m.data->operation() == dbInsert ) {
                    ss << "insert ";
                    receivedInsert(m, ss);
                }
                else if ( m.data->operation() == dbUpdate ) {
                    ss << "update ";
                    receivedUpdate(m, ss);
                }
                else if ( m.data->operation() == dbDelete ) {
                    ss << "remove ";
                    receivedDelete(m, ss);
                }
                else if ( m.data->operation() == dbGetMore ) {
                    DEV log = true;
                    ss << "getmore ";
                    DbResponse dbr;
                    receivedGetMore(dbr, m, ss);
                    jmp.reply(m, *dbr.response, dbr.responseTo);
                }
                else if ( m.data->operation() == dbKillCursors ) {
                    try {
                        log = true;
                        ss << "killcursors ";
                        receivedKillCursors(m);
                    }
                    catch ( AssertionException& ) {
                        problem() << "Caught Assertion in kill cursors, continuing" << endl;
                        ss << " exception ";
                    }
                }
                else {
                    mongo::out() << "    jnicall: operation isn't supported: " << m.data->operation() << endl;
                    assert(false);
                }

                int ms = t.millis();
                log = log || ctr++ % 128 == 0;
                if ( log || ms > 100 ) {
                    ss << ' ' << t.millis() << "ms";
                    mongo::out() << ss.str().c_str() << endl;
                }
                Database *database = cc().database();
                if ( database && database->profile >= 1 ) {
                    if ( database->profile >= 2 || ms >= 100 ) {
                        // profile it
                        profile(ss.str().c_str()+20, ms);
                    }
                }
            }

        }
        catch ( AssertionException& ) {
            problem() << "Caught AssertionException in jniCall()" << endl;
        }

        curOp = curOpOld;
        callDepth--;

        if ( cc().database() != clientOld ) {
            assert(false);
        }
    }
    */

    void getDatabaseNames( vector< string > &names ) {
        boost::filesystem::path path( dbpath );
        for ( boost::filesystem::directory_iterator i( path );
                i != boost::filesystem::directory_iterator(); ++i ) {
            string fileName = boost::filesystem::path(*i).leaf();
            if ( fileName.length() > 3 && fileName.substr( fileName.length() - 3, 3 ) == ".ns" )
                names.push_back( fileName.substr( 0, fileName.length() - 3 ) );
        }
    }

    bool DBDirectClient::call( Message &toSend, Message &response, bool assertOk ) {
        SavedContext c;
        DbResponse dbResponse;
        assembleResponse( toSend, dbResponse );
        assert( dbResponse.response );
        response = *dbResponse.response;
        return true;
    }

    void DBDirectClient::say( Message &toSend ) {
        SavedContext c;
        DbResponse dbResponse;
        assembleResponse( toSend, dbResponse );
    }

    DBDirectClient::AlwaysAuthorized DBDirectClient::SavedContext::always;

    DBClientBase * createDirectClient(){
        return new DBDirectClient();
    }

    void recCacheCloseAll();

    boost::mutex &listenerSocketMutex( *( new boost::mutex ) );
    vector< int > listenerSockets;
    void registerListenerSocket( int socket ) {
        boostlock lk( listenerSocketMutex );
        listenerSockets.push_back( socket );
    }
    
    boost::mutex &exitMutex( *( new boost::mutex ) );
    bool firstExit = true;
    void shutdown();

    bool inShutdown(){
        return ! firstExit;
    }

    /* not using log() herein in case we are already locked */
    void dbexit( ExitCode rc, const char *why) {        
        {
            boostlock lk( exitMutex );
            if ( !firstExit ) {
                stringstream ss;
                ss << "dbexit: " << why << "; exiting immediately" << endl;
                rawOut( ss.str() );
                ::exit( rc );                
            }
            firstExit = false;
        }
            
        stringstream ss;
        ss << "dbexit: " << why << endl;
        rawOut( ss.str() );

		shutdown(); // gracefully shutdown instance

        rawOut( "dbexit: really exiting now\n" );
        ::exit(rc);
    }
    
    void shutdown() {

#ifndef _WIN32
        {
            // close listener sockets
            // We would only hang here if a synchronous signal is received 
            // during a registerListenerSocket() call, which we don't expect.
            boostlock lk( listenerSocketMutex );
            for( vector< int >::iterator i = listenerSockets.begin(); i != listenerSockets.end(); ++i )
                close( *i );
        }
#endif

        log() << "\t shutdown: going to flush oplog..." << endl;
        stringstream ss2;
        flushOpLog( ss2 );
        rawOut( ss2.str() );

        /* must do this before unmapping mem or you may get a seg fault */
        log() << "\t shutdown: going to close sockets..." << endl;
        closeAllSockets();

        // wait until file preallocation finishes
        // we would only hang here if the file_allocator code generates a
        // synchronous signal, which we don't expect
        log() << "\t shutdown: waiting for fs..." << endl;
        theFileAllocator().waitUntilFinished();
        
        log() << "\t shutdown: closing all files..." << endl;
        stringstream ss3;
        MemoryMappedFile::closeAllFiles( ss3 );
        rawOut( ss3.str() );

        // should we be locked here?  we aren't. might be ok as-is.
        recCacheCloseAll();
        
#if !defined(_WIN32) && !defined(__sunos__)
        if ( lockFile ){
            log() << "\t shutdown: removing fs lock..." << endl;
            if( ftruncate( lockFile , 0 ) ) 
                log() << "\t couldn't remove fs lock errno=" << errno << endl;
            flock( lockFile, LOCK_UN );
        }
#endif
    }

    void acquirePathLock() {
#if !defined(_WIN32) && !defined(__sunos__)
        string name = ( boost::filesystem::path( dbpath ) / "mongod.lock" ).native_file_string();
        lockFile = open( name.c_str(), O_RDWR | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO );
        massert( "Unable to create / open lock file for dbpath: " + name, lockFile > 0 );
        massert( "Unable to acquire lock for dbpath: " + name, flock( lockFile, LOCK_EX | LOCK_NB ) == 0 );
        
        stringstream ss;
        ss << getpid() << endl;
        string s = ss.str();
        const char * data = s.c_str();
        assert( write( lockFile , data , strlen( data ) ) );
        fsync( lockFile );
#endif        
    }
    
} // namespace mongo
