/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
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


#include "mongo/platform/basic.h"

#include "mongo/s/mongos_main.h"

#include <boost/optional.hpp>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/remote_command_targeter_factory_impl.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authz_manager_external_state_s.h"
#include "mongo/db/auth/user_cache_invalidator_job.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/client_metadata_propagation_egress_hook.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ftdc/ftdc_mongos.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/kill_sessions.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/logical_session_cache_impl.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/process_health/fault_manager.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_liaison_mongos.h"
#include "mongo/db/session_killer.h"
#include "mongo/db/startup_warnings_common.h"
#include "mongo/db/vector_clock_metadata_hook.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/cluster_server_parameter_refresher.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/process_id.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/client/sharding_connection_hook.h"
#include "mongo/s/commands/kill_sessions_remote.h"
#include "mongo/s/concurrency/locker_mongos_client_observer.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/load_balancer_support.h"
#include "mongo/s/mongos_options.h"
#include "mongo/s/mongos_server_parameters_gen.h"
#include "mongo/s/mongos_topology_coordinator.h"
#include "mongo/s/query/cluster_cursor_cleanup_job.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/read_write_concern_defaults_cache_lookup_mongos.h"
#include "mongo/s/service_entry_point_mongos.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/sessions_collection_sharded.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/s/sharding_uptime_reporter.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/version_mongos.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/admin_access.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/fast_clock_source_factory.h"
#include "mongo/util/latch_analyzer.h"
#include "mongo/util/net/ocsp/ocsp_manager.h"
#include "mongo/util/net/private/ssl_expiration.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

using logv2::LogComponent;

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

// Failpoint for disabling replicaSetChangeConfigServerUpdateHook calls on signaled mongos.
MONGO_FAIL_POINT_DEFINE(failReplicaSetChangeConfigServerUpdateHook);

