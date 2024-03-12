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

#include "mongo/db/service_context_d_test_fixture.h"

#include <type_traits>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/database_holder_impl.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/index_builds_coordinator_mongod.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/collection_sharding_state_factory_shard.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_mongod.h"
#include "mongo/db/session_manager_mongod.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/sharding_state.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/periodic_runner_factory.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

ServiceContextMongoDTest::ServiceContextMongoDTest(Options options)
    : _journalListener(std::move(options._journalListener)),
      _tempDir("service_context_d_test_fixture") {

    if (options._forceDisableTableLogging) {
        storageGlobalParams.forceDisableTableLogging = true;
    }

    if (options._useReplSettings) {
        repl::ReplSettings replSettings;
        replSettings.setOplogSizeBytes(10 * 1024 * 1024);
        replSettings.setReplSetString("rs0");
        setGlobalReplSettings(replSettings);
    } else {
        repl::ReplSettings replSettings;
        // The empty string "disables" replication.
        replSettings.setReplSetString("");
        setGlobalReplSettings(replSettings);
    }

    _stashedStorageParams.engine =
        std::exchange(storageGlobalParams.engine, std::move(options._engine));
    _stashedStorageParams.engineSetByUser =
        std::exchange(storageGlobalParams.engineSetByUser, true);
    _stashedStorageParams.repair =
        std::exchange(storageGlobalParams.repair, (options._repair == RepairAction::kRepair));
    _stashedServerParams.enableMajorityReadConcern = serverGlobalParams.enableMajorityReadConcern;

    if (storageGlobalParams.engine == "devnull") {
        // The devnull storage engine does not support majority read concern.
        LOGV2(4939201,
              "Disabling majority read concern as it isn't supported by the storage engine",
              "storageEngine"_attr = storageGlobalParams.engine);
        serverGlobalParams.enableMajorityReadConcern = false;
    }

    auto const serviceContext = getServiceContext();

    // Authorization manager override must occur before creating new threads like JournalFlusher
    if (options._mockAuthzExternalState) {
        _authzExternalState = options._mockAuthzExternalState.get();
        auto uniqueAuthzManager = std::make_unique<AuthorizationManagerImpl>(
            getService(), std::move(options._mockAuthzExternalState));
        AuthorizationManager::set(getService(), std::move(uniqueAuthzManager));
        AuthorizationManager::get(getService())->setAuthEnabled(true);
    }

    if (options._useMockClock) {
        // Copied from dbtests.cpp. DBTests sets up a controlled mock clock while
        // ServiceContextMongoDTest uses the system clock. Tests moved from dbtests to unittests may
        // depend on a deterministic clock. Additionally, if a test chooses to set a non-zero
        // _autoAdvancingMockClockIncrement, the mock clock will automatically advance by that
        // increment each time it is read.
        auto fastClock = std::make_unique<AutoAdvancingClockSourceMock>(
            options._autoAdvancingMockClockIncrement);
        // Timestamps are split into two 32-bit integers, seconds and "increments". Currently
        // (but maybe not for eternity), a Timestamp with a value of `0` seconds is always
        // considered "null" by `Timestamp::isNull`, regardless of its increment value. Ticking
        // the `ClockSourceMock` only bumps the "increment" counter, thus by default, generating
        // "null" timestamps. Bumping by one second here avoids any accidental interpretations.
        fastClock->advance(Seconds(1));
        serviceContext->setFastClockSource(std::move(fastClock));

        auto preciseClock = std::make_unique<AutoAdvancingClockSourceMock>(
            options._autoAdvancingMockClockIncrement);
        // See above.
        preciseClock->advance(Seconds(1));
        serviceContext->setPreciseClockSource(std::move(preciseClock));
    }

    if (options._mockTickSource) {
        serviceContext->setTickSource(std::move(options._mockTickSource));
    }

    serviceContext->getService()->setServiceEntryPoint(std::make_unique<ServiceEntryPointMongod>());

    auto observerRegistry = std::make_unique<OpObserverRegistry>();
    serviceContext->setOpObserver(std::move(observerRegistry));

    // Set up the periodic runner to allow background job execution for tests that require it.
    auto runner = makePeriodicRunner(serviceContext);
    serviceContext->setPeriodicRunner(std::move(runner));

    storageGlobalParams.dbpath = _tempDir.path();

    storageGlobalParams.ephemeral = options._ephemeral;

    // Since unit tests start in their own directories, by default skip lock file and metadata file
    // for faster startup.
    {
        auto initializeStorageEngineOpCtx = serviceContext->makeOperationContext(&cc());
        shard_role_details::setRecoveryUnit(initializeStorageEngineOpCtx.get(),
                                            std::make_unique<RecoveryUnitNoop>(),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

        initializeStorageEngine(initializeStorageEngineOpCtx.get(), options._initFlags);
    }

    StorageControl::startStorageControls(serviceContext, true /*forTestOnly*/);

    DatabaseHolder::set(serviceContext, std::make_unique<DatabaseHolderImpl>());
    Collection::Factory::set(serviceContext, std::make_unique<CollectionImpl::FactoryImpl>());
    ShardingState::create_forTest_DO_NOT_USE(serviceContext);
    CollectionShardingStateFactory::set(
        serviceContext, std::make_unique<CollectionShardingStateFactoryShard>(serviceContext));
    serviceContext->getStorageEngine()->notifyStorageStartupRecoveryComplete();

    if (options._indexBuildsCoordinator) {
        IndexBuildsCoordinator::set(serviceContext, std::move(options._indexBuildsCoordinator));
    } else {
        IndexBuildsCoordinator::set(serviceContext,
                                    std::make_unique<IndexBuildsCoordinatorMongod>());
    }

    if (_journalListener) {
        serviceContext->getStorageEngine()->setJournalListener(_journalListener.get());
    }
}

ServiceContextMongoDTest::~ServiceContextMongoDTest() {
    CollectionShardingStateFactory::clear(getServiceContext());

    {
        ServiceContext::UniqueOperationContext uniqueOpCtx;
        auto opCtx = getClient()->getOperationContext();
        if (!opCtx) {
            uniqueOpCtx = getClient()->makeOperationContext();
            opCtx = uniqueOpCtx.get();
        }

        Lock::GlobalLock glk(opCtx, MODE_X);
        auto databaseHolder = DatabaseHolder::get(opCtx);
        databaseHolder->closeAll(opCtx);
    }

    shutdownGlobalStorageEngineCleanly(getServiceContext());

    std::swap(storageGlobalParams.engine, _stashedStorageParams.engine);
    std::swap(storageGlobalParams.engineSetByUser, _stashedStorageParams.engineSetByUser);
    std::swap(storageGlobalParams.repair, _stashedStorageParams.repair);
    std::swap(serverGlobalParams.enableMajorityReadConcern,
              _stashedServerParams.enableMajorityReadConcern);

    storageGlobalParams.reset();
}

void ServiceContextMongoDTest::tearDown() {
    {
        // Some tests set the current OperationContext but do not release it until destruction.
        ServiceContext::UniqueOperationContext uniqueOpCtx;
        auto opCtx = getClient()->getOperationContext();
        if (!opCtx) {
            uniqueOpCtx = getClient()->makeOperationContext();
            opCtx = uniqueOpCtx.get();
        }
        IndexBuildsCoordinator::get(opCtx)->shutdown(opCtx);
    }

    ServiceContextTest::tearDown();
}

}  // namespace mongo
