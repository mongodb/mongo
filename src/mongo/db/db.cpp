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

#define MONGO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kStorage

#include "merizo/platform/basic.h"

#include <boost/filesystem/operations.hpp>
#include <boost/optional.hpp>
#include <fstream>
#include <iostream>
#include <limits>
#include <signal.h>
#include <string>

#include "merizo/base/init.h"
#include "merizo/base/initializer.h"
#include "merizo/base/status.h"
#include "merizo/client/global_conn_pool.h"
#include "merizo/client/replica_set_monitor.h"
#include "merizo/config.h"
#include "merizo/db/audit.h"
#include "merizo/db/auth/auth_op_observer.h"
#include "merizo/db/auth/authorization_manager.h"
#include "merizo/db/auth/sasl_options.h"
#include "merizo/db/catalog/collection.h"
#include "merizo/db/catalog/create_collection.h"
#include "merizo/db/catalog/database.h"
#include "merizo/db/catalog/database_catalog_entry.h"
#include "merizo/db/catalog/database_holder_impl.h"
#include "merizo/db/catalog/health_log.h"
#include "merizo/db/catalog/index_catalog.h"
#include "merizo/db/catalog/index_key_validate.h"
#include "merizo/db/catalog/uuid_catalog.h"
#include "merizo/db/client.h"
#include "merizo/db/clientcursor.h"
#include "merizo/db/commands/feature_compatibility_version.h"
#include "merizo/db/commands/feature_compatibility_version_gen.h"
#include "merizo/db/concurrency/d_concurrency.h"
#include "merizo/db/concurrency/lock_state.h"
#include "merizo/db/concurrency/replication_state_transition_lock_guard.h"
#include "merizo/db/concurrency/write_conflict_exception.h"
#include "merizo/db/db_raii.h"
#include "merizo/db/dbdirectclient.h"
#include "merizo/db/dbhelpers.h"
#include "merizo/db/dbmessage.h"
#include "merizo/db/exec/working_set_common.h"
#include "merizo/db/free_mon/free_mon_merizod.h"
#include "merizo/db/ftdc/ftdc_merizod.h"
#include "merizo/db/global_settings.h"
#include "merizo/db/index_builds_coordinator_merizod.h"
#include "merizo/db/index_names.h"
#include "merizo/db/initialize_server_global_state.h"
#include "merizo/db/initialize_server_security_state.h"
#include "merizo/db/initialize_snmp.h"
#include "merizo/db/introspect.h"
#include "merizo/db/json.h"
#include "merizo/db/keys_collection_client_direct.h"
#include "merizo/db/keys_collection_client_sharded.h"
#include "merizo/db/keys_collection_manager.h"
#include "merizo/db/kill_sessions.h"
#include "merizo/db/kill_sessions_local.h"
#include "merizo/db/log_process_details.h"
#include "merizo/db/logical_clock.h"
#include "merizo/db/logical_session_cache.h"
#include "merizo/db/logical_session_cache_factory_merizod.h"
#include "merizo/db/logical_time_metadata_hook.h"
#include "merizo/db/logical_time_validator.h"
#include "merizo/db/merizod_options.h"
#include "merizo/db/namespace_string.h"
#include "merizo/db/op_observer_registry.h"
#include "merizo/db/operation_context.h"
#include "merizo/db/periodic_runner_job_abort_expired_transactions.h"
#include "merizo/db/periodic_runner_job_decrease_snapshot_cache_pressure.h"
#include "merizo/db/query/internal_plans.h"
#include "merizo/db/repair_database_and_check_version.h"
#include "merizo/db/repl/drop_pending_collection_reaper.h"
#include "merizo/db/repl/oplog.h"
#include "merizo/db/repl/repl_settings.h"
#include "merizo/db/repl/replication_consistency_markers_impl.h"
#include "merizo/db/repl/replication_coordinator.h"
#include "merizo/db/repl/replication_coordinator_external_state_impl.h"
#include "merizo/db/repl/replication_coordinator_impl.h"
#include "merizo/db/repl/replication_process.h"
#include "merizo/db/repl/replication_recovery.h"
#include "merizo/db/repl/storage_interface_impl.h"
#include "merizo/db/repl/topology_coordinator.h"
#include "merizo/db/repl_set_member_in_standalone_mode.h"
#include "merizo/db/s/balancer/balancer.h"
#include "merizo/db/s/config/sharding_catalog_manager.h"
#include "merizo/db/s/config_server_op_observer.h"
#include "merizo/db/s/op_observer_sharding_impl.h"
#include "merizo/db/s/shard_server_op_observer.h"
#include "merizo/db/s/sharding_initialization_merizod.h"
#include "merizo/db/s/sharding_state_recovery.h"
#include "merizo/db/server_options.h"
#include "merizo/db/service_context.h"
#include "merizo/db/service_entry_point_merizod.h"
#include "merizo/db/session_killer.h"
#include "merizo/db/startup_warnings_merizod.h"
#include "merizo/db/stats/counters.h"
#include "merizo/db/storage/backup_cursor_hooks.h"
#include "merizo/db/storage/encryption_hooks.h"
#include "merizo/db/storage/storage_engine.h"
#include "merizo/db/storage/storage_engine_init.h"
#include "merizo/db/storage/storage_engine_lock_file.h"
#include "merizo/db/storage/storage_options.h"
#include "merizo/db/system_index.h"
#include "merizo/db/transaction_participant.h"
#include "merizo/db/ttl.h"
#include "merizo/db/wire_version.h"
#include "merizo/executor/network_connection_hook.h"
#include "merizo/executor/network_interface_factory.h"
#include "merizo/executor/network_interface_thread_pool.h"
#include "merizo/executor/thread_pool_task_executor.h"
#include "merizo/platform/process_id.h"
#include "merizo/rpc/metadata/egress_metadata_hook_list.h"
#include "merizo/s/client/shard_registry.h"
#include "merizo/s/grid.h"
#include "merizo/s/sharding_initialization.h"
#include "merizo/scripting/dbdirectclient_factory.h"
#include "merizo/scripting/engine.h"
#include "merizo/stdx/future.h"
#include "merizo/stdx/memory.h"
#include "merizo/stdx/thread.h"
#include "merizo/transport/transport_layer_manager.h"
#include "merizo/util/assert_util.h"
#include "merizo/util/background.h"
#include "merizo/util/cmdline_utils/censor_cmdline.h"
#include "merizo/util/concurrency/idle_thread_block.h"
#include "merizo/util/concurrency/thread_name.h"
#include "merizo/util/exception_filter_win32.h"
#include "merizo/util/exit.h"
#include "merizo/util/fail_point_service.h"
#include "merizo/util/fast_clock_source_factory.h"
#include "merizo/util/log.h"
#include "merizo/util/net/socket_utils.h"
#include "merizo/util/net/ssl_manager.h"
#include "merizo/util/ntservice.h"
#include "merizo/util/options_parser/startup_options.h"
#include "merizo/util/periodic_runner.h"
#include "merizo/util/periodic_runner_factory.h"
#include "merizo/util/quick_exit.h"
#include "merizo/util/ramlog.h"
#include "merizo/util/scopeguard.h"
#include "merizo/util/sequence_util.h"
#include "merizo/util/signal_handlers.h"
#include "merizo/util/stacktrace.h"
#include "merizo/util/startup_test.h"
#include "merizo/util/text.h"
#include "merizo/util/time_support.h"
#include "merizo/util/version.h"

