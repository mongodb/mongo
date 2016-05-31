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

#include <boost/optional.hpp>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/remote_command_targeter_factory_impl.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authz_manager_external_state_s.h"
#include "mongo/db/auth/user_cache_invalidator_job.h"
#include "mongo/db/client.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/instance.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/startup_warnings_common.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/platform/process_id.h"
#include "mongo/s/balancer/balancer.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/client/sharding_connection_hook_for_mongos.h"
#include "mongo/s/commands/request.h"
#include "mongo/s/config.h"
#include "mongo/s/grid.h"
#include "mongo/s/mongos_options.h"
#include "mongo/s/query/cluster_cursor_cleanup_job.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/service_entry_point_mongos.h"
#include "mongo/s/sharding_egress_metadata_hook_for_mongos.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/s/sharding_uptime_reporter.h"
#include "mongo/s/version_mongos.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer_legacy.h"
#include "mongo/util/admin_access.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/exit.h"
#include "mongo/util/fast_clock_source_factory.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostname_canonicalization_worker.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/static_observer.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"

namespace mongo {

using std::string;
using std::vector;

using logger::LogComponent;

namespace {

boost::optional<ShardingUptimeReporter> shardingUptimeReporter;

}  // namespace

#if defined(_WIN32)
ntservice::NtServiceDefaultStrings defaultServiceStrings = {
    L"MongoS", L"MongoDB Router", L"MongoDB Sharding Router"};
static ExitCode initService();
#endif

// NOTE: This function may be called at any time after
// registerShutdownTask is called below. It must not depend on the
// prior execution of mongo initializers or the existence of threads.
static void cleanupTask() {
    {
        Client::initThreadIfNotAlready();
        Client& client = cc();
        ServiceContext::UniqueOperationContext uniqueTxn;
        OperationContext* txn = client.getOperationContext();
        if (!txn) {
            uniqueTxn = client.makeOperationContext();
            txn = uniqueTxn.get();
        }

        auto cursorManager = grid.getCursorManager();
        cursorManager->shutdown();
        grid.getExecutorPool()->shutdownAndJoin();
        grid.catalogClient(txn)->shutDown(txn);
    }

    audit::logShutdown(ClientBasic::getCurrent());
}

static BSONObj buildErrReply(const DBException& ex) {
    BSONObjBuilder errB;
    errB.append("$err", ex.what());
    errB.append("code", ex.getCode());
    if (!ex._shard.empty()) {
        errB.append("shard", ex._shard);
    }
    return errB.obj();
}

}  // namespace mongo

using namespace mongo;

