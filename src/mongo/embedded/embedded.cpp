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

#include "mongo/embedded/embedded.h"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/database_holder_impl.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_impl.h"
#include "mongo/db/session/service_liaison_impl.h"
#include "mongo/db/session/service_liaison_shard.h"
#include "mongo/db/session/sessions_collection.h"
#include "mongo/db/session/sessions_collection_standalone.h"
#include "mongo/db/startup_recovery.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/wire_version.h"
#include "mongo/embedded/collection_sharding_state_factory_embedded.h"
#include "mongo/embedded/embedded_options_parser_init.h"
#include "mongo/embedded/index_builds_coordinator_embedded.h"
#include "mongo/embedded/operation_logger_embedded.h"
#include "mongo/embedded/periodic_runner_embedded.h"
#include "mongo/embedded/read_write_concern_defaults_cache_lookup_embedded.h"
#include "mongo/embedded/replication_coordinator_embedded.h"
#include "mongo/embedded/service_entry_point_embedded.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_options.h"
#include "mongo/platform/process_id.h"
#include "mongo/s/sharding_state.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace embedded {
namespace {

// Noop, to fulfill dependencies for other initializers.
MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {}

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

void setUpCatalog(ServiceContext* serviceContext) {
    DatabaseHolder::set(serviceContext, std::make_unique<DatabaseHolderImpl>());
    Collection::Factory::set(serviceContext, std::make_unique<CollectionImpl::FactoryImpl>());
}

// Create a minimalistic replication coordinator to provide a limited interface for users. Not
// functional to provide any replication logic.
ServiceContext::ConstructorActionRegisterer replicationManagerInitializer(
    "CreateReplicationManager", {"SSLManager", "default"}, [](ServiceContext* serviceContext) {
        repl::StorageInterface::set(serviceContext, std::make_unique<repl::StorageInterfaceImpl>());

        auto replCoord = std::make_unique<ReplicationCoordinatorEmbedded>(serviceContext);
        repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));

        IndexBuildsCoordinator::set(serviceContext,
                                    std::make_unique<IndexBuildsCoordinatorEmbedded>());
    });

MONGO_INITIALIZER(fsyncLockedForWriting)(InitializerContext* context) {
    setLockedForWritingImpl([]() { return false; });
}

GlobalInitializerRegisterer filterAllowedIndexFieldNamesEmbeddedInitializer(
    "FilterAllowedIndexFieldNamesEmbedded",
    [](InitializerContext* service) {
        index_key_validate::filterAllowedIndexFieldNames =
            [](std::map<StringData, std::set<IndexType>>& allowedIndexFieldNames) {
                allowedIndexFieldNames.erase(IndexDescriptor::kBackgroundFieldName);
                allowedIndexFieldNames.erase(IndexDescriptor::kExpireAfterSecondsFieldName);
            };
    },
    DeinitializerFunction(nullptr),
    {},
    {"FilterAllowedIndexFieldNames"});

ServiceContext::ConstructorActionRegisterer shardingStateRegisterer{
    "ShardingState",
    [](ServiceContext* service) {
        ShardingState::create(service);
        CollectionShardingStateFactory::set(
            service, std::make_unique<CollectionShardingStateFactoryEmbedded>(service));
    },
    [](ServiceContext* service) {
        CollectionShardingStateFactory::clear(service);
    }};

}  // namespace

using logv2::LogComponent;

