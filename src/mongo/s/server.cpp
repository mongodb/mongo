// server.cpp

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

#include <boost/thread/thread.hpp>

#include "mongo/base/initializer.h"
#include "mongo/client/connpool.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/lasterror.h"
#include "mongo/s/balance.h"
#include "mongo/s/chunk.h"
#include "mongo/s/client_info.h"
#include "mongo/s/config.h"
#include "mongo/s/config_upgrade.h"
#include "mongo/s/cursors.h"
#include "mongo/s/grid.h"
#include "mongo/s/request.h"
#include "mongo/s/server.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/admin_access.h"
#include "mongo/util/concurrency/task.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/message_server.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"

namespace {
    bool _isUpgradeSwitchSet = false;
}

namespace mongo {

#if defined(_WIN32)
    ntservice::NtServiceDefaultStrings defaultServiceStrings = {
        L"MongoS",
        L"Mongo DB Router",
        L"Mongo DB Sharding Router"
    };
    static void initService();
#endif

    CmdLine cmdLine;
    Database *database = 0;
    string mongosCommand;
    bool dbexitCalled = false;
    static bool scriptingEnabled = true;
    static bool noHttpInterface = false;
    static vector<string> configdbs;

    bool inShutdown() {
        return dbexitCalled;
    }

    string getDbContext() {
        return "?";
    }

    bool haveLocalShardingInfo( const string& ns ) {
        verify( 0 );
        return false;
    }

    class ShardedMessageHandler : public MessageHandler {
    public:
        virtual ~ShardedMessageHandler() {}

        virtual void connected( AbstractMessagingPort* p ) {
            ClientInfo::create(p);
        }

        virtual void process( Message& m , AbstractMessagingPort* p , LastError * le) {
            verify( p );
            Request r( m , p );

            verify( le );
            lastError.startRequest( m , le );

            try {
                r.init();
                r.process();
            }
            catch ( AssertionException & e ) {
                LOG( e.isUserAssertion() ? 1 : 0 ) << "AssertionException while processing op type : " << m.operation() << " to : " << r.getns() << causedBy(e) << endl;

                le->raiseError( e.getCode() , e.what() );

                m.header()->id = r.id();

                if ( r.expectResponse() ) {
                    BSONObj err = BSON( "$err" << e.what() << "code" << e.getCode() );
                    replyToQuery( ResultFlag_ErrSet, p , m , err );
                }
            }
            catch ( DBException& e ) {
                // note that e.toString() is more detailed on a SocketException than 
                // e.what().  we should think about what is the right level of detail both 
                // for logging and return code.
                log() << "DBException in process: " << e.what() << endl;

                le->raiseError( e.getCode() , e.what() );

                m.header()->id = r.id();

                if ( r.expectResponse() ) {
                    BSONObjBuilder b;
                    b.append("$err",e.what()).append("code",e.getCode());
                    if( !e._shard.empty() ) {
                        b.append("shard",e._shard);
                    }
                    replyToQuery( ResultFlag_ErrSet, p , m , b.obj() );
                }
            }
        }

        virtual void disconnected( AbstractMessagingPort* p ) {
            // all things are thread local
        }
    };

    void sighandler(int sig) {
        dbexit(EXIT_CLEAN, (string("received signal ") + BSONObjBuilder::numStr(sig)).c_str());
    }

    // this gets called when new fails to allocate memory
    void my_new_handler() {
        rawOut( "out of memory, printing stack and exiting:" );
        printStackTrace();
        ::_exit(EXIT_ABRUPT);
    }

    void setupSignalHandlers() {
        setupSIGTRAPforGDB();
        setupCoreSignals();

        signal(SIGTERM, sighandler);
        signal(SIGINT, sighandler);

#if defined(SIGQUIT)
        signal( SIGQUIT , printStackAndExit );
#endif
        signal( SIGSEGV , printStackAndExit );
        signal( SIGABRT , printStackAndExit );
        signal( SIGFPE , printStackAndExit );
#if defined(SIGBUS)
        signal( SIGBUS , printStackAndExit );
#endif
#if defined(SIGPIPE)
        signal( SIGPIPE , SIG_IGN );
#endif

        setWindowsUnhandledExceptionFilter();
        set_new_handler( my_new_handler );
    }