#include "merizo/db/storage/flow_control.h"

#ifdef MONGO_CONFIG_SSL
#include "merizo/util/net/ssl_options.h"
#endif

#if !defined(_WIN32)
#include <sys/file.h>
#endif

namespace merizo {

using logger::LogComponent;
using std::endl;

namespace {

const NamespaceString startupLogCollectionName("local.startup_log");

#ifdef _WIN32
const ntservice::NtServiceDefaultStrings defaultServiceStrings = {
    L"MerizoDB", L"MerizoDB", L"MerizoDB Server"};
#endif

void logStartup(OperationContext* opCtx) {
    BSONObjBuilder toLog;
    std::stringstream id;
    id << getHostNameCached() << "-" << jsTime().asInt64();
    toLog.append("_id", id.str());
    toLog.append("hostname", getHostNameCached());

    toLog.appendTimeT("startTime", time(0));
    toLog.append("startTimeLocal", dateToCtimeString(Date_t::now()));

    toLog.append("cmdLine", serverGlobalParams.parsedOpts);
    toLog.append("pid", ProcessId::getCurrent().asLongLong());


    BSONObjBuilder buildinfo(toLog.subobjStart("buildinfo"));
    VersionInfoInterface::instance().appendBuildInfo(&buildinfo);
    appendStorageEngineList(opCtx->getServiceContext(), &buildinfo);
    buildinfo.doneFast();

    BSONObj o = toLog.obj();

    Lock::GlobalWrite lk(opCtx);
    AutoGetOrCreateDb autoDb(opCtx, startupLogCollectionName.db(), merizo::MODE_X);
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
    auto opObserverRegistry = stdx::make_unique<OpObserverRegistry>();
    opObserverRegistry->addObserver(stdx::make_unique<OpObserverShardingImpl>());
    opObserverRegistry->addObserver(stdx::make_unique<UUIDCatalogObserver>());
    opObserverRegistry->addObserver(stdx::make_unique<AuthOpObserver>());

    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        opObserverRegistry->addObserver(stdx::make_unique<ShardServerOpObserver>());
    } else if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        opObserverRegistry->addObserver(stdx::make_unique<ConfigServerOpObserver>());
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
        l << "MerizoDB starting : pid=" << pid << " port=" << serverGlobalParams.port
          << " dbpath=" << storageGlobalParams.dbpath;

