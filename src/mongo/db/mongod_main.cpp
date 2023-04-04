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

#include <boost/filesystem/operations.hpp>
#include <boost/optional.hpp>
#include <csignal>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/auth_op_observer.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder_impl.h"
#include "mongo/db/catalog/health_log.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/change_collection_expired_documents_remover.h"
#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/client.h"
#include "mongo/db/client_metadata_propagation_egress_hook.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_gen.h"
#include "mongo/db/commands/shutdown.h"
#include "mongo/db/commands/test_commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/flow_control_ticketholder.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/free_mon/free_mon_mongod.h"
#include "mongo/db/ftdc/ftdc_mongod.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index_builds_coordinator_mongod.h"
#include "mongo/db/index_names.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/introspect.h"
#include "mongo/db/json.h"
#include "mongo/db/keys_collection_client_direct.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/logical_session_cache_factory_mongod.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/mirror_maestro.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/fcv_op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/oplog_writer_impl.h"
#include "mongo/db/op_observer/oplog_writer_transaction_proxy.h"
#include "mongo/db/op_observer/user_write_block_mode_op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/periodic_runner_job_abort_expired_transactions.h"
#include "mongo/db/pipeline/change_stream_expired_pre_image_remover.h"
#include "mongo/db/pipeline/process_interface/replica_set_node_process_interface.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/stats/stats_cache_loader_impl.h"
#include "mongo/db/query/stats/stats_catalog.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mongod.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/initial_syncer_factory.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state_impl.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_impl_gen.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/db/repl/shard_merge_recipient_op_observer.h"
#include "mongo/db/repl/shard_merge_recipient_service.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_migration_donor_op_observer.h"
#include "mongo/db/repl/tenant_migration_donor_service.h"
#include "mongo/db/repl/tenant_migration_recipient_op_observer.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/repl_set_member_in_standalone_mode.h"
#include "mongo/db/s/collection_sharding_state_factory_shard.h"
#include "mongo/db/s/collection_sharding_state_factory_standalone.h"
#include "mongo/db/s/config/configsvr_coordinator_service.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/config_server_op_observer.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/move_primary/move_primary_recipient_service.h"
#include "mongo/db/s/op_observer_sharding_impl.h"
#include "mongo/db/s/periodic_sharded_index_consistency_checker.h"
#include "mongo/db/s/query_analysis_op_observer.h"
#include "mongo/db/s/rename_collection_participant_service.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding/resharding_op_observer.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/shard_server_op_observer.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/s/sharding_initialization_mongod.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/serverless/shard_split_donor_op_observer.h"
#include "mongo/db/serverless/shard_split_donor_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_mongod.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/db/set_change_stream_state_coordinator.h"
#include "mongo/db/startup_recovery.h"
#include "mongo/db/startup_warnings_mongod.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/disk_space_monitor.h"
#include "mongo/db/storage/durable_history_pin.h"
#include "mongo/db/storage/encryption_hooks.h"
#include "mongo/db/storage/flow_control.h"
#include "mongo/db/storage/flow_control_parameters_gen.h"
#include "mongo/db/storage/oplog_cap_maintainer_thread.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/system_index.h"
#include "mongo/db/transaction/internal_transactions_reap_service.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/ttl.h"
#include "mongo/db/vector_clock_metadata_hook.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/idl/cluster_server_parameter_gen.h"
#include "mongo/idl/cluster_server_parameter_initializer.h"
#include "mongo/idl/cluster_server_parameter_op_observer.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/process_id.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/query_analysis_sampler.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/ingress_handshake_metrics.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fast_clock_source_factory.h"
#include "mongo/util/latch_analyzer.h"
#include "mongo/util/net/ocsp/ocsp_manager.h"
#include "mongo/util/net/private/ssl_expiration.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/sequence_util.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/text.h"
#include "mongo/util/time_support.h"
#include "mongo/util/version.h"
#include "mongo/watchdog/watchdog_mongod.h"

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl_options.h"
#endif

#if !defined(_WIN32)
#include <sys/file.h>
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