namespace {

MONGO_FAIL_POINT_DEFINE(pauseWhileKillingOperationsAtShutdown);

#if defined(_WIN32)
const ntservice::NtServiceDefaultStrings defaultServiceStrings = {
    L"MongoS", L"MongoDB Router", L"MongoDB Sharding Router"};
#endif

constexpr auto kSignKeysRetryInterval = Seconds{1};

boost::optional<ShardingUptimeReporter> shardingUptimeReporter;

Status waitForSigningKeys(OperationContext* opCtx) {
    auto const shardRegistry = Grid::get(opCtx)->shardRegistry();

    while (true) {
        auto configCS = shardRegistry->getConfigServerConnectionString();
        auto rsm = ReplicaSetMonitor::get(configCS.getSetName());
        // mongod will set minWireVersion == maxWireVersion for hello requests from
        // internalClient.
        if (rsm && (rsm->getMaxWireVersion() < WireVersion::SUPPORTS_OP_MSG)) {
            LOGV2(22841, "Waiting for signing keys not supported by config shard");
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
            LOGV2(22842,
                  "Waiting for signing keys, sleeping for {signingKeysCheckInterval} and then"
                  " checking again",
                  "Waiting for signing keys, sleeping before checking again",
                  "signingKeysCheckInterval"_attr = Seconds(kSignKeysRetryInterval));
            sleepFor(kSignKeysRetryInterval);
            continue;
        } catch (const DBException& ex) {
            LOGV2_WARNING(22853,
                          "Error while waiting for signing keys, sleeping for"
                          " {signingKeysCheckInterval} and then checking again {error}",
                          "Error while waiting for signing keys, sleeping before checking again",
                          "signingKeysCheckInterval"_attr = Seconds(kSignKeysRetryInterval),
                          "error"_attr = ex);
            sleepFor(kSignKeysRetryInterval);
            continue;
        }
    }
}


/**
 * Abort all active transactions in the catalog that has not yet been committed.
 *
 * Outline:
 * 1. Mark all sessions as killed and collect killTokens from each session.
 * 2. Create a new Client in order not to pollute the current OperationContext.
 * 3. Create new OperationContexts for each session to be killed and perform the necessary setup
 *    to be able to abort transactions properly: like setting TxnNumber and attaching the session
 *    to the OperationContext.
 * 4. Send abortTransaction.
 */
void implicitlyAbortAllTransactions(OperationContext* opCtx) {
    struct AbortTransactionDetails {
    public:
        AbortTransactionDetails(LogicalSessionId _lsid, SessionCatalog::KillToken _killToken)
            : lsid(std::move(_lsid)), killToken(std::move(_killToken)) {}

        LogicalSessionId lsid;
        SessionCatalog::KillToken killToken;
    };

    const auto catalog = SessionCatalog::get(opCtx);

    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});

    const auto abortDeadline =
        opCtx->getServiceContext()->getFastClockSource()->now() + Seconds(15);

    std::vector<AbortTransactionDetails> toKill;
    catalog->scanSessions(matcherAllSessions, [&](const ObservableSession& session) {
        toKill.emplace_back(session.getSessionId(),
                            session.kill(ErrorCodes::InterruptedAtShutdown));
    });

    auto newClient = opCtx->getServiceContext()->makeClient("ImplicitlyAbortTxnAtShutdown");
    AlternativeClientRegion acr(newClient);

    Status shutDownStatus(ErrorCodes::InterruptedAtShutdown,
                          "aborting transactions due to shutdown");

    for (auto& killDetails : toKill) {
        auto uniqueNewOpCtx = cc().makeOperationContext();
        auto newOpCtx = uniqueNewOpCtx.get();

        newOpCtx->setDeadlineByDate(abortDeadline, ErrorCodes::ExceededTimeLimit);

        OperationContextSession sessionCtx(newOpCtx, std::move(killDetails.killToken));

        auto session = OperationContextSession::get(newOpCtx);
        newOpCtx->setLogicalSessionId(session->getSessionId());

        auto txnRouter = TransactionRouter::get(newOpCtx);
        if (txnRouter.isInitialized()) {
            txnRouter.implicitlyAbortTransaction(newOpCtx, shutDownStatus);
        }
    }
}

/**
 * NOTE: This function may be called at any time after registerShutdownTask is called below. It must
 * not depend on the prior execution of mongo initializers or the existence of threads.
 */
