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

#include "mongo/s/mongos_main.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/remote_command_targeter_factory_impl.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/audit.h"
#include "mongo/db/audit_interface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_factory.h"
#include "mongo/db/auth/user_cache_invalidator_job.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_refresher.h"
#include "mongo/db/commands.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/extension/host/load_extension.h"
#include "mongo/db/ftdc/ftdc_mongos.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader.h"
#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader_impl.h"
#include "mongo/db/global_catalog/ddl/sessions_collection_sharded.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielders.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/process_health/fault_manager.h"
#include "mongo/db/profile_filter_impl.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/kill_sessions_remote.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_impl.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/service_liaison_impl.h"
#include "mongo/db/session/service_liaison_router.h"
#include "mongo/db/session/session.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/db/sharding_environment/client/shard_factory.h"
#include "mongo/db/sharding_environment/client/shard_remote.h"
#include "mongo/db/sharding_environment/client/sharding_connection_hook.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/mongos_server_parameters_gen.h"
#include "mongo/db/sharding_environment/router_uptime_reporter.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_initialization.h"
#include "mongo/db/sharding_environment/version_mongos.h"
#include "mongo/db/startup_warnings_common.h"
#include "mongo/db/topology/mongos_topology_coordinator.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/client_transport_observer_mongos.h"
#include "mongo/s/load_balancer_support.h"
#include "mongo/s/mongos_options.h"
#include "mongo/s/query/exec/cluster_cursor_cleanup_job.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query_analysis_sampler.h"
#include "mongo/s/read_write_concern_defaults_cache_lookup_mongos.h"
#include "mongo/s/service_entry_point_router_role.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/transaction_router.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/ingress_handshake_metrics.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session_manager_common.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/util/allocator_thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/debugger.h"
#include "mongo/util/duration.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fast_clock_source_factory.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ocsp/ocsp_manager.h"
#include "mongo/util/net/private/ssl_expiration.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/ntservice.h"                       // IWYU pragma: keep
#include "mongo/util/options_parser/startup_options.h"  // IWYU pragma: keep
#include "mongo/util/periodic_runner.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/text.h"  // IWYU pragma: keep
#include "mongo/util/time_support.h"
#include "mongo/util/version/releases.h"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#ifdef MONGO_CONFIG_GRPC
#include "mongo/transport/grpc/grpc_feature_flag_gen.h"
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

using logv2::LogComponent;

// Failpoint for disabling replicaSetChangeConfigServerUpdateHook calls on signaled mongos.
MONGO_FAIL_POINT_DEFINE(failReplicaSetChangeConfigServerUpdateHook);

