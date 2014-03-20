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
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authz_manager_external_state_s.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/user_cache_invalidator_job.h"
#include "mongo/db/client_basic.h"
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
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/signal_win32.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"

namespace mongo {

#if defined(_WIN32)
    ntservice::NtServiceDefaultStrings defaultServiceStrings = {
        L"MongoS",
        L"MongoDB Router",
        L"MongoDB Sharding Router"
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

    static BSONObj buildErrReply( const DBException& ex ) {
        BSONObjBuilder errB;
        errB.append( "$err", ex.what() );
        errB.append( "code", ex.getCode() );
        if ( !ex._shard.empty() ) {
            errB.append( "shard", ex._shard );
        }
        return errB.obj();
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
            catch ( const AssertionException& ex ) {

                LOG( ex.isUserAssertion() ? 1 : 0 ) << "Assertion failed"
                    << " while processing " << opToString( m.operation() ) << " op"
                    << " for " << r.getns() << causedBy( ex ) << endl;

                if ( r.expectResponse() ) {
                    m.header()->id = r.id();
                    replyToQuery( ResultFlag_ErrSet, p , m , buildErrReply( ex ) );
                }

                // We *always* populate the last error for now
                le->raiseError( ex.getCode() , ex.what() );
            }
            catch ( const DBException& ex ) {

                log() << "Exception thrown"
                      << " while processing " << opToString( m.operation() ) << " op"
                      << " for " << r.getns() << causedBy( ex ) << endl;

                if ( r.expectResponse() ) {
                    m.header()->id = r.id();
                    replyToQuery( ResultFlag_ErrSet, p , m , buildErrReply( ex ) );
                }

                // We *always* populate the last error for now
                le->raiseError( ex.getCode() , ex.what() );
            }

            // Release connections back to pool, if any still cached
            ShardConnection::releaseMyConnections();
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
#else

    void eventProcessingThread() {
        std::string eventName = getShutdownSignalName(ProcessId::getCurrent().asUInt32());

        HANDLE event = CreateEventA(NULL, TRUE, FALSE, eventName.c_str());
        if (event == NULL) {
            warning() << "eventProcessingThread CreateEvent failed: "
                << errnoWithDescription();
            return;
        }

        ON_BLOCK_EXIT(CloseHandle, event);

        int returnCode = WaitForSingleObject(event, INFINITE);
        if (returnCode != WAIT_OBJECT_0) {
            if (returnCode == WAIT_FAILED) {
                warning() << "eventProcessingThread WaitForSingleObject failed: "
                    << errnoWithDescription();
                return;
            }
            else {
                warning() << "eventProcessingThread WaitForSingleObject failed: "
                    << errnoWithDescription(returnCode);
                return;
            }
        }

        Client::initThread("eventTerminate");
        log() << "shutdown event signaled, will terminate after current cmd ends";
        exitCleanly(EXIT_CLEAN);
    }

    void startSignalProcessingThread() {
        boost::thread it(eventProcessingThread);
    }
#endif  // not _WIN32

    void setupSignalHandlers() {
        setupSIGTRAPforGDB();
        setupCoreSignals();

        signal(SIGTERM, sighandler);
        signal(SIGINT, sighandler);
#if defined(SIGXCPU)
        signal(SIGXCPU, sighandler);
#endif

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
#endif

        startSignalProcessingThread();

        setWindowsUnhandledExceptionFilter();
        set_new_handler( my_new_handler );
    }

    void init() {
        serverID.init();
    }

    void start( const MessageServer::Options& opts ) {
        balancer.go();
        cursorCache.startTimeoutThread();
        UserCacheInvalidator cacheInvalidatorThread(getGlobalAuthorizationManager());
        cacheInvalidatorThread.go();

        PeriodicTask::startRunningPeriodicTasks();

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

    ReplicaSetMonitor::setConfigChangeHook(
        boost::bind(&ConfigServer::replicaSetChange, &configServer, _1 , _2));

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

    if ( doUpgrade ) {
        log() << "Config database is at version v" << CURRENT_CONFIG_VERSION;
        return true;
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

MONGO_INITIALIZER_GENERAL(ForkServer,
                          ("EndStartupOptionHandling"),
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
                                moe::startupOptionsParsed,
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

    Status status = mongo::runGlobalInitializers(argc, argv, envp);
    if (!status.isOK()) {
        severe() << "Failed global initialization: " << status;
        ::_exit(EXIT_FAILURE);
    }

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
    audit::logShutdown(ClientBasic::getCurrent());
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
