// db.cpp : Defines the entry point for the console application.
//

/**
*    Copyright (C) 2008 10gen Inc.info
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
#include "javajs.h"
#include "query.h"
#include "introspect.h"
#include "repl.h"
#include "../util/unittest.h"
#include "dbmessage.h"
#include "instance.h"
#if !defined(_WIN32)
#include <sys/file.h>
#endif

namespace mongo {

    extern bool objcheck, quiet, quota, cpu;
    bool useJNI = true;

    /* only off if --nocursors which is for debugging. */
    extern bool useCursors;
    /* only off if --nohints */
    extern bool useHints;

    extern int port;
    extern int curOp;
    extern bool autoresync;
    extern string dashDashSource;
    extern string dashDashOnly;
    extern int opLogging;
    extern long long oplogSize;
    extern OpLog _oplog;

    extern int ctr;
    extern int callDepth;

    void setupSignals();
    void closeAllSockets();
    void startReplication();
    void pairWith(const char *remoteEnd, const char *arb);

    struct MyStartupTests {
        MyStartupTests() {
            assert( sizeof(OID) == 12 );
        }
    } mystartupdbcpp;

    void quicktest() {
        out() << "quicktest()\n";

        MemoryMappedFile mmf;
        char *m = (char *) mmf.map("/tmp/quicktest", 16384);
        //	out() << "mmf reads: " << m << endl;
        strcpy_s(m, 1000, "hello worldz");
    }

    QueryResult* emptyMoreResult(long long);


    void testTheDb() {
        stringstream ss;

        setClient("sys.unittest.pdfile");

        /* this is not validly formatted, if you query this namespace bad things will happen */
        theDataFileMgr.insert("sys.unittest.pdfile", (void *) "hello worldx", 13);
        theDataFileMgr.insert("sys.unittest.pdfile", (void *) "hello worldx", 13);

        BSONObj j1((const char *) &js1);
        deleteObjects("sys.unittest.delete", j1, false);
        theDataFileMgr.insert("sys.unittest.delete", &js1, sizeof(js1));
        deleteObjects("sys.unittest.delete", j1, false);
        updateObjects("sys.unittest.delete", j1, j1, true,ss);
        updateObjects("sys.unittest.delete", j1, j1, false,ss);

        auto_ptr<Cursor> c = theDataFileMgr.findAll("sys.unittest.pdfile");
        while ( c->ok() ) {
            c->_current();
            c->advance();
        }
        out() << endl;

        database = 0;
    }

    MessagingPort *grab = 0;
    void connThread();

    class OurListener : public Listener {
    public:
        OurListener(int p) : Listener(p) { }
        virtual void accepted(MessagingPort *mp) {
            assert( grab == 0 );
            grab = mp;
            boost::thread thr(connThread);
            while ( grab )
                sleepmillis(1);
        }
    };

    void webServerThread();
    void pdfileInit();

    /* versions
       114 bad memory bug fixed
       115 replay, opLogging
    */
    void listen(int port) {
        const char *Version = "db version v0.8.0.1";
        log() << Version << ", pdfile version " << VERSION << "." << VERSION_MINOR << endl;
        pdfileInit();
        //testTheDb();
        log() << "waiting for connections on port " << port << "..." << endl;
        OurListener l(port);
        startReplication();
        boost::thread thr(webServerThread);
        l.listen();
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
        Message & container;
    };

} // namespace mongo

#include "lasterror.h"
#include "security.h"

namespace mongo {

  void sysInfo() { 
    out() << "sysinfo:\n";
#if defined(_SC_PAGE_SIZE)
    out() << "  page size: " << (int) sysconf(_SC_PAGE_SIZE) << endl;
#endif
#if defined(_SC_PHYS_PAGES)
    out() << "  _SC_PHYS_PAGES: " << sysconf(_SC_PHYS_PAGES) << endl;
#endif
#if defined(_SC_AVPHYS_PAGES)
    out() << "  _SC_AVPHYS_PAGES: " << sysconf(_SC_AVPHYS_PAGES) << endl;
#endif
  }

