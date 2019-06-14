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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <boost/filesystem/operations.hpp>
#include <boost/optional.hpp>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <signal.h>
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
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder_impl.h"
#include "mongo/db/catalog/health_log.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/flow_control_ticketholder.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/free_mon/free_mon_mongod.h"
#include "mongo/db/ftdc/ftdc_mongod.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index/index_access_method_factory_impl.h"
#include "mongo/db/index_builds_coordinator_mongod.h"
#include "mongo/db/index_names.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/initialize_server_security_state.h"
#include "mongo/db/initialize_snmp.h"
#include "mongo/db/introspect.h"
#include "mongo/db/json.h"
#include "mongo/db/keys_collection_client_direct.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/kill_sessions.h"
#include "mongo/db/kill_sessions_local.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/logical_session_cache_factory_mongod.h"
#include "mongo/db/logical_time_metadata_hook.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/periodic_runner_job_abort_expired_transactions.h"
#include "mongo/db/periodic_runner_job_decrease_snapshot_cache_pressure.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repair_database_and_check_version.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state_impl.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/replication_coordinator_impl_gen.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl_set_member_in_standalone_mode.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/config_server_op_observer.h"
#include "mongo/db/s/op_observer_sharding_impl.h"
#include "mongo/db/s/shard_server_op_observer.h"
#include "mongo/db/s/sharding_initialization_mongod.h"
#include "mongo/db/s/sharding_state_recovery.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_mongod.h"
#include "mongo/db/session_killer.h"
#include "mongo/db/startup_warnings_mongod.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/storage/encryption_hooks.h"
#include "mongo/db/storage/flow_control.h"
#include "mongo/db/storage/flow_control_parameters_gen.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/system_index.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/ttl.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/platform/process_id.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/background.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/exception_filter_win32.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/fast_clock_source_factory.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/ramlog.h"
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

namespace mongo {

using logger::LogComponent;
using std::endl;

namespace {

const NamespaceString startupLogCollectionName("local.startup_log");

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
    AutoGetOrCreateDb autoDb(opCtx, startupLogCollectionName.db(), mongo::MODE_X);
    Database* db = autoDb.getDb();
    Collection* collection = db->getCollection(opCtx, startupLogCollectionName);
    WriteUnitOfWork wunit(opCtx);
    if (!collection) {
        BSONObj options = BSON("capped" << true << "size" << 10 * 1024 * 1024);
        repl::UnreplicatedWritesBlock uwb(opCtx);
        CollectionOptions collectionOptions;
        uassertStatusOK(
            collectionOptions.parse(options, CollectionOptions::ParseKind::parseForCommand));
        uassertStatusOK(db->userCreateNS(opCtx, startupLogCollectionName, collectionOptions));
        collection = db->getCollection(opCtx, startupLogCollectionName);
    }
    invariant(collection);

