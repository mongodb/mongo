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

#include "mongo/db/mongod_main.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/admission/execution_control_init.h"
#include "mongo/db/admission/flow_control.h"
#include "mongo/db/admission/flow_control_parameters_gen.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/auth_op_observer.h"
#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_factory.h"
#include "mongo/db/auth/user_cache_invalidator_job.h"
#include "mongo/db/change_collection_expired_documents_remover.h"
#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/change_streams_cluster_parameter_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_initializer.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_op_observer.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_gen.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/commands/shutdown.h"
#include "mongo/db/commands/test_commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/extension/host/load_extension.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/flow_control_ticketholder.h"
#include "mongo/db/ftdc/ftdc_mongod.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/routing_information_cache.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/global_catalog/ddl/rename_collection_participant_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/periodic_sharded_index_consistency_checker.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/index_builds_coordinator_mongod.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/keys_collection_client_direct.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/keys_collection_manager_gen.h"
#include "mongo/db/local_catalog/catalog_helper.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog_helper.h"
#include "mongo/db/local_catalog/collection_impl.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/database_holder_impl.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/ddl/direct_connection_ddl_hook.h"
#include "mongo/db/local_catalog/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/local_catalog/health_log.h"
#include "mongo/db/local_catalog/health_log_interface.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielders.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state_factory_shard.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state_factory_shard.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/local_executor.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/logical_session_cache_factory_mongod.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/mirror_maestro.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/mongod_options_general_gen.h"
#include "mongo/db/mongod_options_storage_gen.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/change_stream_pre_images_op_observer.h"
#include "mongo/db/op_observer/fallback_op_observer.h"
#include "mongo/db/op_observer/fcv_op_observer.h"
#include "mongo/db/op_observer/find_and_modify_images_op_observer.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/op_observer/operation_logger_transaction_proxy.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/periodic_runner_cache_pressure_rollback.h"
#include "mongo/db/periodic_runner_job_abort_expired_transactions.h"
#include "mongo/db/pipeline/process_interface/replica_set_node_process_interface.h"
#include "mongo/db/profile_filter_impl.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/compiler/stats/stats_cache_loader_impl.h"
#include "mongo/db/query/compiler/stats/stats_catalog.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mongod.h"
#include "mongo/db/repl/initial_sync/base_cloner.h"
#include "mongo/db/repl/initial_sync/initial_syncer_factory.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_set_member_in_standalone_mode.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_impl_gen.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/replication_state_transition_lock_guard.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator.h"
#include "mongo/db/s/migration_chunk_cloner_source_op_observer.h"
#include "mongo/db/s/query_analysis_op_observer_configsvr.h"
#include "mongo/db/s/query_analysis_op_observer_rs.h"
#include "mongo/db/s/query_analysis_op_observer_shardsvr.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding/resharding_op_observer.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_lifecycle_monitor.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_rs_endpoint.h"
#include "mongo/db/service_entry_point_shard_role.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/session/kill_sessions_remote.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/db/set_change_stream_state_coordinator.h"
#include "mongo/db/sharding_environment/config_server_op_observer.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_server_op_observer.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_initialization_mongod.h"
#include "mongo/db/sharding_environment/sharding_ready.h"
#include "mongo/db/startup_recovery.h"
#include "mongo/db/startup_warnings_mongod.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/disk_space_monitor.h"
#include "mongo/db/storage/durable_history_pin.h"
#include "mongo/db/storage/encryption_hooks.h"
#include "mongo/db/storage/oplog_cap_maintainer_thread.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/system_index.h"
#include "mongo/db/timeseries/timeseries_op_observer.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/periodic_replica_set_configshard_maintenance_mode_checker.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_gen.h"
#include "mongo/db/ttl/ttl.h"
#include "mongo/db/user_write_block/user_write_block_mode_op_observer.h"
#include "mongo/db/vector_clock/vector_clock_metadata_hook.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/metrics/metrics_initialization.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/process_id.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/query_analysis_client.h"
#include "mongo/s/query_analysis_sampler.h"
#include "mongo/s/read_write_concern_defaults_cache_lookup_mongos.h"
#include "mongo/s/service_entry_point_router_role.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/ingress_handshake_metrics.h"
#include "mongo/transport/session_manager_common.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/util/allocator_thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/buildinfo.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/debugger.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fast_clock_source_factory.h"
#include "mongo/util/net/ocsp/ocsp_manager.h"
#include "mongo/util/net/private/ssl_expiration.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/options_parser/value.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"  // IWYU pragma: keep
#include "mongo/util/thread_safety_context.h"
#include "mongo/util/time_support.h"
#include "mongo/util/version.h"
#include "mongo/watchdog/watchdog_mongod.h"

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>  // IWYU pragma: keep
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>

#ifdef MONGO_CONFIG_GRPC
#include "mongo/transport/grpc/grpc_feature_flag_gen.h"
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

using logv2::LogComponent;
using std::endl;

namespace {

MONGO_FAIL_POINT_DEFINE(hangDuringQuiesceMode);
MONGO_FAIL_POINT_DEFINE(pauseWhileKillingOperationsAtShutdown);
MONGO_FAIL_POINT_DEFINE(hangBeforeShutdown);

#ifdef _WIN32
const ntservice::NtServiceDefaultStrings defaultServiceStrings = {
    L"MongoDB", L"MongoDB", L"MongoDB Server"};
#endif

auto& startupInfoSection =
    *ServerStatusSectionBuilder<BSONObjectStatusSection>("startupInfo").forShard().forRouter();

auto makeTransportLayer(ServiceContext* svcCtx) {
    boost::optional<int> proxyPort;

    // (Ignore FCV check): The proxy port needs to be open before the FCV is set.
    if (gFeatureFlagMongodProxyProtocolSupport.isEnabledAndIgnoreFCVUnsafe()) {
        if (serverGlobalParams.proxyPort) {
            proxyPort = *serverGlobalParams.proxyPort;
            if (*proxyPort == serverGlobalParams.port) {
                LOGV2_ERROR(9967800,
                            "The proxy port must be different from the public listening port.",
                            "port"_attr = serverGlobalParams.port);
                quickExit(ExitCode::badOptions);
            }
        }
    }

    // Mongod should not bind to any ports in repair mode so only allow egress.
    if (storageGlobalParams.repair) {
        return transport::TransportLayerManagerImpl::makeDefaultEgressTransportLayer();
    }

    bool useEgressGRPC = false;
    if (globalMongotParams.useGRPC) {
#ifdef MONGO_CONFIG_GRPC
        uassert(9715900,
                "Egress GRPC for search is not enabled",
                feature_flags::gEgressGrpcForSearch.isEnabled());
        useEgressGRPC = true;
#else
        LOGV2_ERROR(
            10049101,
            "useGRPCForSearch is only supported on Linux platforms built with TLS support.");
        quickExit(ExitCode::badOptions);
#endif
    }

    return transport::TransportLayerManagerImpl::createWithConfig(
        &serverGlobalParams, svcCtx, useEgressGRPC, std::move(proxyPort));
}

ExitCode initializeTransportLayer(ServiceContext* serviceContext, BSONObjBuilder* timerReport) {
    SectionScopedTimer scopedTimer(
        serviceContext->getFastClockSource(), TimedSectionId::setUpTransportLayer, timerReport);
    auto tl = makeTransportLayer(serviceContext);
    if (auto res = tl->setup(); !res.isOK()) {
        LOGV2_ERROR(20568, "Error setting up transport layer", "error"_attr = res);
        return ExitCode::netError;
    }
    serviceContext->setTransportLayerManager(std::move(tl));
    return ExitCode::clean;
}

void logStartup(OperationContext* opCtx) {
    BSONObjBuilder toLog;
    std::stringstream id;
    id << getHostNameCached() << "-" << Date_t::now().asInt64();
    toLog.append("_id", id.str());
    toLog.append("hostname", getHostNameCached());

    toLog.appendTimeT("startTime", time(nullptr));
    toLog.append("startTimeLocal", dateToCtimeString(Date_t::now()));

    toLog.append("cmdLine", serverGlobalParams.parsedOpts);
    toLog.append("pid", ProcessId::getCurrent().asLongLong());


    {
        BSONObjBuilder buildinfo(toLog.subobjStart("buildinfo"));
        auto info = getBuildInfo();
        info.setStorageEngines(getStorageEngineNames(opCtx->getServiceContext()));
        info.serialize(&buildinfo);
    }

    BSONObj o = toLog.obj();

    Lock::GlobalWrite lk(opCtx);
    AutoGetDb autoDb(opCtx, NamespaceString::kStartupLogNamespace.dbName(), mongo::MODE_X);
    auto db = autoDb.ensureDbExists(opCtx);
    // kStartupLogNamespace is always local and doesn't require a placement version check.
    auto collection =
        acquireCollection(opCtx,
                          CollectionAcquisitionRequest{NamespaceString::kStartupLogNamespace,
                                                       PlacementConcern::kPretendUnsharded,
                                                       repl::ReadConcernArgs::get(opCtx),
                                                       AcquisitionPrerequisites::kWrite},
                          MODE_X);
    WriteUnitOfWork wunit(opCtx);
    if (!collection.exists()) {
        BSONObj options = BSON("capped" << true << "size" << 10 * 1024 * 1024);
        repl::UnreplicatedWritesBlock uwb(opCtx);
        CollectionOptions collectionOptions = uassertStatusOK(
            CollectionOptions::parse(options, CollectionOptions::ParseKind::parseForCommand));
        auto newColl =
            db->createCollection(opCtx, NamespaceString::kStartupLogNamespace, collectionOptions);
        invariant(newColl);
        // Re-acquire after creation
        collection =
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest{NamespaceString::kStartupLogNamespace,
                                                           PlacementConcern::kPretendUnsharded,
                                                           repl::ReadConcernArgs::get(opCtx),
                                                           AcquisitionPrerequisites::kWrite},
                              MODE_X);
    }

