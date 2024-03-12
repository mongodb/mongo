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

#include <algorithm>
#include <boost/filesystem/operations.hpp>
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
#include <ratio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>

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
#include "mongo/db/audit.h"
#include "mongo/db/audit_interface.h"
#include "mongo/db/auth/auth_op_observer.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/database_holder_impl.h"
#include "mongo/db/catalog/health_log.h"
#include "mongo/db/catalog/health_log_interface.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_collection_expired_documents_remover.h"
#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/change_streams_cluster_parameter_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_gen.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/shutdown.h"
#include "mongo/db/commands/test_commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/flow_control_ticketholder.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/ftdc/ftdc_mongod.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/index_builds_coordinator_mongod.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/keys_collection_client_direct.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/keys_collection_manager_gen.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/logical_session_cache_factory_mongod.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/mirror_maestro.h"
#include "mongo/db/mongod_options.h"
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
#include "mongo/db/op_observer/user_write_block_mode_op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/periodic_runner_job_abort_expired_transactions.h"
#include "mongo/db/pipeline/change_stream_expired_pre_image_remover.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/pipeline/process_interface/replica_set_node_process_interface.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/stats/stats_cache_loader_impl.h"
#include "mongo/db/query/stats/stats_catalog.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mongod.h"
#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/initial_syncer_factory.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state_impl.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_impl_gen.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/db/repl/shard_merge_recipient_op_observer.h"
#include "mongo/db/repl/shard_merge_recipient_service.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_donor_op_observer.h"
#include "mongo/db/repl/tenant_migration_donor_service.h"
#include "mongo/db/repl/tenant_migration_recipient_op_observer.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/tenant_migration_util.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/repl_set_member_in_standalone_mode.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/collection_sharding_state_factory_shard.h"
#include "mongo/db/s/config/configsvr_coordinator_service.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/config_server_op_observer.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator.h"
#include "mongo/db/s/migration_chunk_cloner_source_op_observer.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/periodic_sharded_index_consistency_checker.h"
#include "mongo/db/s/query_analysis_op_observer_configsvr.h"
#include "mongo/db/s/query_analysis_op_observer_rs.h"
#include "mongo/db/s/query_analysis_op_observer_shardsvr.h"
#include "mongo/db/s/rename_collection_participant_service.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding/resharding_op_observer.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/shard_server_op_observer.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/s/sharding_initialization_mongod.h"
#include "mongo/db/s/sharding_ready.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/serverless/shard_split_donor_op_observer.h"
#include "mongo/db/serverless/shard_split_donor_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_mongod.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/session/kill_sessions_remote.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/db/session_manager_mongod.h"
#include "mongo/db/set_change_stream_state_coordinator.h"
#include "mongo/db/startup_recovery.h"
#include "mongo/db/startup_warnings_mongod.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/disk_space_monitor.h"
#include "mongo/db/storage/durable_history_pin.h"
#include "mongo/db/storage/encryption_hooks.h"
#include "mongo/db/storage/flow_control.h"
#include "mongo/db/storage/flow_control_parameters_gen.h"
#include "mongo/db/storage/oplog_cap_maintainer_thread.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/system_index.h"
#include "mongo/db/timeseries/timeseries_op_observer.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/ttl.h"
#include "mongo/db/vector_clock_metadata_hook.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/idl/cluster_server_parameter_initializer.h"
#include "mongo/idl/cluster_server_parameter_op_observer.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_options.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/mutex.h"
#include "mongo/platform/process_id.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/query_analysis_client.h"
#include "mongo/s/query_analysis_sampler.h"
#include "mongo/s/resource_yielders.h"
#include "mongo/s/service_entry_point_mongos.h"
#include "mongo/s/sharding_state.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/engine.h"
#include "mongo/transport/ingress_handshake_metrics.h"
#include "mongo/transport/session_manager_common.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
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
#include "mongo/util/latch_analyzer.h"
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

auto makeTransportLayer(ServiceContext* svcCtx) {
    boost::optional<int> routerPort;
    boost::optional<int> loadBalancerPort;

    if (serverGlobalParams.routerPort) {
        routerPort = serverGlobalParams.routerPort;
        if (*routerPort == serverGlobalParams.port) {
            LOGV2_ERROR(7791701,
                        "The router port must be different from the public listening port.",
                        "port"_attr = serverGlobalParams.port);
            quickExit(ExitCode::badOptions);
        }
        // TODO SERVER-78730: add support for load-balanced connections.
    }

    return transport::TransportLayerManagerImpl::createWithConfig(
        &serverGlobalParams, svcCtx, std::move(loadBalancerPort), std::move(routerPort));
}