static Status initializeSharding(OperationContext* txn) {
    auto targeterFactory = stdx::make_unique<RemoteCommandTargeterFactoryImpl>();
    auto targeterFactoryPtr = targeterFactory.get();

    ShardFactory::BuilderCallable setBuilder =
        [targeterFactoryPtr](const ShardId& shardId, const ConnectionString& connStr) {
            return stdx::make_unique<ShardRemote>(
                shardId, connStr, targeterFactoryPtr->create(connStr));
        };

    ShardFactory::BuilderCallable masterBuilder =
        [targeterFactoryPtr](const ShardId& shardId, const ConnectionString& connStr) {
            return stdx::make_unique<ShardRemote>(
                shardId, connStr, targeterFactoryPtr->create(connStr));
        };

    ShardFactory::BuildersMap buildersMap{
        {ConnectionString::SET, std::move(setBuilder)},
        {ConnectionString::MASTER, std::move(masterBuilder)},
    };

    auto shardFactory =
        stdx::make_unique<ShardFactory>(std::move(buildersMap), std::move(targeterFactory));

    Status status = initializeGlobalShardingState(
        txn,
        mongosGlobalParams.configdbs,
        generateDistLockProcessId(txn),
        std::move(shardFactory),
        []() { return stdx::make_unique<rpc::ShardingEgressMetadataHookForMongos>(); },
        [](ShardingCatalogClient* catalogClient, std::unique_ptr<executor::TaskExecutor> executor) {
            return nullptr;  // Only config servers get a real ShardingCatalogManager.
        });

    if (!status.isOK()) {
        return status;
    }

    status = reloadShardRegistryUntilSuccess(txn);
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

static void _initWireSpec() {
    WireSpec& spec = WireSpec::instance();
    // accept from any version
    spec.minWireVersionIncoming = RELEASE_2_4_AND_BEFORE;
    spec.maxWireVersionIncoming = COMMANDS_ACCEPT_WRITE_CONCERN;
    // connect to version supporting Write Concern only
    spec.minWireVersionOutgoing = COMMANDS_ACCEPT_WRITE_CONCERN;
    spec.maxWireVersionOutgoing = COMMANDS_ACCEPT_WRITE_CONCERN;
}

static ExitCode runMongosServer() {
    Client::initThread("mongosMain");
    printShardingVersionInfo(false);

    _initWireSpec();

    transport::TransportLayerLegacy::Options opts;
    opts.port = serverGlobalParams.port;
    opts.ipList = serverGlobalParams.bind_ip;

    auto sep =
        std::make_shared<ServiceEntryPointMongos>(getGlobalServiceContext()->getTransportLayer());

    auto transportLayer = stdx::make_unique<transport::TransportLayerLegacy>(opts, sep);
    auto res = transportLayer->setup();
    if (!res.isOK()) {
        return EXIT_NET_ERROR;
    }

    // Add sharding hooks to both connection pools - ShardingConnectionHook includes auth hooks
    globalConnPool.addHook(new ShardingConnectionHookForMongos(false));
    shardConnectionPool.addHook(new ShardingConnectionHookForMongos(true));

    ReplicaSetMonitor::setAsynchronousConfigChangeHook(
        &ConfigServer::replicaSetChangeConfigServerUpdateHook);
    ReplicaSetMonitor::setSynchronousConfigChangeHook(
        &ConfigServer::replicaSetChangeShardRegistryUpdateHook);

    // Mongos connection pools already takes care of authenticating new connections so the
    // replica set connection shouldn't need to.
    DBClientReplicaSet::setAuthPooledSecondaryConn(false);

    if (getHostName().empty()) {
        quickExit(EXIT_BADOPTIONS);
    }

    auto opCtx = cc().makeOperationContext();

    {
        Status status = initializeSharding(opCtx.get());
        if (!status.isOK()) {
            if (status == ErrorCodes::CallbackCanceled) {
                invariant(inShutdown());
                log() << "Shutdown called before mongos finished starting up";
                return EXIT_CLEAN;
            }
            error() << "Error initializing sharding system: " << status;
            return EXIT_SHARDING_ERROR;
        }

        Grid::get(opCtx.get())->getBalancerConfiguration()->refreshAndCheck(opCtx.get());
    }

#if !defined(_WIN32)
    mongo::signalForkSuccess();
#endif

    if (serverGlobalParams.isHttpInterfaceEnabled) {
        std::shared_ptr<DbWebServer> dbWebServer(new DbWebServer(serverGlobalParams.bind_ip,
                                                                 serverGlobalParams.port + 1000,
                                                                 getGlobalServiceContext(),
                                                                 new NoAdminAccess()));
        dbWebServer->setupSockets();

        stdx::thread web(stdx::bind(&webServerListenThread, dbWebServer));
        web.detach();
    }

    HostnameCanonicalizationWorker::start(getGlobalServiceContext());

    Status status = getGlobalAuthorizationManager()->initialize(NULL);
    if (!status.isOK()) {
        error() << "Initializing authorization data failed: " << status;
        return EXIT_SHARDING_ERROR;
    }

    // Construct the sharding uptime reporter after the startup parameters have been parsed in order
    // to ensure that it picks up the server port instead of reporting the default value.
    shardingUptimeReporter.emplace();
    shardingUptimeReporter->startPeriodicThread();

    Balancer::create(getGlobalServiceContext());

    clusterCursorCleanupJob.go();

    UserCacheInvalidator cacheInvalidatorThread(getGlobalAuthorizationManager());
    {
        cacheInvalidatorThread.initialize(opCtx.get());
        cacheInvalidatorThread.go();
    }

    PeriodicTask::startRunningPeriodicTasks();

    auto start = getGlobalServiceContext()->addAndStartTransportLayer(std::move(transportLayer));
    if (!start.isOK()) {
        return EXIT_NET_ERROR;
    }

    // Block until shutdown.
    return waitForShutdown();
}

MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
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
    disallowedOptions.push_back("upgrade");
    ntservice::configureService(
        initService, moe::startupOptionsParsed, defaultServiceStrings, disallowedOptions, argv);
#endif
}