    uassertStatusOK(collection_internal::insertDocument(
        opCtx, collection.getCollectionPtr(), InsertStatement(o), nullptr /* OpDebug */, false));
    wunit.commit();
}

void initializeCommandHooks(ServiceContext* serviceContext) {
    class MongodCommandInvocationHooks final : public CommandInvocationHooks {
    public:
        void onBeforeRun(OperationContext* opCtx, CommandInvocation* invocation) override {
            _nextHook.onBeforeRun(opCtx, invocation);
        }

        void onBeforeAsyncRun(std::shared_ptr<RequestExecutionContext> rec,
                              CommandInvocation* invocation) override {
            _nextHook.onBeforeAsyncRun(rec, invocation);
        }

        void onAfterRun(OperationContext* opCtx,
                        CommandInvocation* invocation,
                        rpc::ReplyBuilderInterface* response) override {
            _nextHook.onAfterRun(opCtx, invocation, response);
            _onAfterRunImpl(opCtx);
        }

        void onAfterAsyncRun(std::shared_ptr<RequestExecutionContext> rec,
                             CommandInvocation* invocation) override {
            _nextHook.onAfterAsyncRun(rec, invocation);
            _onAfterRunImpl(rec->getOpCtx());
        }

    private:
        void _onAfterRunImpl(OperationContext* opCtx) const {
            MirrorMaestro::tryMirrorRequest(opCtx);
            MirrorMaestro::onReceiveMirroredRead(opCtx);
        }

        transport::IngressHandshakeMetricsCommandHooks _nextHook{};
    };

    CommandInvocationHooks::set(serviceContext, std::make_unique<MongodCommandInvocationHooks>());
}

void registerPrimaryOnlyServices(ServiceContext* serviceContext) {
    auto registry = repl::PrimaryOnlyServiceRegistry::get(serviceContext);

    std::vector<std::unique_ptr<repl::PrimaryOnlyService>> services;

    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        services.push_back(std::make_unique<ReshardingCoordinatorService>(serviceContext));
        services.push_back(std::make_unique<ConfigsvrCoordinatorService>(serviceContext));
    }

    if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        auto shardingDDLCoordinatorService =
            std::make_unique<ShardingDDLCoordinatorService>(serviceContext);
        DDLLockManager::get(serviceContext)->setRecoverable(shardingDDLCoordinatorService.get());

        services.emplace_back(std::move(shardingDDLCoordinatorService));
        services.push_back(std::make_unique<RenameCollectionParticipantService>(serviceContext));
        services.push_back(std::make_unique<ReshardingDonorService>(serviceContext));
        services.push_back(std::make_unique<ReshardingRecipientService>(serviceContext));
        services.push_back(std::make_unique<MultiUpdateCoordinatorService>(serviceContext));
    }

    if (change_stream_serverless_helpers::canInitializeServices()) {
        services.push_back(
            std::make_unique<SetChangeStreamStateCoordinatorService>(serviceContext));
    }

    for (auto& service : services) {
        registry->registerService(std::move(service));
    }
}

MONGO_FAIL_POINT_DEFINE(shutdownAtStartup);

// Important:
// _initAndListen among its other tasks initializes the storage subsystem.
// File Copy Based Initial Sync will restart the storage subsystem and may need to repeat some
// of the initialization steps within.  If you add or change any of these steps, make sure
// any necessary changes are also made to File Copy Based Initial Sync.
ExitCode _initAndListen(ServiceContext* serviceContext) {
    // TODO(SERVER-74659): Please revisit if this thread could be made killable.
    Client::initThread("initandlisten",
                       serviceContext->getService(ClusterRole::ShardServer),
                       Client::noSession(),
                       ClientOperationKillableByStepdown{false});

    BSONObjBuilder startupTimeElapsedBuilder;
    BSONObjBuilder startupInfoBuilder;

    SectionScopedTimer initAndListenTotalTimer{serviceContext->getFastClockSource(),
                                               TimedSectionId::initAndListenTotal,
                                               &startupTimeElapsedBuilder};

    DBDirectClientFactory::get(serviceContext).registerImplementation([](OperationContext* opCtx) {
        return std::unique_ptr<DBClientBase>(new DBDirectClient(opCtx));
    });

    const repl::ReplSettings& replSettings =
        repl::ReplicationCoordinator::get(serviceContext)->getSettings();

    {
        ProcessId pid = ProcessId::getCurrent();
        const bool is32bit = sizeof(int*) == 4;
        LOGV2(4615611,
              "MongoDB starting",
              "pid"_attr = pid.toNative(),
              "port"_attr = serverGlobalParams.port,
              "dbPath"_attr = boost::filesystem::path(storageGlobalParams.dbpath).generic_string(),
              "architecture"_attr = (is32bit ? "32-bit" : "64-bit"),
              "host"_attr = getHostNameCached());
    }

    if (kDebugBuild)
        LOGV2(20533, "DEBUG build (which is slower)");

#if defined(_WIN32)
    VersionInfoInterface::instance().logTargetMinOS();
#endif

    logProcessDetails(nullptr);

    initializeCommandHooks(serviceContext);

    ProfileFilterImpl::initializeDefaults(serviceContext);

    {
        // (Ignore FCV check): The ReplicaSetEndpoint service entry point needs to be set even
        // before the FCV is fully upgraded.
        const bool useRSEndpoint =
            feature_flags::gFeatureFlagReplicaSetEndpoint.isEnabledAndIgnoreFCVUnsafe();
        auto shardRoleSEP = std::make_unique<ServiceEntryPointShardRole>();
        auto shardService = serviceContext->getService(ClusterRole::ShardServer);
        if (useRSEndpoint) {
            shardService->setServiceEntryPoint(
                std::make_unique<ServiceEntryPointRSEndpoint>(std::move(shardRoleSEP)));
        } else {
            shardService->setServiceEntryPoint(std::move(shardRoleSEP));
        }
    }

    {
        // Set up the periodic runner for background job execution. This is required to be running
        // before both the storage engine or the transport layer are initialized.
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::setUpPeriodicRunner,
                                       &startupTimeElapsedBuilder);
        auto runner = makePeriodicRunner(serviceContext);
        serviceContext->setPeriodicRunner(std::move(runner));
    }

    // When starting the server with --queryableBackupMode, --recoverFromOplogAsStandalone or
    // --magicRestore, we are in read-only mode and don't allow user-originating operations to
    // perform writes
    if (storageGlobalParams.queryableBackupMode ||
        repl::ReplSettings::shouldRecoverFromOplogAsStandalone() ||
        storageGlobalParams.magicRestore) {
        serviceContext->disallowUserWrites();
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

    if (auto ec = initializeTransportLayer(serviceContext, &startupTimeElapsedBuilder);
        ec != ExitCode::clean)
        return ec;

    auto& rss = rss::ReplicatedStorageService::get(serviceContext);
    auto& serviceLifecycle = rss.getServiceLifecycle();
    serviceLifecycle.initializeFlowControl(serviceContext);

    // If a crash occurred during file-copy based initial sync, we may need to finish or clean up.
    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::initSyncCrashRecovery,
                                       &startupTimeElapsedBuilder);
        repl::InitialSyncerFactory::get(serviceContext)->runCrashRecovery();
    }

    admission::initializeExecutionControl(serviceContext);

    serviceLifecycle.initializeStorageEngineExtensions(serviceContext);

    auto lastShutdownState = [&]() {
        if (rss.getPersistenceProvider().shouldDelayDataAccessDuringStartup()) {
            // If data isn't ready yet, we shouldn't try to read it.
            auto initializeStorageEngineOpCtx = serviceContext->makeOperationContext(&cc());
            return catalog::startUpStorageEngine(initializeStorageEngineOpCtx.get(),
                                                 StorageEngineInitFlags{},
                                                 &startupTimeElapsedBuilder);
        } else {
            return catalog::startUpStorageEngineAndCollectionCatalog(
                serviceContext, &cc(), StorageEngineInitFlags{}, &startupTimeElapsedBuilder);
        }
    }();
    StorageControl::startStorageControls(serviceContext);

    auto logStartupStats = std::make_unique<ScopeGuard<std::function<void()>>>([&] {
        initAndListenTotalTimer = {};
        startupInfoBuilder.append("Startup from clean shutdown?",
                                  lastShutdownState == StorageEngine::LastShutdownState::kClean);
        startupInfoBuilder.append("Statistics", startupTimeElapsedBuilder.obj());
        BSONObj startupInfoObj = startupInfoBuilder.obj();
        LOGV2_INFO(
            8423403, "mongod startup complete", "Summary of time elapsed"_attr = startupInfoObj);
        startupInfoSection.setObject(startupInfoObj);
    });

#ifdef MONGO_CONFIG_WIREDTIGER_ENABLED
    if (EncryptionHooks::get(serviceContext)->restartRequired()) {
        exitCleanly(ExitCode::clean);
    }