MONGO_INITIALIZER_WITH_PREREQUISITES(WireSpec, ("EndStartupOptionHandling"))(InitializerContext*) {
    // The featureCompatibilityVersion behavior defaults to the downgrade behavior while the
    // in-memory version is unset.
    WireSpec::Specification spec;
    spec.incomingInternalClient.minWireVersion = RELEASE_2_4_AND_BEFORE;
    spec.incomingInternalClient.maxWireVersion = LATEST_WIRE_VERSION;
    spec.outgoing.minWireVersion = SUPPORTS_OP_MSG;
    spec.outgoing.maxWireVersion = LATEST_WIRE_VERSION;
    spec.isInternalClient = true;

    WireSpec::instance().initialize(std::move(spec));
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
        services.push_back(std::make_unique<TenantMigrationDonorService>(serviceContext));
        services.push_back(std::make_unique<repl::TenantMigrationRecipientService>(serviceContext));
        services.push_back(std::make_unique<MovePrimaryRecipientService>(serviceContext));
        if (getGlobalReplSettings().isServerless()) {
            services.push_back(std::make_unique<repl::ShardMergeRecipientService>(serviceContext));
        }
    }

    if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
        services.push_back(std::make_unique<TenantMigrationDonorService>(serviceContext));
        services.push_back(std::make_unique<repl::TenantMigrationRecipientService>(serviceContext));
        if (getGlobalReplSettings().isServerless()) {
            services.push_back(std::make_unique<ShardSplitDonorService>(serviceContext));
            services.push_back(std::make_unique<repl::ShardMergeRecipientService>(serviceContext));
        }
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
ExitCode _initAndListen(ServiceContext* serviceContext, int listenPort) {
    Client::initThread("initandlisten");

    serviceContext->setFastClockSource(FastClockSourceFactory::create(Milliseconds(10)));

    DBDirectClientFactory::get(serviceContext).registerImplementation([](OperationContext* opCtx) {
        return std::unique_ptr<DBClientBase>(new DBDirectClient(opCtx));
    });

    const repl::ReplSettings& replSettings =
        repl::ReplicationCoordinator::get(serviceContext)->getSettings();

    {
        ProcessId pid = ProcessId::getCurrent();
        const bool is32bit = sizeof(int*) == 4;
        LOGV2(4615611,
              "MongoDB starting : pid={pid} port={port} dbpath={dbPath} {architecture} host={host}",
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

    serviceContext->setServiceEntryPoint(std::make_unique<ServiceEntryPointMongod>(serviceContext));

    // Set up the periodic runner for background job execution. This is required to be running
    // before both the storage engine or the transport layer are initialized.
    auto runner = makePeriodicRunner(serviceContext);
    serviceContext->setPeriodicRunner(std::move(runner));

    // When starting the server with --queryableBackupMode or --recoverFromOplogAsStandalone, we are
    // in read-only mode and don't allow user-originating operations to perform writes
    if (storageGlobalParams.queryableBackupMode ||
        repl::ReplSettings::shouldRecoverFromOplogAsStandalone()) {
        serviceContext->disallowUserWrites();
    }

#ifdef MONGO_CONFIG_SSL
    OCSPManager::start(serviceContext);
    CertificateExpirationMonitor::get()->start(serviceContext);
#endif

    if (!storageGlobalParams.repair) {
        auto tl =
            transport::TransportLayerManager::createWithConfig(&serverGlobalParams, serviceContext);
        auto res = tl->setup();
        if (!res.isOK()) {
            LOGV2_ERROR(20568,
                        "Error setting up listener: {error}",
                        "Error setting up listener",
                        "error"_attr = res);
            return ExitCode::netError;
        }
        serviceContext->setTransportLayer(std::move(tl));
    }

    FlowControl::set(serviceContext,
                     std::make_unique<FlowControl>(
                         serviceContext, repl::ReplicationCoordinator::get(serviceContext)));

    // If a crash occurred during file-copy based initial sync, we may need to finish or clean up.
    repl::InitialSyncerFactory::get(serviceContext)->runCrashRecovery();

    // Creating the operation context before initializing the storage engine allows the storage
    // engine initialization to make use of the lock manager. As the storage engine is not yet
    // initialized, a noop recovery unit is used until the initialization is complete.
    auto startupOpCtx = serviceContext->makeOperationContext(&cc());

    auto lastShutdownState = initializeStorageEngine(startupOpCtx.get(), StorageEngineInitFlags{});
    StorageControl::startStorageControls(serviceContext);

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
                              "Detected configuration for non-active storage engine {fieldName} "
                              "when current storage engine is {storageEngine}",
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
                    "Running {storageEngine} with profiling is not supported. Make sure you "
                    "are not using --profile",
                    "Running the selected storage engine with profiling is not supported",
                    "storageEngine"_attr = storageGlobalParams.engine);
        exitCleanly(ExitCode::badOptions);
    }

    if (storageGlobalParams.repair && replSettings.usingReplSets()) {
        LOGV2_ERROR(5019200,
                    "Cannot specify both repair and replSet at the same time (remove --replSet to "
                    "be able to --repair)");
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

    try {
        startup_recovery::repairAndRecoverDatabases(startupOpCtx.get(), lastShutdownState);
    } catch (const ExceptionFor<ErrorCodes::MustDowngrade>& error) {
        LOGV2_FATAL_OPTIONS(
            20573,
            logv2::LogOptions(logv2::LogComponent::kControl, logv2::FatalMode::kContinue),
            "** IMPORTANT: {error}",
            "Wrong mongod version",
            "error"_attr = error.toStatus().reason());
        exitCleanly(ExitCode::needDowngrade);
    }

    // If we are on standalone, load cluster parameters from disk. If we are replicated, this is not
    // a concern as the cluster parameter initializer runs automatically.
    auto replCoord = repl::ReplicationCoordinator::get(startupOpCtx.get());
    invariant(replCoord);
    if (!replCoord->isReplEnabled()) {
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
    }

    // Notify the storage engine that startup is completed before repair exits below, as repair sets
    // the upgrade flag to true.
    auto storageEngine = serviceContext->getStorageEngine();
    invariant(storageEngine);
    storageEngine->notifyStartupComplete();

    BackupCursorHooks::initialize(serviceContext);

    startMongoDFTDC();

    if (mongodGlobalParams.scriptingEnabled) {
        uassert(ErrorCodes::InvalidOptions,
                "Scripting engine not supported in the serverless environment",
                !gMultitenancySupport);
        ScriptEngine::setup();
    }

    if (storageGlobalParams.upgrade) {
        LOGV2(20537, "Finished checking dbs");
        exitCleanly(ExitCode::clean);
    }

    // Start up health log writer thread.
    HealthLogInterface::set(serviceContext, std::make_unique<HealthLog>());
    HealthLogInterface::get(startupOpCtx.get())->startup();

    auto const globalAuthzManager = AuthorizationManager::get(serviceContext);
    uassertStatusOK(globalAuthzManager->initialize(startupOpCtx.get()));

    if (audit::initializeManager) {
        audit::initializeManager(startupOpCtx.get());
    }

    // This is for security on certain platforms (nonce generation)
    srand((unsigned)(curTimeMicros64()) ^ (unsigned(uintptr_t(&startupOpCtx))));  // NOLINT

    if (globalAuthzManager->shouldValidateAuthSchemaOnStartup()) {
        Status status = verifySystemIndexes(startupOpCtx.get());
        if (!status.isOK()) {
            LOGV2_WARNING(20538,
                          "Unable to verify system indexes: {error}",
                          "Unable to verify system indexes",
                          "error"_attr = redact(status));
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
            globalAuthzManager->getAuthorizationVersion(startupOpCtx.get(), &foundSchemaVersion);
        if (!status.isOK()) {
            LOGV2_ERROR(
                20539,
                "Auth schema version is incompatible: User and role management commands require "
                "auth data to have at least schema version {minSchemaVersion} but startup could "
                "not verify schema version: {error}",
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
    } else if (globalAuthzManager->isAuthEnabled()) {
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

    WaitForMajorityService::get(serviceContext).startup(serviceContext);

    if (!serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        // A catalog shard initializes sharding awareness after setting up its config server state.

        // This function may take the global lock.
        initializeShardingAwarenessIfNeededAndLoadGlobalSettings(startupOpCtx.get());
    }

    try {
        if ((serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) ||
             serverGlobalParams.clusterRole.has(ClusterRole::None)) &&
            replSettings.usingReplSets()) {
            ReadWriteConcernDefaults::get(startupOpCtx.get()->getServiceContext())
                .refreshIfNecessary(startupOpCtx.get());
        }
    } catch (const DBException& ex) {
        LOGV2_WARNING(20567,
                      "Error loading read and write concern defaults at startup",
                      "error"_attr = redact(ex));
    }
    readWriteConcernDefaultsMongodStartupChecks(startupOpCtx.get());

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
                !replCoord->isReplEnabled());
        replCoord->startup(startupOpCtx.get(), lastShutdownState);
    }

    MirrorMaestro::init(serviceContext);

    if (!storageGlobalParams.queryableBackupMode) {

        if (storageEngine->supportsCappedCollections()) {
            logStartup(startupOpCtx.get());
        }

        startFreeMonitoring(serviceContext);

        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
            // Note: For replica sets, ShardingStateRecovery happens on transition to primary.
            if (!replCoord->isReplEnabled()) {
                if (ShardingState::get(startupOpCtx.get())->enabled()) {
                    uassertStatusOK(ShardingStateRecovery_DEPRECATED::recover(startupOpCtx.get()));
                }
            }
        }

        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            initializeGlobalShardingStateForConfigServerIfNeeded(startupOpCtx.get());

            // This function may take the global lock.
            initializeShardingAwarenessIfNeededAndLoadGlobalSettings(startupOpCtx.get());
        }

        if (serverGlobalParams.clusterRole.has(ClusterRole::None) &&
            replSettings.usingReplSets()) {  // standalone replica set
            // The keys client must use local read concern if the storage engine can't support
            // majority read concern.
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

            ReplicaSetNodeProcessInterface::getReplicaSetNodeExecutor(serviceContext)->startup();
        }

        replCoord->startup(startupOpCtx.get(), lastShutdownState);
        // 'getOldestActiveTimestamp', which is called in the background by the checkpoint thread,
        // requires a read on 'config.transactions' at the stableTimestamp. If this read occurs
        // while applying prepared transactions at the end of replication recovery, it's possible to
        // prepare a transaction at timestamp earlier than the stableTimestamp. This will result in
        // a WiredTiger invariant. Register the callback after the call to 'startup' to ensure we've
        // finished applying prepared transactions.
        if (replCoord->isReplEnabled()) {
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
        } else {
            startTTLMonitor(serviceContext);
        }

        if (replSettings.usingReplSets() || !gInternalValidateFeaturesAsPrimary) {
            serverGlobalParams.validateFeaturesAsPrimary.store(false);
        }

        if (replSettings.usingReplSets()) {
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

    SessionKiller::set(serviceContext,
                       std::make_shared<SessionKiller>(serviceContext, killSessionsLocal));

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
            MONGO_IDLE_THREAD_BLOCK;
            return waitForShutdown();
        }
    }

    // If not in standalone mode, start background tasks to:
    //  * Periodically remove expired pre-images from the 'system.preimages'
    //  * Periodically remove expired documents from change collections
    const auto isStandalone =
        repl::ReplicationCoordinator::get(serviceContext)->getReplicationMode() ==
        repl::ReplicationCoordinator::modeNone;
    if (!isStandalone) {
        startChangeStreamExpiredPreImagesRemover(serviceContext);
        startChangeCollectionExpiredDocumentsRemover(serviceContext);
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
    LogicalSessionCacheServer kind = LogicalSessionCacheServer::kStandalone;
    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        kind = LogicalSessionCacheServer::kConfigServer;
    } else if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        kind = LogicalSessionCacheServer::kSharded;
    } else if (replSettings.usingReplSets()) {
        kind = LogicalSessionCacheServer::kReplicaSet;
    }

    LogicalSessionCache::set(serviceContext, makeLogicalSessionCacheD(kind));

    if (analyze_shard_key::supportsSamplingQueries(serviceContext, true /* ignoreFCV */) &&
        serverGlobalParams.clusterRole.has(ClusterRole::None)) {
        analyze_shard_key::QueryAnalysisSampler::get(serviceContext).onStartup();
    }

    auto cacheLoader = std::make_unique<stats::StatsCacheLoaderImpl>();
    auto catalog = std::make_unique<stats::StatsCatalog>(serviceContext, std::move(cacheLoader));
    stats::StatsCatalog::set(serviceContext, std::move(catalog));

    // MessageServer::run will return when exit code closes its socket and we don't need the
    // operation context anymore
    startupOpCtx.reset();

    auto start = serviceContext->getServiceEntryPoint()->start();
    if (!start.isOK()) {
        LOGV2_ERROR(20571,
                    "Error starting service entry point: {error}",
                    "Error starting service entry point",
                    "error"_attr = start);
        return ExitCode::netError;
    }

    if (!storageGlobalParams.repair) {
        start = serviceContext->getTransportLayer()->start();
        if (!start.isOK()) {
            LOGV2_ERROR(20572,
                        "Error starting listener: {error}",
                        "Error starting listener",
                        "error"_attr = start);
            return ExitCode::netError;
        }
    }

    if (!initialize_server_global_state::writePidFile()) {
        quickExit(ExitCode::fail);
    }

    // Startup options are written to the audit log at the end of startup so that cluster server
    // parameters are guaranteed to have been initialized from disk at this point.
    audit::logStartupOptions(Client::getCurrent(), serverGlobalParams.parsedOpts);

    serviceContext->notifyStartupComplete();

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
        if (getMagicRestoreMain() == nullptr) {
            LOGV2_FATAL_NOTRACE(7180701, "--magicRestore cannot be used with a community build");
        }
        return getMagicRestoreMain()(serviceContext);
    }

    MONGO_IDLE_THREAD_BLOCK;
    return waitForShutdown();
}