    OpDebug* const nullOpDebug = nullptr;
    uassertStatusOK(collection->insertDocument(opCtx, InsertStatement(o), nullOpDebug, false));
    wunit.commit();
}

void initWireSpec() {
    WireSpec& spec = WireSpec::instance();

    // The featureCompatibilityVersion behavior defaults to the downgrade behavior while the
    // in-memory version is unset.

    spec.incomingInternalClient.minWireVersion = RELEASE_2_4_AND_BEFORE;
    spec.incomingInternalClient.maxWireVersion = LATEST_WIRE_VERSION;

    spec.outgoing.minWireVersion = RELEASE_2_4_AND_BEFORE;
    spec.outgoing.maxWireVersion = LATEST_WIRE_VERSION;

    spec.isInternalClient = true;
}

MONGO_FAIL_POINT_DEFINE(shutdownAtStartup);

ExitCode _initAndListen(int listenPort) {
    Client::initThread("initandlisten");

    initWireSpec();
    auto serviceContext = getGlobalServiceContext();

    serviceContext->setFastClockSource(FastClockSourceFactory::create(Milliseconds(10)));
    auto opObserverRegistry = std::make_unique<OpObserverRegistry>();
    opObserverRegistry->addObserver(std::make_unique<OpObserverShardingImpl>());
    opObserverRegistry->addObserver(std::make_unique<AuthOpObserver>());

    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        opObserverRegistry->addObserver(std::make_unique<ShardServerOpObserver>());
    } else if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        opObserverRegistry->addObserver(std::make_unique<ConfigServerOpObserver>());
    }
    setupFreeMonitoringOpObserver(opObserverRegistry.get());


    serviceContext->setOpObserver(std::move(opObserverRegistry));

    DBDirectClientFactory::get(serviceContext).registerImplementation([](OperationContext* opCtx) {
        return std::unique_ptr<DBClientBase>(new DBDirectClient(opCtx));
    });

    const repl::ReplSettings& replSettings =
        repl::ReplicationCoordinator::get(serviceContext)->getSettings();

    {
        ProcessId pid = ProcessId::getCurrent();
        LogstreamBuilder l = log(LogComponent::kControl);
        l << "MongoDB starting : pid=" << pid << " port=" << serverGlobalParams.port
          << " dbpath=" << storageGlobalParams.dbpath;

        const bool is32bit = sizeof(int*) == 4;
        l << (is32bit ? " 32" : " 64") << "-bit host=" << getHostNameCached() << endl;
    }

    DEV log(LogComponent::kControl) << "DEBUG build (which is slower)" << endl;

#if defined(_WIN32)
    VersionInfoInterface::instance().logTargetMinOS();
#endif

    logProcessDetails();

    serviceContext->setServiceEntryPoint(std::make_unique<ServiceEntryPointMongod>(serviceContext));

    if (!storageGlobalParams.repair) {
        auto tl =
            transport::TransportLayerManager::createWithConfig(&serverGlobalParams, serviceContext);
        auto res = tl->setup();
        if (!res.isOK()) {
            error() << "Failed to set up listener: " << res;
            return EXIT_NET_ERROR;
        }
        serviceContext->setTransportLayer(std::move(tl));
    }

    // Set up the periodic runner for background job execution. This is required to be running
    // before the storage engine is initialized.
    auto runner = makePeriodicRunner(serviceContext);
    runner->startup();
    serviceContext->setPeriodicRunner(std::move(runner));
    FlowControl::set(serviceContext,
                     std::make_unique<FlowControl>(
                         serviceContext, repl::ReplicationCoordinator::get(serviceContext)));

    initializeStorageEngine(serviceContext, StorageEngineInitFlags::kNone);