    /* we create one thread for each connection from an app server database.
       app server will open a pool of threads.
    */
    void connThread()
    {
        AuthenticationInfo *ai = new AuthenticationInfo();
        authInfo.reset(ai);
        LastError *le = new LastError();
        lastError.reset(le);

        MessagingPort& dbMsgPort = *grab;
        grab = 0;

        try {

			ai->isLocalHost = dbMsgPort.farEnd.isLocalHost();

            Message m;
            while ( 1 ) {
                m.reset();

                if ( !dbMsgPort.recv(m) ) {
                    log() << "end connection " << dbMsgPort.farEnd.toString() << endl;
                    dbMsgPort.shutdown();
                    break;
                }

                le->nPrev++;

                DbResponse dbresponse;
                if ( !assembleResponse( m, dbresponse ) ) {
                    out() << curTimeMillis() % 10000 << "   end msg " << dbMsgPort.farEnd.toString() << endl;
                    /* todo: we may not wish to allow this, even on localhost: very low priv accounts could stop us. */
                    if ( dbMsgPort.farEnd.isLocalHost() ) {
                        dbMsgPort.shutdown();
                        sleepmillis(50);
                        problem() << "exiting end msg" << endl;
                        exit(EXIT_SUCCESS);
                    }
                    else {
                        out() << "  (not from localhost, ignoring end msg)" << endl;
                    }
                }

                if ( dbresponse.response )
                    dbMsgPort.reply(m, *dbresponse.response, dbresponse.responseTo);
            }

        }
        catch ( AssertionException& ) {
            problem() << "AssertionException in connThread, closing client connection" << endl;
            dbMsgPort.shutdown();
        }
        catch ( SocketException& ) {
            problem() << "SocketException in connThread, closing client connection" << endl;
            dbMsgPort.shutdown();
        }
        catch ( std::exception &e ) {
            problem() << "Uncaught std::exception: " << e.what() << ", terminating" << endl;
            exit( 15 );
        }
        catch ( ... ) {
            problem() << "Uncaught exception, terminating" << endl;
            exit( 15 );
        }
    }


    void msg(const char *m, const char *address, int port, int extras = 0) {

        SockAddr db(address, port);

//	SockAddr db("127.0.0.1", DBPort);
//	SockAddr db("192.168.37.1", MessagingPort::DBPort);
//	SockAddr db("10.0.21.60", MessagingPort::DBPort);
//	SockAddr db("172.16.0.179", MessagingPort::DBPort);

        MessagingPort p;
        if ( !p.connect(db) )
            return;

        const int Loops = 1;
        for ( int q = 0; q < Loops; q++ ) {
            Message send;
            Message response;

            send.setData( dbMsg , m);
            int len = send.data->dataLen();

            for ( int i = 0; i < extras; i++ )
                p.say(/*db, */send);

            Timer t;
            bool ok = p.call(send, response);
            double tm = t.micros() + 1;
            out() << " ****ok. response.data:" << ok << " time:" << tm / 1000.0 << "ms " <<
                 ((double) len) * 8 / 1000000 / (tm/1000000) << "Mbps" << endl;
            if (  q+1 < Loops ) {
                out() << "\t\tSLEEP 8 then sending again as a test" << endl;
                sleepsecs(8);
            }
        }
        sleepsecs(1);

        p.shutdown();
    }

    void msg(const char *m, int extras = 0) {
        msg(m, "127.0.0.1", DBPort, extras);
    }

    void repairDatabases() {
        dblock lk;
        vector< string > dbNames;
        getDatabaseNames( dbNames );
        for ( vector< string >::iterator i = dbNames.begin(); i != dbNames.end(); ++i ) {
            string dbName = *i;
            assert( !setClientTempNs( dbName.c_str() ) );
            MongoDataFile *p = database->getFile( 0 );
            MDFHeader *h = p->getHeader();
            if ( !h->currentVersion() ) {
                // QUESTION: Repair even if file format is higher version than code?
                log() << "repairing database " << dbName << " with pdfile version " << h->version << "." << h->versionMinor << ", "
                << "new version: " << VERSION << "." << VERSION_MINOR << endl;
                string errmsg;
                assert( repairDatabase( dbName.c_str(), errmsg ) );
            } else {
                closeClient( dbName.c_str() );
            }
        }
    }

