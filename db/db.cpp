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
#include "query.h"
#include "introspect.h"
#include "repl.h"
#include "../util/unittest.h"
#include "../util/file_allocator.h"
#include "../util/background.h"
#include "dbmessage.h"
#include "instance.h"
#include "clientcursor.h"
#include "pdfile.h"
#if !defined(_WIN32)
#include <sys/file.h>
#endif

#if defined(_WIN32)
#include "../util/ntservice.h"
#endif

#include "../scripting/engine.h"
#include "module.h"
#include "cmdline.h"

namespace mongo {

    bool useJNI = true;

    /* only off if --nocursors which is for debugging. */
    extern bool useCursors;
    /* only off if --nohints */
    extern bool useHints;

    bool noHttpInterface = false;

    extern string bind_ip;
    extern char *appsrvPath;
    extern bool autoresync;
    extern int diagLogging;
    extern int lenForNewNsFiles;
    extern int lockFile;

    void setupSignals();
    void closeAllSockets();
    void startReplication();
    void pairWith(const char *remoteEnd, const char *arb);
    void setRecCacheSize(unsigned MB);

    const char *ourgetns() { 
        Client *c = currentClient.get();
        return c ? c->ns() : "";
    }

    struct MyStartupTests {
        MyStartupTests() {
            assert( sizeof(OID) == 12 );
        }
    } mystartupdbcpp;

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
        updateObjects("sys.unittest.delete", j1, j1, true,false,ss,true);
        updateObjects("sys.unittest.delete", j1, j1, false,false,ss,true);

        auto_ptr<Cursor> c = theDataFileMgr.findAll("sys.unittest.pdfile");
        while ( c->ok() ) {
            c->_current();
            c->advance();
        }
        out() << endl;

        cc().clearns();
    }

    MessagingPort *grab = 0;
    void connThread();

    class OurListener : public Listener {
    public:
        OurListener(const string &ip, int p) : Listener(ip, p) { }
        virtual void accepted(MessagingPort *mp) {
            assert( grab == 0 );
            grab = mp;
            try {
                boost::thread thr(connThread);
                while ( grab )
                    sleepmillis(1);
            }
            catch ( boost::thread_resource_error& e ){
                log() << "can't create new thread, closing connection" << endl;
                mp->shutdown();
                grab = 0;
            }
        }
    };

    void webServerThread();
    void pdfileInit();

    void listen(int port) {
        log() << mongodVersion() << endl;
        printGitVersion();
        printSysInfo();
        pdfileInit();
        //testTheDb();
        log() << "waiting for connections on port " << port << endl;
        OurListener l(bind_ip, port);
        startReplication();
        if ( !noHttpInterface )
            boost::thread thr(webServerThread);
        if ( l.init() ) {
            registerListenerSocket( l.socket() );
            l.listen();
        }
    }

} // namespace mongo

#include "client.h"

namespace mongo {

