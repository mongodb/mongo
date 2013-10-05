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
#include "mongo/db/log_process_details.h"
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
#include "mongo/s/version_mongos.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/admin_access.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/task.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/gcov.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/message_server.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"

namespace mongo {

#if defined(_WIN32)
    ntservice::NtServiceDefaultStrings defaultServiceStrings = {
        L"MongoS",
        L"Mongo DB Router",
        L"Mongo DB Sharding Router"
    };
    static void initService();
#endif

    Database *database = 0;
    string mongosCommand;
    bool dbexitCalled = false;

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
                logProcessDetailsForLogRotate();
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

    if (!configServer.init(mongosGlobalParams.configdbs)) {
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
    mongo::signalForkSuccess();
#endif

    if (serverGlobalParams.isHttpInterfaceEnabled)
        boost::thread web( boost::bind(&webServerThread, new NoAdminAccess() /* takes ownership */) );

    Status status = getGlobalAuthorizationManager()->initialize();
    if (!status.isOK()) {
        log() << "Initializing authorization data failed: " << status;
        return false;
    }

    MessageServer::Options opts;
    opts.port = serverGlobalParams.port;
    opts.ipList = serverGlobalParams.bind_ip;
    start(opts);

    // listen() will return when exit code closes its socket.
    dbexit( EXIT_NET_ERROR );
    return true;
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
                                serverParsedOptions,
                                defaultServiceStrings,
                                disallowedOptions,
                                argv);
#endif
}

static int _main() {

    if (!initializeServerGlobalState())
        return EXIT_FAILURE;

    // we either have a setting where all processes are in localhost or none are
    for (std::vector<std::string>::const_iterator it = mongosGlobalParams.configdbs.begin();
         it != mongosGlobalParams.configdbs.end(); ++it) {
        try {

            HostAndPort configAddr( *it );  // will throw if address format is invalid

            if (it == mongosGlobalParams.configdbs.begin()) {
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

    return !runMongosServer(mongosGlobalParams.upgrade);
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
    cmdline_utils::censorArgvArray(argc, argv);
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
    flushForGcov();
    ::_exit(rc);
}