namespace {

MONGO_FAIL_POINT_DEFINE(pauseWhileKillingOperationsAtShutdown);
MONGO_FAIL_POINT_DEFINE(pauseAfterImplicitlyAbortAllTransactions)
MONGO_FAIL_POINT_DEFINE(shutdownAtStartup);

#if defined(_WIN32)
const ntservice::NtServiceDefaultStrings defaultServiceStrings = {
    L"MongoS", L"MongoDB Router", L"MongoDB Sharding Router"};
#endif

constexpr auto kSignKeysRetryInterval = Seconds{1};

class ShardingReplicaSetChangeListener final
    : public ReplicaSetChangeNotifier::Listener,
      public std::enable_shared_from_this<ShardingReplicaSetChangeListener> {
public:
    ShardingReplicaSetChangeListener(ServiceContext* serviceContext)
        : _serviceContext(serviceContext) {}
    ~ShardingReplicaSetChangeListener() final = default;

    void onFoundSet(const Key& key) final {}

    void onConfirmedSet(const State& state) final {
        const auto& connStr = state.connStr;

        try {
            LOGV2(471693,
                  "Updating the shard registry with confirmed replica set",
                  "connectionString"_attr = connStr);
            Grid::get(_serviceContext)
                ->shardRegistry()
                ->updateReplSetHosts(connStr,
                                     ShardRegistry::ConnectionStringUpdateType::kConfirmed);
        } catch (const ExceptionFor<ErrorCategory::ShutdownError>& e) {
            LOGV2(471694,
                  "Unable to update the shard registry with confirmed replica set",
                  "error"_attr = e);
        }
    }

    void onPossibleSet(const State& state) final {
        try {
            Grid::get(_serviceContext)
                ->shardRegistry()
                ->updateReplSetHosts(state.connStr,
                                     ShardRegistry::ConnectionStringUpdateType::kPossible);
        } catch (const DBException& ex) {
            LOGV2_DEBUG(22849,
                        2,
                        "Unable to update sharding state with possible replica set",
                        "error"_attr = ex);
        }
    }

    void onDroppedSet(const Key& key) final {}

private:
    ServiceContext* _serviceContext;
};

Status waitForSigningKeys(OperationContext* opCtx) {
    while (true) {
        auto stopStatus = opCtx->checkForInterruptNoAssert();
        if (!stopStatus.isOK()) {
            return stopStatus;
        }

        try {
            if (LogicalTimeValidator::get(opCtx)->shouldGossipLogicalTime()) {
                return Status::OK();
            }
            LOGV2(22842,
                  "Waiting for signing keys, sleeping before checking again",
                  "signingKeysCheckInterval"_attr = Seconds(kSignKeysRetryInterval));
            sleepFor(kSignKeysRetryInterval);
            continue;
        } catch (const DBException& ex) {
            LOGV2_WARNING(22853,
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

    catalog->setDisallowNewTransactions();

    SessionKiller::Matcher matcherAllSessions(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});

    const auto abortDeadline = opCtx->fastClockSource().now() + Seconds(15);

    std::vector<AbortTransactionDetails> toKill;
    catalog->scanSessions(matcherAllSessions, [&](const ObservableSession& session) {
        toKill.emplace_back(session.getSessionId(),
                            session.kill(ErrorCodes::InterruptedAtShutdown));
    });

    // TODO(SERVER-111754): Please revisit if this thread could be made killable.
    auto newClient = opCtx->getServiceContext()
                         ->getService(ClusterRole::RouterServer)
                         ->makeClient("ImplicitlyAbortTxnAtShutdown",
                                      Client::noSession(),
                                      ClientOperationKillableByStepdown{false});
    AlternativeClientRegion acr(newClient);

    Status shutDownStatus(ErrorCodes::InterruptedAtShutdown,
                          "aborting transactions due to shutdown");

    for (auto& killDetails : toKill) {
        auto uniqueNewOpCtx = cc().makeOperationContext();
        auto newOpCtx = uniqueNewOpCtx.get();

        newOpCtx->setDeadlineByDate(abortDeadline, ErrorCodes::ExceededTimeLimit);

        OperationContextSession sessionCtx(newOpCtx, std::move(killDetails.killToken));

        auto session = OperationContextSession::get(newOpCtx);
        {
            auto lk = stdx::lock_guard(*newOpCtx->getClient());
            newOpCtx->setLogicalSessionId(session->getSessionId());
        }

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

    BSONObjBuilder shutdownTimeElapsedBuilder;
    BSONObjBuilder shutdownInfoBuilder;

    ScopeGuard logShutdownStats = [&] {
        shutdownInfoBuilder.append("Statistics", shutdownTimeElapsedBuilder.obj());
        LOGV2_INFO(8423406,
                   "mongos shutdown complete",
                   "Summary of time elapsed"_attr = shutdownInfoBuilder.obj());
    };
    SectionScopedTimer cleanupTaskTotalTimer{serviceContext->getFastClockSource(),
                                             TimedSectionId::cleanupTaskTotal,
                                             &shutdownTimeElapsedBuilder};

    {
        // This client initiation pattern is only to be used here, with plans to eliminate this
        // pattern down the line.
        if (!haveClient()) {
            // TODO(SERVER-111755): Please revisit if this thread could be made killable.
            Client::initThread(getThreadName(),
                               serviceContext->getService(ClusterRole::RouterServer),
                               Client::noSession(),
                               ClientOperationKillableByStepdown{false});
        }

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
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::quiesceMode,
                                           &shutdownTimeElapsedBuilder);
            mongosTopCoord->enterQuiesceModeAndWait(opCtx, quiesceTime);
        }

        UserCacheInvalidator::stop(serviceContext);

        // Inform the TransportLayers to stop accepting new connections.
        if (auto tlm = serviceContext->getTransportLayerManager()) {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::closeListenerSockets,
                                           &shutdownTimeElapsedBuilder);
            LOGV2_OPTIONS(8314101, {LogComponent::kNetwork}, "Shutdown: Closing listener sockets");
            tlm->stopAcceptingSessions();
        }

        if (audit::shutdownSynchronizeJob) {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::shutDownSynchronizeJob,
                                           &shutdownTimeElapsedBuilder);
            audit::shutdownSynchronizeJob();
        }

        {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::shutDownClusterParamRefresher,
                                           &shutdownTimeElapsedBuilder);
            ClusterServerParameterRefresher::onShutdown(serviceContext);
        }