    void clearTmpFiles() {
        boost::filesystem::path path( dbpath );
        for ( boost::filesystem::directory_iterator i( path );
                i != boost::filesystem::directory_iterator(); ++i ) {
            string fileName = i->leaf();
            if ( boost::filesystem::is_directory( *i ) &&
                    fileName.length() > 2 && fileName.substr( 0, 3 ) == "tmp" )
                boost::filesystem::remove_all( *i );
        }
    }
    
    void clearTmpCollections() {
        vector< string > toDelete;
        DBDirectClient cli;
        auto_ptr< DBClientCursor > c = cli.query( "local.system.namespaces", Query( fromjson( "{name:/^local.temp./}" ) ) );
        while( c->more() )
            toDelete.push_back( c->next().getStringField( "name" ) );
        for( vector< string >::iterator i = toDelete.begin(); i != toDelete.end(); ++i ) {
            log() << "Dropping old temporary collection: " << *i << endl;
            cli.dropCollection( *i );
        }
    }

    Timer startupSrandTimer;

    void acquirePathLock() {
#if !defined(_WIN32)
        string name = ( boost::filesystem::path( dbpath ) / "mongod.lock" ).native_file_string();
        int f = open( name.c_str(), O_RDONLY | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO );
        massert( "Unable to create / open lock file for dbpath: " + name, f > 0 );
        massert( "Unable to acquire lock for dbpath: " + name, flock( f, LOCK_EX | LOCK_NB ) == 0 );
#endif        
    }

    void _initAndListen(int listenPort, const char *appserverLoc = null) {
        stringstream ss;
        ss << "dbpath (" << dbpath << ") does not exist";
        massert( ss.str().c_str(), boost::filesystem::exists( dbpath ) );
        
        acquirePathLock();
        
        clearTmpFiles();
        clearTmpCollections();

        if ( opLogging )
            log() << "opLogging = " << opLogging << endl;
        _oplog.init();

#if 0
        {
            stringstream indexpath;
            indexpath << dbpath << "/indexes.dat";
            RecCache::tempStore.init(indexpath.str().c_str(), BucketSize);
        }
#endif

#if !defined(_WIN32)
        pid_t pid = 0;
        pid = getpid();
#else
        int pid=0;
#endif

        log() << "Mongo DB : starting : pid = " << pid << " port = " << port << " dbpath = " << dbpath
        <<  " master = " << master << " slave = " << slave << endl;

#if !defined(NOJNI)
        if ( useJNI ) {
            JavaJS = new JavaJSImpl(appserverLoc);
        }
#endif
      
        repairDatabases();

        /* this is for security on certain platforms */
        srand(curTimeMicros() ^ startupSrandTimer.micros());

        listen(listenPort);
    }
    void initAndListen(int listenPort, const char *appserverLoc = null) {
        try { _initAndListen(listenPort, appserverLoc); }
        catch(...) { 
            log(1) << "assertion exception in initAndListen, terminating" << endl;
            dbexit(1);
        }
    }

    int test2();
    void testClient();

} // namespace mongo


using namespace mongo;

int q;

