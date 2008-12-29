// db.cpp : Defines the entry point for the console application.
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
#include "javajs.h"
#include "query.h"
#include "introspect.h"
#include "repl.h"
#include "../util/unittest.h"
#include "dbmessage.h"
#include "instance.h"

extern bool objcheck, quiet, quota, verbose;
bool useJNI = true;

/* only off if --nocursors which is for debugging. */
extern bool useCursors;

extern int port;
extern int curOp;
extern string dashDashSource;
extern int opLogging;
extern OpLog _oplog;

extern int ctr;
extern int callDepth;

void closeAllSockets();
void startReplication();
void pairWith(const char *remoteEnd, const char *arb);

struct MyStartupTests {
    MyStartupTests() {
        assert( sizeof(OID) == 12 );
    }
} mystartupdbcpp;

void quicktest() {
    cout << "quicktest()\n";

    MemoryMappedFile mmf;
    char *m = (char *) mmf.map("/tmp/quicktest", 16384);
    //	cout << "mmf reads: " << m << endl;
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
    cout << endl;

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
    const char *Version = "db version: 122";
    problem() << Version << endl;
    problem() << "pdfile version " << VERSION << "." << VERSION_MINOR << endl;
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

/* we create one thread for each connection from an app server database.
   app server will open a pool of threads.
*/
void connThread()
{
    try {

        MessagingPort& dbMsgPort = *grab;
        grab = 0;

        Message m;
        while ( 1 ) {
            m.reset();

            if ( !dbMsgPort.recv(m) ) {
                log() << "end connection " << dbMsgPort.farEnd.toString() << endl;
                dbMsgPort.shutdown();
                break;
            }

            DbResponse dbresponse;
            if ( !assembleResponse( m, dbresponse ) ) {
                cout << curTimeMillis() % 10000 << "   end msg " << dbMsgPort.farEnd.toString() << endl;
                if ( dbMsgPort.farEnd.isLocalHost() ) {
                    dbMsgPort.shutdown();
                    sleepmillis(50);
                    problem() << "exiting end msg" << endl;
                    exit(EXIT_SUCCESS);
                }
                else {
                    cout << "  (not from localhost, ignoring end msg)" << endl;
                }
            }

            if ( dbresponse.response )
                dbMsgPort.reply(m, *dbresponse.response, dbresponse.responseTo);
        }

    }
    catch ( AssertionException& ) {
        problem() << "Uncaught AssertionException, terminating" << endl;
        exit(15);
    }
    catch( std::exception &e ) {
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
        cout << " ****ok. response.data:" << ok << " time:" << tm / 1000.0 << "ms " <<
             ((double) len) * 8 / 1000000 / (tm/1000000) << "Mbps" << endl;
        if (  q+1 < Loops ) {
            cout << "\t\tSLEEP 8 then sending again as a test" << endl;
            sleepsecs(8);
        }
    }
    sleepsecs(1);

    p.shutdown();
}

void msg(const char *m, int extras = 0) {
    msg(m, "127.0.0.1", DBPort, extras);
}

#if !defined(_WIN32)

#include <signal.h>

void pipeSigHandler( int signal ) {
    psignal( signal, "Signal Received : ");
}

int segvs = 0;
void segvhandler(int x) {
    if ( ++segvs > 1 ) {
        signal(x, SIG_DFL);
        if ( segvs == 2 ) {
            cout << "\n\n\n got 2nd SIGSEGV" << endl;
            sayDbContext();
        }
        return;
    }
    problem() << "got SIGSEGV " << x << ", terminating :-(" << endl;
    sayDbContext();
//	closeAllSockets();
//	MemoryMappedFile::closeAllFiles();
//	flushOpLog();
    dbexit(14);
}

void mysighandler(int x) {
    signal(x, SIG_IGN);
    log() << "got kill or ctrl c signal " << x << ", will terminate after current cmd ends" << endl;
    {
        dblock lk;
        problem() << "  now exiting" << endl;
        exit(12);
    }
}

void setupSignals() {
    assert( signal(SIGINT, mysighandler) != SIG_ERR );
    assert( signal(SIGTERM, mysighandler) != SIG_ERR );
}

#else
void setupSignals() {}
#endif


void repairDatabases() {
    dblock lk;
    boost::filesystem::path path( dbpath );
    for ( boost::filesystem::directory_iterator i( path );
            i != boost::filesystem::directory_iterator(); ++i ) {
        string fileName = i->leaf();
        if ( fileName.length() > 3 && fileName.substr( fileName.length() - 3, 3 ) == ".ns" ) {
            string dbName = fileName.substr( 0, fileName.length() - 3 );
            assert( !setClientTempNs( dbName.c_str() ) );
            PhysicalDataFile *p = database->getFile( 0 );
            PDFHeader *h = p->getHeader();
            if ( !h->currentVersion() ) {
                // QUESTION: Repair even if file format is higher version than code?
                log() << "repairing database " << dbName << " with pdfile version " << h->version << "." << h->versionMinor << ", ";
                log() << "new version: " << VERSION << "." << VERSION_MINOR << endl;
                repairDatabase( dbName.c_str() );
            } else {
                closeClient( dbName.c_str() );
            }
        }
    }
}

void initAndListen(int listenPort, const char *appserverLoc = null) {
    if ( opLogging )
        log() << "opLogging = " << opLogging << endl;
    _oplog.init();

#if !defined(_WIN32)
    assert( signal(SIGSEGV, segvhandler) != SIG_ERR );
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
        javajstest();
    }
#endif

    setupSignals();

    repairDatabases();

    listen(listenPort);
}

