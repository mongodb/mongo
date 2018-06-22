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
#include "mongo/db/ftdc/ftdc_mongos.h"
#include "mongo/db/generic_cursor_manager.h"
#include "mongo/db/generic_cursor_manager_mongos.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/kill_sessions.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_cache_factory_mongos.h"
#include "mongo/db/logical_time_metadata_hook.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/session_killer.h"
#include "mongo/db/startup_warnings_common.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/platform/process_id.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/client/sharding_connection_hook.h"
#include "mongo/s/commands/kill_sessions_remote.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/mongos_options.h"
#include "mongo/s/query/cluster_cursor_cleanup_job.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/service_entry_point_mongos.h"
#include "mongo/s/sharding_egress_metadata_hook_for_mongos.h"
#include "mongo/s/sharding_egress_metadata_hook_for_mongos.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/s/sharding_uptime_reporter.h"
#include "mongo/s/version_mongos.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/admin_access.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/exit.h"
#include "mongo/util/fast_clock_source_factory.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/stacktrace.h"
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

static constexpr auto kRetryInterval = Seconds{1};

Status waitForSigningKeys(OperationContext* opCtx) {
    while (true) {
        // this should be true when shard registry is up
        invariant(grid.shardRegistry()->isUp());
        auto configCS = grid.shardRegistry()->getConfigServerConnectionString();
        auto rsm = ReplicaSetMonitor::get(configCS.getSetName());
        // mongod will set minWireVersion == maxWireVersion for isMaster requests from
        // internalClient.
        if (rsm && (rsm->getMaxWireVersion() < WireVersion::SUPPORTS_OP_MSG ||
                    rsm->getMaxWireVersion() != rsm->getMinWireVersion())) {
            log() << "Not waiting for signing keys, not supported by the config shard "
                  << configCS.getSetName();
            return Status::OK();
        }
        auto stopStatus = opCtx->checkForInterruptNoAssert();
        if (!stopStatus.isOK()) {
            return stopStatus;
        }

        try {
            if (LogicalTimeValidator::get(opCtx)->shouldGossipLogicalTime()) {
                return Status::OK();
            }
            log() << "Waiting for signing keys, sleeping for " << kRetryInterval
                  << " and trying again.";
            sleepFor(kRetryInterval);
            continue;
        } catch (const DBException& ex) {
            Status status = ex.toStatus();
            warning() << "Error waiting for signing keys, sleeping for " << kRetryInterval
                      << " and trying again " << causedBy(status);
            sleepFor(kRetryInterval);
            continue;
        }
    }
}

}  // namespace

#if defined(_WIN32)
ntservice::NtServiceDefaultStrings defaultServiceStrings = {
    L"MongoS", L"MongoDB Router", L"MongoDB Sharding Router"};
static ExitCode initService();
#endif

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

// NOTE: This function may be called at any time after
// registerShutdownTask is called below. It must not depend on the
// prior execution of mongo initializers or the existence of threads.
static void cleanupTask() {
    {
        auto serviceContext = getGlobalServiceContext();
        Client::initThreadIfNotAlready();
        Client& client = cc();
        ServiceContext::UniqueOperationContext uniqueTxn;
        OperationContext* opCtx = client.getOperationContext();
        if (!opCtx) {
            uniqueTxn = client.makeOperationContext();
            opCtx = uniqueTxn.get();
        }

        if (serviceContext) {
            serviceContext->setKillAllOperations();

            // Shut down the background periodic task runner.
            auto runner = serviceContext->getPeriodicRunner();
            if (runner) {
                runner->shutdown();
            }
        }

        // Perform all shutdown operations after setKillAllOperations is called in order to ensure
        // that any pending threads are about to terminate

        if (auto validator = LogicalTimeValidator::get(serviceContext)) {
            validator->shutDown();
        }

        if (auto cursorManager = Grid::get(opCtx)->getCursorManager()) {
            cursorManager->shutdown(opCtx);
        }

        if (auto pool = Grid::get(opCtx)->getExecutorPool()) {
            pool->shutdownAndJoin();
        }

        if (auto catalog = Grid::get(opCtx)->catalogClient()) {
            catalog->shutDown(opCtx);
        }

#if __has_feature(address_sanitizer)
        // When running under address sanitizer, we get false positive leaks due to disorder around
        // the lifecycle of a connection and request. When we are running under ASAN, we try a lot
        // harder to dry up the server from active connections before going on to really shut down.

        // Shutdown the TransportLayer so that new connections aren't accepted
        if (auto tl = serviceContext->getTransportLayer()) {
            log(LogComponent::kNetwork)
                << "shutdown: going to close all sockets because ASAN is active...";

            tl->shutdown();
        }

        // Shut down the global dbclient pool so callers stop waiting for connections.
        shardConnectionPool.shutdown();

        // Shutdown the Service Entry Point and its sessions and give it a grace period to complete.
        if (auto sep = serviceContext->getServiceEntryPoint()) {
            if (!sep->shutdown(Seconds(10))) {
                log(LogComponent::kNetwork)
                    << "Service entry point failed to shutdown within timelimit.";
            }
        }

        // Shutdown and wait for the service executor to exit
        if (auto svcExec = serviceContext->getServiceExecutor()) {
            Status status = svcExec->shutdown(Seconds(5));
            if (!status.isOK()) {
                log(LogComponent::kNetwork)
                    << "Service executor failed to shutdown within timelimit: " << status.reason();
            }
        }
#endif

        // Shutdown Full-Time Data Capture
        stopMongoSFTDC();
    }

    audit::logShutdown(Client::getCurrent());
}

}  // namespace mongo