#ifdef MONGO_CONFIG_WIREDTIGER_ENABLED
    if (EncryptionHooks::get(serviceContext)->restartRequired()) {
        exitCleanly(EXIT_CLEAN);
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
                warning() << "Detected configuration for non-active storage engine "
                          << e.fieldName() << " when current storage engine is "
                          << storageGlobalParams.engine;
            }
        }
    }

    // Disallow running a storage engine that doesn't support capped collections with --profile
    if (!serviceContext->getStorageEngine()->supportsCappedCollections() &&
        serverGlobalParams.defaultProfile != 0) {
        log() << "Running " << storageGlobalParams.engine << " with profiling is not supported. "
              << "Make sure you are not using --profile.";
        exitCleanly(EXIT_BADOPTIONS);
    }

    // Disallow running WiredTiger with --nojournal in a replica set
    if (storageGlobalParams.engine == "wiredTiger" && !storageGlobalParams.dur &&
        replSettings.usingReplSets()) {
        log() << "Running wiredTiger without journaling in a replica set is not "
              << "supported. Make sure you are not using --nojournal and that "
              << "storage.journal.enabled is not set to 'false'.";
        exitCleanly(EXIT_BADOPTIONS);
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

    initializeSNMP();

    startWatchdog();

    if (!storageGlobalParams.readOnly) {
        boost::filesystem::remove_all(storageGlobalParams.dbpath + "/_tmp/");
    }

    if (mongodGlobalParams.scriptingEnabled) {
        ScriptEngine::setup();
    }

    auto startupOpCtx = serviceContext->makeOperationContext(&cc());

    bool canCallFCVSetIfCleanStartup =
        !storageGlobalParams.readOnly && (storageGlobalParams.engine != "devnull");
    if (canCallFCVSetIfCleanStartup && !replSettings.usingReplSets()) {
        Lock::GlobalWrite lk(startupOpCtx.get());
        FeatureCompatibilityVersion::setIfCleanStartup(startupOpCtx.get(),
                                                       repl::StorageInterface::get(serviceContext));
    }

    bool nonLocalDatabases;
    try {
        nonLocalDatabases = repairDatabasesAndCheckVersion(startupOpCtx.get());
    } catch (const ExceptionFor<ErrorCodes::MustDowngrade>& error) {
        severe(LogComponent::kControl) << "** IMPORTANT: " << error.toStatus().reason();
        exitCleanly(EXIT_NEED_DOWNGRADE);
    }

    // Assert that the in-memory featureCompatibilityVersion parameter has been explicitly set. If
    // we are part of a replica set and are started up with no data files, we do not set the
    // featureCompatibilityVersion until a primary is chosen. For this case, we expect the in-memory
    // featureCompatibilityVersion parameter to still be uninitialized until after startup.
    if (canCallFCVSetIfCleanStartup && (!replSettings.usingReplSets() || nonLocalDatabases)) {
        invariant(serverGlobalParams.featureCompatibility.isVersionInitialized());
    }

    if (gFlowControlEnabled.load()) {
        log() << "Flow Control is enabled on this deployment.";
    }

    if (storageGlobalParams.upgrade) {
        log() << "finished checking dbs";
        exitCleanly(EXIT_CLEAN);
    }

    // Start up health log writer thread.
    HealthLog::get(startupOpCtx.get()).startup();

    auto const globalAuthzManager = AuthorizationManager::get(serviceContext);
    uassertStatusOK(globalAuthzManager->initialize(startupOpCtx.get()));

    // This is for security on certain platforms (nonce generation)
    srand((unsigned)(curTimeMicros64()) ^ (unsigned(uintptr_t(&startupOpCtx))));

    if (globalAuthzManager->shouldValidateAuthSchemaOnStartup()) {
        Status status = verifySystemIndexes(startupOpCtx.get());
        if (!status.isOK()) {
            log() << redact(status);
            if (status == ErrorCodes::AuthSchemaIncompatible) {
                exitCleanly(EXIT_NEED_UPGRADE);
            } else if (status == ErrorCodes::NotMaster) {
                // Try creating the indexes if we become master.  If we do not become master,
                // the master will create the indexes and we will replicate them.
            } else {
                quickExit(EXIT_FAILURE);
            }
        }

        // SERVER-14090: Verify that auth schema version is schemaVersion26Final.
        int foundSchemaVersion;
        status =
            globalAuthzManager->getAuthorizationVersion(startupOpCtx.get(), &foundSchemaVersion);
        if (!status.isOK()) {
            log() << "Auth schema version is incompatible: "
                  << "User and role management commands require auth data to have "
                  << "at least schema version " << AuthorizationManager::schemaVersion26Final
                  << " but startup could not verify schema version: " << status;
            log() << "To manually repair the 'authSchema' document in the admin.system.version "
                     "collection, start up with --setParameter "
                     "startupAuthSchemaValidation=false to disable validation.";
            exitCleanly(EXIT_NEED_UPGRADE);
        }

        if (foundSchemaVersion <= AuthorizationManager::schemaVersion26Final) {
            log() << "This server is using MONGODB-CR, an authentication mechanism which "
                  << "has been removed from MongoDB 4.0. In order to upgrade the auth schema, "
                  << "first downgrade MongoDB binaries to version 3.6 and then run the "
                  << "authSchemaUpgrade command. "
                  << "See http://dochub.mongodb.org/core/3.0-upgrade-to-scram-sha-1";
            exitCleanly(EXIT_NEED_UPGRADE);
        }
    } else if (globalAuthzManager->isAuthEnabled()) {
        error() << "Auth must be disabled when starting without auth schema validation";
        exitCleanly(EXIT_BADOPTIONS);
    } else {
        // If authSchemaValidation is disabled and server is running without auth,
        // warn the user and continue startup without authSchema metadata checks.
        log() << startupWarningsLog;
        log() << "** WARNING: Startup auth schema validation checks are disabled for the "
                 "database."
              << startupWarningsLog;
        log() << "**          This mode should only be used to manually repair corrupted auth "
                 "data."
              << startupWarningsLog;
    }

    // This function may take the global lock.
    auto shardingInitialized = ShardingInitializationMongoD::get(startupOpCtx.get())
                                   ->initializeShardingAwarenessIfNeeded(startupOpCtx.get());
    if (shardingInitialized) {
        auto status = waitForShardRegistryReload(startupOpCtx.get());
        if (!status.isOK()) {
            LOG(0) << "Failed to load the shard registry as part of startup"
                   << causedBy(redact(status));
        }
    }

    auto storageEngine = serviceContext->getStorageEngine();
    invariant(storageEngine);
    BackupCursorHooks::initialize(serviceContext, storageEngine);

    if (!storageGlobalParams.readOnly) {

        if (storageEngine->supportsCappedCollections()) {
            logStartup(startupOpCtx.get());
        }

        startMongoDFTDC();

        startFreeMonitoring(serviceContext);

        auto replCoord = repl::ReplicationCoordinator::get(startupOpCtx.get());
        invariant(replCoord);
        if (replCoord->isReplEnabled()) {
            storageEngine->setOldestActiveTransactionTimestampCallback(
                TransactionParticipant::getOldestActiveTimestamp);
        }

        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            // Note: For replica sets, ShardingStateRecovery happens on transition to primary.
            if (!replCoord->isReplEnabled()) {
                if (ShardingState::get(startupOpCtx.get())->enabled()) {
                    uassertStatusOK(ShardingStateRecovery::recover(startupOpCtx.get()));
                }
            }
        } else if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            initializeGlobalShardingStateForMongoD(startupOpCtx.get(),
                                                   ConnectionString::forLocal(),
                                                   kDistLockProcessIdForConfigServer);

            Balancer::create(startupOpCtx->getServiceContext());

            ShardingCatalogManager::create(
                startupOpCtx->getServiceContext(),
                makeShardingTaskExecutor(executor::makeNetworkInterface("AddShard-TaskExecutor")));

            Grid::get(startupOpCtx.get())->setShardingInitialized();
        } else if (replSettings.usingReplSets()) {  // standalone replica set
            auto keysCollectionClient = std::make_unique<KeysCollectionClientDirect>();
            auto keyManager = std::make_shared<KeysCollectionManager>(
                KeysCollectionManager::kKeyManagerPurposeString,
                std::move(keysCollectionClient),
                Seconds(KeysRotationIntervalSec));
            keyManager->startMonitoring(startupOpCtx->getServiceContext());

            LogicalTimeValidator::set(startupOpCtx->getServiceContext(),
                                      std::make_unique<LogicalTimeValidator>(keyManager));
        }

        replCoord->startup(startupOpCtx.get());
        if (getReplSetMemberInStandaloneMode(serviceContext)) {
            log() << startupWarningsLog;
            log() << "** WARNING: mongod started without --replSet yet document(s) are present in "
                  << NamespaceString::kSystemReplSetNamespace << "." << startupWarningsLog;
            log() << "**          Database contents may appear inconsistent with the oplog and may "
                     "appear to not contain"
                  << startupWarningsLog;
            log() << "**          writes that were visible when this node was running as part of a "
                     "replica set."
                  << startupWarningsLog;
            log() << "**          Restart with --replSet unless you are doing maintenance and no "
                     "other clients are connected."
                  << startupWarningsLog;
            log() << "**          The TTL collection monitor will not start because of this."
                  << startupWarningsLog;
            log() << "**         ";
            log() << " For more info see http://dochub.mongodb.org/core/ttlcollections";
            log() << startupWarningsLog;
        } else {
            startTTLBackgroundJob(serviceContext);
        }

        if (replSettings.usingReplSets() || !gInternalValidateFeaturesAsMaster) {
            serverGlobalParams.validateFeaturesAsMaster.store(false);
        }
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
        startPeriodicThreadToAbortExpiredTransactions(serviceContext);
        startPeriodicThreadToDecreaseSnapshotHistoryIfNotNeeded(serviceContext);
    }

    // Set up the logical session cache
    LogicalSessionCacheServer kind = LogicalSessionCacheServer::kStandalone;
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        kind = LogicalSessionCacheServer::kSharded;
    } else if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        kind = LogicalSessionCacheServer::kConfigServer;
    } else if (replSettings.usingReplSets()) {
        kind = LogicalSessionCacheServer::kReplicaSet;
    }

    LogicalSessionCache::set(serviceContext, makeLogicalSessionCacheD(kind));

    // MessageServer::run will return when exit code closes its socket and we don't need the
    // operation context anymore
    startupOpCtx.reset();

    auto start = serviceContext->getServiceExecutor()->start();
    if (!start.isOK()) {
        error() << "Failed to start the service executor: " << start;
        return EXIT_NET_ERROR;
    }

    start = serviceContext->getServiceEntryPoint()->start();
    if (!start.isOK()) {
        error() << "Failed to start the service entry point: " << start;
        return EXIT_NET_ERROR;
    }

    if (!storageGlobalParams.repair) {
        start = serviceContext->getTransportLayer()->start();
        if (!start.isOK()) {
            error() << "Failed to start the listener: " << start.toString();
            return EXIT_NET_ERROR;
        }
    }

    serviceContext->notifyStartupComplete();

