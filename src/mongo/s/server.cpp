/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/server.h"

#include <boost/shared_ptr.hpp>
#include <boost/thread/thread.hpp>
#include <iostream>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/client/connpool.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authz_manager_external_state_s.h"
#include "mongo/db/auth/user_cache_invalidator_job.h"
#include "mongo/db/client_basic.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/instance.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/startup_warnings_common.h"
#include "mongo/platform/process_id.h"
#include "mongo/s/balance.h"
#include "mongo/s/catalog/legacy/catalog_manager_legacy.h"
#include "mongo/s/catalog/legacy/config_upgrade.h"
#include "mongo/s/client/sharding_connection_hook.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/config.h"
#include "mongo/s/config_server_checker_service.h"
#include "mongo/s/cursors.h"
#include "mongo/s/grid.h"
#include "mongo/s/mongos_options.h"
#include "mongo/s/request.h"
#include "mongo/s/version_mongos.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/admin_access.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/task.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/message_server.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"

namespace mongo {

    using std::cout;
    using std::endl;
    using std::string;
    using std::vector;

    using logger::LogComponent;

#if defined(_WIN32)
    ntservice::NtServiceDefaultStrings defaultServiceStrings = {
        L"MongoS",
        L"MongoDB Router",
        L"MongoDB Sharding Router"
    };
    static ExitCode initService();
#endif

    string mongosCommand;
    bool dbexitCalled = false;

