/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kSharding

#include "merizo/platform/basic.h"

#include <boost/optional.hpp>

#include "merizo/base/init.h"
#include "merizo/base/initializer.h"
#include "merizo/base/status.h"
#include "merizo/client/connpool.h"
#include "merizo/client/dbclient_rs.h"
#include "merizo/client/global_conn_pool.h"
#include "merizo/client/remote_command_targeter.h"
#include "merizo/client/remote_command_targeter_factory_impl.h"
#include "merizo/client/replica_set_monitor.h"
#include "merizo/config.h"
#include "merizo/db/audit.h"
#include "merizo/db/auth/authorization_manager.h"
#include "merizo/db/auth/authz_manager_external_state_s.h"
#include "merizo/db/auth/user_cache_invalidator_job.h"
#include "merizo/db/client.h"
#include "merizo/db/ftdc/ftdc_merizos.h"
#include "merizo/db/initialize_server_global_state.h"
#include "merizo/db/initialize_server_security_state.h"
#include "merizo/db/kill_sessions.h"
#include "merizo/db/lasterror.h"
#include "merizo/db/log_process_details.h"
#include "merizo/db/logical_clock.h"
#include "merizo/db/logical_session_cache_factory_merizos.h"
#include "merizo/db/logical_time_metadata_hook.h"
#include "merizo/db/logical_time_validator.h"
#include "merizo/db/operation_context.h"
#include "merizo/db/server_options.h"
#include "merizo/db/service_context.h"
#include "merizo/db/session_killer.h"
#include "merizo/db/startup_warnings_common.h"
#include "merizo/db/wire_version.h"
#include "merizo/executor/task_executor_pool.h"
#include "merizo/platform/process_id.h"
#include "merizo/rpc/metadata/egress_metadata_hook_list.h"
#include "merizo/s/balancer_configuration.h"
#include "merizo/s/catalog_cache.h"
#include "merizo/s/client/shard_connection.h"
#include "merizo/s/client/shard_factory.h"
#include "merizo/s/client/shard_registry.h"
#include "merizo/s/client/shard_remote.h"
#include "merizo/s/client/sharding_connection_hook.h"
#include "merizo/s/commands/kill_sessions_remote.h"
#include "merizo/s/committed_optime_metadata_hook.h"
#include "merizo/s/config_server_catalog_cache_loader.h"
#include "merizo/s/grid.h"
#include "merizo/s/is_merizos.h"
#include "merizo/s/merizos_options.h"
#include "merizo/s/query/cluster_cursor_cleanup_job.h"
#include "merizo/s/query/cluster_cursor_manager.h"
#include "merizo/s/service_entry_point_merizos.h"
#include "merizo/s/sharding_egress_metadata_hook_for_merizos.h"
#include "merizo/s/sharding_egress_metadata_hook_for_merizos.h"
#include "merizo/s/sharding_initialization.h"
#include "merizo/s/sharding_uptime_reporter.h"
#include "merizo/s/version_merizos.h"
#include "merizo/stdx/memory.h"
#include "merizo/stdx/thread.h"
#include "merizo/transport/transport_layer_manager.h"
#include "merizo/util/admin_access.h"
#include "merizo/util/cmdline_utils/censor_cmdline.h"
#include "merizo/util/concurrency/idle_thread_block.h"
#include "merizo/util/concurrency/thread_name.h"
#include "merizo/util/exception_filter_win32.h"
#include "merizo/util/exit.h"
#include "merizo/util/fast_clock_source_factory.h"
#include "merizo/util/log.h"
#include "merizo/util/net/socket_exception.h"
#include "merizo/util/net/socket_utils.h"
#include "merizo/util/net/ssl_manager.h"
#include "merizo/util/ntservice.h"
#include "merizo/util/options_parser/startup_options.h"
#include "merizo/util/periodic_runner.h"
#include "merizo/util/periodic_runner_factory.h"
#include "merizo/util/processinfo.h"
#include "merizo/util/quick_exit.h"
#include "merizo/util/signal_handlers.h"
#include "merizo/util/stacktrace.h"
#include "merizo/util/stringutils.h"
#include "merizo/util/text.h"
#include "merizo/util/version.h"