#endif

    // Warn if we detect configurations for multiple registered storage engines in the same
    // configuration file/environment.
    if (serverGlobalParams.parsedOpts.hasField("storage")) {
        BSONElement storageElement = serverGlobalParams.parsedOpts.getField("storage");
        invariant(storageElement.isABSONObj());
        for (auto&& e : storageElement.Obj()) {
            // Ignore if field name under "storage" matches current storage engine.
            if (storageGlobalParams.engine == e.fieldName()) {
                continue;
            }

            // Warn if field name matches non-active registered storage engine.
            if (isRegisteredStorageEngine(serviceContext, e.fieldName())) {
                LOGV2_WARNING(20566,
                              "Detected configuration for non-active storage engine",
                              "fieldName"_attr = e.fieldName(),
                              "storageEngine"_attr = storageGlobalParams.engine);
            }
        }
    }

    // Disallow running a storage engine that doesn't support capped collections with --profile
    if (!serviceContext->getStorageEngine()->supportsCappedCollections() &&
        serverGlobalParams.defaultProfile != 0) {
        LOGV2_ERROR(20534,
                    "Running the selected storage engine with profiling is not supported",
                    "storageEngine"_attr = storageGlobalParams.engine);
        exitCleanly(ExitCode::badOptions);
    }

    if (storageGlobalParams.repair && replSettings.isReplSet()) {
        LOGV2_ERROR(5019200,
                    "Cannot specify both repair and replSet at the same time (remove --replSet to "
                    "be able to --repair)");
        exitCleanly(ExitCode::badOptions);
    }

    if (gAllowDocumentsGreaterThanMaxUserSize && replSettings.isReplSet()) {
        LOGV2_ERROR(8472200,
                    "allowDocumentsGreaterThanMaxUserSize can only be used in standalone mode");
        exitCleanly(ExitCode::badOptions);
    }

    logMongodStartupWarnings(storageGlobalParams, serverGlobalParams, serviceContext);

    {
        std::stringstream ss;
        ss << endl;
        ss << "*********************************************************************" << endl;
        ss << " ERROR: dbpath (" << storageGlobalParams.dbpath << ") does not exist." << endl;
        ss << " Create this directory or give existing directory in --dbpath." << endl;
        ss << " See http://dochub.mongodb.org/core/startingandstoppingmongo" << endl;
        ss << "*********************************************************************" << endl;
        uassert(10296, ss.str().c_str(), boost::filesystem::exists(storageGlobalParams.dbpath));
    }

    startWatchdog(serviceContext);

    auto startupOpCtx = serviceContext->makeOperationContext(&cc());

    try {
        startup_recovery::repairAndRecoverDatabases(
            startupOpCtx.get(), lastShutdownState, &startupTimeElapsedBuilder);
    } catch (const ExceptionFor<ErrorCodes::MustDowngrade>& error) {
        LOGV2_FATAL_OPTIONS(
            20573,
            logv2::LogOptions(logv2::LogComponent::kControl, logv2::FatalMode::kContinue),
            "Wrong mongod version",
            "error"_attr = error.toStatus().reason());
        exitCleanly(ExitCode::needDowngrade);
    } catch (const ExceptionFor<ErrorCodes::OfflineValidationFailedToComplete>& e) {
        LOGV2_ERROR(9437300, "Offline validation failed", "error"_attr = e.toString());
        exitCleanly(ExitCode::fail);
    }

    if (storageGlobalParams.validate) {
        LOGV2(9437302, "Finished validating collections");
        exitCleanly(ExitCode::clean);
    }

    // If we are on standalone, load cluster parameters from disk. If we are replicated, this is not
    // a concern as the cluster parameter initializer runs automatically.
    auto replCoord = repl::ReplicationCoordinator::get(startupOpCtx.get());
    invariant(replCoord);
    if (!replCoord->getSettings().isReplSet()) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::standaloneClusterParams,
                                       &startupTimeElapsedBuilder);
        ClusterServerParameterInitializer::synchronizeAllParametersFromDisk(startupOpCtx.get());
    }

    FeatureCompatibilityVersion::afterStartupActions(startupOpCtx.get());

    if (gFlowControlEnabled.load()) {
        LOGV2(20536, "Flow Control is enabled on this deployment");
    }

    {
        Lock::GlobalWrite globalLk(startupOpCtx.get());
        DurableHistoryRegistry::get(serviceContext)->reconcilePins(startupOpCtx.get());

        // Initialize the cached pointer to the oplog collection. We want to do this even as
        // standalone
        // so accesses to the cached pointer in replica set nodes started as standalone still work
        // (mainly AutoGetOplogFastPath). In case the oplog doesn't exist, it is just initialized to
        // null. This initialization must happen within a GlobalWrite lock context.
        repl::acquireOplogCollectionForLogging(startupOpCtx.get());
    }

    // Notify the storage engine that startup is completed before repair exits below, as repair sets
    // the upgrade flag to true.
    auto storageEngine = serviceContext->getStorageEngine();
    invariant(storageEngine);
    storageEngine->notifyStorageStartupRecoveryComplete();

    BackupCursorHooks::initialize(serviceContext);

    // Since extensions modify the global parserMap, which is not thread-safe, they must be loaded
    // prior to starting the FTDC background thread (which reads from the parserMap) to avoid a data
    // race.
    if (!extension::host::loadExtensions(serverGlobalParams.extensions)) {
        exitCleanly(ExitCode::badOptions);
    }

    startMongoDFTDC(serviceContext);

    if (mongodGlobalParams.scriptingEnabled) {
        ScriptEngine::setup(ExecutionEnvironment::Server);
    }

    const auto isStandalone =
        !repl::ReplicationCoordinator::get(serviceContext)->getSettings().isReplSet();

    if (storageGlobalParams.repair) {
        // Change stream collections can exist, even on a standalone, provided the standalone used
        // to be part of a replica set. Ensure the change stream collections on startup contain
        // consistent data.
        //
        // This is here because repair will shutdown the node as it implies --upgrade as well. The
        // branch below will exit the server.
        startup_recovery::recoverChangeStreamCollections(
            startupOpCtx.get(), isStandalone, lastShutdownState);
    }

    if (storageGlobalParams.upgrade) {
        LOGV2(20537, "Finished checking dbs");
        exitCleanly(ExitCode::clean);
    }

    // Start up health log writer thread.
    HealthLogInterface::set(serviceContext, std::make_unique<HealthLog>());
    HealthLogInterface::get(startupOpCtx.get())->startup();

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::userAndRolesGraph,
                                       &startupTimeElapsedBuilder);
        uassertStatusOK(globalAuthzManagerFactory->initialize(startupOpCtx.get()));
    }

    if (audit::initializeManager) {
        audit::initializeManager(startupOpCtx.get());
    }

    getLocalExecutor(serviceContext)->startup();

    // This is for security on certain platforms (nonce generation)
    srand((unsigned)(curTimeMicros64()) ^ (unsigned(uintptr_t(&startupOpCtx))));  // NOLINT

    auto const authzManagerShard =
        AuthorizationManager::get(serviceContext->getService(ClusterRole::ShardServer));

    if (authzManagerShard->shouldValidateAuthSchemaOnStartup()) {
        Status status = verifySystemIndexes(startupOpCtx.get(), &startupTimeElapsedBuilder);
        if (!status.isOK()) {
            LOGV2_WARNING(20538, "Unable to verify system indexes", "error"_attr = redact(status));
            if (status == ErrorCodes::AuthSchemaIncompatible) {
                exitCleanly(ExitCode::needUpgrade);
            } else if (status == ErrorCodes::NotWritablePrimary) {
                // Try creating the indexes if we become primary.  If we do not become primary,
                // the master will create the indexes and we will replicate them.
            } else {
                quickExit(ExitCode::fail);
            }
        }
    } else if (authzManagerShard->isAuthEnabled()) {
        LOGV2_ERROR(20569, "Auth must be disabled when starting without auth schema validation");
        exitCleanly(ExitCode::badOptions);
    } else {
        // If authSchemaValidation is disabled and server is running without auth,
        // warn the user and continue startup without authSchema metadata checks.
        LOGV2_WARNING_OPTIONS(
            20543,
            {logv2::LogTag::kStartupWarnings},
            "** WARNING: Startup auth schema validation checks are disabled for the database");
        LOGV2_WARNING_OPTIONS(
            20544,
            {logv2::LogTag::kStartupWarnings},
            "**          This mode should only be used to manually repair corrupted auth data");
    }

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::waitForMajorityService,
                                       &startupTimeElapsedBuilder);
        WaitForMajorityService::get(serviceContext).startup(serviceContext);
    }

    if (auto shardIdentityDoc =
            ShardingInitializationMongoD::getShardIdentityDoc(startupOpCtx.get())) {
        if (!serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            // A config shard initializes sharding awareness after setting up its config server
            // state.

            // This function will take the global lock.
            initializeShardingAwarenessAndLoadGlobalSettings(
                startupOpCtx.get(), *shardIdentityDoc, &startupTimeElapsedBuilder);
        }
    }

    try {
        if (serverGlobalParams.clusterRole.has(ClusterRole::None) && replSettings.isReplSet()) {
            ReadWriteConcernDefaults::get(startupOpCtx.get())
                .refreshIfNecessary(startupOpCtx.get());
        }
    } catch (const DBException& ex) {
        LOGV2_WARNING(20567,
                      "Error loading read and write concern defaults at startup",
                      "error"_attr = redact(ex));
    }
    readWriteConcernDefaultsMongodStartupChecks(startupOpCtx.get(), replSettings.isReplSet());

    MirrorMaestro::init(serviceContext);

    // Perform replication recovery for queryable backup mode if needed.
    if (storageGlobalParams.queryableBackupMode) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "Cannot specify both queryableBackupMode and "
                              << "recoverFromOplogAsStandalone at the same time",
                !replSettings.shouldRecoverFromOplogAsStandalone());
        uassert(
            ErrorCodes::BadValue,
            str::stream()
                << "Cannot take an unstable checkpoint on shutdown while using queryableBackupMode",
            !gTakeUnstableCheckpointOnShutdown);
        uassert(5576603,
                str::stream() << "Cannot specify both queryableBackupMode and "
                              << "startupRecoveryForRestore at the same time",
                !repl::startupRecoveryForRestore);

        // This uassert will also cover if we are running magic restore.
        uassert(ErrorCodes::BadValue,
                str::stream() << "Cannot use queryableBackupMode in a replica set",
                !replCoord->getSettings().isReplSet());
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::startUpReplCoord,
                                       &startupTimeElapsedBuilder);
        replCoord->startup(startupOpCtx.get(), lastShutdownState);
    } else {
        if (rss.getPersistenceProvider().supportsLocalCollections() &&
            storageEngine->supportsCappedCollections()) {
            logStartup(startupOpCtx.get());
        }

        ResourceYielderFactory::initialize(serviceContext);

        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            {
                SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                               TimedSectionId::configServerState,
                                               &startupTimeElapsedBuilder);

                initializeGlobalShardingStateForConfigServer(startupOpCtx.get());
            }

            // TODO: SERVER-82965 We shouldn't need to read the doc multiple times once we are in
            // sharding only development since config servers can always create it themselves.
            if (auto shardIdentityDoc =
                    ShardingInitializationMongoD::getShardIdentityDoc(startupOpCtx.get())) {
                // This function will take the global lock.
                initializeShardingAwarenessAndLoadGlobalSettings(
                    startupOpCtx.get(), *shardIdentityDoc, &startupTimeElapsedBuilder);
            }
        } else {
            // On a dedicated shard server, ShardingReady is always set because there is guaranteed
            // to be at least one shard in the sharded cluster (either the config shard or a
            // dedicated shard server).
            ShardingReady::get(startupOpCtx.get())->setIsReady();
        }

        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            // Sharding is always ready when there is at least one shard at startup (either the
            // config shard or a dedicated shard server).
            ShardingReady::get(startupOpCtx.get())->setIsReadyIfShardExists(startupOpCtx.get());
        }

        if (serverGlobalParams.clusterRole.has(ClusterRole::RouterServer)) {
            // Router role should use SEPMongos
            serviceContext->getService(ClusterRole::RouterServer)
                ->setServiceEntryPoint(std::make_unique<ServiceEntryPointRouterRole>());
        }

        if (replSettings.isReplSet() &&
            (serverGlobalParams.clusterRole.has(ClusterRole::None) ||
             !Grid::get(startupOpCtx.get())->isShardingInitialized())) {
            // If this is a mongod in a standalone replica set or a shardsvr replica set that has
            // not initialized its sharding identity, start up the cluster time keys manager with a
            // local/direct keys client. The keys client must use local read concern if the storage
            // engine can't support majority read concern. If this is a mongod in a configsvr or
            // shardsvr replica set that has initialized its sharding identity, the keys manager is
            // by design initialized separately with a sharded keys client when the sharding state
            // is initialized.
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::clusterTimeKeysManager,
                                           &startupTimeElapsedBuilder);
            auto keysClientMustUseLocalReads = false;
            auto keysCollectionClient =
                std::make_unique<KeysCollectionClientDirect>(keysClientMustUseLocalReads);
            auto keyManager = std::make_shared<KeysCollectionManager>(
                KeysCollectionManager::kKeyManagerPurposeString,
                std::move(keysCollectionClient),
                Seconds(KeysRotationIntervalSec));
            keyManager->startMonitoring(startupOpCtx->getServiceContext());

            LogicalTimeValidator::set(startupOpCtx->getServiceContext(),
                                      std::make_unique<LogicalTimeValidator>(keyManager));
        }

        if (replSettings.isReplSet() && serverGlobalParams.clusterRole.has(ClusterRole::None)) {
            ReplicaSetNodeProcessInterface::getReplicaSetNodeExecutor(serviceContext)->startup();
        }

        {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::startUpReplCoord,
                                           &startupTimeElapsedBuilder);
            replCoord->startup(startupOpCtx.get(), lastShutdownState);
        }

        // 'getOldestActiveTimestamp', which is called in the background by the checkpoint thread,
        // requires a read on 'config.transactions' at the stableTimestamp. If this read occurs
        // while applying prepared transactions at the end of replication recovery, it's possible to
        // prepare a transaction at timestamp earlier than the stableTimestamp. This will result in
        // a WiredTiger invariant. Register the callback after the call to 'startup' to ensure we've
        // finished applying prepared transactions.
        if (replCoord->getSettings().isReplSet()) {
            storageEngine->setOldestActiveTransactionTimestampCallback(
                TransactionParticipant::getOldestActiveTimestamp);
        }

        if (getReplSetMemberInStandaloneMode(serviceContext)) {
            LOGV2_WARNING_OPTIONS(
                20547,
                {logv2::LogTag::kStartupWarnings},
                "Document(s) exist in 'system.replset', but started without --replSet. Database "
                "contents may appear inconsistent with the writes that were visible when this node "
                "was running as part of a replica set. Restart with --replSet unless you are doing "
                "maintenance and no other clients are connected. The TTL collection monitor will "
                "not start because of this. For more info see "
                "http://dochub.mongodb.org/core/ttlcollections");

            if (gAllowUnsafeUntimestampedWrites &&
                !repl::ReplSettings::shouldRecoverFromOplogAsStandalone()) {
                LOGV2_WARNING_OPTIONS(
                    7692300,
                    {logv2::LogTag::kStartupWarnings},
                    "Replica set member is in standalone mode. Performing any writes will result "
                    "in them being untimestamped. If a write is to an existing document, the "
                    "document's history will be overwritten with the new value since the beginning "
                    "of time. This can break snapshot isolation within the storage engine.");
            }
        } else {
            startTTLMonitor(serviceContext);
        }

        if (replSettings.isReplSet() || !gInternalValidateFeaturesAsPrimary) {
            serverGlobalParams.validateFeaturesAsPrimary.store(false);
        }

        storageEngine->startTimestampMonitor(
            {&catalog_helper::kCollectionCatalogCleanupTimestampListener});

        startFLECrud(serviceContext);

        DiskSpaceMonitor::start(serviceContext);
        if (!storageEngine->storesFilesInDbPath()) {
            LOGV2(7333400,
                  "The index builds DiskSpaceMonitor action which periodically checks if we "
                  "have enough disk space to build indexes will not run when the storage engine "
                  "stores data files in different directories");
        } else {
            auto diskMonitor = DiskSpaceMonitor::get(serviceContext);
            IndexBuildsCoordinator::get(serviceContext)->registerKillIndexBuildAction(*diskMonitor);
        }
    }

    startClientCursorMonitor();

    PeriodicTask::startRunningPeriodicTasks();

    auto shardService = serviceContext->getService(ClusterRole::ShardServer);
    SessionKiller::set(shardService,
                       std::make_shared<SessionKiller>(shardService, killSessionsLocal));

    if (serverGlobalParams.clusterRole.has(ClusterRole::RouterServer)) {
        auto routerService = serviceContext->getService(ClusterRole::RouterServer);
        SessionKiller::set(routerService,
                           std::make_shared<SessionKiller>(routerService, killSessionsRemote));
    }

    // Start up a background task to periodically check for and kill expired transactions; and a
    // background task to periodically check for and decrease cache pressure by decreasing the
    // target size setting for the storage engine's window of available snapshots.
    //
    // Only do this on storage engines supporting snapshot reads, which hold resources we wish to
    // release periodically in order to avoid storage cache pressure build up.
    if (storageEngine->supportsReadConcernSnapshot()) {
        try {
            PeriodicThreadToAbortExpiredTransactions::get(serviceContext)->start();
            if (gCachePressureQueryPeriodMilliseconds.load() != 0) {
                PeriodicThreadToRollbackUnderCachePressure::get(serviceContext)->start();
            }
        } catch (ExceptionFor<ErrorCodes::PeriodicJobIsStopped>&) {
            LOGV2_WARNING(4747501, "Not starting periodic jobs as shutdown is in progress");
            logStartupStats = {};
            // Shutdown has already started before initialization is complete. Wait for the
            // shutdown task to complete and return.
            MONGO_IDLE_THREAD_BLOCK;
            return waitForShutdown();
        }
    }

    // Change stream collections can exist, even on a standalone, provided the standalone used to be
    // part of a replica set. Ensure the change stream collections on startup contain consistent
    // data.
    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::recoverChangeStream,
                                       &startupTimeElapsedBuilder);
        startup_recovery::recoverChangeStreamCollections(
            startupOpCtx.get(), isStandalone, lastShutdownState);
    }

    // If not in standalone mode, start background tasks to:
    //  * Periodically remove expired documents from change collections
    if (!isStandalone) {
        if (!gChangeCollectionRemoverDisabled) {
            startChangeCollectionExpiredDocumentsRemover(serviceContext);
        }
        if (serverGlobalParams.replicaSetConfigShardMaintenanceMode) {
            PeriodicReplicaSetConfigShardMaintenanceModeChecker::get(serviceContext)->start();
        }
    }

    if (computeModeEnabled) {
        if (!isStandalone || !serverGlobalParams.clusterRole.has(ClusterRole::None)) {
            LOGV2_ERROR(6968200, "'enableComputeMode' can be used only in standalone server");
            exitCleanly(ExitCode::badOptions);
        }
        if (externalPipeDir != "" && externalPipeDir.find("..") != std::string::npos) {
            LOGV2_ERROR(7001102, "'externalPipeDir' must not have '..'");
            exitCleanly(ExitCode::badOptions);
        }

        LOGV2_WARNING_OPTIONS(
            6968201,
            {logv2::LogTag::kStartupWarnings},
            "There could be security risks in using 'enableComputeMode'. It is recommended to use "
            "this mode under an isolated environment and execute the server under a user with "
            "restricted access permissions");
    } else {
        if (externalPipeDir != "") {
            LOGV2_WARNING_OPTIONS(
                7001103,
                {logv2::LogTag::kStartupWarnings},
                "'externalPipeDir' is effective only when enableComputeMode=true");
        }
    }

    // Set up the logical session cache
    auto logicalSessionCache = [&] {
        LogicalSessionCacheServer kind = LogicalSessionCacheServer::kStandalone;
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            kind = LogicalSessionCacheServer::kConfigServer;
        } else if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
            kind = LogicalSessionCacheServer::kSharded;
        } else if (replSettings.isReplSet()) {
            kind = LogicalSessionCacheServer::kReplicaSet;
        }
        return makeLogicalSessionCacheD(
            kind, serverGlobalParams.clusterRole.has(ClusterRole::RouterServer));
    }();
    LogicalSessionCache::set(serviceContext, std::move(logicalSessionCache));

    if (analyze_shard_key::supportsSamplingQueries(serviceContext) &&
        serverGlobalParams.clusterRole.has(ClusterRole::None)) {
        analyze_shard_key::QueryAnalysisSampler::get(serviceContext).onStartup();
    }

    auto cacheLoader = std::make_unique<stats::StatsCacheLoaderImpl>();
    auto catalog = std::make_unique<stats::StatsCatalog>(
        serviceContext->getService(ClusterRole::ShardServer), std::move(cacheLoader));
    stats::StatsCatalog::set(serviceContext, std::move(catalog));

    // Startup options are written to the audit log at the end of startup so that cluster server
    // parameters are guaranteed to have been initialized from disk at this point.
    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::logStartupOptions,
                                       &startupTimeElapsedBuilder);
        audit::logStartupOptions(Client::getCurrent(), serverGlobalParams.parsedOpts);
    }

    // MessageServer::run will return when exit code closes its socket and we don't need the
    // operation context anymore
    startupOpCtx.reset();

    executor::startupSearchExecutorsIfNeeded(serviceContext);

    transport::ServiceExecutor::startupAll(serviceContext);

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::startUpTransportLayer,
                                       &startupTimeElapsedBuilder);
        if (auto start = serviceContext->getTransportLayerManager()->start(); !start.isOK()) {
            LOGV2_ERROR(20572, "Error starting listener", "error"_attr = start);
            return ExitCode::netError;
        }
    }

    if (!initialize_server_global_state::writePidFile()) {
        quickExit(ExitCode::fail);
    }

    serviceContext->notifyStorageStartupRecoveryComplete();