        try {
            // Abort transactions while we can still send remote commands.
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::abortAllTransactions,
                                           &shutdownTimeElapsedBuilder);
            implicitlyAbortAllTransactions(opCtx);
            pauseAfterImplicitlyAbortAllTransactions.pauseWhileSet();
        } catch (const DBException& excep) {
            LOGV2_WARNING(22854, "Error aborting all active transactions", "error"_attr = excep);
        }

        if (auto lsc = LogicalSessionCache::get(serviceContext)) {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::joinLogicalSessionCache,
                                           &shutdownTimeElapsedBuilder);
            lsc->joinOnShutDown();
        }

        {
            LOGV2_OPTIONS(
                6973901, {LogComponent::kDefault}, "Shutting down the QueryAnalysisSampler");
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::shutDownQueryAnalysisSampler,
                                           &shutdownTimeElapsedBuilder);
            analyze_shard_key::QueryAnalysisSampler::get(serviceContext).onShutdown();
        }

        {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::shutDownReplicaSetMonitor,
                                           &shutdownTimeElapsedBuilder);
            ReplicaSetMonitor::shutdown();
        }

        {
            stdx::lock_guard lg(client);
            opCtx->setIsExecutingShutdown();
        }

        {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::killAllOperations,
                                           &shutdownTimeElapsedBuilder);
            serviceContext->setKillAllOperations();

            if (MONGO_unlikely(pauseWhileKillingOperationsAtShutdown.shouldFail())) {
                LOGV2(4701800, "pauseWhileKillingOperationsAtShutdown failpoint enabled");
                sleepsecs(1);
            }
            FailPoint* hangBeforeInterruptfailPoint =
                globalFailPointRegistry().find("hangBeforeCheckingMongosShutdownInterrupt");
            if (MONGO_unlikely(hangBeforeInterruptfailPoint &&
                               hangBeforeInterruptfailPoint->shouldFail())) {
                hangBeforeInterruptfailPoint->setMode(FailPoint::Mode::off);
                sleepsecs(3);
            }
        }

        // Perform all shutdown operations after setKillAllOperations is called in order to ensure
        // that any pending threads are about to terminate

        if (auto validator = LogicalTimeValidator::get(serviceContext)) {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::shutDownLogicalTimeValidator,
                                           &shutdownTimeElapsedBuilder);
            validator->shutDown();
        }

        if (auto grid = Grid::get(opCtx)) {
            grid->shutdown(opCtx, &shutdownTimeElapsedBuilder, true /* isMongos */);
        }

        {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::shutDownSearchTaskExecutors,
                                           &shutdownTimeElapsedBuilder);
            executor::shutdownSearchExecutorsIfNeeded(serviceContext);
        }

        // Finish shutting down the TransportLayers
        if (auto tlm = serviceContext->getTransportLayerManager()) {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::shutDownTransportLayer,
                                           &shutdownTimeElapsedBuilder);
            LOGV2_OPTIONS(
                22843, {LogComponent::kNetwork}, "Shutdown: Closing open transport sessions");
            tlm->shutdown();
        }

        // Shutdown Full-Time Data Capture
        {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::shutDownFTDC,
                                           &shutdownTimeElapsedBuilder);
            stopMongoSFTDC();
        }
    }

    audit::logShutdown(Client::getCurrent());


#ifdef MONGO_CONFIG_SSL
    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownOCSP,
                                       &shutdownTimeElapsedBuilder);
        OCSPManager::shutdown(serviceContext);
    }
#endif
}