#ifndef _WIN32
    mongo::signalForkSuccess();
#else
    if (ntservice::shouldStartService()) {
        ntservice::reportStatus(SERVICE_RUNNING);
        log() << "Service running";
    }
#endif

    if (MONGO_FAIL_POINT(shutdownAtStartup)) {
        log() << "starting clean exit via failpoint";
        exitCleanly(EXIT_CLEAN);
    }

    MONGO_IDLE_THREAD_BLOCK;
    return waitForShutdown();
}

ExitCode initAndListen(int listenPort) {
    try {
        return _initAndListen(listenPort);
    } catch (DBException& e) {
        log() << "exception in initAndListen: " << e.toString() << ", terminating";
        return EXIT_UNCAUGHT;
    } catch (std::exception& e) {
        log() << "exception in initAndListen std::exception: " << e.what() << ", terminating";
        return EXIT_UNCAUGHT;
    } catch (int& n) {
        log() << "exception in initAndListen int: " << n << ", terminating";
        return EXIT_UNCAUGHT;
    } catch (...) {
        log() << "exception in initAndListen, terminating";
        return EXIT_UNCAUGHT;
    }
}

#if defined(_WIN32)
ExitCode initService() {
    return initAndListen(serverGlobalParams.port);
}
#endif

MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
    mongo::forkServerOrDie();
    return Status::OK();
}

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
            quickExit(EXIT_SUCCESS);
        }

        if (command[0].compare("run") != 0) {
            std::cout << "Invalid command: " << command[0] << endl;
            printMongodHelp(moe::startupOptions);
            quickExit(EXIT_FAILURE);
        }

        if (command.size() > 1) {
            std::cout << "Too many parameters to 'run' command" << endl;
            printMongodHelp(moe::startupOptions);
            quickExit(EXIT_FAILURE);
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
        bool failed = false;

        std::string name =
            (boost::filesystem::path(storageGlobalParams.dbpath) / kLockFileBasename.toString())
                .string();
        if (!boost::filesystem::exists(name) || boost::filesystem::file_size(name) == 0)
            failed = true;

        pid_t pid;
        std::string procPath;
        if (!failed) {
            try {
                std::ifstream f(name.c_str());
                f >> pid;
                procPath = (str::stream() << "/proc/" << pid);
                if (!boost::filesystem::exists(procPath))
                    failed = true;
            } catch (const std::exception& e) {
                std::cerr << "Error reading pid from lock file [" << name << "]: " << e.what()
                          << endl;
                failed = true;
            }
        }

        if (failed) {
            std::cerr << "There doesn't seem to be a server running with dbpath: "
                      << storageGlobalParams.dbpath << std::endl;
            quickExit(EXIT_FAILURE);
        }

        std::cout << "killing process with pid: " << pid << endl;
        int ret = kill(pid, SIGTERM);
        if (ret) {
            int e = errno;
            std::cerr << "failed to kill process: " << errnoWithDescription(e) << endl;
            quickExit(EXIT_FAILURE);
        }

        while (boost::filesystem::exists(procPath)) {
            sleepsecs(1);
        }

        quickExit(EXIT_SUCCESS);
    }