using namespace mongo;

static Status initializeSharding(OperationContext* opCtx) {
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

    CatalogCacheLoader::set(opCtx->getServiceContext(),
                            stdx::make_unique<ConfigServerCatalogCacheLoader>());

    Status status = initializeGlobalShardingState(
        opCtx,
        mongosGlobalParams.configdbs,
        generateDistLockProcessId(opCtx),
        std::move(shardFactory),
        stdx::make_unique<CatalogCache>(CatalogCacheLoader::get(opCtx)),
        [opCtx]() {
            auto hookList = stdx::make_unique<rpc::EgressMetadataHookList>();
            hookList->addHook(
                stdx::make_unique<rpc::LogicalTimeMetadataHook>(opCtx->getServiceContext()));
            hookList->addHook(stdx::make_unique<rpc::ShardingEgressMetadataHookForMongos>(
                opCtx->getServiceContext()));
            return hookList;
        },
        boost::none);

    if (!status.isOK()) {
        return status;
    }

    status = waitForShardRegistryReload(opCtx);
    if (!status.isOK()) {
        return status;
    }

    status = waitForSigningKeys(opCtx);
    if (!status.isOK()) {
        return status;
    }

    Grid::get(opCtx)->setShardingInitialized();

    return Status::OK();
}

static void _initWireSpec() {
    WireSpec& spec = WireSpec::instance();
    // Since the upgrade order calls for upgrading mongos last, it only needs to talk the latest
    // wire version. This ensures that users will get errors if they upgrade in the wrong order.
    spec.outgoing.minWireVersion = LATEST_WIRE_VERSION;
    spec.outgoing.maxWireVersion = LATEST_WIRE_VERSION;

    spec.isInternalClient = true;
}