ExitCode initAndListen(ServiceContext* service, int listenPort) {
    try {
        return _initAndListen(service, listenPort);
    } catch (DBException& e) {
        LOGV2_ERROR(20557,
                    "Exception in initAndListen: {error}, terminating",
                    "DBException in initAndListen, terminating",
                    "error"_attr = e.toString());
        return ExitCode::uncaught;
    } catch (std::exception& e) {
        LOGV2_ERROR(20558,
                    "Exception in initAndListen std::exception: {error}, terminating",
                    "std::exception in initAndListen, terminating",
                    "error"_attr = e.what());
        return ExitCode::uncaught;
    } catch (int& n) {
        LOGV2_ERROR(20559,
                    "Exception in initAndListen int: {reason}, terminating",
                    "Exception in initAndListen, terminating",
                    "reason"_attr = n);
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

void setUpCollectionShardingState(ServiceContext* serviceContext) {
    if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        CollectionShardingStateFactory::set(
            serviceContext, std::make_unique<CollectionShardingStateFactoryShard>(serviceContext));
    } else {
        CollectionShardingStateFactory::set(
            serviceContext,
            std::make_unique<CollectionShardingStateFactoryStandalone>(serviceContext));
    }
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
    tpOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(std::make_unique<rpc::VectorClockMetadataHook>(serviceContext));
    hookList->addHook(std::make_unique<rpc::ClientMetadataPropagationEgressHook>());
    return std::make_unique<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(tpOptions),
        executor::makeNetworkInterface("ReplNodeDbWorkerNetwork", nullptr, std::move(hookList)));
}

