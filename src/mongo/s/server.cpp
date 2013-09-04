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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include "mongo/pch.h"

#include "mongo/s/server.h"

#include <boost/thread/thread.hpp>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/client/connpool.h"
#include "mongo/db/auth/authz_manager_external_state_s.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/instance.h"
#include "mongo/db/lasterror.h"
#include "mongo/platform/process_id.h"
#include "mongo/s/balance.h"
#include "mongo/s/chunk.h"
#include "mongo/s/client_info.h"
#include "mongo/s/config.h"
#include "mongo/s/config_server_checker_service.h"
#include "mongo/s/config_upgrade.h"
#include "mongo/s/cursors.h"
#include "mongo/s/grid.h"
#include "mongo/s/mongos_options.h"
#include "mongo/s/request.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/admin_access.h"
#include "mongo/util/concurrency/task.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/message_server.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
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
    moe::Environment params;
    moe::OptionSection options("Allowed options");
    Database *database = 0;
    string mongosCommand;
    bool dbexitCalled = false;
    static bool scriptingEnabled = true;
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

                // Release connections after non-write op 
                if ( ShardConnection::releaseConnectionsAfterResponse && r.expectResponse() ) {
                    LOG(2) << "release thread local connections back to pool" << endl;
                    ShardConnection::releaseMyConnections();
                }
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

#ifndef _WIN32
    sigset_t asyncSignals;

    void signalProcessingThread() {
        while (true) {
            int actualSignal = 0;
            int status = sigwait( &asyncSignals, &actualSignal );
            fassert(16779, status == 0);
            switch (actualSignal) {
            case SIGUSR1:
                // log rotate signal
                fassert(16780, rotateLogs());
                break;
            default:
                // no one else should be here
                fassertFailed(16778);
                break;
            }
        }
    }

    void startSignalProcessingThread() {
        verify( pthread_sigmask( SIG_SETMASK, &asyncSignals, 0 ) == 0 );
        boost::thread it( signalProcessingThread );
    }
#endif  // not _WIN32

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

#ifndef _WIN32
        sigemptyset( &asyncSignals );
        sigaddset( &asyncSignals, SIGUSR1 );
        startSignalProcessingThread();
#endif

        setWindowsUnhandledExceptionFilter();
        set_new_handler( my_new_handler );
    }

    void init() {
        serverID.init();
    }

    void start( const MessageServer::Options& opts ) {
        balancer.go();
        cursorCache.startTimeoutThread();
        PeriodicTask::theRunner->go();

        ShardedMessageHandler handler;
        MessageServer * server = createServer( opts , &handler );
        server->setAsTimeTracker();
        server->setupSockets();
        server->run();
    }

    DBClientBase *createDirectClient() {
        uassert( 10197 ,  "createDirectClient not implemented for sharding yet" , 0 );
        return 0;
    }

    void printShardingVersionInfo( bool out ) {
        if ( out ) {
            cout << "MongoS version " << versionString << " starting: pid=" <<
                ProcessId::getCurrent() << " port=" << cmdLine.port <<
                ( sizeof(int*) == 4 ? " 32" : " 64" ) << "-bit host=" << getHostNameCached() <<
                " (--help for usage)" << endl;
            DEV cout << "_DEBUG build" << endl;
            cout << "git version: " << gitVersion() << endl;
            cout << openSSLVersion("OpenSSL version: ") << endl;
            cout <<  "build sys info: " << sysInfo() << endl;
        }
        else {
            log() << "MongoS version " << versionString << " starting: pid=" <<
                ProcessId::getCurrent() << " port=" << cmdLine.port <<
                ( sizeof( int* ) == 4 ? " 32" : " 64" ) << "-bit host=" << getHostNameCached() <<
                " (--help for usage)" << endl;
            DEV log() << "_DEBUG build" << endl;
            printGitVersion();
            printOpenSSLVersion();
            printSysInfo();
            printCommandLineOpts();
        }
    }

} // namespace mongo

using namespace mongo;

static bool runMongosServer( bool doUpgrade ) {
    setupSignalHandlers();
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

    startConfigServerChecker();

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

    if ( cmdLine.isHttpInterfaceEnabled )
        boost::thread web( boost::bind(&webServerThread, new NoAdminAccess() /* takes ownership */) );

    AuthorizationManager* authzManager = getGlobalAuthorizationManager();
    if (authzManager->isAuthEnabled()) {
        Status status = getGlobalAuthorizationManager()->initialize();
        if (!status.isOK()) {
            log() << "Initializing authorization data failed: " << status.toString() << endl;
            return false;
        }
    }

    MessageServer::Options opts;
    opts.port = cmdLine.port;
    opts.ipList = cmdLine.bind_ip;
    start(opts);

    // listen() will return when exit code closes its socket.
    dbexit( EXIT_NET_ERROR );
    return true;
}