#ifndef _WIN32
    initialize_server_global_state::signalForkSuccess();
#else
    if (ntservice::shouldStartService()) {
        ntservice::reportStatus(SERVICE_RUNNING);
        LOGV2(20555, "Service running");
    }
#endif

    if (MONGO_unlikely(shutdownAtStartup.shouldFail())) {
        LOGV2(20556, "Starting clean exit via failpoint");
        exitCleanly(ExitCode::clean);
    }

    globalServerLifecycleMonitor().onFinishingStartup();

    logStartupStats = {};

    MONGO_IDLE_THREAD_BLOCK;
    return waitForShutdown();
}

ExitCode initAndListen(ServiceContext* service) {
    try {
        return _initAndListen(service);
    } catch (DBException& e) {
        LOGV2_ERROR(
            20557, "DBException in initAndListen, terminating", "error"_attr = e.toString());
        return ExitCode::uncaught;
    } catch (std::exception& e) {
        LOGV2_ERROR(20558, "std::exception in initAndListen, terminating", "error"_attr = e.what());
        return ExitCode::uncaught;
    } catch (int& n) {
        LOGV2_ERROR(20559, "Exception in initAndListen, terminating", "reason"_attr = n);
        return ExitCode::uncaught;
    } catch (...) {
        LOGV2_ERROR(20560, "Exception in initAndListen, terminating");
        return ExitCode::uncaught;
    }
}