#endif
}

void setUpCatalog(ServiceContext* serviceContext) {
    DatabaseHolder::set(serviceContext, std::make_unique<DatabaseHolderImpl>());
    IndexAccessMethodFactory::set(serviceContext, std::make_unique<IndexAccessMethodFactoryImpl>());
    Collection::Factory::set(serviceContext, std::make_unique<CollectionImpl::FactoryImpl>());
}

auto makeReplicationExecutor(ServiceContext* serviceContext) {
    ThreadPool::Options tpOptions;
    tpOptions.poolName = "replexec";
    tpOptions.maxThreads = 50;
    tpOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(std::make_unique<rpc::LogicalTimeMetadataHook>(serviceContext));
    return std::make_unique<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(tpOptions),
        executor::makeNetworkInterface("Replication", nullptr, std::move(hookList)));
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

    auto logicalClock = std::make_unique<LogicalClock>(serviceContext);
    LogicalClock::set(serviceContext, std::move(logicalClock));

    auto replCoord = std::make_unique<repl::ReplicationCoordinatorImpl>(
        serviceContext,
        getGlobalReplSettings(),
        std::make_unique<repl::ReplicationCoordinatorExternalStateImpl>(
            serviceContext, dropPendingCollectionReaper, storageInterface, replicationProcess),
        makeReplicationExecutor(serviceContext),
        std::make_unique<repl::TopologyCoordinator>(topoCoordOptions),
        replicationProcess,
        storageInterface,
        SecureRandom::create()->nextInt64());
    repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));
    repl::setOplogCollectionName(serviceContext);

    IndexBuildsCoordinator::set(serviceContext, std::make_unique<IndexBuildsCoordinatorMongod>());
}