Status initializeSharding(
    OperationContext* opCtx,
    std::shared_ptr<ReplicaSetChangeNotifier::Listener>* replicaSetChangeListener,
    BSONObjBuilder* startupTimeElapsedBuilder) {
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

    auto catalogCache = std::make_unique<CatalogCache>(
        opCtx->getServiceContext(), std::make_shared<ConfigServerCatalogCacheLoaderImpl>());

    // List of hooks which will be called by the ShardRegistry when it discovers a shard has been
    // removed.
    std::vector<ShardRegistry::ShardRemovalHook> shardRemovalHooks = {
        // It's safe to capture the CatalogCache pointer since the Grid (and therefore CatalogCache
        // and ShardRegistry) are never destroyed.
        [catCache = catalogCache.get()](const ShardId& removedShard) {
            catCache->advanceTimeInStoreForEntriesThatReferenceShard(removedShard);
        }};

    if (!mongosGlobalParams.configdbs) {
        return {ErrorCodes::BadValue, "Unrecognized connection string."};
    }

    auto shardRegistry = std::make_unique<ShardRegistry>(opCtx->getServiceContext(),
                                                         std::move(shardFactory),
                                                         mongosGlobalParams.configdbs,
                                                         std::move(shardRemovalHooks));

    {
        SectionScopedTimer scopedTimer(&opCtx->fastClockSource(),
                                       TimedSectionId::initializeGlobalShardingState,
                                       startupTimeElapsedBuilder);
        Status status = initializeGlobalShardingState(
            opCtx,
            std::move(catalogCache),
            std::move(shardRegistry),
            [service = opCtx->getServiceContext()] { return makeShardingEgressHooksList(service); },
            boost::none,
            [](ShardingCatalogClient* catalogClient) {
                return std::make_unique<KeysCollectionClientSharded>(catalogClient);
            });
        if (!status.isOK()) {
            return status;
        }
    }

    *replicaSetChangeListener =
        ReplicaSetMonitor::getNotifier().makeListener<ShardingReplicaSetChangeListener>(
            opCtx->getServiceContext());

    // Reset the shard register config connection string in case it missed the replica set monitor
    // notification.
    auto configShardConnStr =
        Grid::get(opCtx->getServiceContext())->shardRegistry()->getConfigServerConnectionString();
    if (configShardConnStr.type() == ConnectionString::ConnectionType::kReplicaSet) {
        SectionScopedTimer scopedTimer(&opCtx->fastClockSource(),
                                       TimedSectionId::resetConfigConnectionString,
                                       startupTimeElapsedBuilder);
        ConnectionString rsMonitorConfigConnStr(
            ReplicaSetMonitor::get(configShardConnStr.getSetName())->getServerAddress(),
            ConnectionString::ConnectionType::kReplicaSet);
        Grid::get(opCtx->getServiceContext())
            ->shardRegistry()
            ->updateReplSetHosts(rsMonitorConfigConnStr,
                                 ShardRegistry::ConnectionStringUpdateType::kConfirmed);
    }

    {
        SectionScopedTimer scopedTimer(&opCtx->fastClockSource(),
                                       TimedSectionId::loadGlobalSettings,
                                       startupTimeElapsedBuilder);
        Status status =
            loadGlobalSettingsFromConfigServer(opCtx, Grid::get(opCtx)->catalogClient());
        if (!status.isOK()) {
            return status;
        }
    }

    {
        SectionScopedTimer scopedTimer(&opCtx->fastClockSource(),
                                       TimedSectionId::waitForSigningKeys,
                                       startupTimeElapsedBuilder);
        Status status = waitForSigningKeys(opCtx);
        if (!status.isOK()) {
            return status;
        }
    }

    // Loading of routing information may fail. Since this is just an optimization (warmup), any
    // failure must not prevent mongos from starting.
    try {
        SectionScopedTimer scopedTimer(&opCtx->fastClockSource(),
                                       TimedSectionId::preCacheRoutingInfo,
                                       startupTimeElapsedBuilder);
        preCacheMongosRoutingInfo(opCtx);
    } catch (const DBException& ex) {
        LOGV2_WARNING(6203601, "Failed to warmup routing information", "error"_attr = redact(ex));
    }

    // Pre-warm the connection pool may fail. Since this is just an optimization, any failure must
    // not prevent mongos from starting.
    {
        SectionScopedTimer scopedTimer(&opCtx->fastClockSource(),
                                       TimedSectionId::preWarmConnectionPool,
                                       startupTimeElapsedBuilder);
        Status status = preWarmConnectionPool(opCtx);
        if (!status.isOK()) {
            LOGV2_WARNING(
                104223, "Failed to warmup the collection pool", "error"_attr = status.reason());
        }
    }

    Grid::get(opCtx)->setShardingInitialized();

    return Status::OK();
}

