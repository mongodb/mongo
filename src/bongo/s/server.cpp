/**
 *    Copyright (C) 2008-2015 BongoDB Inc.
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

#define BONGO_LOG_DEFAULT_COMPONENT ::bongo::logger::LogComponent::kSharding

#include "bongo/platform/basic.h"

#include "bongo/s/server.h"

#include <boost/optional.hpp>

#include "bongo/base/init.h"
#include "bongo/base/initializer.h"
#include "bongo/base/status.h"
#include "bongo/client/connpool.h"
#include "bongo/client/dbclient_rs.h"
#include "bongo/client/global_conn_pool.h"
#include "bongo/client/remote_command_targeter.h"
#include "bongo/client/remote_command_targeter_factory_impl.h"
#include "bongo/client/replica_set_monitor.h"
#include "bongo/config.h"
#include "bongo/db/audit.h"
#include "bongo/db/auth/authorization_manager.h"
#include "bongo/db/auth/authorization_manager_global.h"
#include "bongo/db/auth/authz_manager_external_state_s.h"
#include "bongo/db/auth/user_cache_invalidator_job.h"
#include "bongo/db/client.h"
#include "bongo/db/dbwebserver.h"
#include "bongo/db/initialize_server_global_state.h"
#include "bongo/db/lasterror.h"
#include "bongo/db/log_process_details.h"
#include "bongo/db/logical_clock.h"
#include "bongo/db/operation_context.h"
#include "bongo/db/server_options.h"
#include "bongo/db/service_context.h"
#include "bongo/db/service_context_noop.h"
#include "bongo/db/startup_warnings_common.h"
#include "bongo/db/wire_version.h"
#include "bongo/executor/task_executor_pool.h"
#include "bongo/platform/process_id.h"
#include "bongo/rpc/metadata/egress_metadata_hook_list.h"
#include "bongo/s/balancer_configuration.h"
#include "bongo/s/catalog/sharding_catalog_client.h"
#include "bongo/s/catalog/sharding_catalog_manager.h"
#include "bongo/s/client/shard_connection.h"
#include "bongo/s/client/shard_factory.h"
#include "bongo/s/client/shard_registry.h"
#include "bongo/s/client/shard_remote.h"
#include "bongo/s/client/sharding_connection_hook.h"
#include "bongo/s/grid.h"
#include "bongo/s/is_bongos.h"
#include "bongo/s/bongos_options.h"
#include "bongo/s/query/cluster_cursor_cleanup_job.h"
#include "bongo/s/query/cluster_cursor_manager.h"
#include "bongo/s/service_entry_point_bongos.h"
#include "bongo/s/sharding_egress_metadata_hook_for_bongos.h"
#include "bongo/s/sharding_egress_metadata_hook_for_bongos.h"
#include "bongo/s/sharding_initialization.h"
#include "bongo/s/sharding_uptime_reporter.h"
#include "bongo/s/version_bongos.h"
#include "bongo/stdx/memory.h"
#include "bongo/stdx/thread.h"
#include "bongo/transport/transport_layer_legacy.h"
#include "bongo/util/admin_access.h"
#include "bongo/util/cmdline_utils/censor_cmdline.h"
#include "bongo/util/concurrency/thread_name.h"
#include "bongo/util/exception_filter_win32.h"
#include "bongo/util/exit.h"
#include "bongo/util/fast_clock_source_factory.h"
#include "bongo/util/log.h"
#include "bongo/util/net/message.h"
#include "bongo/util/net/socket_exception.h"
#include "bongo/util/net/ssl_manager.h"
#include "bongo/util/ntservice.h"
#include "bongo/util/options_parser/startup_options.h"
#include "bongo/util/processinfo.h"
#include "bongo/util/quick_exit.h"
#include "bongo/util/signal_handlers.h"
#include "bongo/util/stacktrace.h"
#include "bongo/util/stringutils.h"
#include "bongo/util/system_clock_source.h"
#include "bongo/util/system_tick_source.h"
#include "bongo/util/text.h"
#include "bongo/util/version.h"

namespace bongo {

using std::string;
using std::vector;

using logger::LogComponent;

namespace {

boost::optional<ShardingUptimeReporter> shardingUptimeReporter;

}  // namespace

#if defined(_WIN32)
ntservice::NtServiceDefaultStrings defaultServiceStrings = {
    L"BongoS", L"BongoDB Router", L"BongoDB Sharding Router"};
static ExitCode initService();
#endif

// NOTE: This function may be called at any time after
// registerShutdownTask is called below. It must not depend on the
// prior execution of bongo initializers or the existence of threads.
static void cleanupTask() {
    {
        auto serviceContext = getGlobalServiceContext();
        Client::initThreadIfNotAlready();
        Client& client = cc();
        ServiceContext::UniqueOperationContext uniqueTxn;
        OperationContext* txn = client.getOperationContext();
        if (!txn) {
            uniqueTxn = client.makeOperationContext();
            txn = uniqueTxn.get();
        }

        if (serviceContext)
            serviceContext->setKillAllOperations();

        if (auto cursorManager = Grid::get(txn)->getCursorManager()) {
            cursorManager->shutdown();
        }
        if (auto pool = Grid::get(txn)->getExecutorPool()) {
            pool->shutdownAndJoin();
        }
        if (auto catalog = Grid::get(txn)->catalogClient(txn)) {
            catalog->shutDown(txn);
        }
    }

    audit::logShutdown(Client::getCurrent());
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

}  // namespace bongo

using namespace bongo;

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
        bongosGlobalParams.configdbs,
        generateDistLockProcessId(txn),
        std::move(shardFactory),
        []() {
            auto hookList = stdx::make_unique<rpc::EgressMetadataHookList>();
            // TODO SERVER-27750: add LogicalTimeMetadataHook
            hookList->addHook(stdx::make_unique<rpc::ShardingEgressMetadataHookForBongos>());
            return hookList;
        },
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
    // connect to version supporting Write Concern only
    spec.outgoing.minWireVersion = COMMANDS_ACCEPT_WRITE_CONCERN;
    spec.outgoing.maxWireVersion = COMMANDS_ACCEPT_WRITE_CONCERN;

    spec.isInternalClient = true;
}

static ExitCode runBongosServer() {
    Client::initThread("bongosMain");
    printShardingVersionInfo(false);

    _initWireSpec();

    transport::TransportLayerLegacy::Options opts;
    opts.port = serverGlobalParams.port;
    opts.ipList = serverGlobalParams.bind_ip;

    auto sep =
        stdx::make_unique<ServiceEntryPointBongos>(getGlobalServiceContext()->getTransportLayer());
    auto sepPtr = sep.get();

    getGlobalServiceContext()->setServiceEntryPoint(std::move(sep));

    auto transportLayer = stdx::make_unique<transport::TransportLayerLegacy>(opts, sepPtr);
    auto res = transportLayer->setup();
    if (!res.isOK()) {
        return EXIT_NET_ERROR;
    }

    // Add sharding hooks to both connection pools - ShardingConnectionHook includes auth hooks
    globalConnPool.addHook(new ShardingConnectionHook(
        false, stdx::make_unique<rpc::ShardingEgressMetadataHookForBongos>()));

    shardConnectionPool.addHook(new ShardingConnectionHook(
        true, stdx::make_unique<rpc::ShardingEgressMetadataHookForBongos>()));

    ReplicaSetMonitor::setAsynchronousConfigChangeHook(
        &ShardRegistry::replicaSetChangeConfigServerUpdateHook);
    ReplicaSetMonitor::setSynchronousConfigChangeHook(
        &ShardRegistry::replicaSetChangeShardRegistryUpdateHook);

    // Bongos connection pools already takes care of authenticating new connections so the
    // replica set connection shouldn't need to.
    DBClientReplicaSet::setAuthPooledSecondaryConn(false);

    if (getHostName().empty()) {
        quickExit(EXIT_BADOPTIONS);
    }

    auto opCtx = cc().makeOperationContext();

    std::array<std::uint8_t, 20> tempKey = {};
    TimeProofService::Key key(std::move(tempKey));
    auto timeProofService = stdx::make_unique<TimeProofService>(std::move(key));
    auto logicalClock = stdx::make_unique<LogicalClock>(
        opCtx->getServiceContext(),
        std::move(timeProofService),
        serverGlobalParams.authState == ServerGlobalParams::AuthState::kEnabled);
    LogicalClock::set(opCtx->getServiceContext(), std::move(logicalClock));

    {
        Status status = initializeSharding(opCtx.get());
        if (!status.isOK()) {
            if (status == ErrorCodes::CallbackCanceled) {
                invariant(globalInShutdownDeprecated());
                log() << "Shutdown called before bongos finished starting up";
                return EXIT_CLEAN;
            }
            error() << "Error initializing sharding system: " << status;
            return EXIT_SHARDING_ERROR;
        }

        Grid::get(opCtx.get())->getBalancerConfiguration()->refreshAndCheck(opCtx.get());
    }

    if (serverGlobalParams.isHttpInterfaceEnabled) {
        std::shared_ptr<DbWebServer> dbWebServer(new DbWebServer(serverGlobalParams.bind_ip,
                                                                 serverGlobalParams.port + 1000,
                                                                 getGlobalServiceContext(),
                                                                 new NoAdminAccess()));
        dbWebServer->setupSockets();

        stdx::thread web(stdx::bind(&webServerListenThread, dbWebServer));
        web.detach();
    }

    Status status = getGlobalAuthorizationManager()->initialize(NULL);
    if (!status.isOK()) {
        error() << "Initializing authorization data failed: " << status;
        return EXIT_SHARDING_ERROR;
    }

    // Construct the sharding uptime reporter after the startup parameters have been parsed in order
    // to ensure that it picks up the server port instead of reporting the default value.
    shardingUptimeReporter.emplace();
    shardingUptimeReporter->startPeriodicThread();

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

#if !defined(_WIN32)
    bongo::signalForkSuccess();
#else
    if (ntservice::shouldStartService()) {
        ntservice::reportStatus(SERVICE_RUNNING);
        log() << "Service running";
    }
#endif

    // Block until shutdown.
    return waitForShutdown();
}

namespace {
BONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
    bongo::forkServerOrDie();
    return Status::OK();
}
}  // namespace

// We set the featureCompatibilityVersion to 3.4 in the bongos so that BSON validation always uses
// BSONVersion::kLatest.
BONGO_INITIALIZER_WITH_PREREQUISITES(SetFeatureCompatibilityVersion34, ("EndStartupOptionStorage"))
(InitializerContext* context) {
    bongo::serverGlobalParams.featureCompatibility.version.store(
        ServerGlobalParams::FeatureCompatibility::Version::k34);
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

    auto shardingContext = Grid::get(getGlobalServiceContext());

    // we either have a setting where all processes are in localhost or none are
    std::vector<HostAndPort> configServers = bongosGlobalParams.configdbs.getServers();
    for (std::vector<HostAndPort>::const_iterator it = configServers.begin();
         it != configServers.end();
         ++it) {
        const HostAndPort& configAddr = *it;

        if (it == configServers.begin()) {
            shardingContext->setAllowLocalHost(configAddr.isLocalHost());
        }

        if (configAddr.isLocalHost() != shardingContext->allowLocalHost()) {
            bongo::log(LogComponent::kDefault)
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

    return runBongosServer();
}

#if defined(_WIN32)
namespace bongo {
static ExitCode initService() {
    return runBongosServer();
}
}  // namespace bongo
#endif

namespace {
std::unique_ptr<AuthzManagerExternalState> createAuthzManagerExternalStateBongos() {
    return stdx::make_unique<AuthzManagerExternalStateBongos>();
}

BONGO_INITIALIZER(CreateAuthorizationExternalStateFactory)(InitializerContext* context) {
    AuthzManagerExternalState::create = &createAuthzManagerExternalStateBongos;
    return Status::OK();
}

BONGO_INITIALIZER(SetGlobalEnvironment)(InitializerContext* context) {
    setGlobalServiceContext(stdx::make_unique<ServiceContextNoop>());
    getGlobalServiceContext()->setTickSource(stdx::make_unique<SystemTickSource>());
    getGlobalServiceContext()->setFastClockSource(stdx::make_unique<SystemClockSource>());
    getGlobalServiceContext()->setPreciseClockSource(stdx::make_unique<SystemClockSource>());
    return Status::OK();
}

#ifdef BONGO_CONFIG_SSL
BONGO_INITIALIZER_GENERAL(setSSLManagerType, BONGO_NO_PREREQUISITES, ("SSLManager"))
(InitializerContext* context) {
    isSSLServer = true;
    return Status::OK();
}
#endif

int bongoSMain(int argc, char* argv[], char** envp) {
    bongo::setBongos();

    if (argc < 1)
        return EXIT_FAILURE;

    registerShutdownTask(cleanupTask);

    setupSignalHandlers();

    Status status = bongo::runGlobalInitializers(argc, argv, envp);
    if (!status.isOK()) {
        severe(LogComponent::kDefault) << "Failed global initialization: " << status;
        quickExit(EXIT_FAILURE);
    }

    startupConfigActions(std::vector<std::string>(argv, argv + argc));
    cmdline_utils::censorArgvArray(argc, argv);

    bongo::logCommonStartupWarnings(serverGlobalParams);

    try {
        int exitCode = _main();
        return exitCode;
    } catch (const SocketException& e) {
        error() << "uncaught SocketException in bongos main: " << redact(e);
    } catch (const DBException& e) {
        error() << "uncaught DBException in bongos main: " << redact(e);
    } catch (const std::exception& e) {
        error() << "uncaught std::exception in bongos main:" << redact(e.what());
    } catch (...) {
        error() << "uncaught unknown exception in bongos main";
    }

    return 20;
}

}  // namespace

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables bongoSMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    WindowsCommandLine wcl(argc, argvW, envpW);
    int exitCode = bongoSMain(argc, wcl.argv(), wcl.envp());
    exitCleanly(ExitCode(exitCode));
}
#else
int main(int argc, char* argv[], char** envp) {
    int exitCode = bongoSMain(argc, argv, envp);
    exitCleanly(ExitCode(exitCode));
}
#endif