#ifdef MONGO_CONFIG_SSL
MONGO_INITIALIZER_GENERAL(setSSLManagerType, MONGO_NO_PREREQUISITES, ("SSLManager"))
(InitializerContext* context) {
    isSSLServer = true;
    return Status::OK();
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

        try {
            // For faster tests, we allow a short wait time with setParameter.
            auto waitTime = repl::waitForStepDownOnNonCommandShutdown.load()
                ? Milliseconds(Seconds(10))
                : Milliseconds(100);

            replCoord->stepDown(opCtx, false /* force */, waitTime, Seconds(120));
        } catch (const ExceptionFor<ErrorCodes::NotMaster>&) {
            // ignore not master errors
        } catch (const DBException& e) {
            log() << "Failed to stepDown in non-command initiated shutdown path " << e.toString();
        }
    }

    // Terminate the balancer thread so it doesn't leak memory.
    if (auto balancer = Balancer::get(serviceContext)) {
        balancer->interruptBalancer();
        balancer->waitForBalancerToStop();
    }

    // Join the logical session cache before the transport layer.
    if (auto lsc = LogicalSessionCache::get(serviceContext)) {
        lsc->joinOnShutDown();
    }

    // Shutdown the TransportLayer so that new connections aren't accepted
    if (auto tl = serviceContext->getTransportLayer()) {
        log(LogComponent::kNetwork) << "shutdown: going to close listening sockets...";
        tl->shutdown();
    }

    // Shut down the global dbclient pool so callers stop waiting for connections.
    globalConnPool.shutdown();

    // Shut down the background periodic task runner. This must be done before shutting down the
    // storage engine.
    if (auto runner = serviceContext->getPeriodicRunner()) {
        runner->shutdown();
    }

    // Inform Flow Control to stop gating writes on ticket admission. This is necessary because the
    // ticket refresher thread is stopped as part of the shut down process (see SERVER-41345).
    if (auto flowControlTicketholder = FlowControlTicketholder::get(serviceContext)) {
        flowControlTicketholder->setInShutdown();
    }

    if (serviceContext->getStorageEngine()) {
        ServiceContext::UniqueOperationContext uniqueOpCtx;
        OperationContext* opCtx = client->getOperationContext();
        if (!opCtx) {
            uniqueOpCtx = client->makeOperationContext();
            opCtx = uniqueOpCtx.get();
        }
        opCtx->setIsExecutingShutdown();

        // This can wait a long time while we drain the secondary's apply queue, especially if
        // it is building an index.
        repl::ReplicationCoordinator::get(serviceContext)->shutdown(opCtx);

        ShardingInitializationMongoD::get(serviceContext)->shutDown(opCtx);

        // Acquire the RSTL in mode X. First we enqueue the lock request, then kill all operations,
        // destroy all stashed transaction resources in order to release locks, and finally wait
        // until the lock request is granted.
        repl::ReplicationStateTransitionLockGuard rstl(
            opCtx, MODE_X, repl::ReplicationStateTransitionLockGuard::EnqueueOnly());

        // Kill all operations. After this point, the opCtx will have been marked as killed and will
        // not be usable other than to kill all transactions directly below.
        serviceContext->setKillAllOperations();

        // Destroy all stashed transaction resources, in order to release locks.
        killSessionsLocalShutdownAllTransactions(opCtx);

        rstl.waitForLockUntil(Date_t::max());
    }

    // Shuts down the thread pool and waits for index builds to finish.
    // Depends on setKillAllOperations() above to interrupt the index build operations.
    IndexBuildsCoordinator::get(serviceContext)->shutdown();

    ReplicaSetMonitor::shutdown();

    if (auto sr = Grid::get(serviceContext)->shardRegistry()) {
        sr->shutdown();
    }

    // Validator shutdown must be called after setKillAllOperations is called. Otherwise, this can
    // deadlock.
    if (auto validator = LogicalTimeValidator::get(serviceContext)) {
        validator->shutDown();
    }