auto makeReplicationExecutor(ServiceContext* serviceContext) {
    ThreadPool::Options tpOptions;
    tpOptions.threadNamePrefix = "ReplCoord-";
    tpOptions.poolName = "ReplCoordThreadPool";
    tpOptions.maxThreads = 50;
    tpOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
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
    if (serverGlobalParams.clusterRole.has(ClusterRole::None) && replCoord->isReplEnabled())
        ReplicaSetNodeProcessInterface::setReplicaSetNodeExecutor(
            serviceContext, makeReplicaSetNodeExecutor(serviceContext));

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
        opObserverRegistry->addObserver(std::make_unique<OpObserverShardingImpl>(
            std::make_unique<OplogWriterTransactionProxy>(std::make_unique<OplogWriterImpl>())));
        opObserverRegistry->addObserver(std::make_unique<ShardServerOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<ReshardingOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<repl::TenantMigrationDonorOpObserver>());
        opObserverRegistry->addObserver(
            std::make_unique<repl::TenantMigrationRecipientOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<UserWriteBlockModeOpObserver>());
        if (getGlobalReplSettings().isServerless()) {
            opObserverRegistry->addObserver(std::make_unique<ShardSplitDonorOpObserver>());
            opObserverRegistry->addObserver(
                std::make_unique<repl::ShardMergeRecipientOpObserver>());
        }
    }

    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        if (!gFeatureFlagCatalogShard.isEnabledAndIgnoreFCVUnsafeAtStartup()) {
            opObserverRegistry->addObserver(
                std::make_unique<OpObserverImpl>(std::make_unique<OplogWriterImpl>()));
        }

        opObserverRegistry->addObserver(std::make_unique<ConfigServerOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<ReshardingOpObserver>());
    }

    if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
        opObserverRegistry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OplogWriterImpl>()));
        opObserverRegistry->addObserver(std::make_unique<repl::TenantMigrationDonorOpObserver>());
        opObserverRegistry->addObserver(
            std::make_unique<repl::TenantMigrationRecipientOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<UserWriteBlockModeOpObserver>());
        if (getGlobalReplSettings().isServerless()) {
            opObserverRegistry->addObserver(std::make_unique<ShardSplitDonorOpObserver>());
            opObserverRegistry->addObserver(
                std::make_unique<repl::ShardMergeRecipientOpObserver>());
        }
    }

    opObserverRegistry->addObserver(std::make_unique<AuthOpObserver>());
    opObserverRegistry->addObserver(
        std::make_unique<repl::PrimaryOnlyServiceOpObserver>(serviceContext));
    opObserverRegistry->addObserver(std::make_unique<FcvOpObserver>());
    opObserverRegistry->addObserver(std::make_unique<ClusterServerParameterOpObserver>());
    opObserverRegistry->addObserver(std::make_unique<analyze_shard_key::QueryAnalysisOpObserver>());

    setupFreeMonitoringOpObserver(opObserverRegistry.get());

    if (audit::opObserverRegistrar) {
        audit::opObserverRegistrar(opObserverRegistry.get());
    }

    serviceContext->setOpObserver(std::move(opObserverRegistry));
}