#if defined(_WIN32)
ExitCode initService() {
    return initAndListen(getGlobalServiceContext());
}
#endif

MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
    initialize_server_global_state::forkServerOrDie();
}

#ifdef __linux__
/**
 * Read the pid file from the dbpath for the process ID used by this instance of the server.
 * Use that process number to kill the running server.
 *
 * Equivalent to: `kill -SIGTERM $(cat $DBPATH/mongod.lock)`
 *
 * Performs additional checks to make sure the PID as read is reasonable (>= 1)
 * and can be found in the /proc filesystem.
 */
Status shutdownProcessByDBPathPidFile(const std::string& dbpath) {
    auto pidfile = (boost::filesystem::path(dbpath) / std::string{kLockFileBasename}).string();
    if (!boost::filesystem::exists(pidfile)) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "There doesn't seem to be a server running with dbpath: "
                              << dbpath};
    }

    pid_t pid;
    try {
        std::ifstream f(pidfile.c_str());
        f >> pid;
    } catch (const std::exception& ex) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "Error reading pid from lock file [" << pidfile
                              << "]: " << ex.what()};
    }

    if (pid <= 0) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "Invalid process ID '" << pid
                              << "' read from pidfile: " << pidfile};
    }

    std::string procPath = str::stream() << "/proc/" << pid;
    if (!boost::filesystem::exists(procPath)) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "Process ID '" << pid << "' read from pidfile '" << pidfile
                              << "' does not appear to be running"};
    }

    std::cout << "Killing process with pid: " << pid << std::endl;
    int ret = kill(pid, SIGTERM);
    if (ret) {
        auto ec = lastSystemError();
        return {ErrorCodes::OperationFailed,
                str::stream() << "Failed to kill process: " << errorMessage(ec)};
    }

    // Wait for process to terminate.
    for (;;) {
        std::uintmax_t pidsize = boost::filesystem::file_size(pidfile);
        if (pidsize == 0) {
            // File empty.
            break;
        }
        if (pidsize == static_cast<decltype(pidsize)>(-1)) {
            // File does not exist.
            break;
        }
        sleepsecs(1);
    }

    return Status::OK();
}
#endif  // __linux__

/*
 * This function should contain the startup "actions" that we take based on the startup config.
 * It is intended to separate the actions from "storage" and "validation" of our startup
 * configuration.
 */
void startupConfigActions(const std::vector<std::string>& args) {
    // The "command" option is deprecated.  For backward compatibility, still support the "run"
    // and "dbppath" command.  The "run" command is the same as just running mongod, so just
    // falls through.
    if (moe::startupOptionsParsed.count("command")) {
        const auto command = moe::startupOptionsParsed["command"].as<std::vector<std::string>>();

        if (command[0].compare("dbpath") == 0) {
            std::cout << storageGlobalParams.dbpath << endl;
            quickExit(ExitCode::clean);
        }

        if (command[0].compare("run") != 0) {
            std::cout << "Invalid command: " << command[0] << endl;
            printMongodHelp(moe::startupOptions);
            quickExit(ExitCode::fail);
        }

        if (command.size() > 1) {
            std::cout << "Too many parameters to 'run' command" << endl;
            printMongodHelp(moe::startupOptions);
            quickExit(ExitCode::fail);
        }
    }

#ifdef _WIN32
    ntservice::configureService(initService,
                                moe::startupOptionsParsed,
                                defaultServiceStrings,
                                std::vector<std::string>(),
                                args);
#endif  // _WIN32

#ifdef __linux__
    if (moe::startupOptionsParsed.count("shutdown") &&
        moe::startupOptionsParsed["shutdown"].as<bool>() == true) {
        auto status = shutdownProcessByDBPathPidFile(storageGlobalParams.dbpath);
        if (!status.isOK()) {
            std::cerr << status.reason() << std::endl;
            quickExit(ExitCode::fail);
        }

        quickExit(ExitCode::clean);
    }
#endif
}

void setUpCatalog(ServiceContext* serviceContext) {
    DatabaseHolder::set(serviceContext, std::make_unique<DatabaseHolderImpl>());
    Collection::Factory::set(serviceContext, std::make_unique<CollectionImpl::FactoryImpl>());
}

auto makeReplicaSetNodeExecutor(ServiceContext* serviceContext) {
    ThreadPool::Options tpOptions;
    tpOptions.threadNamePrefix = "ReplNodeDbWorker-";
    tpOptions.poolName = "ReplNodeDbWorkerThreadPool";
    tpOptions.maxThreads = ThreadPool::Options::kUnlimited;
    tpOptions.onCreateThread = [serviceContext](const std::string& threadName) {
        Client::initThread(threadName,
                           serviceContext->getService(ClusterRole::ShardServer),
                           Client::noSession(),
                           ClientOperationKillableByStepdown{false});
    };
    return executor::ThreadPoolTaskExecutor::create(
        std::make_unique<ThreadPool>(tpOptions),
        executor::makeNetworkInterface(
            "ReplNodeDbWorkerNetwork", nullptr, makeShardingEgressHooksList(serviceContext)));
}

void setUpReplicaSetDDLHooks(ServiceContext* serviceContext) {
    ReplicaSetDDLTracker::create(serviceContext);
    DirectConnectionDDLHook::create(serviceContext);
}