#if __has_feature(address_sanitizer)
    // When running under address sanitizer, we get false positive leaks due to disorder around
    // the lifecycle of a connection and request. When we are running under ASAN, we try a lot
    // harder to dry up the server from active connections before going on to really shut down.

    // Shutdown the Service Entry Point and its sessions and give it a grace period to complete.
    if (auto sep = serviceContext->getServiceEntryPoint()) {
        if (!sep->shutdown(Seconds(10))) {
            log(LogComponent::kNetwork)
                << "Service entry point failed to shutdown within timelimit.";
        }
    }

    // Shutdown and wait for the service executor to exit
    if (auto svcExec = serviceContext->getServiceExecutor()) {
        Status status = svcExec->shutdown(Seconds(10));
        if (!status.isOK()) {
            log(LogComponent::kNetwork) << "Service executor failed to shutdown within timelimit: "
                                        << status.reason();
        }
    }
#endif
    stopFreeMonitoring();

    // Shutdown Full-Time Data Capture
    stopMongoDFTDC();

    HealthLog::get(serviceContext).shutdown();

    // We should always be able to acquire the global lock at shutdown.
    //
    // TODO: This call chain uses the locker directly, because we do not want to start an
    // operation context, which also instantiates a recovery unit. Also, using the
    // lockGlobalBegin/lockGlobalComplete sequence, we avoid taking the flush lock.
    //
    // For a Windows service, dbexit does not call exit(), so we must leak the lock outside
    // of this function to prevent any operations from running that need a lock.
    //
    LockerImpl* globalLocker = new LockerImpl();
    LockResult result = globalLocker->lockGlobalBegin(MODE_X, Date_t::max());
    if (result == LOCK_WAITING) {
        globalLocker->lockGlobalComplete(Date_t::max());
    }

    // Global storage engine may not be started in all cases before we exit
    if (serviceContext->getStorageEngine()) {
        shutdownGlobalStorageEngineCleanly(serviceContext);
    }

    // We drop the scope cache because leak sanitizer can't see across the
    // thread we use for proxying MozJS requests. Dropping the cache cleans up
    // the memory and makes leak sanitizer happy.
    ScriptEngine::dropScopeCache();

    log(LogComponent::kControl) << "now exiting";

    audit::logShutdown(client);
}