void logStartup(OperationContext* opCtx) {
    BSONObjBuilder toLog;
    std::stringstream id;
    id << getHostNameCached() << "-" << jsTime().asInt64();
    toLog.append("_id", id.str());
    toLog.append("hostname", getHostNameCached());

    toLog.appendTimeT("startTime", time(nullptr));
    toLog.append("startTimeLocal", dateToCtimeString(Date_t::now()));

    toLog.append("cmdLine", serverGlobalParams.parsedOpts);
    toLog.append("pid", ProcessId::getCurrent().asLongLong());


    BSONObjBuilder buildinfo(toLog.subobjStart("buildinfo"));
    VersionInfoInterface::instance().appendBuildInfo(&buildinfo);
    appendStorageEngineList(opCtx->getServiceContext(), &buildinfo);
    buildinfo.doneFast();

    BSONObj o = toLog.obj();

    Lock::GlobalWrite lk(opCtx);
    AutoGetDb autoDb(opCtx, NamespaceString::kStartupLogNamespace.dbName(), mongo::MODE_X);
    auto db = autoDb.ensureDbExists(opCtx);
    auto collection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
        opCtx, NamespaceString::kStartupLogNamespace);
    WriteUnitOfWork wunit(opCtx);
    if (!collection) {
        BSONObj options = BSON("capped" << true << "size" << 10 * 1024 * 1024);
        repl::UnreplicatedWritesBlock uwb(opCtx);
        CollectionOptions collectionOptions = uassertStatusOK(
            CollectionOptions::parse(options, CollectionOptions::ParseKind::parseForCommand));
        uassertStatusOK(
            db->userCreateNS(opCtx, NamespaceString::kStartupLogNamespace, collectionOptions));
        collection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
            opCtx, NamespaceString::kStartupLogNamespace);
    }
    invariant(collection);

    uassertStatusOK(collection_internal::insertDocument(
        opCtx, CollectionPtr(collection), InsertStatement(o), nullptr /* OpDebug */, false));
    wunit.commit();
}