#ifdef MONGO_CONFIG_SSL
MONGO_INITIALIZER_GENERAL(setSSLManagerType, (), ("SSLManager"))
(InitializerContext* context) {
    isSSLServer = true;
}
#endif

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

// NOTE: This function may be called at any time after registerShutdownTask is called below. It
// must not depend on the prior execution of mongo initializers or the existence of threads.
void shutdownTask(const ShutdownTaskArgs& shutdownArgs) {
    // This client initiation pattern is only to be used here, with plans to eliminate this pattern
    // down the line.
    if (!haveClient())
        Client::initThread(getThreadName());

    auto const client = Client::getCurrent();
    auto const serviceContext = client->getServiceContext();

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

    // If we don't have shutdownArgs, we're shutting down from a signal, or other clean shutdown
    // path.
    //
    // In that case, do a default step down, still shutting down if stepDown fails.
    if (auto replCoord = repl::ReplicationCoordinator::get(serviceContext);
        replCoord && !shutdownArgs.isUserInitiated) {
        replCoord->enterTerminalShutdown();
        ServiceContext::UniqueOperationContext uniqueOpCtx;
        OperationContext* opCtx = client->getOperationContext();
        if (!opCtx) {
            uniqueOpCtx = client->makeOperationContext();
            opCtx = uniqueOpCtx.get();
        }

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

    if (auto replCoord = repl::ReplicationCoordinator::get(serviceContext);
        replCoord && replCoord->enterQuiesceModeIfSecondary(shutdownTimeout)) {
        ServiceContext::UniqueOperationContext uniqueOpCtx;
        OperationContext* opCtx = client->getOperationContext();
        if (!opCtx) {
            uniqueOpCtx = client->makeOperationContext();
            opCtx = uniqueOpCtx.get();
        }
        if (MONGO_unlikely(hangDuringQuiesceMode.shouldFail())) {
            LOGV2_OPTIONS(
                4695101, {LogComponent::kReplication}, "hangDuringQuiesceMode failpoint enabled");
            hangDuringQuiesceMode.pauseWhileSet(opCtx);
        }

        LOGV2_OPTIONS(4695102,
                      {LogComponent::kReplication},
                      "Entering quiesce mode for shutdown",
                      "quiesceTime"_attr = shutdownTimeout);
        opCtx->sleepFor(shutdownTimeout);
        LOGV2_OPTIONS(4695103, {LogComponent::kReplication}, "Exiting quiesce mode for shutdown");
    }

    DiskSpaceMonitor::stop(serviceContext);

    LOGV2_OPTIONS(6371601, {LogComponent::kDefault}, "Shutting down the FLE Crud thread pool");
    stopFLECrud();

    LOGV2_OPTIONS(4784901, {LogComponent::kCommand}, "Shutting down the MirrorMaestro");
    MirrorMaestro::shutdown(serviceContext);

    LOGV2_OPTIONS(4784902, {LogComponent::kSharding}, "Shutting down the WaitForMajorityService");
    WaitForMajorityService::get(serviceContext).shutDown();

    // Join the logical session cache before the transport layer.
    if (auto lsc = LogicalSessionCache::get(serviceContext)) {
        LOGV2(4784903, "Shutting down the LogicalSessionCache");
        lsc->joinOnShutDown();
    }

    if (analyze_shard_key::supportsSamplingQueries(serviceContext, true /* ignoreFCV */)) {
        LOGV2_OPTIONS(7350601, {LogComponent::kDefault}, "Shutting down the QueryAnalysisSampler");
        analyze_shard_key::QueryAnalysisSampler::get(serviceContext).onShutdown();
    }

    // Shutdown the TransportLayer so that new connections aren't accepted
    if (auto tl = serviceContext->getTransportLayer()) {
        LOGV2_OPTIONS(
            20562, {LogComponent::kNetwork}, "Shutdown: going to close listening sockets");
        tl->shutdown();
    }

    // Shut down the global dbclient pool so callers stop waiting for connections.
    LOGV2_OPTIONS(4784905, {LogComponent::kNetwork}, "Shutting down the global connection pool");
    globalConnPool.shutdown();

    // Inform Flow Control to stop gating writes on ticket admission. This must be done before the
    // Periodic Runner is shut down (see SERVER-41751).
    if (auto flowControlTicketholder = FlowControlTicketholder::get(serviceContext)) {
        LOGV2(4784906, "Shutting down the FlowControlTicketholder");
        flowControlTicketholder->setInShutdown();
    }

    if (auto exec = ReplicaSetNodeProcessInterface::getReplicaSetNodeExecutor(serviceContext)) {
        LOGV2_OPTIONS(
            4784907, {LogComponent::kReplication}, "Shutting down the replica set node executor");
        exec->shutdown();
        exec->join();
    }

    if (auto storageEngine = serviceContext->getStorageEngine()) {
        if (storageEngine->supportsReadConcernSnapshot()) {
            LOGV2(4784908, "Shutting down the PeriodicThreadToAbortExpiredTransactions");
            PeriodicThreadToAbortExpiredTransactions::get(serviceContext)->stop();
        }

        ServiceContext::UniqueOperationContext uniqueOpCtx;
        OperationContext* opCtx = client->getOperationContext();
        if (!opCtx) {
            uniqueOpCtx = client->makeOperationContext();
            opCtx = uniqueOpCtx.get();
        }
        {
            stdx::lock_guard lg(*client);
            opCtx->setIsExecutingShutdown();
        }

        // This can wait a long time while we drain the secondary's apply queue, especially if
        // it is building an index.
        LOGV2_OPTIONS(
            4784909, {LogComponent::kReplication}, "Shutting down the ReplicationCoordinator");
        repl::ReplicationCoordinator::get(serviceContext)->shutdown(opCtx);

        // Terminate the index consistency check.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
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
        const std::set<std::string> excludedClients = {std::string(kFTDCThreadName)};
        serviceContext->setKillAllOperations(excludedClients);

        // Clear tenant migration access blockers after killing all operation contexts to ensure
        // that no operation context cancellation token continuation holds the last reference to the
        // TenantMigrationAccessBlockerExecutor.
        LOGV2_OPTIONS(5093807,
                      {LogComponent::kTenantMigration},
                      "Shutting down all TenantMigrationAccessBlockers on global shutdown");
        TenantMigrationAccessBlockerRegistry::get(serviceContext).shutDown();

        if (MONGO_unlikely(pauseWhileKillingOperationsAtShutdown.shouldFail())) {
            LOGV2_OPTIONS(4701700,
                          {LogComponent::kDefault},
                          "pauseWhileKillingOperationsAtShutdown failpoint enabled");
            sleepsecs(1);
        }

        // Destroy all stashed transaction resources, in order to release locks.
        LOGV2_OPTIONS(4784913, {LogComponent::kCommand}, "Shutting down all open transactions");
        killSessionsLocalShutdownAllTransactions(opCtx);

        LOGV2_OPTIONS(4784914,
                      {LogComponent::kReplication},
                      "Acquiring the ReplicationStateTransitionLock for shutdown");
        rstl.waitForLockUntil(Date_t::max());

        // Release the rstl before waiting for the index build threads to join as index build
        // reacquires rstl in uninterruptible lock guard to finish their cleanup process.
        rstl.release();

        // Shuts down the thread pool and waits for index builds to finish.
        // Depends on setKillAllOperations() above to interrupt the index build operations.
        LOGV2_OPTIONS(4784915, {LogComponent::kIndex}, "Shutting down the IndexBuildsCoordinator");
        IndexBuildsCoordinator::get(serviceContext)->shutdown(opCtx);
    }

    LOGV2_OPTIONS(4784918, {LogComponent::kNetwork}, "Shutting down the ReplicaSetMonitor");
    ReplicaSetMonitor::shutdown();

    if (auto sr = Grid::get(serviceContext)->shardRegistry()) {
        LOGV2_OPTIONS(4784919, {LogComponent::kSharding}, "Shutting down the shard registry");
        sr->shutdown();
    }

    if (ShardingState::get(serviceContext)->enabled()) {
        TransactionCoordinatorService::get(serviceContext)->shutdown();
    }

    // Validator shutdown must be called after setKillAllOperations is called. Otherwise, this can
    // deadlock.
    if (auto validator = LogicalTimeValidator::get(serviceContext)) {
        LOGV2_OPTIONS(
            4784920, {LogComponent::kReplication}, "Shutting down the LogicalTimeValidator");
        validator->shutDown();
    }

    // The migrationutil executor must be shut down before shutting down the CatalogCacheLoader.
    // Otherwise, it may try to schedule work on the CatalogCacheLoader and fail.
    LOGV2_OPTIONS(4784921, {LogComponent::kSharding}, "Shutting down the MigrationUtilExecutor");
    auto migrationUtilExecutor = migrationutil::getMigrationUtilExecutor(serviceContext);
    migrationUtilExecutor->shutdown();
    migrationUtilExecutor->join();

    if (ShardingState::get(serviceContext)->enabled()) {
        LOGV2_OPTIONS(4784922, {LogComponent::kSharding}, "Shutting down the CatalogCacheLoader");
        CatalogCacheLoader::get(serviceContext).shutDown();
    }

    // Shutdown the Service Entry Point and its sessions and give it a grace period to complete.
    if (auto sep = serviceContext->getServiceEntryPoint()) {
        LOGV2_OPTIONS(4784923, {LogComponent::kCommand}, "Shutting down the ServiceEntryPoint");
        if (!sep->shutdown(Seconds(10))) {
            LOGV2_OPTIONS(20563,
                          {LogComponent::kNetwork},
                          "Service entry point did not shutdown within the time limit");
        }
    }

    LOGV2(4784925, "Shutting down free monitoring");
    stopFreeMonitoring();

    if (auto* healthLog = HealthLogInterface::get(serviceContext)) {
        LOGV2(4784927, "Shutting down the HealthLog");
        healthLog->shutdown();
    }

    LOGV2(4784928, "Shutting down the TTL monitor");
    shutdownTTLMonitor(serviceContext);

    LOGV2(6278511, "Shutting down the Change Stream Expired Pre-images Remover");
    shutdownChangeStreamExpiredPreImagesRemover(serviceContext);

    shutdownChangeCollectionExpiredDocumentsRemover(serviceContext);

    // We should always be able to acquire the global lock at shutdown.
    // An OperationContext is not necessary to call lockGlobal() during shutdown, as it's only used
    // to check that lockGlobal() is not called after a transaction timestamp has been set.
    //
    // For a Windows service, dbexit does not call exit(), so we must leak the lock outside
    // of this function to prevent any operations from running that need a lock.
    //
    LOGV2(4784929, "Acquiring the global lock for shutdown");
    LockerImpl* globalLocker = new LockerImpl(serviceContext);
    globalLocker->lockGlobal(nullptr, MODE_X);

    // Global storage engine may not be started in all cases before we exit
    if (serviceContext->getStorageEngine()) {
        LOGV2(4784930, "Shutting down the storage engine");
        shutdownGlobalStorageEngineCleanly(serviceContext);
    }

    // We wait for the oplog cap maintainer thread to stop. This has to be done after the engine has
    // been closed since the thread will only die once all references to the oplog have been deleted
    // and we're performing a shutdown.
    OplogCapMaintainerThread::get(serviceContext)->waitForFinish();

    // We drop the scope cache because leak sanitizer can't see across the
    // thread we use for proxying MozJS requests. Dropping the cache cleans up
    // the memory and makes leak sanitizer happy.
    LOGV2_OPTIONS(4784931, {LogComponent::kDefault}, "Dropping the scope cache for shutdown");
    ScriptEngine::dropScopeCache();

    // Shutdown Full-Time Data Capture
    stopMongoDFTDC();

    LOGV2(20565, "Now exiting");

    audit::logShutdown(client);

#ifndef MONGO_CONFIG_USE_RAW_LATCHES
    LatchAnalyzer::get(serviceContext).dump();
#endif

#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    // SessionKiller relies on the network stack being cleanly shutdown which only occurs under
    // sanitizers
    SessionKiller::shutdown(serviceContext);
#endif

    FlowControl::shutdown(serviceContext);
#ifdef MONGO_CONFIG_SSL
    OCSPManager::shutdown(serviceContext);
#endif
}

}  // namespace

int mongod_main(int argc, char* argv[]) {
    ThreadSafetyContext::getThreadSafetyContext()->forbidMultiThreading();

    registerShutdownTask(shutdownTask);

    setupSignalHandlers();

    srand(static_cast<unsigned>(curTimeMicros64()));  // NOLINT

    Status status = mongo::runGlobalInitializers(std::vector<std::string>(argv, argv + argc));
    if (!status.isOK()) {
        LOGV2_FATAL_OPTIONS(
            20574,
            logv2::LogOptions(logv2::LogComponent::kControl, logv2::FatalMode::kContinue),
            "Error during global initialization: {error}",
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
                "Error creating service context: {error}",
                "Error creating service context",
                "error"_attr = redact(cause));
            quickExit(ExitCode::fail);
        }
    }();

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

    setUpCollectionShardingState(service);
    setUpCatalog(service);
    setUpReplication(service);
    setUpObservers(service);
    service->setServiceEntryPoint(std::make_unique<ServiceEntryPointMongod>(service));
    SessionCatalog::get(service)->setOnEagerlyReapedSessionsFn(
        InternalTransactionsReapService::onEagerlyReapedSessions);

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