int main(int argc, char* argv[], char *envp[] )
{
    setupSignals();
    
    dbExecCommand = argv[0];
    
    srand(curTimeMicros());
    boost::filesystem::path::default_name_check( boost::filesystem::no_check );

    {
        unsigned x = 0x12345678;
        unsigned char& b = (unsigned char&) x;
        if ( b != 0x78 ) {
            out() << "big endian cpus not yet supported" << endl;
            return 33;
        }
    }

    DEV out() << "warning: DEV mode enabled\n";
    
    UnitTest::runTests();

    if ( argc >= 2 ) {
        if ( strcmp(argv[1], "quicktest") == 0 ) {
            quicktest();
            return 0;
        }
        if ( strcmp(argv[1], "javatest") == 0 ) {
#if !defined(NOJNI)
            JavaJS = new JavaJSImpl();
            javajstest();
#else
            out() << "NOJNI build cannot test" << endl;
#endif
            return 0;
        }
        if ( strcmp(argv[1], "test2") == 0 ) {
            return test2();
        }
        if ( strcmp(argv[1], "msg") == 0 ) {

            // msg(argc >= 3 ? argv[2] : "ping");

            const char *m = "ping";
            int thePort = DBPort;

            if (argc >= 3) {
                m = argv[2];

                if (argc > 3) {
                    thePort = atoi(argv[3]);
                }
            }

            msg(m, "127.0.0.1", thePort);

            return 0;
        }
        if ( strcmp(argv[1], "msglots") == 0 ) {
            msg(argc >= 3 ? argv[2] : "ping", 1000);
            return 0;
        }
        if ( strcmp( argv[1], "testclient") == 0 ) {
            testClient();
            return 0;
        }
        if ( strcmp(argv[1], "zzz") == 0 ) {
            msg(argc >= 3 ? argv[2] : "ping", 1000);
            return 0;
        }
        if ( strcmp(argv[1], "run") == 0 ) {
            initAndListen(port);
            return 0;
        }
        if ( strcmp(argv[1], "longmsg") == 0 ) {
            char buf[800000];
            memset(buf, 'a', 799999);
            buf[799999] = 0;
            buf[799998] = 'b';
            buf[0] = 'c';
            msg(buf);
            return 0;
        }

        /*
         *  *** POST STANDARD SWITCH METHOD - if we don't satisfy, we switch to a
         *     slightly different mode where "run" is assumed and we can set values
         */

        char *appsrvPath = null;

        for (int i = 1; i < argc; i++)  {

            if ( argv[i] == 0 ) continue;
            string s = argv[i];

            if ( s == "--port" )
                port = atoi(argv[++i]);
            else if ( s == "--nojni" )
                useJNI = false;
            else if ( s == "--master" )
                master = true;
            else if ( s == "--slave" )
                slave = true;
            else if ( s == "--autoresync" )
                autoresync = true;
            else if ( s == "--help" || s == "-?" || s == "--?" )
                goto usage;
            else if ( s == "--quiet" )
                quiet = true;
            else if ( s == "--cpu" )
                cpu = true;
            else if ( s == "--noauth" )
                noauth = true;
            else if ( s == "--auth" )
                noauth = false;
            else if( s == "--sysinfo" ) { 
                sysInfo();
                return 0;
            }
            else if ( s == "--verbose" )
                logLevel = 1;
            else if ( s.find( "-v" ) == 0 ){
                logLevel = s.size() - 1;
            }
            else if ( s == "--quota" )
                quota = true;
            else if ( s == "--objcheck" )
                objcheck = true;
            else if( s == "--only" ) 
                dashDashOnly = argv[++i];
            else if ( s == "--source" ) {
                /* specifies what the source in local.sources should be */
                dashDashSource = argv[++i];
            }
            else if ( s == "--pairwith" ) {
                pairWith( argv[i+1], argv[i+2] );
                i += 2;
            }
            else if ( s == "--dbpath" )
                dbpath = argv[++i];
            else if ( s == "--appsrvpath" )
                appsrvPath = argv[++i];
            else if ( s == "--nocursors" )
                useCursors = false;
            else if ( s == "--nohints" )
                useHints = false;
            else if ( s == "--oplogSize" ) {
                long x = strtol( argv[ ++i ], 0, 10 );
                uassert("bad arg", x > 0);
                oplogSize = x * 1024 * 1024;
                assert(oplogSize > 0);
            }
            else if ( strncmp(s.c_str(), "--oplog", 7) == 0 ) {
                int x = s[7] - '0';
                if ( x < 0 || x > 7 ) {
                    out() << "can't interpret --oplog setting" << endl;
                    dbexit(13);
                }
                opLogging = x;
            }
        }
        
        initAndListen(port, appsrvPath);

        dbexit(0);
    }

usage:
    out() << "Mongo db ";
#if defined(NOJNI)
    out() << "[nojni build] ";
#endif
    out() << "usage:\n";
    out() << "  run                      run db" << endl;
    out() << "  msg [msg] [port]         send a request to the db server listening on port (or default)" << endl;
    out() << "  msglots                  send many test messages, and then wait for answer on the last one" << endl;
    out() << "  longmsg                  send a long test message to the db server" << endl;
    out() << "  quicktest                just check basic assertions and exit" << endl;
    out() << "  test2                    run test2() - see code" << endl;
    out() << "\nOptions:\n";
    out() << " --help                    show this usage information\n";
    out() << " --port <portno>           specify port number, default is 27017\n";
    out() << " --dbpath <root>           directory for datafiles, default is /data/db/\n";
    out() << " --quiet                   quieter output\n";
    out() << " --cpu                     show cpu+iowait utilization periodically\n";
    out() << " --noauth                  run without security\n";
    out() << " --auth                    run with security\n";
    out() << " --verbose\n";
    out() << " -v+                       increase verbose level -v = --verbose\n";
    out() << " --objcheck                inspect client data for validity on receipt\n";
    out() << " --quota                   enable db quota management\n";
    out() << " --appsrvpath <path>       root directory for the babble app server\n";
    out() << " --nocursors               diagnostic/debugging option\n";
    out() << " --nohints                 ignore query hints\n";
    out() << " --nojni" << endl;
    out() << " --oplog<n>                0=off 1=W 2=R 3=both 7=W+some reads" << endl;
    out() << " --oplogSize <size_in_MB>  custom size if creating new replication operation log" << endl;
    out() << " --sysinfo                 print some diagnostic system information\n";
    out() << "\nReplication:" << endl;
    out() << " --master\n";
    out() << " --slave" << endl;
    out() << " --source <server:port>    when a slave, specifies master" << endl;
    out() << " --only <dbname>           when a slave, only replicate db <dbname>" << endl;
    out() << " --pairwith <server:port> <arbiter>" << endl;
    out() << " --autoresync" << endl;
    out() << endl;

    return 0;
}