    void init() {
        serverID.init();
        setupSignalHandlers();
        Logstream::get().addGlobalTee( new RamLog("global") );
    }

    void start( const MessageServer::Options& opts ) {
        balancer.go();
        cursorCache.startTimeoutThread();
        PeriodicTask::theRunner->go();

        ShardedMessageHandler handler;
        MessageServer * server = createServer( opts , &handler );
        server->setAsTimeTracker();
        server->run();
    }

    DBClientBase *createDirectClient() {
        uassert( 10197 ,  "createDirectClient not implemented for sharding yet" , 0 );
        return 0;
    }

    void printShardingVersionInfo( bool out ) {
        if ( out ) {
            cout << "MongoS version " << versionString << " starting: pid=" << getpid() << " port=" << cmdLine.port <<
                    ( sizeof(int*) == 4 ? " 32" : " 64" ) << "-bit host=" << getHostNameCached() << " (--help for usage)" << endl;
            DEV cout << "_DEBUG build" << endl;
            cout << "git version: " << gitVersion() << endl;
            cout <<  "build sys info: " << sysInfo() << endl;
        }
        else {
            log() << "MongoS version " << versionString << " starting: pid=" << getpid() << " port=" << cmdLine.port <<
                    ( sizeof( int* ) == 4 ? " 32" : " 64" ) << "-bit host=" << getHostNameCached() << " (--help for usage)" << endl;
            DEV log() << "_DEBUG build" << endl;
            printGitVersion();
            printSysInfo();
            printCommandLineOpts();
        }
    }

} // namespace mongo

using namespace mongo;

static bool runMongosServer( bool doUpgrade ) {

    setThreadName( "mongosMain" );
    printShardingVersionInfo( false );

    // set some global state

    pool.addHook( new ShardingConnectionHook( false ) );
    pool.setName( "mongos connectionpool" );

    shardConnectionPool.addHook( new ShardingConnectionHook( true ) );
    shardConnectionPool.setName( "mongos shardconnection connectionpool" );

    // Mongos shouldn't lazily kill cursors, otherwise we can end up with extras from migration
    DBClientConnection::setLazyKillCursor( false );

    ReplicaSetMonitor::setConfigChangeHook( boost::bind( &ConfigServer::replicaSetChange , &configServer , _1 ) );

    if ( ! configServer.init( configdbs ) ) {
        log() << "couldn't resolve config db address" << endl;
        return false;
    }

    if ( ! configServer.ok( true ) ) {
        log() << "configServer connection startup check failed" << endl;
        return false;
    }

    {
        class CheckConfigServers : public task::Task {
            virtual string name() const { return "CheckConfigServers"; }
            virtual void doWork() { configServer.ok(true); }
        };

        task::repeat(new CheckConfigServers, 60*1000);
    }

    VersionType initVersionInfo;
    VersionType versionInfo;
    string errMsg;
    string configServerURL = configServer.getPrimary().getConnString();
    ConnectionString configServerConnString = ConnectionString::parse(configServerURL, errMsg);
    if (!configServerConnString.isValid()) {
        error() << "Invalid connection string for config servers: " << configServerURL << endl;
        return false;
    }
    bool upgraded = checkAndUpgradeConfigVersion(configServerConnString,
                                                 doUpgrade,
                                                 &initVersionInfo,
                                                 &versionInfo,
                                                 &errMsg);

    if (!upgraded) {
        error() << "error upgrading config database to v" << CURRENT_CONFIG_VERSION
                << causedBy(errMsg) << endl;
        return false;
    }

    configServer.reloadSettings();

    init();

#if !defined(_WIN32)
    CmdLine::launchOk();
#endif

    if ( !noHttpInterface )
        boost::thread web( boost::bind(&webServerThread, new NoAdminAccess() /* takes ownership */) );

    MessageServer::Options opts;
    opts.port = cmdLine.port;
    opts.ipList = cmdLine.bind_ip;
    start(opts);

    // listen() will return when exit code closes its socket.
    dbexit( EXIT_NET_ERROR );
    return true;
}