void setUpReplication(ServiceContext* serviceContext) {
    auto& serviceLifecycle =
        rss::ReplicatedStorageService::get(serviceContext).getServiceLifecycle();
    serviceLifecycle.initializeStateRequiredForStorageAccess(serviceContext);

    repl::StorageInterface::set(serviceContext, std::make_unique<repl::StorageInterfaceImpl>());
    auto storageInterface = repl::StorageInterface::get(serviceContext);

    auto consistencyMarkers =
        std::make_unique<repl::ReplicationConsistencyMarkersImpl>(storageInterface);
    auto recovery =
        std::make_unique<repl::ReplicationRecoveryImpl>(storageInterface, consistencyMarkers.get());
    repl::ReplicationProcess::set(
        serviceContext,
        std::make_unique<repl::ReplicationProcess>(
            storageInterface, std::move(consistencyMarkers), std::move(recovery)));

    std::unique_ptr<repl::ReplicationCoordinator> replCoord =
        serviceLifecycle.initializeReplicationCoordinator(serviceContext);

    // Only create a ReplicaSetNodeExecutor if sharding is disabled and replication is enabled.
    // Note that sharding sets up its own executors for scheduling work to remote nodes.
    if (serverGlobalParams.clusterRole.has(ClusterRole::None) &&
        replCoord->getSettings().isReplSet()) {
        ReplicaSetNodeProcessInterface::setReplicaSetNodeExecutor(
            serviceContext, makeReplicaSetNodeExecutor(serviceContext));

        analyze_shard_key::QueryAnalysisClient::get(serviceContext)
            .setTaskExecutor(
                serviceContext,
                ReplicaSetNodeProcessInterface::getReplicaSetNodeExecutor(serviceContext));
    }

    repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));

    MongoDSessionCatalog::set(
        serviceContext,
        std::make_unique<MongoDSessionCatalog>(
            std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

    IndexBuildsCoordinator::set(serviceContext, std::make_unique<IndexBuildsCoordinatorMongod>());

    // Register primary-only services here so that the services are started up when the replication
    // coordinator starts up.
    registerPrimaryOnlyServices(serviceContext);

    // Register replica set DDL hooks.
    setUpReplicaSetDDLHooks(serviceContext);
}

void setUpObservers(ServiceContext* serviceContext) {
    auto opObserverRegistry = std::make_unique<OpObserverRegistry>();
    if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        DurableHistoryRegistry::get(serviceContext)
            ->registerPin(std::make_unique<ReshardingHistoryHook>());
        opObserverRegistry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerTransactionProxy>(
                std::make_unique<OperationLoggerImpl>())));
        opObserverRegistry->addObserver(std::make_unique<FindAndModifyImagesOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<ChangeStreamPreImagesOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<MigrationChunkClonerSourceOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<ShardServerOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<ReshardingOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<UserWriteBlockModeOpObserver>());

        if (!gMultitenancySupport) {
            opObserverRegistry->addObserver(
                std::make_unique<analyze_shard_key::QueryAnalysisOpObserverShardSvr>());
        }
    }

    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        opObserverRegistry->addObserver(std::make_unique<ConfigServerOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<ReshardingOpObserver>());
        if (!gMultitenancySupport) {
            opObserverRegistry->addObserver(
                std::make_unique<analyze_shard_key::QueryAnalysisOpObserverConfigSvr>());
        }
    }

    if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
        opObserverRegistry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
        opObserverRegistry->addObserver(std::make_unique<FindAndModifyImagesOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<ChangeStreamPreImagesOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<UserWriteBlockModeOpObserver>());

        auto replCoord = repl::ReplicationCoordinator::get(serviceContext);
        if (!gMultitenancySupport && replCoord && replCoord->getSettings().isReplSet()) {
            opObserverRegistry->addObserver(
                std::make_unique<analyze_shard_key::QueryAnalysisOpObserverRS>());
        }
    }

    opObserverRegistry->addObserver(std::make_unique<FallbackOpObserver>());
    opObserverRegistry->addObserver(std::make_unique<TimeSeriesOpObserver>());
    opObserverRegistry->addObserver(std::make_unique<AuthOpObserver>());
    opObserverRegistry->addObserver(
        std::make_unique<repl::PrimaryOnlyServiceOpObserver>(serviceContext));
    opObserverRegistry->addObserver(std::make_unique<FcvOpObserver>());
    opObserverRegistry->addObserver(std::make_unique<ClusterServerParameterOpObserver>());

    if (audit::opObserverRegistrar) {
        audit::opObserverRegistrar(opObserverRegistry.get());
    }

    serviceContext->setOpObserver(std::move(opObserverRegistry));
}

void setUpSharding(ServiceContext* service) {
    ShardingState::create(service);
    CollectionShardingStateFactory::set(
        service, std::make_unique<CollectionShardingStateFactoryShard>(service));
    DatabaseShardingStateFactory::set(service,
                                      std::make_unique<DatabaseShardingStateFactoryShard>());
}

namespace {
ServiceContext::ConstructorActionRegisterer registerWireSpec{
    "RegisterWireSpec", [](ServiceContext* service) {
        // The featureCompatibilityVersion behavior defaults to the downgrade behavior while the
        // in-memory version is unset.
        WireSpec::Specification spec;
        spec.incomingInternalClient.minWireVersion = RELEASE_2_4_AND_BEFORE;
        spec.incomingInternalClient.maxWireVersion = LATEST_WIRE_VERSION;
        spec.outgoing.minWireVersion = SUPPORTS_OP_MSG;
        spec.outgoing.maxWireVersion = LATEST_WIRE_VERSION;
        spec.isInternalClient = true;

        WireSpec::getWireSpec(service).initialize(std::move(spec));
    }};
}  // namespace

#ifdef MONGO_CONFIG_SSL
MONGO_INITIALIZER_GENERAL(setSSLManagerType, (), ("SSLManager"))
(InitializerContext* context) {
    isSSLServer = true;
}
#endif

struct ShutdownContext {
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
};

// Stash the ShutdownContext as a ServiceContext decoration. The main purpose of this is to keep the
// OperationContext alive until the process calls exit, to avoid releasing the global lock.
ServiceContext::Decoration<ShutdownContext> getShutdownContext =
    ServiceContext::declareDecoration<ShutdownContext>();

