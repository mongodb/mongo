/**
*    Copyright (C) 2018 MongoDB Inc.
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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/embedded/embedded.h"

#include "mongo/base/initializer.h"
#include "mongo/config.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/health_log.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index_rebuilder.h"
#include "mongo/db/kill_sessions_local.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repair_database_and_check_version.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_registrar.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/session_killer.h"
#include "mongo/db/storage/encryption_hooks.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/ttl.h"
#include "mongo/embedded/replication_coordinator_embedded.h"
#include "mongo/embedded/service_context_embedded.h"
#include "mongo/embedded/service_entry_point_embedded.h"
#include "mongo/logger/log_component.h"
#include "mongo/scripting/dbdirectclient_factory.h"
#include "mongo/util/background.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/time_support.h"

#include <boost/filesystem.hpp>


namespace mongo {
namespace embedded {
namespace {
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


// Noop, to fulfull dependencies for other initializers
MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
    return Status::OK();
}

// Create a minimalistic replication coordinator to provide a limited interface for users. Not
// functional to provide any replication logic.
GlobalInitializerRegisterer replicationManagerInitializer(
    "CreateReplicationManager",
    {"SSLManager", "ServiceContext", "default"},
    [](InitializerContext* context) {
        auto serviceContext = getGlobalServiceContext();
        repl::StorageInterface::set(serviceContext, std::make_unique<repl::StorageInterfaceImpl>());

        auto logicalClock = stdx::make_unique<LogicalClock>(serviceContext);
        LogicalClock::set(serviceContext, std::move(logicalClock));

        auto replCoord = std::make_unique<ReplicationCoordinatorEmbedded>(serviceContext);
        repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));
        repl::setOplogCollectionName(serviceContext);
        return Status::OK();
    },
    [](DeinitializerContext* context) {
        auto serviceContext = getGlobalServiceContext();

        repl::ReplicationCoordinator::set(serviceContext, nullptr);
        LogicalClock::set(serviceContext, nullptr);
        repl::StorageInterface::set(serviceContext, nullptr);

        return Status::OK();
    });

MONGO_INITIALIZER(fsyncLockedForWriting)(InitializerContext* context) {
    setLockedForWritingImpl([]() { return false; });
    return Status::OK();
}
}  // namespace

using logger::LogComponent;
using std::endl;

void shutdown(ServiceContext* srvContext) {

    Client::initThreadIfNotAlready();
    auto const client = Client::getCurrent();
    auto const serviceContext = client->getServiceContext();
    invariant(srvContext == serviceContext);

    serviceContext->setKillAllOperations();

    // We should always be able to acquire the global lock at shutdown.
    // Close all open databases, shutdown storage engine and run all deinitializers.
    auto shutdownOpCtx = serviceContext->makeOperationContext(client);
    {
        UninterruptibleLockGuard noInterrupt(shutdownOpCtx->lockState());
        Lock::GlobalLock lk(shutdownOpCtx.get(), MODE_X);
        DatabaseHolder::getDatabaseHolder().closeAll(shutdownOpCtx.get(), "shutdown");

        // Shut down the background periodic task runner
        if (auto runner = serviceContext->getPeriodicRunner()) {
            runner->shutdown();
        }

        // Global storage engine may not be started in all cases before we exit
        if (serviceContext->getStorageEngine()) {
            shutdownGlobalStorageEngineCleanly(serviceContext);
        }

        Status status = mongo::runGlobalDeinitializers();
        uassertStatusOKWithContext(status, "Global deinitilization failed");
    }
    shutdownOpCtx.reset();

    if (Client::getCurrent())
        Client::destroy();

    setGlobalServiceContext(nullptr);

    log(LogComponent::kControl) << "now exiting";
}


ServiceContext* initialize(const char* yaml_config) {
    srand(static_cast<unsigned>(curTimeMicros64()));

    // yaml_config is passed to the options parser through the argc/argv interface that already
    // existed. If it is nullptr then use 0 count which will be interpreted as empty string.
    const char* argv[2] = {yaml_config, nullptr};

    Status status = mongo::runGlobalInitializers(yaml_config ? 1 : 0, argv, nullptr);
    uassertStatusOKWithContext(status, "Global initilization failed");

    Client::initThread("initandlisten");

    initWireSpec();

    auto serviceContext = getGlobalServiceContext();

    auto opObserverRegistry = std::make_unique<OpObserverRegistry>();
    opObserverRegistry->addObserver(std::make_unique<OpObserverImpl>());
    opObserverRegistry->addObserver(std::make_unique<UUIDCatalogObserver>());
    serviceContext->setOpObserver(std::move(opObserverRegistry));

    DBDirectClientFactory::get(serviceContext).registerImplementation([](OperationContext* opCtx) {
        return std::unique_ptr<DBClientBase>(new DBDirectClient(opCtx));
    });

    {
        ProcessId pid = ProcessId::getCurrent();
        LogstreamBuilder l = log(LogComponent::kControl);
        l << "MongoDB starting : pid=" << pid << " port=" << serverGlobalParams.port
          << " dbpath=" << storageGlobalParams.dbpath;

        const bool is32bit = sizeof(int*) == 4;
        l << (is32bit ? " 32" : " 64") << "-bit" << endl;
    }

    DEV log(LogComponent::kControl) << "DEBUG build (which is slower)" << endl;

    createLockFile(serviceContext);

    initializeStorageEngine(serviceContext);

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

    {
        std::stringstream ss;
        ss << endl;
        ss << "*********************************************************************" << endl;
        ss << " ERROR: dbpath (" << storageGlobalParams.dbpath << ") does not exist." << endl;
        ss << " Create this directory or give existing directory in --dbpath." << endl;
        ss << " See http://dochub.mongodb.org/core/startingandstoppingmongo" << endl;
        ss << "*********************************************************************" << endl;
        uassert(50677, ss.str().c_str(), boost::filesystem::exists(storageGlobalParams.dbpath));
    }

    {
        std::stringstream ss;
        ss << "repairpath (" << storageGlobalParams.repairpath << ") does not exist";
        uassert(50678, ss.str().c_str(), boost::filesystem::exists(storageGlobalParams.repairpath));
    }

    if (!storageGlobalParams.readOnly) {
        boost::filesystem::remove_all(storageGlobalParams.dbpath + "/_tmp/");
    }

    auto startupOpCtx = serviceContext->makeOperationContext(&cc());

    bool canCallFCVSetIfCleanStartup =
        !storageGlobalParams.readOnly && !(storageGlobalParams.engine == "devnull");
    if (canCallFCVSetIfCleanStartup) {
        Lock::GlobalWrite lk(startupOpCtx.get());
        FeatureCompatibilityVersion::setIfCleanStartup(startupOpCtx.get(),
                                                       repl::StorageInterface::get(serviceContext));
    }

    auto swNonLocalDatabases = repairDatabasesAndCheckVersion(startupOpCtx.get());
    if (!swNonLocalDatabases.isOK()) {
        // SERVER-31611 introduced a return value to `repairDatabasesAndCheckVersion`. Previously,
        // a failing condition would fassert. SERVER-31611 covers a case where the binary (3.6) is
        // refusing to start up because it refuses acknowledgement of FCV 3.2 and requires the
        // user to start up with an older binary. Thus shutting down the server must leave the
        // datafiles in a state that the older binary can start up. This requires going through a
        // clean shutdown.
        //
        // The invariant is *not* a statement that `repairDatabasesAndCheckVersion` must return
        // `MustDowngrade`. Instead, it is meant as a guardrail to protect future developers from
        // accidentally buying into this behavior. New errors that are returned from the method
        // may or may not want to go through a clean shutdown, and they likely won't want the
        // program to return an exit code of `EXIT_NEED_DOWNGRADE`.
        severe(LogComponent::kControl) << "** IMPORTANT: "
                                       << swNonLocalDatabases.getStatus().reason();
        invariant(swNonLocalDatabases == ErrorCodes::MustDowngrade);
        quickExit(EXIT_NEED_DOWNGRADE);
    }

    // Assert that the in-memory featureCompatibilityVersion parameter has been explicitly set. If
    // we are part of a replica set and are started up with no data files, we do not set the
    // featureCompatibilityVersion until a primary is chosen. For this case, we expect the in-memory
    // featureCompatibilityVersion parameter to still be uninitialized until after startup.
    if (canCallFCVSetIfCleanStartup) {
        invariant(serverGlobalParams.featureCompatibility.isVersionInitialized());
    }

    if (storageGlobalParams.upgrade) {
        log() << "finished checking dbs";
        exitCleanly(EXIT_CLEAN);
    }

    // This is for security on certain platforms (nonce generation)
    srand((unsigned)(curTimeMicros64()) ^ (unsigned(uintptr_t(&startupOpCtx))));

    if (!storageGlobalParams.readOnly) {
        restartInProgressIndexesFromLastShutdown(startupOpCtx.get());
    }

    // MessageServer::run will return when exit code closes its socket and we don't need the
    // operation context anymore
    startupOpCtx.reset();

    // Make sure current thread have no client set in thread_local
    Client::releaseCurrent();

    serviceContext->notifyStartupComplete();

    return serviceContext;
}
}  // namespace embedded
}  // namespace mongo