//ofstream problems("dbproblems.log", ios_base::app | ios_base::out);
int test2();
void testClient();

int main(int argc, char* argv[], char *envp[] )
{
    {
        unsigned x = 0x12345678;
        unsigned char& b = (unsigned char&) x;
        if ( b != 0x78 ) {
            cout << "big endian cpus not yet supported" << endl;
            return 33;
        }
    }

    DEV cout << "warning: DEV mode enabled\n";

#if !defined(_WIN32)
    signal(SIGPIPE, pipeSigHandler);
#endif
    srand(curTimeMillis());

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
            cout << "NOJNI build cannot test" << endl;
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
            else if ( s == "--help" || s == "-?" || s == "--?" )
                goto usage;
            else if ( s == "--quiet" )
                quiet = true;
            else if ( s == "--verbose" )
                verbose = true;
            else if ( s == "--quota" )
                quota = true;
            else if ( s == "--objcheck" )
                objcheck = true;
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
            else if ( strncmp(s.c_str(), "--oplog", 7) == 0 ) {
                int x = s[7] - '0';
                if ( x < 0 || x > 7 ) {
                    cout << "can't interpret --oplog setting" << endl;
                    exit(13);
                }
                opLogging = x;
            }
        }

        initAndListen(port, appsrvPath);

        exit(0);
    }

usage:
    cout << "Mongo db ";
#if defined(NOJNI)
    cout << "[nojni build] ";
#endif
    cout << "usage:\n";
    cout << "  run                run db" << endl;
    cout << "  msg end [port]     shut down db server listening on port (or default)" << endl;
    cout << "  msg [msg] [port]   send a request to the db server listening on port (or default)" << endl;
    cout << "  msglots            send a bunch of test messages, and then wait for answer on the last one" << endl;
    cout << "  longmsg            send a long test message to the db server" << endl;
    cout << "  quicktest          just check basic assertions and exit" << endl;
    cout << "  test2              run test2() - see code" << endl;
    cout << "\nOptions:\n";
    cout << " --help              show this usage information\n";
    cout << " --port <portno>     specify port number, default is 27017\n";
    cout << " --dbpath <root>     directory for datafiles, default is /data/db/\n";
    cout << " --quiet             quieter output (no cpu outputs)\n";
    cout << " --verbose\n";
    cout << " --objcheck          inspect client data for validity on receipt\n";
    cout << " --quota             enable db quota management\n";
    cout << " --appsrvpath <path> root directory for the babble app server\n";
    cout << " --nocursors         diagnostic/debugging option\n";
    cout << " --nojni" << endl;
    cout << " --oplog<n> 0=off 1=W 2=R 3=both 7=W+some reads" << endl;
    cout << "\nReplication:" << endl;
    cout << " --master\n";
    cout << " --slave" << endl;
    cout << " --source <server:port>" << endl;
    cout << " --pairwith <server:port> <arbiter>" << endl;
    cout << endl;

    return 0;
}