void initializeCommandHooks(ServiceContext* serviceContext) {
    class MongodCommandInvocationHooks final : public CommandInvocationHooks {
    public:
        void onBeforeRun(OperationContext* opCtx,
                         const OpMsgRequest& request,
                         CommandInvocation* invocation) override {
            _nextHook.onBeforeRun(opCtx, request, invocation);
        }

        void onBeforeAsyncRun(std::shared_ptr<RequestExecutionContext> rec,
                              CommandInvocation* invocation) override {
            _nextHook.onBeforeAsyncRun(rec, invocation);
        }

        void onAfterRun(OperationContext* opCtx,
                        const OpMsgRequest& request,
                        CommandInvocation* invocation,
                        rpc::ReplyBuilderInterface* response) override {
            _nextHook.onAfterRun(opCtx, request, invocation, response);
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
        services.push_back(std::make_unique<RenameCollectionParticipantService>(serviceContext));
        services.push_back(std::make_unique<ShardingDDLCoordinatorService>(serviceContext));
        services.push_back(std::make_unique<ReshardingDonorService>(serviceContext));
        services.push_back(std::make_unique<ReshardingRecipientService>(serviceContext));
        services.push_back(std::make_unique<MultiUpdateCoordinatorService>(serviceContext));
    }

    if (getGlobalReplSettings().isServerless()) {
        services.push_back(std::make_unique<TenantMigrationDonorService>(serviceContext));
        services.push_back(std::make_unique<repl::TenantMigrationRecipientService>(serviceContext));
        services.push_back(std::make_unique<ShardSplitDonorService>(serviceContext));
        services.push_back(std::make_unique<repl::ShardMergeRecipientService>(serviceContext));
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

void logMongodStartupTimeElapsedStatistics(ServiceContext* serviceContext,
                                           Date_t beginInitAndListen,
                                           BSONObjBuilder* startupTimeElapsedBuilder,
                                           BSONObjBuilder* startupInfoBuilder,
                                           StorageEngine::LastShutdownState lastShutdownState) {
    mongo::Milliseconds elapsedInitAndListen =
        serviceContext->getFastClockSource()->now() - beginInitAndListen;
    startupTimeElapsedBuilder->append("_initAndListen total elapsed time",
                                      elapsedInitAndListen.toString());
    startupInfoBuilder->append("Startup from clean shutdown?",
                               lastShutdownState == StorageEngine::LastShutdownState::kClean);
    startupInfoBuilder->append("Statistics", startupTimeElapsedBuilder->obj());
    LOGV2_INFO(8423403,
               "mongod startup complete",
               "Summary of time elapsed"_attr = startupInfoBuilder->obj());
}

// Important:
// _initAndListen among its other tasks initializes the storage subsystem.
// File Copy Based Initial Sync will restart the storage subsystem and may need to repeat some
// of the initialization steps within.  If you add or change any of these steps, make sure
// any necessary changes are also made to File Copy Based Initial Sync.
ExitCode _initAndListen(ServiceContext* serviceContext, int listenPort) {
    Client::initThread("initandlisten", serviceContext->getService(ClusterRole::ShardServer));

    // TODO(SERVER-74659): Please revisit if this thread could be made killable.
    {
        stdx::lock_guard<Client> lk(cc());
        cc().setSystemOperationUnkillableByStepdown(lk);
    }

    serviceContext->setFastClockSource(FastClockSourceFactory::create(Milliseconds(10)));

    BSONObjBuilder startupTimeElapsedBuilder;
    BSONObjBuilder startupInfoBuilder;

    Date_t beginInitAndListen = serviceContext->getFastClockSource()->now();

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

    serviceContext->getService(ClusterRole::ShardServer)
        ->setServiceEntryPoint(std::make_unique<ServiceEntryPointMongod>());

    {
        // Set up the periodic runner for background job execution. This is required to be running
        // before both the storage engine or the transport layer are initialized.
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Set up periodic runner",
                                                  &startupTimeElapsedBuilder);
        auto runner = makePeriodicRunner(serviceContext);
        serviceContext->setPeriodicRunner(std::move(runner));
    }

    // When starting the server with --queryableBackupMode or --recoverFromOplogAsStandalone, we are
    // in read-only mode and don't allow user-originating operations to perform writes
    if (storageGlobalParams.queryableBackupMode ||
        repl::ReplSettings::shouldRecoverFromOplogAsStandalone()) {
        serviceContext->disallowUserWrites();
    }

#ifdef MONGO_CONFIG_SSL
    {
        TimeElapsedBuilderScopedTimer scopedTimer(
            serviceContext->getFastClockSource(),
            "Set up online certificate status protocol manager",
            &startupTimeElapsedBuilder);
        OCSPManager::start(serviceContext);
    }
    CertificateExpirationMonitor::get()->start(serviceContext);
#endif

    if (!storageGlobalParams.repair) {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Transport layer setup",
                                                  &startupTimeElapsedBuilder);
        auto tl = makeTransportLayer(serviceContext);
        if (auto res = tl->setup(); !res.isOK()) {
            LOGV2_ERROR(20568, "Error setting up listener", "error"_attr = res);
            return ExitCode::netError;
        }
        serviceContext->setTransportLayerManager(std::move(tl));
    }

    FlowControl::set(serviceContext,
                     std::make_unique<FlowControl>(
                         serviceContext, repl::ReplicationCoordinator::get(serviceContext)));

    // If a crash occurred during file-copy based initial sync, we may need to finish or clean up.
    {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Run initial syncer crash recovery",
                                                  &startupTimeElapsedBuilder);
        repl::InitialSyncerFactory::get(serviceContext)->runCrashRecovery();
    }

    admission::initializeExecutionControl(serviceContext);

    // Creating the operation context before initializing the storage engine allows the storage
    // engine initialization to make use of the lock manager. As the storage engine is not yet
    // initialized, a noop recovery unit is used until the initialization is complete.
    auto lastShutdownState = [&] {
        auto initializeStorageEngineOpCtx = serviceContext->makeOperationContext(&cc());
        shard_role_details::setRecoveryUnit(initializeStorageEngineOpCtx.get(),
                                            std::make_unique<RecoveryUnitNoop>(),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

        auto lastShutdownState = initializeStorageEngine(initializeStorageEngineOpCtx.get(),
                                                         StorageEngineInitFlags{},
                                                         &startupTimeElapsedBuilder);

        StorageControl::startStorageControls(serviceContext);
        return lastShutdownState;
    }();

    ScopeGuard logStartupStats([serviceContext,
                                beginInitAndListen,
                                &startupTimeElapsedBuilder,
                                &startupInfoBuilder,
                                lastShutdownState] {
        logMongodStartupTimeElapsedStatistics(serviceContext,
                                              beginInitAndListen,
                                              &startupTimeElapsedBuilder,
                                              &startupInfoBuilder,
                                              lastShutdownState);
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
    }

    // If we are on standalone, load cluster parameters from disk. If we are replicated, this is not
    // a concern as the cluster parameter initializer runs automatically.
    auto replCoord = repl::ReplicationCoordinator::get(startupOpCtx.get());
    invariant(replCoord);
    if (!replCoord->getSettings().isReplSet()) {
        TimeElapsedBuilderScopedTimer scopedTimer(
            serviceContext->getFastClockSource(),
            "Load cluster parameters from disk for a standalone",
            &startupTimeElapsedBuilder);
        ClusterServerParameterInitializer::synchronizeAllParametersFromDisk(startupOpCtx.get());
    }

    // Ensure FCV document exists and is initialized in-memory. Fatally asserts if there is an
    // error.
    FeatureCompatibilityVersion::fassertInitializedAfterStartup(startupOpCtx.get());

    // TODO (SERVER-74847): Remove this function call once we remove testing around downgrading from
    // latest to last continuous.
    if (!mongo::repl::disableTransitionFromLatestToLastContinuous) {
        FeatureCompatibilityVersion::addTransitionFromLatestToLastContinuous();
    }

    if (gFlowControlEnabled.load()) {
        LOGV2(20536, "Flow Control is enabled on this deployment");
    }

    {
        Lock::GlobalWrite globalLk(startupOpCtx.get());
        DurableHistoryRegistry::get(serviceContext)->reconcilePins(startupOpCtx.get());

        // Initialize the cached pointer to the oplog collection. We want to do this even as
        // standalone
        // so accesses to the cached pointer in replica set nodes started as standalone still work
        // (mainly AutoGetOplog). In case the oplog doesn't exist, it is just initialized to null.
        // This initialization must happen within a GlobalWrite lock context.
        repl::acquireOplogCollectionForLogging(startupOpCtx.get());
    }

    // Notify the storage engine that startup is completed before repair exits below, as repair sets
    // the upgrade flag to true.
    auto storageEngine = serviceContext->getStorageEngine();
    invariant(storageEngine);
    storageEngine->notifyStorageStartupRecoveryComplete();

    BackupCursorHooks::initialize(serviceContext);

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

    auto const authzManagerShard =
        AuthorizationManager::get(serviceContext->getService(ClusterRole::ShardServer));
    {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Build user and roles graph",
                                                  &startupTimeElapsedBuilder);
        uassertStatusOK(authzManagerShard->initialize(startupOpCtx.get()));
    }

    if (audit::initializeManager) {
        audit::initializeManager(startupOpCtx.get());
    }

    // This is for security on certain platforms (nonce generation)
    srand((unsigned)(curTimeMicros64()) ^ (unsigned(uintptr_t(&startupOpCtx))));  // NOLINT

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

        // SERVER-14090: Verify that auth schema version is schemaVersion26Final.
        int foundSchemaVersion;
        status =
            authzManagerShard->getAuthorizationVersion(startupOpCtx.get(), &foundSchemaVersion);
        if (!status.isOK()) {
            LOGV2_ERROR(20539,
                        "Failed to verify auth schema version",
                        "minSchemaVersion"_attr = AuthorizationManager::schemaVersion26Final,
                        "error"_attr = status);
            LOGV2(20540,
                  "To manually repair the 'authSchema' document in the admin.system.version "
                  "collection, start up with --setParameter "
                  "startupAuthSchemaValidation=false to disable validation");
            exitCleanly(ExitCode::needUpgrade);
        }

        if (foundSchemaVersion <= AuthorizationManager::schemaVersion26Final) {
            LOGV2_ERROR(
                20541,
                "This server is using MONGODB-CR, an authentication mechanism which has been "
                "removed from MongoDB 4.0. In order to upgrade the auth schema, first downgrade "
                "MongoDB binaries to version 3.6 and then run the authSchemaUpgrade command. See "
                "http://dochub.mongodb.org/core/3.0-upgrade-to-scram-sha-1");
            exitCleanly(ExitCode::needUpgrade);
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
        TimeElapsedBuilderScopedTimer scopedTimer(
            serviceContext->getFastClockSource(),
            "Set up the background thread pool responsible for "
            "waiting for opTimes to be majority committed",
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
            ReadWriteConcernDefaults::get(startupOpCtx.get()->getServiceContext())
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

        uassert(ErrorCodes::BadValue,
                str::stream() << "Cannot use queryableBackupMode in a replica set",
                !replCoord->getSettings().isReplSet());
        TimeElapsedBuilderScopedTimer scopedTimer(
            serviceContext->getFastClockSource(),
            "Start up the replication coordinator for queryable backup mode",
            &startupTimeElapsedBuilder);
        replCoord->startup(startupOpCtx.get(), lastShutdownState);
    } else {
        if (storageEngine->supportsCappedCollections()) {
            logStartup(startupOpCtx.get());
        }

        ResourceYielderFactory::initialize(serviceContext);

        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            {
                TimeElapsedBuilderScopedTimer scopedTimer(
                    serviceContext->getFastClockSource(),
                    "Initialize the sharding components for a config server",
                    &startupTimeElapsedBuilder);

                initializeGlobalShardingStateForMongoD(startupOpCtx.get());
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
                ->setServiceEntryPoint(std::make_unique<ServiceEntryPointMongos>());
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
            TimeElapsedBuilderScopedTimer scopedTimer(
                serviceContext->getFastClockSource(),
                "Start up cluster time keys manager with a local/direct keys client",
                &startupTimeElapsedBuilder);
            auto keysClientMustUseLocalReads =
                !serviceContext->getStorageEngine()->supportsReadConcernMajority();
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
            TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                      "Start up the replication coordinator",
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

        if (replSettings.isReplSet()) {
            TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                      "Create an oplog view for tenant migrations",
                                                      &startupTimeElapsedBuilder);
            Lock::GlobalWrite lk(startupOpCtx.get());
            OldClientContext ctx(startupOpCtx.get(), NamespaceString::kRsOplogNamespace);
            tenant_migration_util::createOplogViewForTenantMigrations(startupOpCtx.get(), ctx.db());
        }

        storageEngine->startTimestampMonitor();

        startFLECrud(serviceContext);

        DiskSpaceMonitor::start(serviceContext);
        auto diskMonitor = DiskSpaceMonitor::get(serviceContext);
        diskMonitor->registerAction(
            IndexBuildsCoordinator::get(serviceContext)->makeKillIndexBuildOnLowDiskSpaceAction());
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
        } catch (ExceptionFor<ErrorCodes::PeriodicJobIsStopped>&) {
            LOGV2_WARNING(4747501, "Not starting periodic jobs as shutdown is in progress");
            // Shutdown has already started before initialization is complete. Wait for the
            // shutdown task to complete and return.

            logStartupStats.dismiss();
            logMongodStartupTimeElapsedStatistics(serviceContext,
                                                  beginInitAndListen,
                                                  &startupTimeElapsedBuilder,
                                                  &startupInfoBuilder,
                                                  lastShutdownState);

            MONGO_IDLE_THREAD_BLOCK;
            return waitForShutdown();
        }
    }

    // Change stream collections can exist, even on a standalone, provided the standalone used to be
    // part of a replica set. Ensure the change stream collections on startup contain consistent
    // data.
    {
        TimeElapsedBuilderScopedTimer scopedTimer(
            serviceContext->getFastClockSource(),
            "Ensure the change stream collections on startup contain consistent data",
            &startupTimeElapsedBuilder);
        startup_recovery::recoverChangeStreamCollections(
            startupOpCtx.get(), isStandalone, lastShutdownState);
    }

    // If not in standalone mode, start background tasks to:
    //  * Periodically remove expired pre-images from the 'system.preimages'
    //  * Periodically remove expired documents from change collections
    if (!isStandalone) {
        if (!gPreImageRemoverDisabled) {
            startChangeStreamExpiredPreImagesRemover(serviceContext);
        }
        if (!gChangeCollectionRemoverDisabled) {
            startChangeCollectionExpiredDocumentsRemover(serviceContext);
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
    auto catalog = std::make_unique<stats::StatsCatalog>(serviceContext, std::move(cacheLoader));
    stats::StatsCatalog::set(serviceContext, std::move(catalog));

    // Startup options are written to the audit log at the end of startup so that cluster server
    // parameters are guaranteed to have been initialized from disk at this point.
    {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Write startup options to the audit log",
                                                  &startupTimeElapsedBuilder);
        audit::logStartupOptions(Client::getCurrent(), serverGlobalParams.parsedOpts);
    }

    // MessageServer::run will return when exit code closes its socket and we don't need the
    // operation context anymore
    startupOpCtx.reset();

    transport::ServiceExecutor::startupAll(serviceContext);

    if (!storageGlobalParams.repair) {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Start transport layer",
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

    if (storageGlobalParams.magicRestore) {
        TimeElapsedBuilderScopedTimer scopedTimer(
            serviceContext->getFastClockSource(), "Magic restore", &startupTimeElapsedBuilder);
        if (getMagicRestoreMain() == nullptr) {
            LOGV2_FATAL_NOTRACE(7180701, "--magicRestore cannot be used with a community build");
        }
        return getMagicRestoreMain()(serviceContext);
    }

    logStartupStats.dismiss();
    logMongodStartupTimeElapsedStatistics(serviceContext,
                                          beginInitAndListen,
                                          &startupTimeElapsedBuilder,
                                          &startupInfoBuilder,
                                          lastShutdownState);

    MONGO_IDLE_THREAD_BLOCK;
    return waitForShutdown();
}

ExitCode initAndListen(ServiceContext* service, int listenPort) {
    try {
        return _initAndListen(service, listenPort);
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
    return initAndListen(getGlobalServiceContext(), serverGlobalParams.port);
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
    auto pidfile = (boost::filesystem::path(dbpath) / kLockFileBasename.toString()).string();
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
        Client::initThread(threadName.c_str(),
                           serviceContext->getService(ClusterRole::ShardServer));

        stdx::lock_guard<Client> lk(cc());
        cc().setSystemOperationUnkillableByStepdown(lk);
    };
    return std::make_unique<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(tpOptions),
        executor::makeNetworkInterface(
            "ReplNodeDbWorkerNetwork", nullptr, makeShardingEgressHooksList(serviceContext)));
}

auto makeReplicationExecutor(ServiceContext* serviceContext) {
    ThreadPool::Options tpOptions;
    tpOptions.threadNamePrefix = "ReplCoord-";
    tpOptions.poolName = "ReplCoordThreadPool";
    tpOptions.maxThreads = 50;
    tpOptions.onCreateThread = [serviceContext](const std::string& threadName) {
        Client::initThread(threadName.c_str(),
                           serviceContext->getService(ClusterRole::ShardServer));

        stdx::lock_guard<Client> lk(cc());
        cc().setSystemOperationUnkillableByStepdown(lk);
    };
    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(std::make_unique<rpc::VectorClockMetadataHook>(serviceContext));
    return std::make_unique<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(tpOptions),
        executor::makeNetworkInterface("ReplNetwork", nullptr, std::move(hookList)));
}

void setUpReplication(ServiceContext* serviceContext) {
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
    auto replicationProcess = repl::ReplicationProcess::get(serviceContext);

    repl::DropPendingCollectionReaper::set(
        serviceContext, std::make_unique<repl::DropPendingCollectionReaper>(storageInterface));
    auto dropPendingCollectionReaper = repl::DropPendingCollectionReaper::get(serviceContext);

    repl::TopologyCoordinator::Options topoCoordOptions;
    topoCoordOptions.maxSyncSourceLagSecs = Seconds(repl::maxSyncSourceLagSecs);
    topoCoordOptions.clusterRole = serverGlobalParams.clusterRole;

    auto replCoord = std::make_unique<repl::ReplicationCoordinatorImpl>(
        serviceContext,
        getGlobalReplSettings(),
        std::make_unique<repl::ReplicationCoordinatorExternalStateImpl>(
            serviceContext, dropPendingCollectionReaper, storageInterface, replicationProcess),
        makeReplicationExecutor(serviceContext),
        std::make_unique<repl::TopologyCoordinator>(topoCoordOptions),
        replicationProcess,
        storageInterface,
        SecureRandom().nextInt64());
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
        if (getGlobalReplSettings().isServerless()) {
            opObserverRegistry->addObserver(
                std::make_unique<repl::TenantMigrationDonorOpObserver>());
            opObserverRegistry->addObserver(
                std::make_unique<repl::TenantMigrationRecipientOpObserver>());
            opObserverRegistry->addObserver(std::make_unique<ShardSplitDonorOpObserver>());
            opObserverRegistry->addObserver(
                std::make_unique<repl::ShardMergeRecipientOpObserver>());
        }
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
        if (getGlobalReplSettings().isServerless()) {
            opObserverRegistry->addObserver(
                std::make_unique<repl::TenantMigrationDonorOpObserver>());
            opObserverRegistry->addObserver(
                std::make_unique<repl::TenantMigrationRecipientOpObserver>());
            opObserverRegistry->addObserver(std::make_unique<ShardSplitDonorOpObserver>());
            opObserverRegistry->addObserver(
                std::make_unique<repl::ShardMergeRecipientOpObserver>());
        }

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
}

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

void logMongodShutdownTimeElapsedStatistics(ServiceContext* serviceContext,
                                            Date_t beginShutdownTask,
                                            BSONObjBuilder* shutdownTimeElapsedBuilder,
                                            BSONObjBuilder* shutdownInfoBuilder) {
    mongo::Milliseconds elapsedInitAndListen =
        serviceContext->getFastClockSource()->now() - beginShutdownTask;
    shutdownTimeElapsedBuilder->append("shutdownTask total elapsed time",
                                       elapsedInitAndListen.toString());
    shutdownInfoBuilder->append("Statistics", shutdownTimeElapsedBuilder->obj());
    LOGV2_INFO(8423404,
               "mongod shutdown complete",
               "Summary of time elapsed"_attr = shutdownInfoBuilder->obj());
}

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
        stdx::unique_lock<Latch> stateLock(fsyncStateMutex);
        if (globalFsyncLockThread) {
            globalFsyncLockThread->shutdown(stateLock);
        }
    }

    auto const serviceContext = getGlobalServiceContext();

    Date_t beginShutdownTask = serviceContext->getFastClockSource()->now();
    ScopeGuard logShutdownStats(
        [serviceContext, beginShutdownTask, &shutdownTimeElapsedBuilder, &shutdownInfoBuilder] {
            logMongodShutdownTimeElapsedStatistics(serviceContext,
                                                   beginShutdownTask,
                                                   &shutdownTimeElapsedBuilder,
                                                   &shutdownInfoBuilder);
        });

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
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Enter terminal shutdown",
                                                  &shutdownTimeElapsedBuilder);
        replCoord->enterTerminalShutdown();
    }

    // Store previous client, to be restored when function scope ends.
    ServiceContext::UniqueClient oldClient;
    if (Client::getCurrent()) {
        oldClient = Client::releaseCurrent();
    }
    Client::setCurrent(
        serviceContext->getService(ClusterRole::ShardServer)->makeClient("shutdownTask"));
    const auto client = Client::getCurrent();
    {
        stdx::lock_guard<Client> lk(*client);
        client->setSystemOperationUnkillableByStepdown(lk);
    }
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
        TimeElapsedBuilderScopedTimer scopedTimer(
            serviceContext->getFastClockSource(),
            "Step down the replication coordinator for shutdown",
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
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Time spent in quiesce mode",
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
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down FLE Crud subsystem",
                                                  &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(6371601, {LogComponent::kDefault}, "Shutting down the FLE Crud thread pool");
        stopFLECrud();
    }

    {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down MirrorMaestro",
                                                  &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(4784901, {LogComponent::kCommand}, "Shutting down the MirrorMaestro");
        MirrorMaestro::shutdown(serviceContext);
    }

    {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down WaitForMajorityService",
                                                  &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(
            4784902, {LogComponent::kSharding}, "Shutting down the WaitForMajorityService");
        WaitForMajorityService::get(serviceContext).shutDown();
    }

    // Join the logical session cache before the transport layer.
    if (auto lsc = LogicalSessionCache::get(serviceContext)) {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down the logical session cache",
                                                  &shutdownTimeElapsedBuilder);
        LOGV2(4784903, "Shutting down the LogicalSessionCache");
        lsc->joinOnShutDown();
    }

    if (analyze_shard_key::supportsSamplingQueries(serviceContext)) {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down the Query Analysis Sampler",
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
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down the global connection pool",
                                                  &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(
            4784905, {LogComponent::kNetwork}, "Shutting down the global connection pool");
        globalConnPool.shutdown();
    }

    // Inform Flow Control to stop gating writes on ticket admission. This must be done before the
    // Periodic Runner is shut down (see SERVER-41751).
    if (auto flowControlTicketholder = FlowControlTicketholder::get(serviceContext)) {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down the flow control ticket holder",
                                                  &shutdownTimeElapsedBuilder);
        LOGV2(4784906, "Shutting down the FlowControlTicketholder");
        flowControlTicketholder->setInShutdown();
    }

    if (auto exec = ReplicaSetNodeProcessInterface::getReplicaSetNodeExecutor(serviceContext)) {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down the replica set node executor",
                                                  &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(
            4784907, {LogComponent::kReplication}, "Shutting down the replica set node executor");
        exec->shutdown();
        exec->join();
    }

    if (auto storageEngine = serviceContext->getStorageEngine()) {
        if (storageEngine->supportsReadConcernSnapshot()) {
            TimeElapsedBuilderScopedTimer scopedTimer(
                serviceContext->getFastClockSource(),
                "Shut down the thread that aborts expired transactions",
                &shutdownTimeElapsedBuilder);
            LOGV2(4784908, "Shutting down the PeriodicThreadToAbortExpiredTransactions");
            PeriodicThreadToAbortExpiredTransactions::get(serviceContext)->stop();
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
            TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                      "Shut down the index consistency checker",
                                                      &shutdownTimeElapsedBuilder);
            LOGV2_OPTIONS(4784904,
                          {LogComponent::kSharding},
                          "Shutting down the PeriodicShardedIndexConsistencyChecker");
            PeriodicShardedIndexConsistencyChecker::get(serviceContext).onShutDown();
        }

        LOGV2_OPTIONS(
            4784910, {LogComponent::kSharding}, "Shutting down the ShardingInitializationMongoD");
        ShardingInitializationMongoD::get(serviceContext)->shutDown(opCtx);

        // Acquire the RSTL in mode X. First we enqueue the lock request, then kill all operations,
        // destroy all stashed transaction resources in order to release locks, and finally wait
        // until the lock request is granted.
        LOGV2_OPTIONS(4784911,
                      {LogComponent::kReplication},
                      "Enqueuing the ReplicationStateTransitionLock for shutdown");
        repl::ReplicationStateTransitionLockGuard rstl(
            opCtx, MODE_X, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());

        // Kill all operations except FTDC to continue gathering metrics. This makes all newly
        // created opCtx to be immediately interrupted. After this point, the opCtx will have been
        // marked as killed and will not be usable other than to kill all transactions directly
        // below.
        LOGV2_OPTIONS(4784912, {LogComponent::kDefault}, "Killing all operations for shutdown");
        {
            const std::set<std::string> excludedClients = {std::string(kFTDCThreadName)};
            TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                      "Kill all operations for shutdown",
                                                      &shutdownTimeElapsedBuilder);
            serviceContext->setKillAllOperations(excludedClients);

            if (MONGO_unlikely(pauseWhileKillingOperationsAtShutdown.shouldFail())) {
                LOGV2_OPTIONS(4701700,
                              {LogComponent::kDefault},
                              "pauseWhileKillingOperationsAtShutdown failpoint enabled");
                sleepsecs(1);
            }
        }

        {
            // Clear tenant migration access blockers after killing all operation contexts to ensure
            // that no operation context cancellation token continuation holds the last reference to
            // the TenantMigrationAccessBlockerExecutor.
            TimeElapsedBuilderScopedTimer scopedTimer(
                serviceContext->getFastClockSource(),
                "Shut down all tenant migration access blockers on global shutdown",
                &shutdownTimeElapsedBuilder);
            LOGV2_OPTIONS(5093807,
                          {LogComponent::kTenantMigration},
                          "Shutting down all TenantMigrationAccessBlockers on global shutdown");
            TenantMigrationAccessBlockerRegistry::get(serviceContext).shutDown();
        }

        // Destroy all stashed transaction resources, in order to release locks.
        {
            TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                      "Shut down all open transactions",
                                                      &shutdownTimeElapsedBuilder);
            LOGV2_OPTIONS(4784913, {LogComponent::kCommand}, "Shutting down all open transactions");
            killSessionsLocalShutdownAllTransactions(opCtx);
        }

        {
            TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                      "Acquire the RSTL for shutdown",
                                                      &shutdownTimeElapsedBuilder);
            LOGV2_OPTIONS(4784914,
                          {LogComponent::kReplication},
                          "Acquiring the ReplicationStateTransitionLock for shutdown");
            rstl.waitForLockUntil(Date_t::max());
        }

        // Release the rstl before waiting for the index build threads to join as index build
        // reacquires rstl in uninterruptible lock guard to finish their cleanup process.
        rstl.release();

        // Shuts down the thread pool and waits for index builds to finish.
        // Depends on setKillAllOperations() above to interrupt the index build operations.
        {
            TimeElapsedBuilderScopedTimer scopedTimer(
                serviceContext->getFastClockSource(),
                "Shut down the IndexBuildsCoordinator and wait for index builds to finish",
                &shutdownTimeElapsedBuilder);
            LOGV2_OPTIONS(
                4784915, {LogComponent::kIndex}, "Shutting down the IndexBuildsCoordinator");
            IndexBuildsCoordinator::get(serviceContext)->shutdown(opCtx);
        }
    }

    {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down the replica set monitor",
                                                  &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(4784918, {LogComponent::kNetwork}, "Shutting down the ReplicaSetMonitor");
        ReplicaSetMonitor::shutdown();
    }

    auto sr = Grid::get(serviceContext)->isInitialized()
        ? Grid::get(serviceContext)->shardRegistry()
        : nullptr;
    if (sr) {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down the shard registry",
                                                  &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(4784919, {LogComponent::kSharding}, "Shutting down the shard registry");
        sr->shutdown();
    }

    if (ShardingState::get(serviceContext)->enabled()) {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down the transaction coordinator service",
                                                  &shutdownTimeElapsedBuilder);
        TransactionCoordinatorService::get(serviceContext)->shutdown();
    }

    // Validator shutdown must be called after setKillAllOperations is called. Otherwise, this can
    // deadlock.
    if (auto validator = LogicalTimeValidator::get(serviceContext)) {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down the logical time validator",
                                                  &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(
            4784920, {LogComponent::kReplication}, "Shutting down the LogicalTimeValidator");
        validator->shutDown();
    }

    // The migrationutil executor must be shut down before shutting down the CatalogCacheLoader and
    // the ExecutorPool. Otherwise, it may try to schedule work on those components and fail.
    LOGV2_OPTIONS(4784921, {LogComponent::kSharding}, "Shutting down the MigrationUtilExecutor");
    auto migrationUtilExecutor = migrationutil::getMigrationUtilExecutor(serviceContext);
    {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down the migration util executor",
                                                  &shutdownTimeElapsedBuilder);
        migrationUtilExecutor->shutdown();
        migrationUtilExecutor->join();
    }

    if (TestingProctor::instance().isEnabled()) {
        auto pool = Grid::get(serviceContext)->isInitialized()
            ? Grid::get(serviceContext)->getExecutorPool()
            : nullptr;
        if (pool) {
            TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                      "Shut down the executor pool",
                                                      &shutdownTimeElapsedBuilder);
            LOGV2_OPTIONS(6773200, {LogComponent::kSharding}, "Shutting down the ExecutorPool");
            pool->shutdownAndJoin();
        }
    }

    if (Grid::get(serviceContext)->isShardingInitialized()) {
        // The CatalogCache must be shuted down before shutting down the CatalogCacheLoader as the
        // CatalogCache may try to schedule work on CatalogCacheLoader and fail.
        TimeElapsedBuilderScopedTimer scopedTimer(
            serviceContext->getFastClockSource(),
            "Shut down the catalog cache and catalog cache loader",
            &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(6773201, {LogComponent::kSharding}, "Shutting down the CatalogCache");
        Grid::get(serviceContext)->catalogCache()->shutDownAndJoin();

        LOGV2_OPTIONS(4784922, {LogComponent::kSharding}, "Shutting down the CatalogCacheLoader");
        CatalogCacheLoader::get(serviceContext).shutDown();
    }

    // Finish shutting down the TransportLayers
    if (auto tlm = serviceContext->getTransportLayerManager()) {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down the transport layer",
                                                  &shutdownTimeElapsedBuilder);
        LOGV2_OPTIONS(20562, {LogComponent::kNetwork}, "Shutdown: Closing open transport sessions");
        tlm->shutdown();
    }

    if (auto* healthLog = HealthLogInterface::get(serviceContext)) {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down the health log",
                                                  &shutdownTimeElapsedBuilder);
        LOGV2(4784927, "Shutting down the HealthLog");
        healthLog->shutdown();
    }

    {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down the TTL monitor",
                                                  &shutdownTimeElapsedBuilder);
        LOGV2(4784928, "Shutting down the TTL monitor");
        shutdownTTLMonitor(serviceContext);
    }

    {
        TimeElapsedBuilderScopedTimer scopedTimer(
            serviceContext->getFastClockSource(),
            "Shut down expired pre-images and documents removers",
            &shutdownTimeElapsedBuilder);
        LOGV2(6278511, "Shutting down the Change Stream Expired Pre-images Remover");
        shutdownChangeStreamExpiredPreImagesRemover(serviceContext);

        shutdownChangeCollectionExpiredDocumentsRemover(serviceContext);
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
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down the storage engine",
                                                  &shutdownTimeElapsedBuilder);
        LOGV2(4784930, "Shutting down the storage engine");
        shutdownGlobalStorageEngineCleanly(serviceContext);
    }

    {
        // We wait for the oplog cap maintainer thread to stop. This has to be done after the engine
        // has been closed since the thread will only die once all references to the oplog have been
        // deleted and we're performing a shutdown.
        TimeElapsedBuilderScopedTimer scopedTimer(
            serviceContext->getFastClockSource(),
            "Wait for the oplog cap maintainer thread to stop",
            &shutdownTimeElapsedBuilder);
        OplogCapMaintainerThread::get(serviceContext)->waitForFinish();
    }

    // We drop the scope cache because leak sanitizer can't see across the
    // thread we use for proxying MozJS requests. Dropping the cache cleans up
    // the memory and makes leak sanitizer happy.
    LOGV2_OPTIONS(4784931, {LogComponent::kDefault}, "Dropping the scope cache for shutdown");
    ScriptEngine::dropScopeCache();

    // Shutdown Full-Time Data Capture
    {
        TimeElapsedBuilderScopedTimer scopedTimer(serviceContext->getFastClockSource(),
                                                  "Shut down full-time data capture",
                                                  &shutdownTimeElapsedBuilder);
        stopMongoDFTDC(serviceContext);
    }

    LOGV2(20565, "Now exiting");

    audit::logShutdown(client);