static int _main() {
    if (!initializeServerGlobalState())
        return EXIT_FAILURE;

    startSignalProcessingThread();

    getGlobalServiceContext()->setFastClockSource(FastClockSourceFactory::create(Milliseconds{10}));

    // we either have a setting where all processes are in localhost or none are
    std::vector<HostAndPort> configServers = mongosGlobalParams.configdbs.getServers();
    for (std::vector<HostAndPort>::const_iterator it = configServers.begin();
         it != configServers.end();
         ++it) {
        const HostAndPort& configAddr = *it;

        if (it == configServers.begin()) {
            grid.setAllowLocalHost(configAddr.isLocalHost());
        }

        if (configAddr.isLocalHost() != grid.allowLocalHost()) {
            mongo::log(LogComponent::kDefault)
                << "cannot mix localhost and ip addresses in configdbs";
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

    return runMongosServer();
}

#if defined(_WIN32)
namespace mongo {
static ExitCode initService() {
    ntservice::reportStatus(SERVICE_RUNNING);
    log() << "Service running";

    return runMongosServer();
}
}  // namespace mongo
#endif

namespace {
std::unique_ptr<AuthzManagerExternalState> createAuthzManagerExternalStateMongos() {
    return stdx::make_unique<AuthzManagerExternalStateMongos>();
}

MONGO_INITIALIZER(CreateAuthorizationExternalStateFactory)(InitializerContext* context) {
    AuthzManagerExternalState::create = &createAuthzManagerExternalStateMongos;
    return Status::OK();
}

MONGO_INITIALIZER(SetGlobalEnvironment)(InitializerContext* context) {
    setGlobalServiceContext(stdx::make_unique<ServiceContextNoop>());
    getGlobalServiceContext()->setTickSource(stdx::make_unique<SystemTickSource>());
    getGlobalServiceContext()->setFastClockSource(stdx::make_unique<SystemClockSource>());
    getGlobalServiceContext()->setPreciseClockSource(stdx::make_unique<SystemClockSource>());
    return Status::OK();
}

#ifdef MONGO_CONFIG_SSL
MONGO_INITIALIZER_GENERAL(setSSLManagerType, MONGO_NO_PREREQUISITES, ("SSLManager"))
(InitializerContext* context) {
    isSSLServer = true;
    return Status::OK();
}
#endif

int mongoSMain(int argc, char* argv[], char** envp) {
    static StaticObserver staticObserver;
    if (argc < 1)
        return EXIT_FAILURE;

    registerShutdownTask(cleanupTask);

    setupSignalHandlers();

    Status status = mongo::runGlobalInitializers(argc, argv, envp);
    if (!status.isOK()) {
        severe(LogComponent::kDefault) << "Failed global initialization: " << status;
        quickExit(EXIT_FAILURE);
    }

    startupConfigActions(std::vector<std::string>(argv, argv + argc));
    cmdline_utils::censorArgvArray(argc, argv);

    mongo::logCommonStartupWarnings(serverGlobalParams);

    try {
        int exitCode = _main();
        return exitCode;
    } catch (const SocketException& e) {
        error() << "uncaught SocketException in mongos main: " << e.toString();
    } catch (const DBException& e) {
        error() << "uncaught DBException in mongos main: " << e.toString();
    } catch (const std::exception& e) {
        error() << "uncaught std::exception in mongos main:" << e.what();
    } catch (...) {
        error() << "uncaught unknown exception in mongos main";
    }

    return 20;
}

}  // namespace

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables mongoSMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    WindowsCommandLine wcl(argc, argvW, envpW);
    int exitCode = mongoSMain(argc, wcl.argv(), wcl.envp());
    exitCleanly(ExitCode(exitCode));
}
#else
int main(int argc, char* argv[], char** envp) {
    int exitCode = mongoSMain(argc, argv, envp);
    exitCleanly(ExitCode(exitCode));
}
#endif