#include <boost/program_options.hpp>

namespace po = boost::program_options;

static void processCommandLineOptions(const std::vector<std::string>& argv) {
    po::options_description general_options("General options");
#if defined(_WIN32)
    po::options_description windows_scm_options("Windows Service Control Manager options");
#endif
    po::options_description ssl_options("SSL options");
    po::options_description sharding_options("Sharding options");
    po::options_description visible_options("Allowed options");
    po::options_description hidden_options("Hidden options");
    po::positional_options_description positional_options;

    CmdLine::addGlobalOptions( general_options, hidden_options, ssl_options );

    general_options.add_options()
    ("nohttpinterface", "disable http interface");

    hidden_options.add_options()
    ("noAutoSplit", "do not send split commands with writes");

#if defined(_WIN32)
    CmdLine::addWindowsOptions( windows_scm_options, hidden_options );
#endif

    sharding_options.add_options()
    ( "configdb" , po::value<string>() , "1 or 3 comma separated config servers" )
    ( "localThreshold", po::value <int>(), "ping time (in ms) for a node to be "
                                           "considered local (default 15ms)" )
    ( "test" , "just run unit tests" )
    ( "upgrade" , "upgrade meta data version" )
    ( "chunkSize" , po::value<int>(), "maximum amount of data per chunk" )
    ( "ipv6", "enable IPv6 support (disabled by default)" )
    ( "jsonp","allow JSONP access via http (has security implications)" )
    ( "noscripting", "disable scripting engine" )
    ;

    visible_options.add(general_options);

#if defined(_WIN32)
    visible_options.add(windows_scm_options);
#endif

    visible_options.add(sharding_options);

#ifdef MONGO_SSL
    visible_options.add(ssl_options);
#endif

    // parse options
    po::variables_map params;
    if (!CmdLine::store(argv,
                        visible_options,
                        hidden_options,
                        positional_options,
                        params)) {
        ::_exit(EXIT_FAILURE);
    }

    // The default value may vary depending on compile options, but for mongos
    // we want durability to be disabled.
    cmdLine.dur = false;

    if ( params.count( "help" ) ) {
        cout << visible_options << endl;
        ::_exit(EXIT_SUCCESS);
    }

    if ( params.count( "version" ) ) {
        printShardingVersionInfo(true);
        ::_exit(EXIT_SUCCESS);
    }

    if ( params.count( "chunkSize" ) ) {
        int csize = params["chunkSize"].as<int>();

        // validate chunksize before proceeding
        if ( csize == 0 ) {
            out() << "error: need a non-zero chunksize" << endl;
            ::_exit(EXIT_FAILURE);
        }

        if ( !Chunk::setMaxChunkSizeSizeMB( csize ) ) {
            out() << "MaxChunkSize invalid" << endl;
            ::_exit(EXIT_FAILURE);
        }
    }

    if ( params.count( "localThreshold" ) ) {
        cmdLine.defaultLocalThresholdMillis = params["localThreshold"].as<int>();
    }

    if ( params.count( "ipv6" ) ) {
        enableIPv6();
    }

    if ( params.count( "jsonp" ) ) {
        cmdLine.jsonp = true;
    }

    if ( params.count( "test" ) ) {
        logLevel = 5;
        StartupTest::runTests();
        cout << "tests passed" << endl;
        ::_exit(EXIT_SUCCESS);
    }

    if (params.count("noscripting")) {
        scriptingEnabled = false;
    }

    if (params.count("nohttpinterface")) {
        noHttpInterface = true;
    }

    if (params.count("noAutoSplit")) {
        warning() << "running with auto-splitting disabled" << endl;
        Chunk::ShouldAutoSplit = false;
    }

    if ( ! params.count( "configdb" ) ) {
        out() << "error: no args for --configdb" << endl;
        ::_exit(EXIT_FAILURE);
    }

    splitStringDelim( params["configdb"].as<string>() , &configdbs , ',' );
    if ( configdbs.size() != 1 && configdbs.size() != 3 ) {
        out() << "need either 1 or 3 configdbs" << endl;
        ::_exit(EXIT_FAILURE);
    }

    if( configdbs.size() == 1 ) {
        warning() << "running with 1 config server should be done only for testing purposes and is not recommended for production" << endl;
    }

    _isUpgradeSwitchSet = params.count("upgrade");

#if defined(_WIN32)
    vector<string> disallowedOptions;
    disallowedOptions.push_back( "upgrade" );
    ntservice::configureService(initService,
                                params,
                                defaultServiceStrings,
                                disallowedOptions,
                                argv);
#endif
}