int mongoDbMain(int argc, char* argv[], char** envp) {
    registerShutdownTask(shutdownTask);

    setupSignalHandlers();

    srand(static_cast<unsigned>(curTimeMicros64()));

    Status status = mongo::runGlobalInitializers(argc, argv, envp);
    if (!status.isOK()) {
        severe(LogComponent::kControl) << "Failed global initialization: " << status;
        quickExit(EXIT_FAILURE);
    }

    try {
        setGlobalServiceContext(ServiceContext::make());
    } catch (...) {
        auto cause = exceptionToStatus();
        severe(LogComponent::kControl) << "Failed to create service context: " << redact(cause);
        quickExit(EXIT_FAILURE);
    }

    auto service = getGlobalServiceContext();
    setUpCatalog(service);
    setUpReplication(service);
    service->setServiceEntryPoint(std::make_unique<ServiceEntryPointMongod>(service));

    ErrorExtraInfo::invariantHaveAllParsers();

    startupConfigActions(std::vector<std::string>(argv, argv + argc));
    cmdline_utils::censorArgvArray(argc, argv);

    if (!initializeServerGlobalState(service))
        quickExit(EXIT_FAILURE);

    if (!initializeServerSecurityGlobalState(service))
        quickExit(EXIT_FAILURE);

    // Per SERVER-7434, startSignalProcessingThread must run after any forks (i.e.
    // initializeServerGlobalState) and before the creation of any other threads
    startSignalProcessingThread();

#if defined(_WIN32)
    if (ntservice::shouldStartService()) {
        ntservice::startService();
        // exits directly and so never reaches here either.
    }
#endif

    ExitCode exitCode = initAndListen(serverGlobalParams.port);
    exitCleanly(exitCode);
    return 0;
}

}  // namespace
}  // namespace mongo

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables mongoDbMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    mongo::WindowsCommandLine wcl(argc, argvW, envpW);
    int exitCode = mongo::mongoDbMain(argc, wcl.argv(), wcl.envp());
    mongo::quickExit(exitCode);
}
#else
int main(int argc, char* argv[], char** envp) {
    int exitCode = mongo::mongoDbMain(argc, argv, envp);
    mongo::quickExit(exitCode);
}
#endif