void shutdown(ServiceContext* srvContext) {
    {
        ThreadClient tc(srvContext->getService());
        auto const client = Client::getCurrent();
        auto const serviceContext = client->getServiceContext();

        // TODO(SERVER-74659): Please revisit if this thread could be made killable.
        {
            stdx::lock_guard<Client> lk(*tc.get());
            tc.get()->setSystemOperationUnkillableByStepdown(lk);
        }

        serviceContext->setKillAllOperations();

        // We should always be able to acquire the global lock at shutdown.
        // Close all open databases, shutdown storage engine and run all deinitializers.
        auto shutdownOpCtx = serviceContext->makeOperationContext(client);
        // Service context is in shutdown mode, even new operation contexts are considered killed.
        // Marking the opCtx as executing shutdown prevents this, and makes the opCtx ignore all
        // interrupts.
        shutdownOpCtx->setIsExecutingShutdown();

        Lock::GlobalLock lk(shutdownOpCtx.get(), MODE_X);
        auto databaseHolder = DatabaseHolder::get(shutdownOpCtx.get());
        databaseHolder->closeAll(shutdownOpCtx.get());

        LogicalSessionCache::set(serviceContext, nullptr);

        repl::ReplicationCoordinator::get(serviceContext)
            ->shutdown(shutdownOpCtx.get(), nullptr /* shutdownTimeElapsedBuilder */);
        IndexBuildsCoordinator::get(serviceContext)->shutdown(shutdownOpCtx.get());

        // Global storage engine may not be started in all cases before we exit
        if (serviceContext->getStorageEngine()) {
            shutdownGlobalStorageEngineCleanly(serviceContext);
        }
    }
    setGlobalServiceContext(nullptr);

    Status status = mongo::runGlobalDeinitializers();
    uassertStatusOKWithContext(status, "Global deinitilization failed");

    LOGV2_OPTIONS(22551, {LogComponent::kControl}, "now exiting");
}