  void sysRuntimeInfo() {
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
        Client::initThread("conn");

        /* todo: move to Client object */
        LastError *le = new LastError();
        lastError.reset(le);

        MessagingPort& dbMsgPort = *grab;
        grab = 0;

        try {

            currentClient.get()->ai->isLocalHost = dbMsgPort.farEnd.isLocalHost();

            Message m;
            while ( 1 ) {
                m.reset();

                if ( !dbMsgPort.recv(m) ) {
                    if( !cmdLine.quiet )
                    log() << "end connection " << dbMsgPort.farEnd.toString() << endl;
                    dbMsgPort.shutdown();
                    break;
                }

                lastError.startRequest( m , le );

                DbResponse dbresponse;
                if ( !assembleResponse( m, dbresponse, dbMsgPort.farEnd.sa ) ) {
                    out() << curTimeMillis() % 10000 << "   end msg " << dbMsgPort.farEnd.toString() << endl;
                    /* todo: we may not wish to allow this, even on localhost: very low priv accounts could stop us. */
                    if ( dbMsgPort.farEnd.isLocalHost() ) {
                        dbMsgPort.shutdown();
                        sleepmillis(50);
                        problem() << "exiting end msg" << endl;
                        dbexit(EXIT_CLEAN);
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
            dbexit( EXIT_UNCAUGHT );
        }
        catch ( ... ) {
            problem() << "Uncaught exception, terminating" << endl;
            dbexit( EXIT_UNCAUGHT );
        }

        // any thread cleanup can happen here

        if ( currentClient.get() )
            currentClient->shutdown();
        globalScriptEngine->threadDone();
    }


    void msg(const char *m, const char *address, int port, int extras = 0) {

        SockAddr db(address, port);

//  SockAddr db("127.0.0.1", DBPort);
//  SockAddr db("192.168.37.1", MessagingPort::DBPort);
//  SockAddr db("10.0.21.60", MessagingPort::DBPort);
//  SockAddr db("172.16.0.179", MessagingPort::DBPort);

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
            double tm = ((double) t.micros()) + 1;
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
        msg(m, "127.0.0.1", CmdLine::DefaultDBPort, extras);
    }

    bool shouldRepairDatabases = 0;
    bool forceRepair = 0;
    
    bool doDBUpgrade( const string& dbName , string errmsg , MDFHeader * h ){
        static DBDirectClient db;
        
        if ( forceRepair == false && h->version == 4 && h->versionMinor == 4 ){
            assert( VERSION == 4 );
            assert( VERSION_MINOR == 5 );
            
            list<string> colls = db.getCollectionNames( dbName );
            for ( list<string>::iterator i=colls.begin(); i!=colls.end(); i++){
                string c = *i;
                log() << "\t upgrading collection:" << c << endl;
                BSONObj out;
                bool ok = db.runCommand( dbName , BSON( "reIndex" << c.substr( dbName.size() + 1 ) ) , out );
                if ( ! ok ){
                    errmsg = "reindex failed";
                    log() << "\t\t reindex failed: " << out << endl;
                    return false;
                }
            }
            
            h->versionMinor = 5;
            return true;
        }
        
        // do this in the general case
        return repairDatabase( dbName.c_str(), errmsg );
    }
    
    void repairDatabases() {
        log(1) << "enter repairDatabases" << endl;
        dblock lk;
        vector< string > dbNames;
        getDatabaseNames( dbNames );
        for ( vector< string >::iterator i = dbNames.begin(); i != dbNames.end(); ++i ) {
            string dbName = *i;
            log(1) << "\t" << dbName << endl;
            assert( !setClient( dbName.c_str() ) );
            MongoDataFile *p = cc().database()->getFile( 0 );
            MDFHeader *h = p->getHeader();
            if ( !h->currentVersion() || forceRepair ) {
                log() << "****" << endl;
                log() << "****" << endl;
                log() << "need to upgrade database " << dbName << " with pdfile version " << h->version << "." << h->versionMinor << ", "
                      << "new version: " << VERSION << "." << VERSION_MINOR << endl;
                if ( shouldRepairDatabases ){
                    // QUESTION: Repair even if file format is higher version than code?
                    log() << "\t starting upgrade" << endl;
                    string errmsg;
                    assert( doDBUpgrade( dbName , errmsg , h ) );
                }
                else {
                    log() << "\t Not upgrading, exiting!" << endl;
                    log() << "\t run --upgrade to upgrade dbs, then start again" << endl;
                    log() << "****" << endl;
                    dbexit( EXIT_NEED_UPGRADE );
                    shouldRepairDatabases = 1;
                    return;
                }
            } else {
                closeClient( dbName.c_str() );
            }
        }

        log(1) << "done repairDatabases" << endl;

        if ( shouldRepairDatabases ){
            log() << "finished checking dbs" << endl;
            cc().shutdown();
            dbexit( EXIT_CLEAN );
        }
    }

    void clearTmpFiles() {
        boost::filesystem::path path( dbpath );
        for ( boost::filesystem::directory_iterator i( path );
                i != boost::filesystem::directory_iterator(); ++i ) {
            string fileName = boost::filesystem::path(*i).leaf();
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

    /**
     * does background async flushes of mmapped files
     */
    class DataFileSync : public BackgroundJob {
    public:
        void run(){
            log(1) << "will flush memory every: " << _sleepsecs << " seconds" << endl;
            while ( ! inShutdown() ){
                if ( _sleepsecs == 0 ){
                    // in case at some point we add an option to change at runtime
                    sleepsecs(5);
                    continue;
                }
                sleepmillis( (int)(_sleepsecs * 1000) );
                MemoryMappedFile::flushAll( false );
                log(1) << "flushing mmmap" << endl;
            }
        }
        
        double _sleepsecs; // default value controlled by program options
    } dataFileSync;

    void show_32_warning(){
#if BOOST_VERSION < 103500
#warning built with boost version <= 1.34, limited concurrency
        //cout << \n** NOTE: built with boost version <= 1.34, limited concurrency" << endl;
#endif

        if ( sizeof(int*) != 4 )
            return;
        cout << endl;
        cout << "** NOTE: when using MongoDB 32 bit, you are limited to about 2 gigabytes of data" << endl;
        cout << "**       see http://blog.mongodb.org/post/137788967/32-bit-limitations for more" << endl;
        cout << endl;
    }

    Timer startupSrandTimer;

    void _initAndListen(int listenPort, const char *appserverLoc = null) {

#if !defined(_WIN32)
        pid_t pid = 0;
        pid = getpid();
#else
        int pid=0;
#endif

        bool is32bit = sizeof(int*) == 4;

        log() << "Mongo DB : starting : pid = " << pid << " port = " << cmdLine.port << " dbpath = " << dbpath
              <<  " master = " << master << " slave = " << (int) slave << "  " << ( is32bit ? "32" : "64" ) << "-bit " << endl;

        show_32_warning();

        stringstream ss;
        ss << "dbpath (" << dbpath << ") does not exist";
        massert( ss.str().c_str(), boost::filesystem::exists( dbpath ) );

        acquirePathLock();
        remove_all( dbpath + "/_tmp/" );

        theFileAllocator().start();

        BOOST_CHECK_EXCEPTION( clearTmpFiles() );

        Client::initThread("initandlisten");

        clearTmpCollections();

        _diaglog.init();

        Module::initAll();

#if 0
        {
            stringstream indexpath;
            indexpath << dbpath << "/indexes.dat";
            RecCache::tempStore.init(indexpath.str().c_str(), BucketSize);
        }
#endif

        if ( useJNI ) {
            ScriptEngine::setup();
        }

        repairDatabases();
        if ( shouldRepairDatabases )
            return;

        /* this is for security on certain platforms (nonce generation) */
        srand((unsigned) (curTimeMicros() ^ startupSrandTimer.micros()));

        listen(listenPort);

        // listen() will return when exit code closes its socket.
        while( 1 )
            sleepsecs( 100 );
    }
    void initAndListen(int listenPort, const char *appserverLoc = null) {
        try { _initAndListen(listenPort, appserverLoc); }
        catch ( std::exception &e ) {
            problem() << "exception in initAndListen std::exception: " << e.what() << ", terminating" << endl;
            dbexit( EXIT_UNCAUGHT );
        }
        catch ( int& n ){
            problem() << "exception in initAndListen int: " << n << ", terminating" << endl;
            dbexit( EXIT_UNCAUGHT );
        }
        catch(...) {
            log() << " exception in initAndListen, terminating" << endl;
            dbexit( EXIT_UNCAUGHT );
        }
    }

    #if defined(_WIN32)
    bool initService() {
        ServiceController::reportStatus( SERVICE_RUNNING );
        initAndListen( cmdLine.port, appsrvPath );
        return true;
    }
    #endif

} // namespace mongo


using namespace mongo;

#include <boost/program_options.hpp>

namespace po = boost::program_options;


void show_help_text(po::options_description options) {
    show_32_warning();
    cout << options << endl;
};

/* Return error string or "" if no errors. */
string arg_error_check(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        string s = argv[i];
        /* check for inclusion of old-style arbiter setting. */
        if (s == "--pairwith") {
            if (argc > i + 2) {
                string old_arbiter = argv[i + 2];
                if (old_arbiter == "-" || old_arbiter.substr(0, 1) != "-") {
                    return "Specifying arbiter using --pairwith is no longer supported, please use --arbiter";
                }
            }
        }
    }
    return "";
}

int main(int argc, char* argv[], char *envp[] )
{
    getcurns = ourgetns;

    po::options_description general_options("General options");
    po::options_description replication_options("Replication options");
    po::options_description sharding_options("Sharding options");
    po::options_description visible_options("Allowed options");
    po::options_description hidden_options("Hidden options");
    po::options_description cmdline_options("Command line options");

    po::positional_options_description positional_options;

    general_options.add_options()
        ("help,h", "show this usage information")
        ("version", "show version information")
        ("config,f", po::value<string>(), "configuration file specifying additional options")
        ("port", po::value<int>(&cmdLine.port)/*->default_value(CmdLine::DefaultDBPort)*/, "specify port number")
        ("bind_ip", po::value<string>(&bind_ip),
         "local ip address to bind listener - all local ips bound by default")
        ("verbose,v", "be more verbose (include multiple times for more verbosity e.g. -vvvvv)")
        ("dbpath", po::value<string>()->default_value("/data/db/"), "directory for datafiles")
        ("quiet", "quieter output")
        ("logpath", po::value<string>() , "file to send all output to instead of stdout" )
        ("logappend" , "appnd to logpath instead of over-writing" )
#ifndef _WIN32
        ("fork" , "fork server process" )
#endif
        ("cpu", "periodically show cpu and iowait utilization")
        ("noauth", "run without security")
        ("auth", "run with security")
        ("objcheck", "inspect client data for validity on receipt")
        ("quota", "enable db quota management")
        ("quotaFiles", po::value<int>(), "number of files allower per db, requires --quota")
        ("appsrvpath", po::value<string>(), "root directory for the babble app server")
        ("nocursors", "diagnostic/debugging option")
        ("nohints", "ignore query hints")
        ("nohttpinterface", "disable http interface")
        ("noscripting", "disable scripting engine")
        ("noprealloc", "disable data file preallocation")
        ("smallfiles", "use a smaller default file size")
        ("nssize", po::value<int>()->default_value(16), ".ns file size (in MB) for new databases")
        ("diaglog", po::value<int>(), "0=off 1=W 2=R 3=both 7=W+some reads")
        ("sysinfo", "print some diagnostic system information")
        ("upgrade", "upgrade db if needed")
        ("repair", "run repair on all dbs")
        ("notablescan", "do not allow table scans")
        ("syncdelay",po::value<double>(&dataFileSync._sleepsecs)->default_value(60), "seconds between disk syncs (0 for never)")
#if defined(_WIN32)
        ("install", "install mongodb service")
        ("remove", "remove mongodb service")
        ("service", "start mongodb service")
#endif
        ;

    replication_options.add_options()
        ("master", "master mode")
        ("slave", "slave mode")
        ("source", po::value<string>(), "when slave: specify master as <server:port>")
        ("only", po::value<string>(), "when slave: specify a single database to replicate")
        ("pairwith", po::value<string>(), "address of server to pair with")
        ("arbiter", po::value<string>(), "address of arbiter server")
        ("autoresync", "automatically resync if slave data is stale")
        ("oplogSize", po::value<long>(), "size limit (in MB) for op log")
        ("opIdMem", po::value<long>(), "size limit (in bytes) for in memory storage of op ids")
        ;

	sharding_options.add_options()
		("configsvr", "declare this is a config db of a cluster")
		("shardsvr", "declare this is a shard db of a cluster")
		;

    hidden_options.add_options()
        ("command", po::value< vector<string> >(), "command")
        ("cacheSize", po::value<long>(), "cache size (in MB) for rec store")
        ;

    /* support for -vv -vvvv etc. */
    for (string s = "vv"; s.length() <= 10; s.append("v")) {
        hidden_options.add_options()(s.c_str(), "verbose");
    }

    positional_options.add("command", 3);
    visible_options.add(general_options);
    visible_options.add(replication_options);
    visible_options.add(sharding_options);
    Module::addOptions( visible_options );
    cmdline_options.add(visible_options);
    cmdline_options.add(hidden_options);

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

    DEV out() << "DEV is defined (using _DEBUG), which is slower...\n";

    UnitTest::runTests();

    if (argc == 1) {
        cout << dbExecCommand << " --help for help and startup options" << endl;
    }

    {
        bool installService = false;
        bool removeService = false;
        bool startService = false;
        po::variables_map params;

        string error_message = arg_error_check(argc, argv);
        if (error_message != "") {
            cout << error_message << endl << endl;
            show_help_text(visible_options);
            return 0;
        }

        /* don't allow guessing - creates ambiguities when some options are
         * prefixes of others. allow long disguises and don't allow guessing
         * to get away with our vvvvvvv trick. */
        int command_line_style = (((po::command_line_style::unix_style ^
                                    po::command_line_style::allow_guessing) |
                                   po::command_line_style::allow_long_disguise) ^
                                  po::command_line_style::allow_sticky);

        try {
            po::store(po::command_line_parser(argc, argv).options(cmdline_options).
                      positional(positional_options).
                      style(command_line_style).run(), params);

            if (params.count("config")) {
                ifstream config_file (params["config"].as<string>().c_str());
                if (config_file.is_open()) {
                    po::store(po::parse_config_file(config_file, cmdline_options), params);
                    config_file.close();
                } else {
                    cout << "ERROR: could not read from config file" << endl << endl;
                    cout << visible_options << endl;
                    return 0;
                }
            }

            po::notify(params);
        } catch (po::error &e) {
            cout << "ERROR: " << e.what() << endl << endl;
            cout << visible_options << endl;
            return 0;
        }

        if (params.count("help")) {
            show_help_text(visible_options);
            return 0;
        }
        if (params.count("version")) {
            cout << mongodVersion() << endl;
            printGitVersion();
            return 0;
        }
        dbpath = params["dbpath"].as<string>();
        if (params.count("quiet")) {
            cmdLine.quiet = true;
        }
        if (params.count("verbose")) {
            logLevel = 1;
        }
        for (string s = "vv"; s.length() <= 10; s.append("v")) {
            if (params.count(s)) {
                logLevel = s.length();
            }
        }
        if (params.count("cpu")) {
            cmdLine.cpu = true;
        }
        if (params.count("noauth")) {
            noauth = true;
        }
        if (params.count("auth")) {
            noauth = false;
        }
        if (params.count("quota")) {
            cmdLine.quota = true;
        }
        if (params.count("quotaFiles")) {
            cmdLine.quota = true;
            cmdLine.quotaFiles = params["quotaFiles"].as<int>() - 1;
        }
        if (params.count("objcheck")) {
            objcheck = true;
        }
        if (params.count("appsrvpath")) {
            /* casting away the const-ness here */
            appsrvPath = (char*)(params["appsrvpath"].as<string>().c_str());
        }
#ifndef _WIN32
        if (params.count("fork")) {
            if ( ! params.count( "logpath" ) ){
                cout << "--fork has to be used with --logpath" << endl;
                return -1;
            }
            pid_t c = fork();
            if ( c ){
                cout << "forked process: " << c << endl;
                ::exit(0);
            }
            setsid();
            setupSignals();
        }
#endif
        if (params.count("logpath")) {
            string lp = params["logpath"].as<string>();
            uassert( "logpath has to be non-zero" , lp.size() );
            initLogging( lp , params.count( "logappend" ) );
        }
        if (params.count("nocursors")) {
            useCursors = false;
        }
        if (params.count("nohints")) {
            useHints = false;
        }
        if (params.count("nohttpinterface")) {
            noHttpInterface = true;
        }
        if (params.count("noscripting")) {
            useJNI = false;
        }
        if (params.count("noprealloc")) {
            cmdLine.prealloc = false;
        }
        if (params.count("smallfiles")) {
            cmdLine.smallfiles = true;
        }
        if (params.count("diaglog")) {
            int x = params["diaglog"].as<int>();
            if ( x < 0 || x > 7 ) {
                out() << "can't interpret --diaglog setting" << endl;
                dbexit( EXIT_BADOPTIONS );
            }
            _diaglog.level = x;
        }
        if (params.count("sysinfo")) {
            sysRuntimeInfo();
            return 0;
        }
        if (params.count("repair")) {
            shouldRepairDatabases = 1;
            forceRepair = 1;
        }
        if (params.count("upgrade")) {
            shouldRepairDatabases = 1;
        }
        if (params.count("notablescan")) {
            cmdLine.notablescan = true;
        }
        if (params.count("install")) {
            installService = true;
        }
        if (params.count("remove")) {
            removeService = true;
        }
        if (params.count("service")) {
            startService = true;
        }
        if (params.count("master")) {
            master = true;
        }
        if (params.count("slave")) {
            slave = SimpleSlave;
        }
        if (params.count("source")) {
            /* specifies what the source in local.sources should be */
            cmdLine.source = params["source"].as<string>().c_str();
        }
        if (params.count("only")) {
            cmdLine.only = params["only"].as<string>().c_str();
        }
        if (params.count("pairwith")) {
            string paired = params["pairwith"].as<string>();
            if (params.count("arbiter")) {
                string arbiter = params["arbiter"].as<string>();
                pairWith(paired.c_str(), arbiter.c_str());
            } else {
                pairWith(paired.c_str(), "-");
            }
        } else if (params.count("arbiter")) {
            uasserted("specifying --arbiter without --pairwith");
        }
        if (params.count("autoresync")) {
            autoresync = true;
        }
        if( params.count("nssize") ) {
            int x = params["nssize"].as<int>();
            uassert("bad --nssize arg", x > 0 && x <= (0x7fffffff/1024/1024));
            lenForNewNsFiles = x * 1024 * 1024;
            assert(lenForNewNsFiles > 0);
        }
        if (params.count("oplogSize")) {
            long x = params["oplogSize"].as<long>();
            uassert("bad --oplogSize arg", x > 0);
            cmdLine.oplogSize = x * 1024 * 1024;
            assert(cmdLine.oplogSize > 0);
        }
        if (params.count("opIdMem")) {
            long x = params["opIdMem"].as<long>();
            uassert("bad --opIdMem arg", x > 0);
            opIdMem = x;
            assert(opIdMem > 0);
        }
        if (params.count("cacheSize")) {
            long x = params["cacheSize"].as<long>();
            uassert("bad --cacheSize arg", x > 0);
            setRecCacheSize(x);
        }
		if (params.count("port") == 0 ) { 
			if( params.count("configsvr") ) {
				cmdLine.port = CmdLine::ConfigServerPort;
			}
			if( params.count("shardsvr") )
				cmdLine.port = CmdLine::ShardServerPort;
		}
        if ( params.count("configsvr" ) && params.count( "diaglog" ) == 0 ){
            _diaglog.level = 1;
        }

        Module::configAll( params );
        dataFileSync.go();

        if (params.count("command")) {
            vector<string> command = params["command"].as< vector<string> >();

            if (command[0].compare("msg") == 0) {
                const char *m;

                if (command.size() < 3) {
                    cout << "Too few parameters to 'msg' command" << endl;
                    cout << visible_options << endl;
                    return 0;
                }

                m = command[1].c_str();

                msg(m, "127.0.0.1", atoi(command[2].c_str()));
                return 0;
            }
            if (command[0].compare("run") == 0) {
                if (command.size() > 1) {
                    cout << "Too many parameters to 'run' command" << endl;
                    cout << visible_options << endl;
                    return 0;
                }

                initAndListen(cmdLine.port);
                return 0;
            }

            if (command[0].compare("dbpath") == 0) {
                cout << dbpath << endl;
                return 0;
            }

            cout << "Invalid command: " << command[0] << endl;
            cout << visible_options << endl;
            return 0;
        }

#if defined(_WIN32)
        if ( installService ) {
            if ( !ServiceController::installService( L"MongoDB", L"Mongo DB", L"Mongo DB Server", argc, argv ) )
                dbexit( EXIT_NTSERVICE_ERROR );
            dbexit( EXIT_CLEAN );
        }
        else if ( removeService ) {
            if ( !ServiceController::removeService( L"MongoDB" ) )
                dbexit( EXIT_NTSERVICE_ERROR );
            dbexit( EXIT_CLEAN );
        }
        else if ( startService ) {
            if ( !ServiceController::startService( L"MongoDB", mongo::initService ) )
                dbexit( EXIT_NTSERVICE_ERROR );
            dbexit( EXIT_CLEAN );
        }
#endif
    }

    initAndListen(cmdLine.port, appsrvPath);
    dbexit(EXIT_CLEAN);
    return 0;
}

namespace mongo {

    /* we do not use log() below as it uses a mutex and that could cause deadlocks.
    */

    string getDbContext();

#undef out

    void exitCleanly() {
        goingAway = true;
        killCurrentOp = 1;
        {
            dblock lk;
            log() << "now exiting" << endl;
            dbexit( EXIT_KILL );        
        }
    }

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

        /*
        ostringstream ossOp;
        ossOp << "Last op: " << currentOp.infoNoauth() << endl;
        rawOut( ossOp.str() );
        */

        ostringstream oss;
        oss << "Backtrace:" << endl;
        printStackTrace( oss );
        rawOut( oss.str() );
        dbexit( EXIT_ABRUBT );
    }

    sigset_t asyncSignals;
    // The above signals will be processed by this thread only, in order to
    // ensure the db and log mutexes aren't held.
    void interruptThread() {
        int x;
        sigwait( &asyncSignals, &x );
        log() << "got kill or ctrl c signal " << x << " (" << strsignal( x ) << "), will terminate after current cmd ends" << endl;
        exitCleanly();
    }

    void setupSignals() {
        assert( signal(SIGSEGV, abruptQuit) != SIG_ERR );
        assert( signal(SIGFPE, abruptQuit) != SIG_ERR );
        assert( signal(SIGABRT, abruptQuit) != SIG_ERR );
        assert( signal(SIGBUS, abruptQuit) != SIG_ERR );
        assert( signal(SIGPIPE, pipeSigHandler) != SIG_ERR );
        assert( signal(SIGUSR1 , rotateLogs ) != SIG_ERR );

        setupSIGTRAPforGDB();

        sigemptyset( &asyncSignals );
        sigaddset( &asyncSignals, SIGINT );
        sigaddset( &asyncSignals, SIGTERM );
        assert( pthread_sigmask( SIG_SETMASK, &asyncSignals, 0 ) == 0 );
        boost::thread it( interruptThread );
    }

#else
void ctrlCTerminate() {
    log() << "got kill or ctrl c signal, will terminate after current cmd ends" << endl;
    exitCleanly();
}
BOOL CtrlHandler( DWORD fdwCtrlType )
{
    switch( fdwCtrlType )
    {
    case CTRL_C_EVENT:
        rawOut("Ctrl-C signal\n");
        ctrlCTerminate();
        return( TRUE );
    case CTRL_CLOSE_EVENT:
        rawOut("CTRL_CLOSE_EVENT signal\n");
        ctrlCTerminate();
        return( TRUE );
    case CTRL_BREAK_EVENT:
        rawOut("CTRL_BREAK_EVENT signal\n");
        ctrlCTerminate();
        return TRUE;
    case CTRL_LOGOFF_EVENT:
        rawOut("CTRL_LOGOFF_EVENT signal (ignored)\n");
        return FALSE;
    case CTRL_SHUTDOWN_EVENT:
         rawOut("CTRL_SHUTDOWN_EVENT signal (ignored)\n");
         return FALSE;
    default:
        return FALSE;
    }
}

    void setupSignals() {
        if( SetConsoleCtrlHandler( (PHANDLER_ROUTINE) CtrlHandler, TRUE ) )
            ;
        else
            massert("Couldn't register Windows Ctrl-C handler", false);
    }
#endif

void temptestfoo() {
    MongoMutex m;
    m.lock();
//    m.lock_upgrade();
    m.lock_shared();
}


} // namespace mongo

#include "recstore.h"
#include "reccache.h"