namespace merizo {

using logger::LogComponent;

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

namespace {

#if defined(_WIN32)
const ntservice::NtServiceDefaultStrings defaultServiceStrings = {
    L"MongoS", L"MerizoDB Router", L"MerizoDB Sharding Router"};
#endif

constexpr auto kSignKeysRetryInterval = Seconds{1};

boost::optional<ShardingUptimeReporter> shardingUptimeReporter;

Status waitForSigningKeys(OperationContext* opCtx) {
    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();

    while (true) {
        // This should be true when shard registry is up
        invariant(shardRegistry->isUp());

        auto configCS = shardRegistry->getConfigServerConnectionString();
        auto rsm = ReplicaSetMonitor::get(configCS.getSetName());
        // merizod will set minWireVersion == maxWireVersion for isMaster requests from
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
            log() << "Waiting for signing keys, sleeping for " << kSignKeysRetryInterval
                  << " and trying again.";
            sleepFor(kSignKeysRetryInterval);
            continue;
        } catch (const DBException& ex) {
            Status status = ex.toStatus();
            warning() << "Error waiting for signing keys, sleeping for " << kSignKeysRetryInterval
                      << " and trying again " << causedBy(status);
            sleepFor(kSignKeysRetryInterval);
            continue;
        }
    }
}

/**
 * NOTE: This function may be called at any time after registerShutdownTask is called below. It must
 * not depend on the prior execution of merizo initializers or the existence of threads.
 */
void cleanupTask(ServiceContext* serviceContext) {
    {
        // This client initiation pattern is only to be used here, with plans to eliminate this
        // pattern down the line.
        if (!haveClient())
            Client::initThread(getThreadName());
        Client& client = cc();

        // Shutdown the TransportLayer so that new connections aren't accepted
        if (auto tl = serviceContext->getTransportLayer()) {
            log(LogComponent::kNetwork) << "shutdown: going to close all sockets...";

            tl->shutdown();
        }

        ServiceContext::UniqueOperationContext uniqueTxn;
        OperationContext* opCtx = client.getOperationContext();
        if (!opCtx) {
            uniqueTxn = client.makeOperationContext();
            opCtx = uniqueTxn.get();
        }

        opCtx->setIsExecutingShutdown();

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

        if (auto shardRegistry = Grid::get(opCtx)->shardRegistry()) {
            shardRegistry->shutdown();
        }

#if __has_feature(address_sanitizer)
        // When running under address sanitizer, we get false positive leaks due to disorder around
        // the lifecycle of a connection and request. When we are running under ASAN, we try a lot
        // harder to dry up the server from active connections before going on to really shut down.

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

Status initializeSharding(OperationContext* opCtx) {
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
        merizosGlobalParams.configdbs,
        generateDistLockProcessId(opCtx),
        std::move(shardFactory),
        stdx::make_unique<CatalogCache>(CatalogCacheLoader::get(opCtx)),
        [opCtx]() {
            auto hookList = stdx::make_unique<rpc::EgressMetadataHookList>();
            hookList->addHook(
                stdx::make_unique<rpc::LogicalTimeMetadataHook>(opCtx->getServiceContext()));
            hookList->addHook(
                stdx::make_unique<rpc::CommittedOpTimeMetadataHook>(opCtx->getServiceContext()));
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

void initWireSpec() {
    WireSpec& spec = WireSpec::instance();

    // Since the upgrade order calls for upgrading merizos last, it only needs to talk the latest
    // wire version. This ensures that users will get errors if they upgrade in the wrong order.
    spec.outgoing.minWireVersion = LATEST_WIRE_VERSION;
    spec.outgoing.maxWireVersion = LATEST_WIRE_VERSION;

    spec.isInternalClient = true;
}

ExitCode runMongosServer(ServiceContext* serviceContext) {
    Client::initThread("merizosMain");
    printShardingVersionInfo(false);

    initWireSpec();

    serviceContext->setServiceEntryPoint(
        stdx::make_unique<ServiceEntryPointMongos>(serviceContext));

    auto tl =
        transport::TransportLayerManager::createWithConfig(&serverGlobalParams, serviceContext);
    auto res = tl->setup();
    if (!res.isOK()) {
        error() << "Failed to set up listener: " << res;
        return EXIT_NET_ERROR;
    }
    serviceContext->setTransportLayer(std::move(tl));

    auto unshardedHookList = stdx::make_unique<rpc::EgressMetadataHookList>();
    unshardedHookList->addHook(stdx::make_unique<rpc::LogicalTimeMetadataHook>(serviceContext));
    unshardedHookList->addHook(
        stdx::make_unique<rpc::ShardingEgressMetadataHookForMongos>(serviceContext));
    // TODO SERVER-33053: readReplyMetadata is not called on hooks added through
    // ShardingConnectionHook with _shardedConnections=false, so this hook will not run for
    // connections using globalConnPool.
    unshardedHookList->addHook(stdx::make_unique<rpc::CommittedOpTimeMetadataHook>(serviceContext));

    // Add sharding hooks to both connection pools - ShardingConnectionHook includes auth hooks
    globalConnPool.addHook(new ShardingConnectionHook(false, std::move(unshardedHookList)));

    auto shardedHookList = stdx::make_unique<rpc::EgressMetadataHookList>();
    shardedHookList->addHook(stdx::make_unique<rpc::LogicalTimeMetadataHook>(serviceContext));
    shardedHookList->addHook(
        stdx::make_unique<rpc::ShardingEgressMetadataHookForMongos>(serviceContext));
    shardedHookList->addHook(stdx::make_unique<rpc::CommittedOpTimeMetadataHook>(serviceContext));

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
                log() << "Shutdown called before merizos finished starting up";
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

    Status status = AuthorizationManager::get(serviceContext)->initialize(opCtx.get());
    if (!status.isOK()) {
        error() << "Initializing authorization data failed: " << status;
        return EXIT_SHARDING_ERROR;
    }

    // Construct the sharding uptime reporter after the startup parameters have been parsed in order
    // to ensure that it picks up the server port instead of reporting the default value.
    shardingUptimeReporter.emplace();
    shardingUptimeReporter->startPeriodicThread();

    clusterCursorCleanupJob.go();

    UserCacheInvalidator cacheInvalidatorThread(AuthorizationManager::get(serviceContext));
    {
        cacheInvalidatorThread.initialize(opCtx.get());
        cacheInvalidatorThread.go();
    }

    PeriodicTask::startRunningPeriodicTasks();

    // Set up the periodic runner for background job execution
    auto runner = makePeriodicRunner(serviceContext);
    runner->startup();
    serviceContext->setPeriodicRunner(std::move(runner));

    SessionKiller::set(serviceContext,
                       std::make_shared<SessionKiller>(serviceContext, killSessionsRemote));

    // Set up the logical session cache
    LogicalSessionCache::set(serviceContext, makeLogicalSessionCacheS());

    status = serviceContext->getServiceExecutor()->start();
    if (!status.isOK()) {
        error() << "Failed to start the service executor: " << redact(status);
        return EXIT_NET_ERROR;
    }

    status = serviceContext->getServiceEntryPoint()->start();
    if (!status.isOK()) {
        error() << "Failed to start the service entry point: " << redact(status);
        return EXIT_NET_ERROR;
    }

    status = serviceContext->getTransportLayer()->start();
    if (!status.isOK()) {
        error() << "Failed to start the transport layer: " << redact(status);
        return EXIT_NET_ERROR;
    }

    serviceContext->notifyStartupComplete();

#if !defined(_WIN32)
    signalForkSuccess();
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

#if defined(_WIN32)
ExitCode initService() {
    return runMongosServer(getGlobalServiceContext());
}
#endif

/**
 * This function should contain the startup "actions" that we take based on the startup config. It
 * is intended to separate the actions from "storage" and "validation" of our startup configuration.
 */
void startupConfigActions(const std::vector<std::string>& argv) {
#if defined(_WIN32)
    std::vector<std::string> disallowedOptions;
    disallowedOptions.push_back("upgrade");
    ntservice::configureService(
        initService, moe::startupOptionsParsed, defaultServiceStrings, disallowedOptions, argv);
#endif
}

std::unique_ptr<AuthzManagerExternalState> createAuthzManagerExternalStateMongos() {
    return stdx::make_unique<AuthzManagerExternalStateMongos>();
}

ExitCode main(ServiceContext* serviceContext) {
    serviceContext->setFastClockSource(FastClockSourceFactory::create(Milliseconds{10}));

    auto const shardingContext = Grid::get(serviceContext);

    // We either have a setting where all processes are in localhost or none are
    std::vector<HostAndPort> configServers = merizosGlobalParams.configdbs.getServers();
    for (std::vector<HostAndPort>::const_iterator it = configServers.begin();
         it != configServers.end();
         ++it) {
        const HostAndPort& configAddr = *it;

        if (it == configServers.begin()) {
            shardingContext->setAllowLocalHost(configAddr.isLocalHost());
        }

        if (configAddr.isLocalHost() != shardingContext->allowLocalHost()) {
            log(LogComponent::kDefault) << "cannot mix localhost and ip addresses in configdbs";
            return EXIT_BADOPTIONS;
        }
    }

#if defined(_WIN32)
    if (ntservice::shouldStartService()) {
        ntservice::startService();
        // If we reach here, then we are not running as a service. Service installation exits
        // directly and so never reaches here either.
    }
#endif

    return runMongosServer(serviceContext);
}

namespace {
MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
    forkServerOrDie();
    return Status::OK();
}

// Initialize the featureCompatibilityVersion server parameter since merizos does not have a
// featureCompatibilityVersion document from which to initialize the parameter. The parameter is set
// to the latest version because there is no feature gating that currently occurs at the merizos
// level. The shards are responsible for rejecting usages of new features if their
// featureCompatibilityVersion is lower.
MONGO_INITIALIZER_WITH_PREREQUISITES(SetFeatureCompatibilityVersion42, ("EndStartupOptionStorage"))
(InitializerContext* context) {
    serverGlobalParams.featureCompatibility.setVersion(
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42);
    return Status::OK();
}

#ifdef MONGO_CONFIG_SSL
MONGO_INITIALIZER_GENERAL(setSSLManagerType, MONGO_NO_PREREQUISITES, ("SSLManager"))
(InitializerContext* context) {
    isSSLServer = true;
    return Status::OK();
}
#endif

}  // namespace

ExitCode merizoSMain(int argc, char* argv[], char** envp) {
    setMongos();

    if (argc < 1)
        return EXIT_BADOPTIONS;


    setupSignalHandlers();

    Status status = runGlobalInitializers(argc, argv, envp);
    if (!status.isOK()) {
        severe(LogComponent::kDefault) << "Failed global initialization: " << status;
        return EXIT_ABRUPT;
    }

    try {
        setGlobalServiceContext(ServiceContext::make());
    } catch (...) {
        auto cause = exceptionToStatus();
        severe(LogComponent::kDefault) << "Failed to create service context: " << redact(cause);
        return EXIT_ABRUPT;
    }

    const auto service = getGlobalServiceContext();

    registerShutdownTask([service]() { cleanupTask(service); });

    ErrorExtraInfo::invariantHaveAllParsers();

    startupConfigActions(std::vector<std::string>(argv, argv + argc));
    cmdline_utils::censorArgvArray(argc, argv);

    logCommonStartupWarnings(serverGlobalParams);

    try {
        if (!initializeServerGlobalState(service))
            return EXIT_ABRUPT;

        if (!initializeServerSecurityGlobalState(service))
            quickExit(EXIT_FAILURE);

        startSignalProcessingThread();

        return main(service);
    } catch (const DBException& e) {
        error() << "uncaught DBException in merizos main: " << redact(e);
        return EXIT_UNCAUGHT;
    } catch (const std::exception& e) {
        error() << "uncaught std::exception in merizos main:" << redact(e.what());
        return EXIT_UNCAUGHT;
    } catch (...) {
        error() << "uncaught unknown exception in merizos main";
        return EXIT_UNCAUGHT;
    }
}

}  // namespace
}  // namespace merizo

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables merizoSMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    merizo::WindowsCommandLine wcl(argc, argvW, envpW);
    merizo::exitCleanly(merizo::merizoSMain(argc, wcl.argv(), wcl.envp()));
}
#else
int main(int argc, char* argv[], char** envp) {
    merizo::exitCleanly(merizo::merizoSMain(argc, argv, envp));
}
#endif