namespace {
ServiceContext::ConstructorActionRegisterer registerWireSpec{
    "RegisterWireSpec", [](ServiceContext* service) {
        WireSpec::Specification spec;
        spec.outgoing.minWireVersion = LATEST_WIRE_VERSION;
        spec.outgoing.maxWireVersion = LATEST_WIRE_VERSION;
        spec.isInternalClient = true;

        WireSpec::getWireSpec(service).initialize(std::move(spec));
    }};
}  // namespace

ExitCode runMongosServer(ServiceContext* serviceContext) {
    BSONObjBuilder startupTimeElapsedBuilder;
    BSONObjBuilder startupInfoBuilder;

    SectionScopedTimer runMongosTotalTimer{serviceContext->getFastClockSource(),
                                           TimedSectionId::runMongosTotal,
                                           &startupTimeElapsedBuilder};
    auto logStartupStats = std::make_unique<ScopeGuard<std::function<void()>>>([&] {
        runMongosTotalTimer = {};
        startupInfoBuilder.append("Statistics", startupTimeElapsedBuilder.obj());
        LOGV2_INFO(8423405,
                   "mongos startup complete",
                   "Summary of time elapsed"_attr = startupInfoBuilder.obj());
    });

    // TODO(SERVER-111755): Please revisit if this thread could be made killable.
    ThreadClient tc("mongosMain",
                    serviceContext->getService(ClusterRole::RouterServer),
                    ClientOperationKillableByStepdown{false});

    logMongosVersionInfo(nullptr);

    // Set up the periodic runner for background job execution
    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::setUpPeriodicRunner,
                                       &startupTimeElapsedBuilder);
        auto runner = makePeriodicRunner(serviceContext);
        serviceContext->setPeriodicRunner(std::move(runner));
    }

#ifdef MONGO_CONFIG_SSL
    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::setUpOCSP,
                                       &startupTimeElapsedBuilder);
        OCSPManager::start(serviceContext);
    }
    CertificateExpirationMonitor::get()->start(serviceContext);