// NOTE: This function may be called at any time after registerShutdownTask is called below. It
// must not depend on the prior execution of mongo initializers or the existence of threads.
void shutdownTask(const ShutdownTaskArgs& shutdownArgs) {
    Milliseconds shutdownTimeout;
    if (shutdownArgs.quiesceTime) {
        shutdownTimeout = *shutdownArgs.quiesceTime;
    } else {
        invariant(!shutdownArgs.isUserInitiated);
        shutdownTimeout = Milliseconds(repl::shutdownTimeoutMillisForSignaledShutdown.load());
    }

    if (MONGO_unlikely(hangBeforeShutdown.shouldFail())) {
        LOGV2(4944800, "Hanging before shutdown due to hangBeforeShutdown failpoint");
        hangBeforeShutdown.pauseWhileSet();
    }

    BSONObjBuilder shutdownTimeElapsedBuilder;
    BSONObjBuilder shutdownInfoBuilder;

    // Before doing anything else, ensure fsync is inactive or make it release its GlobalRead lock.
    {
        stdx::unique_lock<stdx::mutex> stateLock(fsyncStateMutex);
        if (globalFsyncLockThread) {
            globalFsyncLockThread->shutdown(stateLock);
        }
    }

    auto const serviceContext = getGlobalServiceContext();

    SectionScopedTimer shutdownTotalTimerGuard(serviceContext->getFastClockSource(),
                                               TimedSectionId::shutdownTaskTotal,
                                               &shutdownTimeElapsedBuilder);
    ScopeGuard logShutdownStats = [&] {
        shutdownTotalTimerGuard = {};
        shutdownInfoBuilder.append("Statistics", shutdownTimeElapsedBuilder.obj());
        LOGV2_INFO(8423404,
                   "mongod shutdown complete",
                   "Summary of time elapsed"_attr = shutdownInfoBuilder.obj());
    };

    UserCacheInvalidator::stop(serviceContext);

    // If we don't have shutdownArgs, we're shutting down from a signal, or other clean shutdown
    // path.
    //
    // In that case, do a default step down, still shutting down if stepDown fails.
    auto const replCoord = repl::ReplicationCoordinator::get(serviceContext);
    bool const defaultStepdownRequired = replCoord && !shutdownArgs.isUserInitiated;

    // The operation context used for shutdown must be created after starting terminal shutdown on
    // the replication coordinator. This is because terminal shutdown might involve waiting for all
    // opCtx's to be destroyed if FCBIS is swapping the storage engine.
    if (defaultStepdownRequired) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::enterTerminalShutdown,
                                       &shutdownTimeElapsedBuilder);
        replCoord->enterTerminalShutdown();
    }

    // Store previous client, to be restored when function scope ends.
    ServiceContext::UniqueClient oldClient;
    if (Client::getCurrent()) {
        oldClient = Client::releaseCurrent();
    }
    Client::setCurrent(serviceContext->getService(ClusterRole::ShardServer)
                           ->makeClient("shutdownTask",
                                        Client::noSession(),
                                        ClientOperationKillableByStepdown{false}));
    const auto client = Client::getCurrent();

    // The new client and opCtx are stashed in the ServiceContext, will survive past this
    // function and are never destructed. This is required to avoid releasing the global lock until
    // the process calls exit().
    ServiceContext::UniqueOperationContext uniqueOpCtx = client->makeOperationContext();
    auto const opCtx = uniqueOpCtx.get();

    ON_BLOCK_EXIT([&]() {
        auto& shutdownContext = getShutdownContext(client->getServiceContext());
        invariant(!shutdownContext.client.get());
        invariant(!shutdownContext.opCtx.get());

        shutdownContext.client = Client::releaseCurrent();
        shutdownContext.opCtx = std::move(uniqueOpCtx);

        if (oldClient) {
            Client::setCurrent(std::move(oldClient));
        }
    });

    if (defaultStepdownRequired) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::stepDownReplCoord,
                                       &shutdownTimeElapsedBuilder);
        const auto forceShutdown = true;
        auto stepDownStartTime = opCtx->getServiceContext()->getPreciseClockSource()->now();
        // stepDown should never return an error during force shutdown.
        LOGV2_OPTIONS(4784900,
                      {LogComponent::kReplication},
                      "Stepping down the ReplicationCoordinator for shutdown",
                      "waitTime"_attr = shutdownTimeout);
        invariantStatusOK(stepDownForShutdown(opCtx, shutdownTimeout, forceShutdown));
        shutdownTimeout = std::max(
            Milliseconds::zero(),
            shutdownTimeout -
                (opCtx->getServiceContext()->getPreciseClockSource()->now() - stepDownStartTime));
    }

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::quiesceMode,
                                       &shutdownTimeElapsedBuilder);
        if (auto replCoord = repl::ReplicationCoordinator::get(serviceContext);
            replCoord && replCoord->enterQuiesceModeIfSecondary(shutdownTimeout)) {
            if (MONGO_unlikely(hangDuringQuiesceMode.shouldFail())) {
                LOGV2_OPTIONS(4695101,
                              {LogComponent::kReplication},
                              "hangDuringQuiesceMode failpoint enabled");
                hangDuringQuiesceMode.pauseWhileSet(opCtx);
            }

            LOGV2_OPTIONS(4695102,
                          {LogComponent::kReplication},
                          "Entering quiesce mode for shutdown",
                          "quiesceTime"_attr = shutdownTimeout);
            opCtx->sleepFor(shutdownTimeout);
            LOGV2_OPTIONS(
                4695103, {LogComponent::kReplication}, "Exiting quiesce mode for shutdown");
        }
    }

    DiskSpaceMonitor::stop(serviceContext);

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::stopFLECrud,
                                       &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(6371601, {LogComponent::kDefault}, "Shutting down the FLE Crud thread pool");
        stopFLECrud();
    }

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownMirrorMaestro,
                                       &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(4784901, {LogComponent::kCommand}, "Shutting down the MirrorMaestro");
        MirrorMaestro::shutdown(serviceContext);
    }

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownWaitForMajorityService,
                                       &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(
            4784902, {LogComponent::kSharding}, "Shutting down the WaitForMajorityService");
        WaitForMajorityService::get(serviceContext).shutDown();
    }

    // Join the logical session cache before the transport layer.
    if (auto lsc = LogicalSessionCache::get(serviceContext)) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownLogicalSessionCache,
                                       &shutdownTimeElapsedBuilder);
        LOGV2(4784903, "Shutting down the LogicalSessionCache");
        lsc->joinOnShutDown();
    }

    if (analyze_shard_key::supportsSamplingQueries(serviceContext)) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownQueryAnalysisSampler,
                                       &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(7350601, {LogComponent::kDefault}, "Shutting down the QueryAnalysisSampler");
        analyze_shard_key::QueryAnalysisSampler::get(serviceContext).onShutdown();
    }

    // Inform the TransportLayers to stop accepting new connections.
    if (auto tlm = serviceContext->getTransportLayerManager()) {
        LOGV2_OPTIONS(8314100, {LogComponent::kNetwork}, "Shutdown: Closing listener sockets");
        tlm->stopAcceptingSessions();
    }

    // Shut down the global dbclient pool so callers stop waiting for connections.
    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownGlobalConnectionPool,
                                       &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(
            4784905, {LogComponent::kNetwork}, "Shutting down the global connection pool");
        globalConnPool.shutdown();
    }

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownSearchTaskExecutors,
                                       &shutdownTimeElapsedBuilder);
        executor::shutdownSearchExecutorsIfNeeded(serviceContext);
    }

    // Inform Flow Control to stop gating writes on ticket admission. This must be done before the
    // Periodic Runner is shut down (see SERVER-41751).
    if (auto flowControlTicketholder = FlowControlTicketholder::get(serviceContext)) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownFlowControlTicketHolder,
                                       &shutdownTimeElapsedBuilder);
        LOGV2(4784906, "Shutting down the FlowControlTicketholder");
        flowControlTicketholder->setInShutdown();
    }

    if (auto exec = ReplicaSetNodeProcessInterface::getReplicaSetNodeExecutor(serviceContext)) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownReplicaSetNodeExecutor,
                                       &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(
            4784907, {LogComponent::kReplication}, "Shutting down the replica set node executor");
        exec->shutdown();
        exec->join();
    }

    if (auto storageEngine = serviceContext->getStorageEngine()) {
        if (storageEngine->supportsReadConcernSnapshot()) {
            {
                SectionScopedTimer scopedTimer(
                    serviceContext->getFastClockSource(),
                    TimedSectionId::shutDownAbortExpiredTransactionsThread,
                    &shutdownTimeElapsedBuilder);
                LOGV2(4784908, "Shutting down PeriodicThreadToAbortExpiredTransactions");
                PeriodicThreadToAbortExpiredTransactions::get(serviceContext)->stop();
            }
            {
                SectionScopedTimer scopedTimer(
                    serviceContext->getFastClockSource(),
                    TimedSectionId::shutDownRollbackUnderCachePressureThread,
                    &shutdownTimeElapsedBuilder);
                LOGV2(10036707, "Shutting down PeriodicThreadToRollbackUnderCachePressure");
                PeriodicThreadToRollbackUnderCachePressure::get(serviceContext)->stop();
            }
        }
        {
            stdx::lock_guard lg(*client);
            opCtx->setIsExecutingShutdown();
        }

        // This can wait a long time while we drain the secondary's apply queue, especially if
        // it is building an index.
        LOGV2_OPTIONS(
            4784909, {LogComponent::kReplication}, "Shutting down the ReplicationCoordinator");
        repl::ReplicationCoordinator::get(serviceContext)
            ->shutdown(opCtx, &shutdownTimeElapsedBuilder);

        // Terminate the index consistency check.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::shutDownIndexConsistencyChecker,
                                           &shutdownTimeElapsedBuilder);
            LOGV2_OPTIONS(4784904,
                          {LogComponent::kSharding},
                          "Shutting down the PeriodicShardedIndexConsistencyChecker");
            PeriodicShardedIndexConsistencyChecker::get(serviceContext).onShutDown();
        }

        LOGV2_OPTIONS(
            4784910, {LogComponent::kSharding}, "Shutting down the ShardingInitializationMongoD");
        ShardingInitializationMongoD::get(serviceContext)->shutDown(opCtx);

        // Acquire the RSTL in mode X. First we enqueue the lock request, then kill all
        // operations,
        // destroy all stashed transaction resources in order to release locks, and finally wait
        // until the lock request is granted.
        LOGV2_OPTIONS(4784911,
                      {LogComponent::kReplication},
                      "Enqueuing the ReplicationStateTransitionLock for shutdown");
        boost::optional<rss::consensus::ReplicationStateTransitionGuard> rstg;
        if (gFeatureFlagIntentRegistration.isEnabled()) {
            rstg.emplace(rss::consensus::IntentRegistry::get(serviceContext)
                             .killConflictingOperations(
                                 rss::consensus::IntentRegistry::InterruptionType::Shutdown,
                                 opCtx,
                                 0 /* no timeout */)
                             .get());
        }
        repl::ReplicationStateTransitionLockGuard rstl(
            opCtx, MODE_X, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());

        // Kill all operations except FTDC to continue gathering metrics. This makes all newly
        // created opCtx to be immediately interrupted. After this point, the opCtx will have been
        // marked as killed and will not be usable other than to kill all transactions directly
        // below.
        LOGV2_OPTIONS(4784912, {LogComponent::kDefault}, "Killing all operations for shutdown");
        {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::killAllOperations,
                                           &shutdownTimeElapsedBuilder);
            auto& serviceLifecycle =
                rss::ReplicatedStorageService::get(serviceContext).getServiceLifecycle();
            serviceContext->setKillAllOperations([&serviceLifecycle](const StringData t) {
                return t == kFTDCThreadName ||
                    serviceLifecycle.shouldKeepThreadAliveUntilStorageEngineHasShutDown(t);
            });

            if (MONGO_unlikely(pauseWhileKillingOperationsAtShutdown.shouldFail())) {
                LOGV2_OPTIONS(4701700,
                              {LogComponent::kDefault},
                              "pauseWhileKillingOperationsAtShutdown failpoint enabled");
                sleepsecs(1);
            }
        }

        // Destroy all stashed transaction resources, in order to release locks.
        {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::shutDownOpenTransactions,
                                           &shutdownTimeElapsedBuilder);
            LOGV2_OPTIONS(4784913, {LogComponent::kCommand}, "Shutting down all open transactions");
            killSessionsLocalShutdownAllTransactions(opCtx);
        }

        {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::acquireRSTL,
                                           &shutdownTimeElapsedBuilder);
            LOGV2_OPTIONS(4784914,
                          {LogComponent::kReplication},
                          "Acquiring the ReplicationStateTransitionLock for shutdown");
            rstl.waitForLockUntil(Date_t::max());
        }

        // Release the rstl before waiting for the index build threads to join as index build
        // reacquires rstl in uninterruptible lock guard to finish their cleanup process.
        rstl.release();
        rstg = boost::none;

        // Shuts down the thread pool and waits for index builds to finish.
        // Depends on setKillAllOperations() above to interrupt the index build operations.
        {
            SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                           TimedSectionId::shutDownIndexBuildsCoordinator,
                                           &shutdownTimeElapsedBuilder);
            LOGV2_OPTIONS(
                4784915, {LogComponent::kIndex}, "Shutting down the IndexBuildsCoordinator");
            IndexBuildsCoordinator::get(serviceContext)->shutdown(opCtx);
        }
    }

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownReplicaSetMonitor,
                                       &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(4784918, {LogComponent::kNetwork}, "Shutting down the ReplicaSetMonitor");
        ReplicaSetMonitor::shutdown();
    }

    if (ShardingState::get(serviceContext)->enabled()) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownTransactionCoord,
                                       &shutdownTimeElapsedBuilder);
        TransactionCoordinatorService::get(serviceContext)->shutdown();
    }

    // Validator shutdown must be called after setKillAllOperations is called. Otherwise, this can
    // deadlock.
    if (auto validator = LogicalTimeValidator::get(serviceContext)) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownLogicalTimeValidator,
                                       &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(
            4784920, {LogComponent::kReplication}, "Shutting down the LogicalTimeValidator");
        validator->shutDown();
    }

    if (auto grid = Grid::get(serviceContext)) {
        grid->shutdown(opCtx, &shutdownTimeElapsedBuilder, false /* isMongos */);
    }

    LOGV2_OPTIONS(9439300, {LogComponent::kSharding}, "Shutting down the filtering metadata cache");
    FilteringMetadataCache::get(opCtx)->shutDown();

    if (auto configServerRoutingInfoCache = RoutingInformationCache::get(serviceContext);
        configServerRoutingInfoCache != nullptr && !feature_flags::gDualCatalogCache.isEnabled()) {
        LOGV2_OPTIONS(
            8778000, {LogComponent::kSharding}, "Shutting down the RoutingInformationCache");
        configServerRoutingInfoCache->shutDownAndJoin();
    }

    // Finish shutting down the TransportLayers
    if (auto tlm = serviceContext->getTransportLayerManager()) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownTransportLayer,
                                       &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(20562, {LogComponent::kNetwork}, "Shutdown: Closing open transport sessions");
        tlm->shutdown();
    }

    if (auto* healthLog = HealthLogInterface::get(serviceContext)) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownHealthLog,
                                       &shutdownTimeElapsedBuilder);
        LOGV2(4784927, "Shutting down the HealthLog");
        healthLog->shutdown();
    }

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownTTLMonitor,
                                       &shutdownTimeElapsedBuilder);
        LOGV2(4784928, "Shutting down the TTL monitor");
        shutdownTTLMonitor(serviceContext);
    }

    PeriodicReplicaSetConfigShardMaintenanceModeChecker::get(serviceContext)->stop();

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownExpiredDocumentRemover,
                                       &shutdownTimeElapsedBuilder);
        shutdownChangeCollectionExpiredDocumentsRemover(serviceContext);
    }

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownOplogCapMaintainer,
                                       &shutdownTimeElapsedBuilder);
        OplogCapMaintainerThread::get(serviceContext)->shutdown();
    }

    // We should always be able to acquire the global lock at shutdown.
    //
    // For a Windows service, dbexit does not call exit(), so we must leak the lock outside
    // of this function to prevent any operations from running that need a lock.
    //
    LOGV2(4784929, "Acquiring the global lock for shutdown");
    shard_role_details::getLocker(opCtx)->lockGlobal(opCtx, MODE_X);

    // Global storage engine may not be started in all cases before we exit
    if (serviceContext->getStorageEngine()) {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownStorageEngine,
                                       &shutdownTimeElapsedBuilder);
        LOGV2(4784930, "Shutting down the storage engine");
        // Allow memory leak for faster shutdown.
        catalog::shutDownCollectionCatalogAndGlobalStorageEngineCleanly(serviceContext,
                                                                        true /* memLeakAllowed */);
    }

    // Depending on the underlying implementation, there may be some state that needs to be shut
    // down after the replication subsystem and the storage engine.
    auto& serviceLifecycle =
        rss::ReplicatedStorageService::get(serviceContext).getServiceLifecycle();
    serviceLifecycle.shutdownStateRequiredForStorageAccess(serviceContext,
                                                           &shutdownTimeElapsedBuilder);

    // We drop the scope cache because leak sanitizer can't see across the
    // thread we use for proxying MozJS requests. Dropping the cache cleans up
    // the memory and makes leak sanitizer happy.
    LOGV2_OPTIONS(4784931, {LogComponent::kDefault}, "Dropping the scope cache for shutdown");
    ScriptEngine::dropScopeCache();


    // Shutdown OpenTelemetry metrics
    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownOtelMetrics,
                                       &shutdownTimeElapsedBuilder);
        otel::metrics::shutdown();
    }

    // Shutdown Full-Time Data Capture
    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownFTDC,
                                       &shutdownTimeElapsedBuilder);
        stopMongoDFTDC();
    }

    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownReplicaSetNodeExecutor,
                                       &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(10175800, {LogComponent::kDefault}, "Shutting down the standalone executor");
        getLocalExecutor(serviceContext)->shutdown();
        getLocalExecutor(serviceContext)->join();
    }

    LOGV2(20565, "Now exiting");

    audit::logShutdown(client);