static int _main() {

    if (!initializeServerGlobalState())
        return EXIT_FAILURE;

    // we either have a setting where all processes are in localhost or none are
    for ( vector<string>::const_iterator it = configdbs.begin() ; it != configdbs.end() ; ++it ) {
        try {

            HostAndPort configAddr( *it );  // will throw if address format is invalid

            if ( it == configdbs.begin() ) {
                grid.setAllowLocalHost( configAddr.isLocalHost() );
            }

            if ( configAddr.isLocalHost() != grid.allowLocalHost() ) {
                out() << "cannot mix localhost and ip addresses in configdbs" << endl;
                return 10;
            }

        }
        catch ( DBException& e) {
            out() << "configdb: " << e.what() << endl;
            return 9;
        }
    }

#if defined(_WIN32)
    if (ntservice::shouldStartService()) {
        ntservice::startService();
        // if we reach here, then we are not running as a service.  service installation
        // exits directly and so never reaches here either.
    }
#endif

    runMongosServer(_isUpgradeSwitchSet);
    return 0;
}

#if defined(_WIN32)
namespace mongo {
    static void initService() {
        ntservice::reportStatus( SERVICE_RUNNING );
        log() << "Service running" << endl;
        runMongosServer( false );
    }
}  // namespace mongo
#endif

int mongoSMain(int argc, char* argv[], char** envp) {
    static StaticObserver staticObserver;
    if (argc < 1)
        return EXIT_FAILURE;

    mongosCommand = argv[0];

    processCommandLineOptions(std::vector<std::string>(argv, argv + argc));
    mongo::runGlobalInitializersOrDie(argc, argv, envp);
    CmdLine::censor(argc, argv);
    try {
        int exitCode = _main();
        return exitCode;
    }
    catch(SocketException& e) {
        cout << "uncaught SocketException in mongos main:" << endl;
        cout << e.toString() << endl;
    }
    catch(DBException& e) {
        cout << "uncaught DBException in mongos main:" << endl;
        cout << e.toString() << endl;
    }
    catch(std::exception& e) {
        cout << "uncaught std::exception in mongos main:" << endl;
        cout << e.what() << endl;
    }
    catch(...) {
        cout << "uncaught unknown exception in mongos main" << endl;
    }
    return 20;
}

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables mongoSMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    WindowsCommandLine wcl(argc, argvW, envpW);
    int exitCode = mongoSMain(argc, wcl.argv(), wcl.envp());
    ::_exit(exitCode);
}
#else
int main(int argc, char* argv[], char** envp) {
    int exitCode = mongoSMain(argc, argv, envp);
    ::_exit(exitCode);
}
#endif

#undef exit

void mongo::exitCleanly( ExitCode code ) {
    // TODO: do we need to add anything?
    mongo::dbexit( code );
}

void mongo::dbexit( ExitCode rc, const char *why ) {
    dbexitCalled = true;
#if defined(_WIN32)
    if ( rc == EXIT_WINDOWS_SERVICE_STOP ) {
        log() << "dbexit: exiting because Windows service was stopped" << endl;
        return;
    }
#endif
    log() << "dbexit: " << why
          << " rc:" << rc
          << " " << ( why ? why : "" )
          << endl;
    ::_exit(rc);
}