static Status processCommandLineOptions(const std::vector<std::string>& argv) {
    Status ret = addMongosOptions(&options);
    if (!ret.isOK()) {
        StringBuilder sb;
        sb << "Error getting mongos options descriptions: " << ret.toString();
        return Status(ErrorCodes::InternalError, sb.str());
    }

    // parse options
    ret = CmdLine::store(argv, options, params);
    if (!ret.isOK()) {
        std::cerr << "Error parsing command line: " << ret.toString() << std::endl;
        ::_exit(EXIT_BADOPTIONS);
    }

    // The default value may vary depending on compile options, but for mongos
    // we want durability to be disabled.
    cmdLine.dur = false;

    if ( params.count( "help" ) ) {
        std::cout << options.helpString() << std::endl;
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
            std::cerr << "error: need a non-zero chunksize" << std::endl;
            ::_exit(EXIT_BADOPTIONS);
        }

        if ( !Chunk::setMaxChunkSizeSizeMB( csize ) ) {
            std::cerr << "MaxChunkSize invalid" << std::endl;
            ::_exit(EXIT_BADOPTIONS);
        }
    }

    if (params.count( "port" ) ) {
        int port = params["port"].as<int>();
        if ( port <= 0 || port > 65535 ) {
            out() << "error: port number must be between 1 and 65535" << endl;
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
        ::mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
                                                        ::mongo::logger::LogSeverity::Debug(5));
        StartupTest::runTests();
        ::_exit(EXIT_SUCCESS);
    }

    if (params.count("noscripting")) {
        scriptingEnabled = false;
    }

    if (params.count("httpinterface")) {
        if (params.count("nohttpinterface")) {
            std::cerr << "can't have both --httpinterface and --nohttpinterface" << std::endl;
            ::_exit(EXIT_BADOPTIONS);
        }
        cmdLine.isHttpInterfaceEnabled = true;
    }

    if (params.count("noAutoSplit")) {
        warning() << "running with auto-splitting disabled" << endl;
        Chunk::ShouldAutoSplit = false;
    }

    if ( ! params.count( "configdb" ) ) {
        std::cerr << "error: no args for --configdb" << std::endl;
        ::_exit(EXIT_BADOPTIONS);
    }

    splitStringDelim( params["configdb"].as<string>() , &configdbs , ',' );
    if ( configdbs.size() != 1 && configdbs.size() != 3 ) {
        std::cerr << "need either 1 or 3 configdbs" << std::endl;
        ::_exit(EXIT_BADOPTIONS);
    }

    if( configdbs.size() == 1 ) {
        warning() << "running with 1 config server should be done only for testing purposes and is not recommended for production" << endl;
    }

    _isUpgradeSwitchSet = params.count("upgrade");

    // dbpath currently must be linked in to mongos, but the directory should never be written to.
    dbpath = "";

    return Status::OK();
}

MONGO_INITIALIZER_GENERAL(ParseStartupConfiguration,
                            ("GlobalLogManager"),
                            ("default", "completedStartupConfig"))(InitializerContext* context) {

    Status ret = processCommandLineOptions(context->args());
    if (!ret.isOK()) {
        return ret;
    }

    return Status::OK();
}

MONGO_INITIALIZER_GENERAL(ForkServerOrDie,
                          ("completedStartupConfig"),
                          ("default"))(InitializerContext* context) {
    mongo::forkServerOrDie();
    return Status::OK();
}

/*
 * This function should contain the startup "actions" that we take based on the startup config.  It
 * is intended to separate the actions from "storage" and "validation" of our startup configuration.
 */
static void startupConfigActions(const std::vector<std::string>& argv) {
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

    return !runMongosServer(_isUpgradeSwitchSet);
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

MONGO_INITIALIZER_GENERAL(CreateAuthorizationManager,
                          ("SetupInternalSecurityUser"),
                          MONGO_NO_DEPENDENTS)
        (InitializerContext* context) {
    AuthorizationManager* authzManager =
                new AuthorizationManager(new AuthzManagerExternalStateMongos());
    authzManager->addInternalUser(internalSecurity.user);
    setGlobalAuthorizationManager(authzManager);
    return Status::OK();
}

#ifdef MONGO_SSL
MONGO_INITIALIZER_GENERAL(setSSLManagerType, 
                          MONGO_NO_PREREQUISITES, 
                          ("SSLManager"))(InitializerContext* context) {
    isSSLServer = true;
    return Status::OK();
}
#endif

int mongoSMain(int argc, char* argv[], char** envp) {
    static StaticObserver staticObserver;
    if (argc < 1)
        return EXIT_FAILURE;

    mongosCommand = argv[0];

    mongo::runGlobalInitializersOrDie(argc, argv, envp);
    startupConfigActions(std::vector<std::string>(argv, argv + argc));
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