    bool inShutdown() {
        return dbexitCalled;
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

        virtual void connected(AbstractMessagingPort* p) {
            Client::initThread("conn", getGlobalServiceContext(), p);
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
                    m.header().setId(r.id());
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
                    m.header().setId(r.id());
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

    DBClientBase* createDirectClient(OperationContext* txn) {
        uassert( 10197 ,  "createDirectClient not implemented for sharding yet" , 0 );
        return 0;
    }

} // namespace mongo

using namespace mongo;

static ExitCode runMongosServer( bool doUpgrade ) {
    setThreadName( "mongosMain" );
    printShardingVersionInfo( false );

    // set some global state

    // Add sharding hooks to both connection pools - ShardingConnectionHook includes auth hooks
    pool.addHook( new ShardingConnectionHook( false ) );
    shardConnectionPool.addHook( new ShardingConnectionHook( true ) );

    // Mongos shouldn't lazily kill cursors, otherwise we can end up with extras from migration
    DBClientConnection::setLazyKillCursor( false );

    ReplicaSetMonitor::setConfigChangeHook(
        stdx::bind(&ConfigServer::replicaSetChange, &configServer, stdx::placeholders::_1 , stdx::placeholders::_2));

    // Mongos connection pools already takes care of authenticating new connections so the
    // replica set connection shouldn't need to.
    DBClientReplicaSet::setAuthPooledSecondaryConn(false);

    if (getHostName().empty()) {
        dbexit(EXIT_BADOPTIONS);
    }

    auto catalogManager = stdx::make_unique<CatalogManagerLegacy>();
    Status statusCatalogManagerInit = catalogManager->init(mongosGlobalParams.configdbs);
    if (!statusCatalogManagerInit.isOK()) {
        mongo::log(LogComponent::kSharding) << "couldn't initialize catalog manager "
                                            << statusCatalogManagerInit;
        return EXIT_SHARDING_ERROR;
    }

    grid.setCatalogManager(std::move(catalogManager));

    if (!configServer.init(mongosGlobalParams.configdbs)) {
        mongo::log(LogComponent::kSharding) << "couldn't resolve config db address" << endl;
        return EXIT_SHARDING_ERROR;
    }

    if (!configServer.ok(true)) {
        mongo::log(LogComponent::kSharding) << "configServer connection startup check failed" << endl;
        return EXIT_SHARDING_ERROR;
    }

    startConfigServerChecker();

    VersionType initVersionInfo;
    VersionType versionInfo;
    string errMsg;
    string configServerURL = configServer.getPrimary().getConnString();
    ConnectionString configServerConnString = ConnectionString::parse(configServerURL, errMsg);
    if (!configServerConnString.isValid()) {
        error(LogComponent::kDefault) << "Invalid connection string for config servers: " << configServerURL << endl;
        return EXIT_SHARDING_ERROR;
    }
    bool upgraded = checkAndUpgradeConfigVersion(configServerConnString,
                                                 doUpgrade,
                                                 &initVersionInfo,
                                                 &versionInfo,
                                                 &errMsg);

    if (!upgraded) {
        error(LogComponent::kDefault) << "error upgrading config database to v"
                << CURRENT_CONFIG_VERSION
                << causedBy(errMsg) << endl;
        return EXIT_SHARDING_ERROR;
    }

    if ( doUpgrade ) {
        mongo::log(LogComponent::kDefault) << "Config database is at version v"
                << CURRENT_CONFIG_VERSION;
        return EXIT_CLEAN;
    }

    configServer.reloadSettings();

#if !defined(_WIN32)
    mongo::signalForkSuccess();
#endif

    if (serverGlobalParams.isHttpInterfaceEnabled) {
        boost::shared_ptr<DbWebServer> dbWebServer(
                                new DbWebServer(serverGlobalParams.bind_ip,
                                                serverGlobalParams.port + 1000,
                                                new NoAdminAccess()));
        dbWebServer->setupSockets();

        boost::thread web(stdx::bind(&webServerListenThread, dbWebServer));
        web.detach();
    }

    Status status = getGlobalAuthorizationManager()->initialize(NULL);
    if (!status.isOK()) {
        mongo::log(LogComponent::kDefault) << "Initializing authorization data failed: " << status;
        return EXIT_SHARDING_ERROR;
    }

    MessageServer::Options opts;
    opts.port = serverGlobalParams.port;
    opts.ipList = serverGlobalParams.bind_ip;
    start(opts);

    // listen() will return when exit code closes its socket.
    return EXIT_NET_ERROR;
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

    startSignalProcessingThread();

    // we either have a setting where all processes are in localhost or none are
    std::vector<HostAndPort> configServers = mongosGlobalParams.configdbs.getServers();
    for (std::vector<HostAndPort>::const_iterator it = configServers.begin();
            it != configServers.end(); ++it) {

        const HostAndPort& configAddr = *it;

        if (it == configServers.begin()) {
            grid.setAllowLocalHost( configAddr.isLocalHost() );
        }

        if ( configAddr.isLocalHost() != grid.allowLocalHost() ) {
            mongo::log(LogComponent::kDefault)
                << "cannot mix localhost and ip addresses in configdbs" << endl;
            return 10;
        }
    }

#if defined(_WIN32)
    if (ntservice::shouldStartService()) {
        ntservice::startService();
        // if we reach here, then we are not running as a service.  service installation
        // exits directly and so never reaches here either.
    }
#endif

    ExitCode exitCode = runMongosServer(mongosGlobalParams.upgrade);

    // To maintain backwards compatibility, we exit with EXIT_NET_ERROR if the listener loop returns.
    if (exitCode == EXIT_NET_ERROR) {
        dbexit( EXIT_NET_ERROR );
    }

    return (exitCode == EXIT_CLEAN) ? 0 : 1;
}

#if defined(_WIN32)
namespace mongo {
    static ExitCode initService() {
        ntservice::reportStatus( SERVICE_RUNNING );
        log() << "Service running" << endl;

        ExitCode exitCode = runMongosServer(mongosGlobalParams.upgrade);

        // ignore EXIT_NET_ERROR on clean shutdown since we return this when the listening socket
        // is closed
        return (exitCode == EXIT_NET_ERROR && inShutdown()) ? EXIT_CLEAN : exitCode;
    }
}  // namespace mongo
#endif

MONGO_INITIALIZER_GENERAL(CreateAuthorizationManager,
                          ("SetupInternalSecurityUser",
                           "OIDGeneration",
                           "SetGlobalEnvironment",
                           "EndStartupOptionStorage"),
                          MONGO_NO_DEPENDENTS)
        (InitializerContext* context) {
    auto authzManager = stdx::make_unique<AuthorizationManager>(
            new AuthzManagerExternalStateMongos());
    authzManager->setAuthEnabled(serverGlobalParams.isAuthEnabled);
    AuthorizationManager::set(getGlobalServiceContext(), std::move(authzManager));
    return Status::OK();
}

MONGO_INITIALIZER(SetGlobalEnvironment)(InitializerContext* context) {
    setGlobalServiceContext(stdx::make_unique<ServiceContextNoop>());
    return Status::OK();
}

#ifdef MONGO_CONFIG_SSL
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

    setupSignalHandlers(false);

    mongosCommand = argv[0];

    Status status = mongo::runGlobalInitializers(argc, argv, envp);
    if (!status.isOK()) {
        severe(LogComponent::kDefault) << "Failed global initialization: " << status;
        quickExit(EXIT_FAILURE);
    }

    startupConfigActions(std::vector<std::string>(argv, argv + argc));
    cmdline_utils::censorArgvArray(argc, argv);

    mongo::logCommonStartupWarnings();

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
    quickExit(exitCode);
}
#else
int main(int argc, char* argv[], char** envp) {
    int exitCode = mongoSMain(argc, argv, envp);
    quickExit(exitCode);
}
#endif

#undef exit

void mongo::signalShutdown() {
    // Notify all threads shutdown has started
    dbexitCalled = true;
}

void mongo::exitCleanly(ExitCode code) {
    // TODO: do we need to add anything?
    mongo::dbexit( code );
}

void mongo::dbexit( ExitCode rc, const char *why ) {
    dbexitCalled = true;
    audit::logShutdown(ClientBasic::getCurrent());

#if defined(_WIN32)
    // Windows Service Controller wants to be told when we are done shutting down
    // and call quickExit itself.
    //
    if ( rc == EXIT_WINDOWS_SERVICE_STOP ) {
        log() << "dbexit: exiting because Windows service was stopped" << endl;
        return;
    }
#endif
    log() << "dbexit: " << why
          << " rc:" << rc
          << endl;
    quickExit(rc);
}