        const bool is32bit = sizeof(int*) == 4;
        l << (is32bit ? " 32" : " 64") << "-bit host=" << getHostNameCached() << endl;
    }

    DEV log(LogComponent::kControl) << "DEBUG build (which is slower)" << endl;

#if defined(_WIN32)
    VersionInfoInterface::instance().logTargetMinOS();
#endif

    logProcessDetails();

    serviceContext->setServiceEntryPoint(
        stdx::make_unique<ServiceEntryPointMongod>(serviceContext));

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
                     stdx::make_unique<FlowControl>(
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

#ifdef MONGO_CONFIG_SSL
    if (sslGlobalParams.sslAllowInvalidCertificates &&
        ((serverGlobalParams.clusterAuthMode.load() == ServerGlobalParams::ClusterAuthMode_x509) ||
         sequenceContains(saslGlobalParams.authenticationMechanisms, "MONGODB-X509"))) {
        log() << "** WARNING: While invalid X509 certificates may be used to" << startupWarningsLog;
        log() << "**          connect to this server, they will not be considered"
              << startupWarningsLog;
        log() << "**          permissible for authentication." << startupWarningsLog;
        log() << startupWarningsLog;
    }
#endif

    {
        std::stringstream ss;
        ss << endl;
        ss << "*********************************************************************" << endl;
        ss << " ERROR: dbpath (" << storageGlobalParams.dbpath << ") does not exist." << endl;
        ss << " Create this directory or give existing directory in --dbpath." << endl;
        ss << " See http://dochub.merizodb.org/core/startingandstoppingmerizo" << endl;
        ss << "*********************************************************************" << endl;
        uassert(10296, ss.str().c_str(), boost::filesystem::exists(storageGlobalParams.dbpath));
    }

    initializeSNMP();

    if (!storageGlobalParams.readOnly) {
        boost::filesystem::remove_all(storageGlobalParams.dbpath + "/_tmp/");
    }

    if (merizodGlobalParams.scriptingEnabled) {
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
                  << "has been removed from MerizoDB 4.0. In order to upgrade the auth schema, "
                  << "first downgrade MerizoDB binaries to version 3.6 and then run the "
                  << "authSchemaUpgrade command. "
                  << "See http://dochub.merizodb.org/core/3.0-upgrade-to-scram-sha-1";
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
        waitForShardRegistryReload(startupOpCtx.get()).transitional_ignore();
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
            auto keysCollectionClient = stdx::make_unique<KeysCollectionClientDirect>();
            auto keyManager = std::make_shared<KeysCollectionManager>(
                KeysCollectionManager::kKeyManagerPurposeString,
                std::move(keysCollectionClient),
                Seconds(KeysRotationIntervalSec));
            keyManager->startMonitoring(startupOpCtx->getServiceContext());

            LogicalTimeValidator::set(startupOpCtx->getServiceContext(),
                                      stdx::make_unique<LogicalTimeValidator>(keyManager));
        }

        replCoord->startup(startupOpCtx.get());
        if (getReplSetMemberInStandaloneMode(serviceContext)) {
            log() << startupWarningsLog;
            log() << "** WARNING: merizod started without --replSet yet document(s) are present in "
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
            log() << " For more info see http://dochub.merizodb.org/core/ttlcollections";
            log() << startupWarningsLog;
        } else {
            startTTLBackgroundJob();
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
        startPeriodicThreadToDecreaseSnapshotHistoryCachePressure(serviceContext);
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

    auto sessionCache = makeLogicalSessionCacheD(kind);
    LogicalSessionCache::set(serviceContext, std::move(sessionCache));

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
    merizo::signalForkSuccess();
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
    merizo::forkServerOrDie();
    return Status::OK();
}

/*
 * This function should contain the startup "actions" that we take based on the startup config.
 * It is intended to separate the actions from "storage" and "validation" of our startup
 * configuration.
 */
void startupConfigActions(const std::vector<std::string>& args) {
    // The "command" option is deprecated.  For backward compatibility, still support the "run"
    // and "dbppath" command.  The "run" command is the same as just running merizod, so just
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
}

auto makeReplicationExecutor(ServiceContext* serviceContext) {
    ThreadPool::Options tpOptions;
    tpOptions.poolName = "replexec";
    tpOptions.maxThreads = 50;
    tpOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    auto hookList = stdx::make_unique<rpc::EgressMetadataHookList>();
    hookList->addHook(stdx::make_unique<rpc::LogicalTimeMetadataHook>(serviceContext));
    return stdx::make_unique<executor::ThreadPoolTaskExecutor>(
        stdx::make_unique<ThreadPool>(tpOptions),
        executor::makeNetworkInterface("Replication", nullptr, std::move(hookList)));
}

void setUpReplication(ServiceContext* serviceContext) {
    repl::StorageInterface::set(serviceContext, stdx::make_unique<repl::StorageInterfaceImpl>());
    auto storageInterface = repl::StorageInterface::get(serviceContext);

    auto consistencyMarkers =
        stdx::make_unique<repl::ReplicationConsistencyMarkersImpl>(storageInterface);
    auto recovery = stdx::make_unique<repl::ReplicationRecoveryImpl>(storageInterface,
                                                                     consistencyMarkers.get());
    repl::ReplicationProcess::set(
        serviceContext,
        stdx::make_unique<repl::ReplicationProcess>(
            storageInterface, std::move(consistencyMarkers), std::move(recovery)));
    auto replicationProcess = repl::ReplicationProcess::get(serviceContext);

    repl::DropPendingCollectionReaper::set(
        serviceContext, stdx::make_unique<repl::DropPendingCollectionReaper>(storageInterface));
    auto dropPendingCollectionReaper = repl::DropPendingCollectionReaper::get(serviceContext);

    repl::TopologyCoordinator::Options topoCoordOptions;
    topoCoordOptions.maxSyncSourceLagSecs = Seconds(repl::maxSyncSourceLagSecs);
    topoCoordOptions.clusterRole = serverGlobalParams.clusterRole;

    auto logicalClock = stdx::make_unique<LogicalClock>(serviceContext);
    LogicalClock::set(serviceContext, std::move(logicalClock));

    auto replCoord = stdx::make_unique<repl::ReplicationCoordinatorImpl>(
        serviceContext,
        getGlobalReplSettings(),
        stdx::make_unique<repl::ReplicationCoordinatorExternalStateImpl>(
            serviceContext, dropPendingCollectionReaper, storageInterface, replicationProcess),
        makeReplicationExecutor(serviceContext),
        stdx::make_unique<repl::TopologyCoordinator>(topoCoordOptions),
        replicationProcess,
        storageInterface,
        static_cast<int64_t>(curTimeMillis64()));
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
// must not depend on the prior execution of merizo initializers or the existence of threads.
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
            replCoord->stepDown(opCtx, false /* force */, Seconds(10), Seconds(120));
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
        Status status = svcExec->shutdown(Seconds(5));
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

int merizoDbMain(int argc, char* argv[], char** envp) {
    registerShutdownTask(shutdownTask);

    setupSignalHandlers();

    srand(static_cast<unsigned>(curTimeMicros64()));

    Status status = merizo::runGlobalInitializers(argc, argv, envp);
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

    StartupTest::runTests();
    ExitCode exitCode = initAndListen(serverGlobalParams.port);
    exitCleanly(exitCode);
    return 0;
}

}  // namespace
}  // namespace merizo

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables merizoDbMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    merizo::WindowsCommandLine wcl(argc, argvW, envpW);
    int exitCode = merizo::merizoDbMain(argc, wcl.argv(), wcl.envp());
    merizo::quickExit(exitCode);
}
#else
int main(int argc, char* argv[], char** envp) {
    int exitCode = merizo::merizoDbMain(argc, argv, envp);
    merizo::quickExit(exitCode);
}
#endif