#endif

    ProfileFilterImpl::initializeDefaults(serviceContext);

    serviceContext->getService(ClusterRole::RouterServer)
        ->setServiceEntryPoint(std::make_unique<ServiceEntryPointRouterRole>());

    {
        const auto loadBalancerPort = load_balancer_support::getLoadBalancerPort();
        if (loadBalancerPort && *loadBalancerPort == serverGlobalParams.port) {
            LOGV2_ERROR(6067901,
                        "Load balancer port must be different from the normal ingress port.",
                        "port"_attr = serverGlobalParams.port);
            quickExit(ExitCode::badOptions);
        }

        bool useEgressGRPC = false;
        if (globalMongotParams.useGRPC) {
#ifdef MONGO_CONFIG_GRPC
            uassert(9925000,
                    "Egress GRPC for search is not enabled",
                    feature_flags::gEgressGrpcForSearch.isEnabled());
            useEgressGRPC = true;
#else
            LOGV2_ERROR(
                10049100,
                "useGRPCForSearch is only supported on Linux platforms built with TLS support.");
            quickExit(ExitCode::badOptions);
#endif
        }

        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::setUpTransportLayer,
                                       &startupTimeElapsedBuilder);
        auto tl = transport::TransportLayerManagerImpl::createWithConfig(
            &serverGlobalParams,
            serviceContext,
            useEgressGRPC,
            loadBalancerPort,
            std::make_unique<ClientTransportObserverMongos>());
        if (auto res = tl->setup(); !res.isOK()) {
            LOGV2_ERROR(22856, "Error setting up transport layer", "error"_attr = res);
            return ExitCode::netError;
        }
        serviceContext->setTransportLayerManager(std::move(tl));
    }

    executor::startupSearchExecutorsIfNeeded(serviceContext);

    // Add sharding hooks to both connection pools - ShardingConnectionHook includes auth hooks
    globalConnPool.addHook(new ShardingConnectionHook(makeShardingEgressHooksList(serviceContext)));

    // Mongos connection pools already takes care of authenticating new connections so the
    // replica set connection shouldn't need to.
    DBClientReplicaSet::setAuthPooledSecondaryConn(false);

    if (getHostName().empty()) {
        quickExit(ExitCode::badOptions);
    }

    ReadWriteConcernDefaults::create(serviceContext->getService(ClusterRole::RouterServer),
                                     readWriteConcernDefaultsCacheLookupMongoS);
    ChangeStreamOptionsManager::create(serviceContext);

    auto opCtxHolder = tc->makeOperationContext();
    auto const opCtx = opCtxHolder.get();

    // Keep listener alive until shutdown.
    std::shared_ptr<ReplicaSetChangeNotifier::Listener> replicaSetChangeListener;

    // Only initialize Router ResourceYielder Factory since we do not have a Shard role.
    ResourceYielderFactory::set(*serviceContext->getService(ClusterRole::RouterServer),
                                std::make_unique<RouterResourceYielderFactory>());

    // Since extensions modify the global parserMap, which is not thread-safe, they must be loaded
    // prior to sharding initialization to avoid a data race. Once sharding is initialized, the
    // CatalogCacheLoader will issue internal aggregations that can concurrently read from the
    // parserMap.
    if (!extension::host::loadExtensions(serverGlobalParams.extensions)) {
        return ExitCode::badOptions;
    }

    try {
        uassertStatusOK(
            initializeSharding(opCtx, &replicaSetChangeListener, &startupTimeElapsedBuilder));
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::CallbackCanceled) {
            invariant(globalInShutdownDeprecated());
            LOGV2(22850, "Shutdown called before mongos finished starting up");
            return ExitCode::clean;
        }

        LOGV2_ERROR(22857, "Error initializing sharding system", "error"_attr = redact(ex));
        return ExitCode::shardingError;
    }

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::refreshBalancerConfig,
                                       &startupTimeElapsedBuilder);
        Grid::get(serviceContext)
            ->getBalancerConfiguration()
            ->refreshAndCheck(opCtx)
            .transitional_ignore();
    }

    try {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::refreshRWConcernDefaults,
                                       &startupTimeElapsedBuilder);
        ReadWriteConcernDefaults::get(opCtx).refreshIfNecessary(opCtx);
    } catch (const DBException& ex) {
        LOGV2_WARNING(22855,
                      "Error loading read and write concern defaults at startup",
                      "error"_attr = redact(ex));
    }

    CommandInvocationHooks::set(serviceContext,
                                std::make_unique<transport::IngressHandshakeMetricsCommandHooks>());

    // Must happen before FTDC, because Periodic Metadata Collustion calls getClusterParameter
    ClusterServerParameterRefresher::start(serviceContext, opCtx);

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::startMongosFTDC,
                                       &startupTimeElapsedBuilder);
        startMongoSFTDC(serviceContext);
    }

    if (mongosGlobalParams.scriptingEnabled) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::setUpScriptEngine,
                                       &startupTimeElapsedBuilder);
        ScriptEngine::setup(ExecutionEnvironment::Server);
    }

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::userAndRolesGraph,
                                       &startupTimeElapsedBuilder);
        Status status = globalAuthzManagerFactory->initialize(opCtx);
        if (!status.isOK()) {
            LOGV2_ERROR(22858, "Error initializing authorization data", "error"_attr = status);
            return ExitCode::shardingError;
        }
    }

    // Construct the router uptime reporter after the startup parameters have been parsed in order
    // to ensure that it picks up the server port instead of reporting the default value.
    RouterUptimeReporter::get(serviceContext).startPeriodicThread(serviceContext);

    clusterCursorCleanupJob.go();

    UserCacheInvalidator::start(serviceContext, opCtx);

    if (audit::initializeSynchronizeJob) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::initializeAuditSynchronizeJob,
                                       &startupTimeElapsedBuilder);
        audit::initializeSynchronizeJob(serviceContext);
    }

    PeriodicTask::startRunningPeriodicTasks();

    Status status =
        process_health::FaultManager::get(serviceContext)->startPeriodicHealthChecks().getNoThrow();
    if (!status.isOK()) {
        LOGV2_ERROR(
            5936510, "Error completing initial health check", "error"_attr = redact(status));
        return ExitCode::processHealthCheck;
    }

    srand((unsigned)(curTimeMicros64()) ^ (unsigned(uintptr_t(&opCtx))));  // NOLINT

    SessionKiller::set(
        serviceContext->getService(ClusterRole::RouterServer),
        std::make_shared<SessionKiller>(serviceContext->getService(ClusterRole::RouterServer),
                                        killSessionsRemote));

    LogicalSessionCache::set(
        serviceContext,
        std::make_unique<LogicalSessionCacheImpl>(
            std::make_unique<ServiceLiaisonImpl>(
                service_liaison_router_callbacks::getOpenCursorSessions,
                service_liaison_router_callbacks::killCursorsWithMatchingSessions),
            std::make_unique<SessionsCollectionSharded>(),
            RouterSessionCatalog::reapSessionsOlderThan));

    transport::ServiceExecutor::startupAll(serviceContext);

    if (auto status = serviceContext->getTransportLayerManager()->start(); !status.isOK()) {
        LOGV2_ERROR(22861, "Error starting transport layer", "error"_attr = redact(status));
        return ExitCode::netError;
    }

    if (!initialize_server_global_state::writePidFile()) {
        return ExitCode::abrupt;
    }

    // Startup options are written to the audit log at the end of startup so that cluster server
    // parameters are guaranteed to have been initialized from disk at this point.
    audit::logStartupOptions(tc.get(), serverGlobalParams.parsedOpts);

    serviceContext->notifyStorageStartupRecoveryComplete();