ServiceContext* initialize(const char* yaml_config) {
    srand(static_cast<unsigned>(curTimeMicros64()));  // NOLINT

    if (yaml_config)
        embedded::EmbeddedOptionsConfig::instance().set(yaml_config);

    Status status = mongo::runGlobalInitializers(std::vector<std::string>{});
    uassertStatusOKWithContext(status, "Global initilization failed");
    ScopeGuard giGuard([] { mongo::runGlobalDeinitializers().ignore(); });
    setGlobalServiceContext(ServiceContext::make());

    auto serviceContext = getGlobalServiceContext();
    serviceContext->getService()->setServiceEntryPoint(
        std::make_unique<ServiceEntryPointEmbedded>());
    serviceContext->setTransportLayerManager(std::make_unique<transport::TransportLayerManagerImpl>(
        std::make_unique<transport::TransportLayerMock>()));

    Client::initThread("initandlisten", getGlobalServiceContext()->getService());

    // TODO(SERVER-74659): Please revisit if this thread could be made killable.
    {
        stdx::lock_guard<Client> lk(cc());
        cc().setSystemOperationUnkillableByStepdown(lk);
    }

    // Make sure current thread have no client set in thread_local when we leave this function
    ScopeGuard clientGuard([] { Client::releaseCurrent(); });

    auto opObserverRegistry = std::make_unique<OpObserverRegistry>();
    opObserverRegistry->addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerEmbedded>()));
    serviceContext->setOpObserver(std::move(opObserverRegistry));

    DBDirectClientFactory::get(serviceContext).registerImplementation([](OperationContext* opCtx) {
        return std::unique_ptr<DBClientBase>(new DBDirectClient(opCtx));
    });

    {
        ProcessId pid = ProcessId::getCurrent();
        const bool is32bit = sizeof(int*) == 4;
        LOGV2_OPTIONS(4615667,
                      {logv2::LogComponent::kControl},
                      "MongoDB starting",
                      "pid"_attr = pid.toNative(),
                      "port"_attr = serverGlobalParams.port,
                      "dbpath"_attr =
                          boost::filesystem::path(storageGlobalParams.dbpath).generic_string(),
                      "architecture"_attr = (is32bit ? "32-bit" : "64-bit"));
    }

    if (kDebugBuild)
        LOGV2_OPTIONS(22552, {LogComponent::kControl}, "DEBUG build (which is slower)");

    // The periodic runner is required by the storage engine to be running beforehand.
    auto periodicRunner = std::make_unique<PeriodicRunnerEmbedded>(
        serviceContext, serviceContext->getPreciseClockSource());
    serviceContext->setPeriodicRunner(std::move(periodicRunner));

    // When starting the server with --queryableBackupMode or --recoverFromOplogAsStandalone, we are
    // in read-only mode and don't allow user-originating operations to perform writes
    if (storageGlobalParams.queryableBackupMode ||
        repl::ReplSettings::shouldRecoverFromOplogAsStandalone()) {
        serviceContext->disallowUserWrites();
    }

    setUpCatalog(serviceContext);

    // Creating the operation context before initializing the storage engine allows the storage
    // engine initialization to make use of the lock manager.
    auto lastShutdownState = [&] {
        auto initializeStorageEngineOpCtx = serviceContext->makeOperationContext(&cc());
        shard_role_details::setRecoveryUnit(initializeStorageEngineOpCtx.get(),
                                            std::make_unique<RecoveryUnitNoop>(),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

        auto lastShutdownState = initializeStorageEngine(initializeStorageEngineOpCtx.get(),
                                                         StorageEngineInitFlags::kAllowNoLockFile);
        invariant(StorageEngine::LastShutdownState::kClean == lastShutdownState);
        StorageControl::startStorageControls(serviceContext);
        return lastShutdownState;
    }();

    auto startupOpCtx = serviceContext->makeOperationContext(&cc());

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
                LOGV2_WARNING(22554,
                              "Detected configuration for non-active storage engine {e_fieldName} "
                              "when current storage engine is {storageGlobalParams_engine}",
                              "e_fieldName"_attr = e.fieldName(),
                              "storageGlobalParams_engine"_attr = storageGlobalParams.engine);
            }
        }
    }

    {
        std::stringstream ss;
        ss << std::endl;
        ss << "*********************************************************************" << std::endl;
        ss << " ERROR: dbpath (" << storageGlobalParams.dbpath << ") does not exist." << std::endl;
        ss << " Create this directory or give existing directory in --dbpath." << std::endl;
        ss << " See http://dochub.mongodb.org/core/startingandstoppingmongo" << std::endl;
        ss << "*********************************************************************" << std::endl;
        uassert(50677, ss.str().c_str(), boost::filesystem::exists(storageGlobalParams.dbpath));
    }

    boost::filesystem::remove_all(storageGlobalParams.dbpath + "/_tmp/");

    ReadWriteConcernDefaults::create(serviceContext, readWriteConcernDefaultsCacheLookupEmbedded);

    if (storageGlobalParams.engine != "devnull") {
        Lock::GlobalWrite lk(startupOpCtx.get());
        FeatureCompatibilityVersion::setIfCleanStartup(startupOpCtx.get(),
                                                       repl::StorageInterface::get(serviceContext));
    }

    try {
        startup_recovery::repairAndRecoverDatabases(startupOpCtx.get(), lastShutdownState);
    } catch (const ExceptionFor<ErrorCodes::MustDowngrade>& error) {
        LOGV2_FATAL_OPTIONS(22555,
                            logv2::LogOptions(LogComponent::kControl, logv2::FatalMode::kContinue),
                            "** IMPORTANT: {error_toStatus_reason}",
                            "error_toStatus_reason"_attr = error.toStatus().reason());
        quickExit(ExitCode::needDowngrade);
    }

    // Ensure FCV document exists and is initialized in-memory. Fatally asserts if there is an
    // error.
    FeatureCompatibilityVersion::fassertInitializedAfterStartup(startupOpCtx.get());

    // Notify the storage engine that startup is completed before repair exits below, as repair sets
    // the upgrade flag to true.
    serviceContext->getStorageEngine()->notifyStorageStartupRecoveryComplete();

    if (storageGlobalParams.upgrade) {
        LOGV2(22553, "finished checking dbs");
        exitCleanly(ExitCode::clean);
    }

    // This is for security on certain platforms (nonce generation)
    srand((unsigned)(curTimeMicros64()) ^ (unsigned(uintptr_t(&startupOpCtx))));  // NOLINT

    // Set up the logical session cache
    LogicalSessionCache::set(
        serviceContext,
        std::make_unique<LogicalSessionCacheImpl>(
            std::make_unique<ServiceLiaisonImpl>(
                service_liaison_shard_callbacks::getOpenCursorSessions,
                service_liaison_shard_callbacks::killCursorsWithMatchingSessions),
            std::make_shared<SessionsCollectionStandalone>(),
            [](OperationContext*, SessionsCollection&, Date_t) {
                return 0; /* No op */
            }));

    // MessageServer::run will return when exit code closes its socket and we don't need the
    // operation context anymore
    startupOpCtx.reset();

    serviceContext->notifyStorageStartupRecoveryComplete();

    // Init succeeded, no need for global deinit.
    giGuard.dismiss();

    return serviceContext;
}

}  // namespace embedded
}  // namespace mongo