void cleanupTask(const ShutdownTaskArgs& shutdownArgs) {
    const auto serviceContext = getGlobalServiceContext();
    {
        // This client initiation pattern is only to be used here, with plans to eliminate this
        // pattern down the line.
        if (!haveClient())
            Client::initThread(getThreadName());
        Client& client = cc();

        ServiceContext::UniqueOperationContext uniqueTxn;
        OperationContext* opCtx = client.getOperationContext();
        if (!opCtx) {
            uniqueTxn = client.makeOperationContext();
            opCtx = uniqueTxn.get();
        }

        Milliseconds quiesceTime;
        if (shutdownArgs.quiesceTime) {
            quiesceTime = *shutdownArgs.quiesceTime;
        } else {
            // IDL gaurantees that quiesceTime is populated.
            invariant(!shutdownArgs.isUserInitiated);
            quiesceTime = Milliseconds(mongosShutdownTimeoutMillisForSignaledShutdown.load());
        }

        if (auto mongosTopCoord = MongosTopologyCoordinator::get(opCtx)) {
            mongosTopCoord->enterQuiesceModeAndWait(opCtx, quiesceTime);
        }

        // Shutdown the TransportLayer so that new connections aren't accepted
        if (auto tl = serviceContext->getTransportLayer()) {
            LOGV2_OPTIONS(
                22843, {LogComponent::kNetwork}, "shutdown: going to close all sockets...");

            tl->shutdown();
        }

        try {
            // Abort transactions while we can still send remote commands.
            implicitlyAbortAllTransactions(opCtx);
        } catch (const DBException& excep) {
            LOGV2_WARNING(22854,
                          "Encountered {error} while trying to abort all active transactions",
                          "Error aborting all active transactions",
                          "error"_attr = excep);
        }

        if (auto lsc = LogicalSessionCache::get(serviceContext)) {
            lsc->joinOnShutDown();
        }

        ReplicaSetMonitor::shutdown();

        opCtx->setIsExecutingShutdown();

        if (serviceContext) {
            serviceContext->setKillAllOperations();

            if (MONGO_unlikely(pauseWhileKillingOperationsAtShutdown.shouldFail())) {
                LOGV2(4701800, "pauseWhileKillingOperationsAtShutdown failpoint enabled");
                sleepsecs(1);
            }
            FailPoint* hangBeforeInterruptfailPoint =
                globalFailPointRegistry().find("hangBeforeCheckingMongosShutdownInterrupt");
            if (hangBeforeInterruptfailPoint) {
                hangBeforeInterruptfailPoint->setMode(FailPoint::Mode::off);
                sleepsecs(3);
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

        if (auto shardRegistry = Grid::get(opCtx)->shardRegistry()) {
            shardRegistry->shutdown();
        }

        if (Grid::get(serviceContext)->isShardingInitialized()) {
            CatalogCacheLoader::get(serviceContext).shutDown();
        }

        // Shutdown the Service Entry Point and its sessions and give it a grace period to complete.
        if (auto sep = serviceContext->getServiceEntryPoint()) {
            if (!sep->shutdown(Seconds(10))) {
                LOGV2_OPTIONS(22844,
                              {LogComponent::kNetwork},
                              "Service entry point did not shutdown within the time limit");
            }
        }

        // Shutdown Full-Time Data Capture
        stopMongoSFTDC();
    }

    audit::logShutdown(Client::getCurrent());

#ifndef MONGO_CONFIG_USE_RAW_LATCHES
    LatchAnalyzer::get(serviceContext).dump();
#endif

#ifdef MONGO_CONFIG_SSL
    OCSPManager::shutdown(serviceContext);
#endif
}

Status initializeSharding(OperationContext* opCtx) {
    auto targeterFactory = std::make_unique<RemoteCommandTargeterFactoryImpl>();
    auto targeterFactoryPtr = targeterFactory.get();

    ShardFactory::BuilderCallable setBuilder = [targeterFactoryPtr](
                                                   const ShardId& shardId,
                                                   const ConnectionString& connStr) {
        return std::make_unique<ShardRemote>(shardId, connStr, targeterFactoryPtr->create(connStr));
    };

    ShardFactory::BuilderCallable masterBuilder = [targeterFactoryPtr](
                                                      const ShardId& shardId,
                                                      const ConnectionString& connStr) {
        return std::make_unique<ShardRemote>(shardId, connStr, targeterFactoryPtr->create(connStr));
    };

    ShardFactory::BuildersMap buildersMap{
        {ConnectionString::ConnectionType::kReplicaSet, std::move(setBuilder)},
        {ConnectionString::ConnectionType::kStandalone, std::move(masterBuilder)},
    };

    auto shardFactory =
        std::make_unique<ShardFactory>(std::move(buildersMap), std::move(targeterFactory));

    CatalogCacheLoader::set(opCtx->getServiceContext(),
                            std::make_unique<ConfigServerCatalogCacheLoader>());

    auto catalogCache =
        std::make_unique<CatalogCache>(opCtx->getServiceContext(), CatalogCacheLoader::get(opCtx));

    // List of hooks which will be called by the ShardRegistry when it discovers a shard has been
    // removed.
    std::vector<ShardRegistry::ShardRemovalHook> shardRemovalHooks = {
        // Invalidate appropriate entries in the catalog cache when a shard is removed. It's safe to
        // capture the catalog cache pointer since the Grid (and therefore CatalogCache and
        // ShardRegistry) are never destroyed.
        [catCache = catalogCache.get()](const ShardId& removedShard) {
            catCache->invalidateEntriesThatReferenceShard(removedShard);
        }};

    if (!mongosGlobalParams.configdbs) {
        return {ErrorCodes::BadValue, "Unrecognized connection string."};
    }

    auto shardRegistry = std::make_unique<ShardRegistry>(opCtx->getServiceContext(),
                                                         std::move(shardFactory),
                                                         mongosGlobalParams.configdbs,
                                                         std::move(shardRemovalHooks));

    Status status = initializeGlobalShardingState(
        opCtx,
        std::move(catalogCache),
        std::move(shardRegistry),
        [opCtx]() {
            auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
            hookList->addHook(
                std::make_unique<rpc::VectorClockMetadataHook>(opCtx->getServiceContext()));
            hookList->addHook(std::make_unique<rpc::ClientMetadataPropagationEgressHook>());
            return hookList;
        },
        boost::none);

    if (!status.isOK()) {
        return status;
    }

    status = loadGlobalSettingsFromConfigServer(opCtx);
    if (!status.isOK()) {
        return status;
    }

    status = waitForSigningKeys(opCtx);
    if (!status.isOK()) {
        return status;
    }

    // Loading of routing information may fail. Since this is just an optimization (warmup), any
    // failure must not prevent mongos from starting.
    try {
        preCacheMongosRoutingInfo(opCtx);
    } catch (const DBException& ex) {
        LOGV2_WARNING(6203601, "Failed to warmup routing information", "error"_attr = redact(ex));
    }

    status = preWarmConnectionPool(opCtx);
    if (!status.isOK()) {
        return status;
    }

    Grid::get(opCtx)->setShardingInitialized();

    return Status::OK();
}

MONGO_INITIALIZER_WITH_PREREQUISITES(WireSpec, ("EndStartupOptionHandling"))(InitializerContext*) {
    // Since the upgrade order calls for upgrading mongos last, it only needs to talk the latest
    // wire version. This ensures that users will get errors if they upgrade in the wrong order.
    WireSpec::Specification spec;
    spec.outgoing.minWireVersion = LATEST_WIRE_VERSION;
    spec.outgoing.maxWireVersion = LATEST_WIRE_VERSION;
    spec.isInternalClient = true;

    WireSpec::instance().initialize(std::move(spec));
}

class ShardingReplicaSetChangeListener final
    : public ReplicaSetChangeNotifier::Listener,
      public std::enable_shared_from_this<ShardingReplicaSetChangeListener> {
public:
    ShardingReplicaSetChangeListener(ServiceContext* serviceContext)
        : _serviceContext(serviceContext) {}
    ~ShardingReplicaSetChangeListener() final = default;

    void onFoundSet(const Key& key) noexcept final {}

    void onConfirmedSet(const State& state) noexcept final {
        auto connStr = state.connStr;
        try {
            LOGV2(471693,
                  "Updating the shard registry with confirmed replica set",
                  "connectionString"_attr = connStr);
            Grid::get(_serviceContext)
                ->shardRegistry()
                ->updateReplSetHosts(connStr,
                                     ShardRegistry::ConnectionStringUpdateType::kConfirmed);
        } catch (const ExceptionForCat<ErrorCategory::ShutdownError>& e) {
            LOGV2(471694,
                  "Unable to update the shard registry with confirmed replica set",
                  "error"_attr = e);
        }

        auto setName = connStr.getSetName();
        bool updateInProgress = false;
        {
            stdx::lock_guard lock(_mutex);
            if (!_hasUpdateState(lock, setName)) {
                _updateStates.emplace(setName, std::make_shared<ReplSetConfigUpdateState>());
            }
            auto updateState = _updateStates.at(setName);
            updateState->nextUpdateToSend = connStr;
            updateInProgress = updateState->updateInProgress;
        }

        if (!updateInProgress) {
            _scheduleUpdateConfigServer(setName);
        }
    }

    void onPossibleSet(const State& state) noexcept final {
        try {
            Grid::get(_serviceContext)
                ->shardRegistry()
                ->updateReplSetHosts(state.connStr,
                                     ShardRegistry::ConnectionStringUpdateType::kPossible);
        } catch (const DBException& ex) {
            LOGV2_DEBUG(22849,
                        2,
                        "Unable to update sharding state with possible replica set due to {error}",
                        "Unable to update sharding state with possible replica set",
                        "error"_attr = ex);
        }
    }

    void onDroppedSet(const Key& key) noexcept final {}

private:
    // Schedules updates for replica set 'setName' on the config server. Loosly preserves ordering
    // of update execution. Newer updates will not be overwritten by older updates in config.shards.
    void _scheduleUpdateConfigServer(std::string setName) {
        ConnectionString update;
        {
            stdx::lock_guard lock(_mutex);
            if (!_hasUpdateState(lock, setName)) {
                return;
            }
            auto updateState = _updateStates.at(setName);
            if (updateState->updateInProgress) {
                return;
            }
            updateState->updateInProgress = true;
            update = updateState->nextUpdateToSend.get();
            updateState->nextUpdateToSend = boost::none;
        }

        auto executor = Grid::get(_serviceContext)->getExecutorPool()->getFixedExecutor();
        auto schedStatus =
            executor
                ->scheduleWork([self = shared_from_this(), setName, update](auto args) {
                    self->_updateConfigServer(args.status, setName, update);
                })
                .getStatus();
        if (ErrorCodes::isCancellationError(schedStatus.code())) {
            LOGV2_DEBUG(22848,
                        2,
                        "Unable to schedule updating sharding state with confirmed replica set due"
                        " to {error}",
                        "Unable to schedule updating sharding state with confirmed replica set",
                        "error"_attr = schedStatus);
            return;
        }
        uassertStatusOK(schedStatus);
    }

    void _updateConfigServer(Status status, std::string setName, ConnectionString update) {
        if (ErrorCodes::isCancellationError(status.code())) {
            stdx::lock_guard lock(_mutex);
            _updateStates.erase(setName);
            return;
        }

        if (MONGO_unlikely(failReplicaSetChangeConfigServerUpdateHook.shouldFail())) {
            _endUpdateConfigServer(setName, update);
            return;
        }

        try {
            LOGV2(22846,
                  "Updating sharding state with confirmed replica set",
                  "connectionString"_attr = update);
            ShardRegistry::updateReplicaSetOnConfigServer(_serviceContext, update);
        } catch (const ExceptionForCat<ErrorCategory::ShutdownError>& e) {
            LOGV2(22847,
                  "Unable to update sharding state with confirmed replica set",
                  "error"_attr = e);
        } catch (...) {
            _endUpdateConfigServer(setName, update);
            throw;
        }
        _endUpdateConfigServer(setName, update);
    }

    void _endUpdateConfigServer(std::string setName, ConnectionString update) {
        bool moreUpdates = false;
        {
            stdx::lock_guard lock(_mutex);
            invariant(_hasUpdateState(lock, setName));
            auto updateState = _updateStates.at(setName);
            updateState->updateInProgress = false;
            moreUpdates = (updateState->nextUpdateToSend != boost::none);
            if (!moreUpdates) {
                _updateStates.erase(setName);
            }
        }
        if (moreUpdates) {
            auto executor = Grid::get(_serviceContext)->getExecutorPool()->getFixedExecutor();
            executor->schedule([self = shared_from_this(), setName](auto args) {
                self->_scheduleUpdateConfigServer(setName);
            });
        }
    }

    // Returns true if a ReplSetConfigUpdateState exists for replica set setName.
    bool _hasUpdateState(WithLock, std::string setName) {
        return (_updateStates.find(setName) != _updateStates.end());
    }

    ServiceContext* _serviceContext;

    mutable Mutex _mutex = MONGO_MAKE_LATCH("ShardingReplicaSetChangeListenerMongod::mutex");

    struct ReplSetConfigUpdateState {
        // True when an update to the config.shards is in progress.
        bool updateInProgress = false;
        boost::optional<ConnectionString> nextUpdateToSend;
    };
    stdx::unordered_map<std::string, std::shared_ptr<ReplSetConfigUpdateState>> _updateStates;
};

ExitCode runMongosServer(ServiceContext* serviceContext) {
    ThreadClient tc("mongosMain", serviceContext);

    logMongosVersionInfo(nullptr);

    // Set up the periodic runner for background job execution
    {
        auto runner = makePeriodicRunner(serviceContext);
        serviceContext->setPeriodicRunner(std::move(runner));
    }

#ifdef MONGO_CONFIG_SSL
    OCSPManager::start(serviceContext);
    CertificateExpirationMonitor::get()->start(serviceContext);
#endif

    serviceContext->setServiceEntryPoint(std::make_unique<ServiceEntryPointMongos>(serviceContext));

    const auto loadBalancerPort = load_balancer_support::getLoadBalancerPort();
    if (loadBalancerPort && *loadBalancerPort == serverGlobalParams.port) {
        LOGV2_ERROR(6067901,
                    "Load balancer port must be different from the normal ingress port.",
                    "port"_attr = serverGlobalParams.port);
        quickExit(ExitCode::badOptions);
    }

    auto tl = transport::TransportLayerManager::createWithConfig(
        &serverGlobalParams, serviceContext, loadBalancerPort);
    auto res = tl->setup();
    if (!res.isOK()) {
        LOGV2_ERROR(22856,
                    "Error setting up listener: {error}",
                    "Error setting up listener",
                    "error"_attr = res);
        return ExitCode::netError;
    }
    serviceContext->setTransportLayer(std::move(tl));

    auto unshardedHookList = std::make_unique<rpc::EgressMetadataHookList>();
    unshardedHookList->addHook(std::make_unique<rpc::VectorClockMetadataHook>(serviceContext));
    unshardedHookList->addHook(std::make_unique<rpc::ClientMetadataPropagationEgressHook>());

    // Add sharding hooks to both connection pools - ShardingConnectionHook includes auth hooks
    globalConnPool.addHook(new ShardingConnectionHook(std::move(unshardedHookList)));

    // Hook up a Listener for changes from the ReplicaSetMonitor
    // This will last for the scope of this function. i.e. until shutdown finishes
    auto shardingRSCL =
        ReplicaSetMonitor::getNotifier().makeListener<ShardingReplicaSetChangeListener>(
            serviceContext);

    // Mongos connection pools already takes care of authenticating new connections so the
    // replica set connection shouldn't need to.
    DBClientReplicaSet::setAuthPooledSecondaryConn(false);

    if (getHostName().empty()) {
        quickExit(ExitCode::badOptions);
    }

    ReadWriteConcernDefaults::create(serviceContext, readWriteConcernDefaultsCacheLookupMongoS);
    ChangeStreamOptionsManager::create(serviceContext);

    auto opCtxHolder = tc->makeOperationContext();
    auto const opCtx = opCtxHolder.get();

    try {
        uassertStatusOK(initializeSharding(opCtx));
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::CallbackCanceled) {
            invariant(globalInShutdownDeprecated());
            LOGV2(22850, "Shutdown called before mongos finished starting up");
            return ExitCode::clean;
        }

        LOGV2_ERROR(22857,
                    "Error initializing sharding system: {error}",
                    "Error initializing sharding system",
                    "error"_attr = redact(ex));
        return ExitCode::shardingError;
    }

    Grid::get(serviceContext)
        ->getBalancerConfiguration()
        ->refreshAndCheck(opCtx)
        .transitional_ignore();

    try {
        ReadWriteConcernDefaults::get(serviceContext).refreshIfNecessary(opCtx);
    } catch (const DBException& ex) {
        LOGV2_WARNING(22855,
                      "Error loading read and write concern defaults at startup",
                      "error"_attr = redact(ex));
    }

    startMongoSFTDC();

    if (mongosGlobalParams.scriptingEnabled) {
        ScriptEngine::setup();
    }

    Status status = AuthorizationManager::get(serviceContext)->initialize(opCtx);
    if (!status.isOK()) {
        LOGV2_ERROR(22858,
                    "Error initializing authorization data: {error}",
                    "Error initializing authorization data",
                    "error"_attr = status);
        return ExitCode::shardingError;
    }

    // Construct the sharding uptime reporter after the startup parameters have been parsed in order
    // to ensure that it picks up the server port instead of reporting the default value.
    shardingUptimeReporter.emplace();
    shardingUptimeReporter->startPeriodicThread();

    clusterCursorCleanupJob.go();

    UserCacheInvalidator::start(serviceContext, opCtx);
    if (gFeatureFlagClusterWideConfigM2.isEnabled(serverGlobalParams.featureCompatibility)) {
        ClusterServerParameterRefresher::start(serviceContext, opCtx);
    }

    if (audit::initializeSynchronizeJob) {
        audit::initializeSynchronizeJob(serviceContext);
    }

    PeriodicTask::startRunningPeriodicTasks();

    status =
        process_health::FaultManager::get(serviceContext)->startPeriodicHealthChecks().getNoThrow();
    if (!status.isOK()) {
        LOGV2_ERROR(5936510,
                    "Error completing initial health check: {error}",
                    "Error completing initial health check",
                    "error"_attr = redact(status));
        return ExitCode::processHealthCheck;
    }

    SessionKiller::set(serviceContext,
                       std::make_shared<SessionKiller>(serviceContext, killSessionsRemote));

    LogicalSessionCache::set(
        serviceContext,
        std::make_unique<LogicalSessionCacheImpl>(std::make_unique<ServiceLiaisonMongos>(),
                                                  std::make_unique<SessionsCollectionSharded>(),
                                                  RouterSessionCatalog::reapSessionsOlderThan));

    status = serviceContext->getServiceEntryPoint()->start();
    if (!status.isOK()) {
        LOGV2_ERROR(22860,
                    "Error starting service entry point: {error}",
                    "Error starting service entry point",
                    "error"_attr = redact(status));
        return ExitCode::netError;
    }

    status = serviceContext->getTransportLayer()->start();
    if (!status.isOK()) {
        LOGV2_ERROR(22861,
                    "Error starting transport layer: {error}",
                    "Error starting transport layer",
                    "error"_attr = redact(status));
        return ExitCode::netError;
    }

    if (!initialize_server_global_state::writePidFile()) {
        return ExitCode::abrupt;
    }

    // Startup options are written to the audit log at the end of startup so that cluster server
    // parameters are guaranteed to have been initialized from disk at this point.
    audit::logStartupOptions(tc.get(), serverGlobalParams.parsedOpts);

    serviceContext->notifyStartupComplete();

#if !defined(_WIN32)
    initialize_server_global_state::signalForkSuccess();
#else
    if (ntservice::shouldStartService()) {
        ntservice::reportStatus(SERVICE_RUNNING);
        LOGV2(22851, "Service running");
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
    return std::make_unique<AuthzManagerExternalStateMongos>();
}

ExitCode main(ServiceContext* serviceContext) {
    serviceContext->setFastClockSource(FastClockSourceFactory::create(Milliseconds{10}));

    // We either have a setting where all processes are in localhost or none are
    const auto& configServers = mongosGlobalParams.configdbs.getServers();
    invariant(!configServers.empty());
    const auto allowLocalHost = configServers.front().isLocalHost();

    for (const auto& configServer : configServers) {
        if (configServer.isLocalHost() != allowLocalHost) {
            LOGV2_OPTIONS(22852,
                          {LogComponent::kDefault},
                          "cannot mix localhost and ip addresses in configdbs");
            return ExitCode::badOptions;
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

MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
    initialize_server_global_state::forkServerOrDie();
}

// Initialize the featureCompatibilityVersion server parameter since mongos does not have a
// featureCompatibilityVersion document from which to initialize the parameter. The parameter is set
// to the latest version because there is no feature gating that currently occurs at the mongos
// level. The shards are responsible for rejecting usages of new features if their
// featureCompatibilityVersion is lower.
MONGO_INITIALIZER_WITH_PREREQUISITES(SetFeatureCompatibilityVersionLatest,
                                     ("EndStartupOptionStorage"))
// (Generic FCV reference): This FCV reference should exist across LTS binary versions.
(InitializerContext* context) {
    serverGlobalParams.mutableFeatureCompatibility.setVersion(multiversion::GenericFCV::kLatest);
}

#ifdef MONGO_CONFIG_SSL
MONGO_INITIALIZER_GENERAL(setSSLManagerType, (), ("SSLManager"))
(InitializerContext* context) {
    isSSLServer = true;
}
#endif

}  // namespace

ExitCode mongos_main(int argc, char* argv[]) {
    setMongos();

    if (argc < 1)
        return ExitCode::badOptions;


    setupSignalHandlers();

    Status status = runGlobalInitializers(std::vector<std::string>(argv, argv + argc));
    if (!status.isOK()) {
        LOGV2_FATAL_OPTIONS(
            22865,
            logv2::LogOptions(logv2::LogComponent::kDefault, logv2::FatalMode::kContinue),
            "Error during global initialization: {error}",
            "Error during global initialization",
            "error"_attr = status);
        return ExitCode::abrupt;
    }

    try {
        auto serviceContextHolder = ServiceContext::make();
        serviceContextHolder->registerClientObserver(
            std::make_unique<LockerMongosClientObserver>());
        setGlobalServiceContext(std::move(serviceContextHolder));
    } catch (...) {
        auto cause = exceptionToStatus();
        LOGV2_FATAL_OPTIONS(
            22866,
            logv2::LogOptions(logv2::LogComponent::kDefault, logv2::FatalMode::kContinue),
            "Error creating service context: {error}",
            "Error creating service context",
            "error"_attr = redact(cause));
        return ExitCode::abrupt;
    }

    // Attempt to rotate the audit log pre-emptively on startup to avoid any potential conflicts
    // with existing log state. If this rotation fails, then exit nicely with failure
    try {
        audit::rotateAuditLog();
    } catch (...) {

        Status err = mongo::exceptionToStatus();
        LOGV2(6169901, "Error rotating audit log", "error"_attr = err);

        quickExit(ExitCode::auditRotateError);
    }

    registerShutdownTask(cleanupTask);

    const auto service = getGlobalServiceContext();

    ErrorExtraInfo::invariantHaveAllParsers();

    startupConfigActions(std::vector<std::string>(argv, argv + argc));
    cmdline_utils::censorArgvArray(argc, argv);

    logCommonStartupWarnings(serverGlobalParams);

    try {
        if (!initialize_server_global_state::checkSocketPath())
            return ExitCode::abrupt;

        startSignalProcessingThread();

        return main(service);
    } catch (const DBException& e) {
        LOGV2_ERROR(22862,
                    "uncaught DBException in mongos main: {error}",
                    "uncaught DBException in mongos main",
                    "error"_attr = redact(e));
        return ExitCode::uncaught;
    } catch (const std::exception& e) {
        LOGV2_ERROR(22863,
                    "uncaught std::exception in mongos main: {error}",
                    "uncaught std::exception in mongos main",
                    "error"_attr = redact(e.what()));
        return ExitCode::uncaught;
    } catch (...) {
        LOGV2_ERROR(22864, "uncaught unknown exception in mongos main");
        return ExitCode::uncaught;
    }
}

}  // namespace mongo