#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    // SessionKiller relies on the network stack being cleanly shutdown which only occurs under
    // sanitizers
    SessionKiller::shutdown(serviceContext->getService(ClusterRole::ShardServer));
    if (serverGlobalParams.clusterRole.has(ClusterRole::RouterServer)) {
        SessionKiller::shutdown(serviceContext->getService(ClusterRole::RouterServer));
    }
#endif

    FlowControl::shutdown(serviceContext);
#ifdef MONGO_CONFIG_SSL
    {
        SectionScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                       TimedSectionId::shutDownOCSP,
                                       &shutdownTimeElapsedBuilder);
        OCSPManager::shutdown(serviceContext);
    }
#endif
}

}  // namespace

int mongod_main(int argc, char* argv[]) {
    ThreadSafetyContext::getThreadSafetyContext()->forbidMultiThreading();

    waitForDebugger();

    setupSignalHandlers();

    srand(static_cast<unsigned>(curTimeMicros64()));  // NOLINT

    Status status = mongo::runGlobalInitializers(std::vector<std::string>(argv, argv + argc));
    if (!status.isOK()) {
        LOGV2_FATAL_OPTIONS(
            20574,
            logv2::LogOptions(logv2::LogComponent::kControl, logv2::FatalMode::kContinue),
            "Error during global initialization",
            "error"_attr = status);
        quickExit(ExitCode::fail);
    }

    // There is no single-threaded guarantee beyond this point.
    ThreadSafetyContext::getThreadSafetyContext()->allowMultiThreading();

    // Per SERVER-7434, startSignalProcessingThread must run after any forks (i.e.
    // initialize_server_global_state::forkServerOrDie) and before the creation of any other threads
    startSignalProcessingThread();

    uassertStatusOK(otel::metrics::initialize("mongod"));

    auto* service = [] {
        try {
            auto serviceContextHolder =
                ServiceContext::make(FastClockSourceFactory::create(Milliseconds(10)));
            auto* serviceContext = serviceContextHolder.get();

            setGlobalServiceContext(std::move(serviceContextHolder));

            return serviceContext;
        } catch (...) {
            auto cause = exceptionToStatus();
            LOGV2_FATAL_OPTIONS(
                20575,
                logv2::LogOptions(logv2::LogComponent::kControl, logv2::FatalMode::kContinue),
                "Error creating service context",
                "error"_attr = redact(cause));
            quickExit(ExitCode::fail);
        }
    }();

    registerShutdownTask(shutdownTask);

    {
        // Create the durable history registry prior to calling the `setUp*` methods. They may
        // depend on it existing at this point.
        DurableHistoryRegistry::set(service, std::make_unique<DurableHistoryRegistry>());
        DurableHistoryRegistry* registry = DurableHistoryRegistry::get(service);
        if (getTestCommandsEnabled()) {
            registry->registerPin(std::make_unique<TestingDurableHistoryPin>());
        }
    }

    // Attempt to rotate the audit log pre-emptively on startup to avoid any potential conflicts
    // with existing log state. If this rotation fails, then exit nicely with failure
    try {
        audit::rotateAuditLog();
    } catch (...) {

        Status err = mongo::exceptionToStatus();
        LOGV2(6169900, "Error rotating audit log", "error"_attr = err);

        quickExit(ExitCode::auditRotateError);
    }

    setLocalExecutor(service, createLocalExecutor(service, "Standalone"));

    setUpCatalog(service);
    setUpReplication(service);
    setUpObservers(service);
    setUpSharding(service);

    ErrorExtraInfo::invariantHaveAllParsers();

    startupConfigActions(std::vector<std::string>(argv, argv + argc));
    cmdline_utils::censorArgvArray(argc, argv);

    if (!initialize_server_global_state::checkSocketPath())
        quickExit(ExitCode::fail);

    LOGV2(5945603, "Multi threading initialized");

    startAllocatorThread();

    auto routerService = service->getService(ClusterRole::RouterServer);
    auto shardService = service->getService(ClusterRole::ShardServer);
    if (routerService) {
        ReadWriteConcernDefaults::create(routerService, readWriteConcernDefaultsCacheLookupMongoS);
    }
    ReadWriteConcernDefaults::create(shardService, readWriteConcernDefaultsCacheLookupMongoD);

    ChangeStreamOptionsManager::create(service);

    if (change_stream_serverless_helpers::canInitializeServices()) {
        ChangeStreamChangeCollectionManager::create(service);
    }

#if defined(_WIN32)
    if (ntservice::shouldStartService()) {
        ntservice::startService();
        // exits directly and so never reaches here either.
    }
#endif

    ExitCode exitCode = initAndListen(service);

    exitCleanly(exitCode);
    return 0;
}

}  // namespace mongo