namespace mongo {

    /* we do not use log() below as it uses a mutex and that could cause deadlocks.
    */

    string getDbContext();

#undef out

#if !defined(_WIN32)

} // namespace mongo

#include <signal.h>
#include <string.h>

namespace mongo {

    void pipeSigHandler( int signal ) {
#ifdef psignal
        psignal( signal, "Signal Received : ");
#else
        cout << "got pipe signal:" << signal << endl;
#endif
    }

    void abruptQuit(int x) {
        ostringstream ossSig;
        ossSig << "Got signal: " << x << " (" << strsignal( x ) << ")." << endl;
        rawOut( ossSig.str() );

        ostringstream ossOp;
        ossOp << "Last op: " << currentOp.infoNoauth() << endl;
        rawOut( ossOp.str() );

        ostringstream oss;
        oss << "Backtrace:" << endl;
        printStackTrace( oss );
        rawOut( oss.str() );
        exit(14);
    }

    sigset_t asyncSignals;
    // The above signals will be processed by this thread only, in order to
    // ensure the db and log mutexes aren't held.
    void interruptThread() {
        int x;
        sigwait( &asyncSignals, &x );
        log() << "got kill or ctrl c signal " << x << " (" << strsignal( x ) << "), will terminate after current cmd ends" << endl;
        {
            dblock lk;
            log() << "now exiting" << endl;
            exit(12);
        }
    }
        
    void setupSignals() {
        assert( signal(SIGSEGV, abruptQuit) != SIG_ERR );
        assert( signal(SIGFPE, abruptQuit) != SIG_ERR );
        assert( signal(SIGABRT, abruptQuit) != SIG_ERR );
        assert( signal(SIGBUS, abruptQuit) != SIG_ERR );
        assert( signal(SIGPIPE, pipeSigHandler) != SIG_ERR );

        sigemptyset( &asyncSignals );
        sigaddset( &asyncSignals, SIGINT );
        sigaddset( &asyncSignals, SIGTERM );
        assert( pthread_sigmask( SIG_SETMASK, &asyncSignals, 0 ) == 0 );
        boost::thread it( interruptThread );
    }

#else
    void setupSignals() {}
#endif

} // namespace mongo

#include "recstore.h"
#include "reccache.h"