#if !defined(_WIN32)
    initialize_server_global_state::signalForkSuccess();
#else
    if (ntservice::shouldStartService()) {
        ntservice::reportStatus(SERVICE_RUNNING);
        LOGV2(22851, "Service running");
    }
#endif

    logStartupStats = {};

    if (MONGO_unlikely(shutdownAtStartup.shouldFail())) {
        LOGV2(9494000, "Starting clean exit via failpoint");
        exitCleanly(ExitCode::clean);
    }

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

ExitCode main(ServiceContext* serviceContext) {
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
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
}

#ifdef MONGO_CONFIG_SSL
MONGO_INITIALIZER_GENERAL(setSSLManagerType, (), ("SSLManager"))
(InitializerContext* context) {
    isSSLServer = true;
}
#endif

}  // namespace

ExitCode mongos_main(int argc, char* argv[]) {
    serverGlobalParams.clusterRole = ClusterRole::RouterServer;

    if (argc < 1)
        return ExitCode::badOptions;

    waitForDebugger();

    setupSignalHandlers();

    Status status = runGlobalInitializers(std::vector<std::string>(argv, argv + argc));
    if (!status.isOK()) {
        LOGV2_FATAL_OPTIONS(
            22865,
            logv2::LogOptions(logv2::LogComponent::kDefault, logv2::FatalMode::kContinue),
            "Error during global initialization",
            "error"_attr = status);
        return ExitCode::abrupt;
    }

    startSignalProcessingThread();

    try {
        setGlobalServiceContext(
            ServiceContext::make(FastClockSourceFactory::create(Milliseconds{10})));
    } catch (...) {
        auto cause = exceptionToStatus();
        LOGV2_FATAL_OPTIONS(
            22866,
            logv2::LogOptions(logv2::LogComponent::kDefault, logv2::FatalMode::kContinue),
            "Error creating service context",
            "error"_attr = redact(cause));
        return ExitCode::abrupt;
    }

    const auto service = getGlobalServiceContext();

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

    ErrorExtraInfo::invariantHaveAllParsers();

    startupConfigActions(std::vector<std::string>(argv, argv + argc));
    cmdline_utils::censorArgvArray(argc, argv);

    logCommonStartupWarnings(serverGlobalParams);

    ShardingState::create(service);

    try {
        if (!initialize_server_global_state::checkSocketPath())
            return ExitCode::abrupt;

        startAllocatorThread();

        return main(service);
    } catch (const DBException& e) {
        LOGV2_ERROR(22862, "uncaught DBException in mongos main", "error"_attr = redact(e));
        return ExitCode::uncaught;
    } catch (const std::exception& e) {
        LOGV2_ERROR(
            22863, "uncaught std::exception in mongos main", "error"_attr = redact(e.what()));
        return ExitCode::uncaught;
    } catch (...) {
        LOGV2_ERROR(22864, "uncaught unknown exception in mongos main");
        return ExitCode::uncaught;
    }
}

}  // namespace mongo