#ifndef MONGO_CONFIG_USE_RAW_LATCHES
    LatchAnalyzer::get(serviceContext).dump();
#endif

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
        TimeElapsedBuilderScopedTimer scopedTimer(
            serviceContext->getFastClockSource(),
            "Shut down online certificate status protocol manager",
            &shutdownTimeElapsedBuilder);
        OCSPManager::shutdown(serviceContext);
    }
#endif
}

}  // namespace

int mongod_main(int argc, char* argv[]) {
    ThreadSafetyContext::getThreadSafetyContext()->forbidMultiThreading();

    waitForDebugger();

    registerShutdownTask(shutdownTask);

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

    auto* service = [] {
        try {
            auto serviceContextHolder = ServiceContext::make();
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

    if (audit::setAuditInterface) {
        audit::setAuditInterface(service);
    }

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

    setUpCatalog(service);
    setUpReplication(service);
    setUpObservers(service);
    setUpSharding(service);

    ErrorExtraInfo::invariantHaveAllParsers();

    startupConfigActions(std::vector<std::string>(argv, argv + argc));
    cmdline_utils::censorArgvArray(argc, argv);

    if (!initialize_server_global_state::checkSocketPath())
        quickExit(ExitCode::fail);

    // There is no single-threaded guarantee beyond this point.
    ThreadSafetyContext::getThreadSafetyContext()->allowMultiThreading();
    LOGV2(5945603, "Multi threading initialized");

    // Per SERVER-7434, startSignalProcessingThread must run after any forks (i.e.
    // initialize_server_global_state::forkServerOrDie) and before the creation of any other threads
    startSignalProcessingThread();

    ReadWriteConcernDefaults::create(service, readWriteConcernDefaultsCacheLookupMongoD);
    ChangeStreamOptionsManager::create(service);

    if (change_stream_serverless_helpers::canInitializeServices()) {
        ChangeStreamChangeCollectionManager::create(service);
    }

    query_settings::QuerySettingsManager::create(service, {});

#if defined(_WIN32)
    if (ntservice::shouldStartService()) {
        ntservice::startService();
        // exits directly and so never reaches here either.
    }
#endif

    LOGV2_OPTIONS(
        7091600, {LogComponent::kTenantMigration}, "Starting TenantMigrationAccessBlockerRegistry");
    TenantMigrationAccessBlockerRegistry::get(service).startup();

    ExitCode exitCode = initAndListen(service, serverGlobalParams.port);
    exitCleanly(exitCode);
    return 0;
}

}  // namespace mongo