static ExitCode runMongosServer() {
    Client::initThread("mongosMain");
    printShardingVersionInfo(false);

    _initWireSpec();

    auto sep = stdx::make_unique<ServiceEntryPointMongos>(getGlobalServiceContext());
    getGlobalServiceContext()->setServiceEntryPoint(std::move(sep));

    auto tl = transport::TransportLayerManager::createWithConfig(&serverGlobalParams,
                                                                 getGlobalServiceContext());
    auto res = tl->setup();
    if (!res.isOK()) {
        error() << "Failed to set up listener: " << res;
        return EXIT_NET_ERROR;
    }
    getGlobalServiceContext()->setTransportLayer(std::move(tl));

    auto unshardedHookList = stdx::make_unique<rpc::EgressMetadataHookList>();
    unshardedHookList->addHook(
        stdx::make_unique<rpc::LogicalTimeMetadataHook>(getGlobalServiceContext()));
    unshardedHookList->addHook(
        stdx::make_unique<rpc::ShardingEgressMetadataHookForMongos>(getGlobalServiceContext()));

    // Add sharding hooks to both connection pools - ShardingConnectionHook includes auth hooks
    globalConnPool.addHook(new ShardingConnectionHook(false, std::move(unshardedHookList)));

    auto shardedHookList = stdx::make_unique<rpc::EgressMetadataHookList>();
    shardedHookList->addHook(
        stdx::make_unique<rpc::LogicalTimeMetadataHook>(getGlobalServiceContext()));
    shardedHookList->addHook(
        stdx::make_unique<rpc::ShardingEgressMetadataHookForMongos>(getGlobalServiceContext()));

    shardConnectionPool.addHook(new ShardingConnectionHook(true, std::move(shardedHookList)));

    ReplicaSetMonitor::setAsynchronousConfigChangeHook(
        &ShardRegistry::replicaSetChangeConfigServerUpdateHook);
    ReplicaSetMonitor::setSynchronousConfigChangeHook(
        &ShardRegistry::replicaSetChangeShardRegistryUpdateHook);

    // Mongos connection pools already takes care of authenticating new connections so the
    // replica set connection shouldn't need to.
    DBClientReplicaSet::setAuthPooledSecondaryConn(false);

    if (getHostName().empty()) {
        quickExit(EXIT_BADOPTIONS);
    }

    auto opCtx = cc().makeOperationContext();

    auto logicalClock = stdx::make_unique<LogicalClock>(opCtx->getServiceContext());
    LogicalClock::set(opCtx->getServiceContext(), std::move(logicalClock));

    {
        Status status = initializeSharding(opCtx.get());
        if (!status.isOK()) {
            if (status == ErrorCodes::CallbackCanceled) {
                invariant(globalInShutdownDeprecated());
                log() << "Shutdown called before mongos finished starting up";
                return EXIT_CLEAN;
            }
            error() << "Error initializing sharding system: " << status;
            return EXIT_SHARDING_ERROR;
        }

        Grid::get(opCtx.get())
            ->getBalancerConfiguration()
            ->refreshAndCheck(opCtx.get())
            .transitional_ignore();
    }

    startMongoSFTDC();

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

    // Set up the periodic runner for background job execution
    auto runner = makePeriodicRunner(getGlobalServiceContext());
    runner->startup();
    getGlobalServiceContext()->setPeriodicRunner(std::move(runner));

    SessionKiller::set(
        getGlobalServiceContext(),
        std::make_shared<SessionKiller>(getGlobalServiceContext(), killSessionsRemote));

    GenericCursorManager::set(getGlobalServiceContext(),
                              stdx::make_unique<GenericCursorManagerMongos>());

    // Set up the logical session cache
    LogicalSessionCache::set(getGlobalServiceContext(), makeLogicalSessionCacheS());

    auto start = getGlobalServiceContext()->getServiceExecutor()->start();
    if (!start.isOK()) {
        error() << "Failed to start the service executor: " << start;
        return EXIT_NET_ERROR;
    }

    start = getGlobalServiceContext()->getServiceEntryPoint()->start();
    if (!start.isOK()) {
        error() << "Failed to start the service entry point: " << redact(start);
        return EXIT_NET_ERROR;
    }

    start = getGlobalServiceContext()->getTransportLayer()->start();
    if (!start.isOK()) {
        error() << "Failed to start the transport layer: " << redact(start);
        return EXIT_NET_ERROR;
    }

    getGlobalServiceContext()->notifyStartupComplete();
#if !defined(_WIN32)
    mongo::signalForkSuccess();
#else
    if (ntservice::shouldStartService()) {
        ntservice::reportStatus(SERVICE_RUNNING);
        log() << "Service running";
    }
#endif

    // Block until shutdown.
    MONGO_IDLE_THREAD_BLOCK;
    return waitForShutdown();
}

namespace {
MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
    mongo::forkServerOrDie();
    return Status::OK();
}
}  // namespace

// We set the featureCompatibilityVersion to 3.6 in the mongos and rely on the shards to reject
// usages of new features if their featureCompatibilityVersion is lower.
MONGO_INITIALIZER_WITH_PREREQUISITES(SetFeatureCompatibilityVersion36, ("EndStartupOptionStorage"))
(InitializerContext* context) {
    mongo::serverGlobalParams.featureCompatibility.setVersion(
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo36);
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
    std::vector<HostAndPort> configServers = mongosGlobalParams.configdbs.getServers();
    for (std::vector<HostAndPort>::const_iterator it = configServers.begin();
         it != configServers.end();
         ++it) {
        const HostAndPort& configAddr = *it;

        if (it == configServers.begin()) {
            shardingContext->setAllowLocalHost(configAddr.isLocalHost());
        }

        if (configAddr.isLocalHost() != shardingContext->allowLocalHost()) {
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
    mongo::setMongos();

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
        error() << "uncaught SocketException in mongos main: " << redact(e);
    } catch (const DBException& e) {
        error() << "uncaught DBException in mongos main: " << redact(e);
    } catch (const std::exception& e) {
        error() << "uncaught std::exception in mongos main:" << redact(e.what());
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
