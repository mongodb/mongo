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

#include "mongo/db/client.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/index_builds_coordinator_mongod.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog_helper.h"
#include "mongo/db/local_catalog/collection_impl.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/database_holder_impl.h"
#include "mongo/db/local_catalog/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state_factory_shard.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state_factory_mock.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_shard_role.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/periodic_runner_factory.h"

#include <type_traits>

namespace mongo {

ServiceContext::UniqueServiceContext MongoDScopedGlobalServiceContextForTest::makeServiceContext(
    bool useMockClock,
    Milliseconds autoAdvancingMockClockIncrement,
    std::unique_ptr<TickSource> tickSource) {
    {
        // Reset the global clock source
        ClockSourceMock clkSource;
        clkSource.reset();
    }

    std::unique_ptr<ClockSource> fastClockSource, preciseClockSource;

    if (useMockClock) {
        // Copied from dbtests.cpp. DBTests sets up a controlled mock clock while
        // ScopedGlobalServiceContextMongoDForTest uses the system clock. Tests moved from dbtests
        // to unittests may depend on a deterministic clock. Additionally, if a test chooses to set
        // a non-zero _autoAdvancingMockClockIncrement, the mock clock will automatically advance by
        // that increment each time it is read.
        auto fastClock =
            std::make_unique<AutoAdvancingClockSourceMock>(autoAdvancingMockClockIncrement);
        // Timestamps are split into two 32-bit integers, seconds and "increments". Currently
        // (but maybe not for eternity), a Timestamp with a value of `0` seconds is always
        // considered "null" by `Timestamp::isNull`, regardless of its increment value. Ticking
        // the `ClockSourceMock` only bumps the "increment" counter, thus by default, generating
        // "null" timestamps. Bumping by one second here avoids any accidental interpretations.
        fastClock->advance(Seconds(1));
        fastClockSource = std::move(fastClock);

        auto preciseClock =
            std::make_unique<AutoAdvancingClockSourceMock>(autoAdvancingMockClockIncrement);
        // See above.
        preciseClock->advance(Seconds(1));
        preciseClockSource = std::move(preciseClock);
    }

    return ServiceContext::make(
        std::move(fastClockSource), std::move(preciseClockSource), std::move(tickSource));
}

MongoDScopedGlobalServiceContextForTest::MongoDScopedGlobalServiceContextForTest(Options options,
                                                                                 bool shouldSetupTL)
    : MongoDScopedGlobalServiceContextForTest(nullptr, std::move(options), shouldSetupTL) {}

MongoDScopedGlobalServiceContextForTest::MongoDScopedGlobalServiceContextForTest(
    ServiceContext::UniqueServiceContext serviceContextHolder, Options options, bool shouldSetupTL)
    : ScopedGlobalServiceContextForTest(
          serviceContextHolder ? std::move(serviceContextHolder)
                               : makeServiceContext(options._useMockClock,
                                                    options._autoAdvancingMockClockIncrement,
                                                    std::move(options._mockTickSource)),
          shouldSetupTL),
      _journalListener(std::move(options._journalListener)) {
    auto serviceContext = getServiceContext();

    for (auto& observer : options._clientObservers) {
        serviceContext->registerClientObserver(std::move(observer));
    }

    auto setupClient = serviceContext->getService()->makeClient("MongoDSCTestCtor");
    AlternativeClientRegion acr(setupClient);

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

    // TODO: SERVER-97042 move to service_context_test_fixture.
    repl::ReplicationCoordinator::set(
        serviceContext,
        std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext, repl::ReplSettings()));

    _stashedStorageParams.engine =
        std::exchange(storageGlobalParams.engine, std::move(options._engine));
    _stashedStorageParams.engineSetByUser =
        std::exchange(storageGlobalParams.engineSetByUser, true);
    _stashedStorageParams.repair =
        std::exchange(storageGlobalParams.repair, (options._repair == RepairAction::kRepair));

    serviceContext->getService()->setServiceEntryPoint(
        std::make_unique<ServiceEntryPointShardRole>());

    auto observerRegistry = std::make_unique<OpObserverRegistry>();
    _opObserverRegistry = observerRegistry.get();
    serviceContext->setOpObserver(std::move(observerRegistry));

    ReplicaSetDDLTracker::create(serviceContext);

    // Set up the periodic runner to allow background job execution for tests that require it.
    auto runner = makePeriodicRunner(serviceContext);
    serviceContext->setPeriodicRunner(std::move(runner));

    storageGlobalParams.dbpath = _tempDir.path();

    storageGlobalParams.inMemory = options._inMemory;

    // Since unit tests start in their own directories, by default skip lock file and metadata file
    // for faster startup.
    catalog::startUpStorageEngineAndCollectionCatalog(serviceContext, &cc(), options._initFlags);
    StorageControl::startStorageControls(serviceContext, true /*forTestOnly*/);

    DatabaseHolder::set(serviceContext, std::make_unique<DatabaseHolderImpl>());
    Collection::Factory::set(serviceContext, std::make_unique<CollectionImpl::FactoryImpl>());
    if (options._createShardingState) {
        ShardingState::create(serviceContext);
    }
    CollectionShardingStateFactory::set(
        serviceContext, std::make_unique<CollectionShardingStateFactoryShard>(serviceContext));
    DatabaseShardingStateFactory::set(serviceContext,
                                      std::make_unique<DatabaseShardingStateFactoryMock>());
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

MongoDScopedGlobalServiceContextForTest::~MongoDScopedGlobalServiceContextForTest() {
    auto teardownClient = getServiceContext()->getService()->makeClient("MongoDSCTestDtor");
    AlternativeClientRegion acr(teardownClient);
    auto opCtx = Client::getCurrent()->makeOperationContext();

    IndexBuildsCoordinator::get(opCtx.get())->shutdown(opCtx.get());
    CollectionShardingStateFactory::clear(getServiceContext());
    DatabaseShardingStateFactory::clear(getServiceContext());

    {
        Lock::GlobalLock glk(opCtx.get(), MODE_X);
        auto databaseHolder = DatabaseHolder::get(opCtx.get());
        databaseHolder->closeAll(opCtx.get());
    }

    catalog::shutDownCollectionCatalogAndGlobalStorageEngineCleanly(getServiceContext(),
                                                                    true /* memLeakAllowed */);

    std::swap(storageGlobalParams.engine, _stashedStorageParams.engine);
    std::swap(storageGlobalParams.engineSetByUser, _stashedStorageParams.engineSetByUser);
    std::swap(storageGlobalParams.repair, _stashedStorageParams.repair);

    storageGlobalParams.reset_forTest();
}

}  // namespace mongo
