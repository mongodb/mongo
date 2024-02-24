/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/unittest/assert.h"
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/database_holder_impl.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/index_builds_coordinator_mongod.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_batcher.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_buffer_blocking_queue.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/collection_sharding_state_factory_shard.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_mongod.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session_manager_mongod.h"
#include "mongo/db/storage/execution_control/concurrency_adjustment_parameters_gen.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/s/sharding_state.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

namespace mongo {
namespace {

constexpr std::size_t kOplogBufferSize = 256 * 1024 * 1024;
class TestServiceContext {
public:
    TestServiceContext(int numThreads) {
        // Disable execution control.
        gStorageEngineConcurrencyAdjustmentAlgorithm = "fixedConcurrentTransactions";

        // Disable server info logging so that the benchmark output is cleaner.
        logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            mongo::logv2::LogComponent::kDefault, mongo::logv2::LogSeverity::Error());

        // (Generic FCV reference): Test latest FCV behavior. This FCV reference should exist across
        // LTS binary versions.
        serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);

        if (haveClient()) {
            Client::releaseCurrent();
        }
        setGlobalServiceContext(ServiceContext::make());
        _svcCtx = getGlobalServiceContext();

        _svcCtx->getService()->setServiceEntryPoint(std::make_unique<ServiceEntryPointMongod>());

        auto fastClock = std::make_unique<ClockSourceMock>();
        // Timestamps are split into two 32-bit integers, seconds and "increments". Currently (but
        // maybe not for eternity), a Timestamp with a value of `0` seconds is always considered
        // "null" by `Timestamp::isNull`, regardless of its increment value. Ticking the
        // `ClockSourceMock` only bumps the "increment" counter, thus by default, generating "null"
        // timestamps. Bumping by one second here avoids any accidental interpretations.
        fastClock->advance(Seconds(1));
        _svcCtx->setFastClockSource(std::move(fastClock));

        auto preciseClock = std::make_unique<ClockSourceMock>();
        // See above.
        preciseClock->advance(Seconds(1));
        CursorManager::get(_svcCtx)->setPreciseClockSource(preciseClock.get());
        _svcCtx->setPreciseClockSource(std::move(preciseClock));

        auto runner = makePeriodicRunner(_svcCtx);
        _svcCtx->setPeriodicRunner(std::move(runner));

        Collection::Factory::set(_svcCtx, std::make_unique<CollectionImpl::FactoryImpl>());
        storageGlobalParams.engine = "wiredTiger";
        storageGlobalParams.engineSetByUser = true;

        _tempDir.emplace("oplog_write_bm_data");
        storageGlobalParams.dbpath = _tempDir->path();
        storageGlobalParams.ephemeral = false;

        Client::initThread("oplog write main", getGlobalServiceContext()->getService());
        _client = Client::getCurrent();

        repl::ReplSettings replSettings;
        replSettings.setOplogSizeBytes(1024 * 1024 * 1024);
        replSettings.setReplSetString("oplog write benchmark replset");
        setGlobalReplSettings(replSettings);
        _replCoord = new repl::ReplicationCoordinatorMock(_svcCtx, replSettings);
        repl::ReplicationCoordinator::set(
            _svcCtx, std::unique_ptr<repl::ReplicationCoordinator>(_replCoord));

        // Disable fast shutdown so that WT can free memory.
        globalFailPointRegistry().find("WTDisableFastShutDown")->setMode(FailPoint::alwaysOn);

        auto startupOpCtx = _svcCtx->makeOperationContext(&cc());
        initializeStorageEngine(startupOpCtx.get(),
                                StorageEngineInitFlags::kAllowNoLockFile |
                                    StorageEngineInitFlags::kSkipMetadataFile);
        DatabaseHolder::set(_svcCtx, std::make_unique<DatabaseHolderImpl>());
        repl::StorageInterface::set(_svcCtx, std::make_unique<repl::StorageInterfaceImpl>());
        _storageInterface = repl::StorageInterface::get(_svcCtx);

        repl::DropPendingCollectionReaper::set(
            _svcCtx, std::make_unique<repl::DropPendingCollectionReaper>(_storageInterface));
        IndexBuildsCoordinator::set(_svcCtx, std::make_unique<IndexBuildsCoordinatorMongod>());

        auto registry = std::make_unique<OpObserverRegistry>();
        registry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
        _svcCtx->setOpObserver(std::move(registry));
        ShardingState::create(_svcCtx);
        CollectionShardingStateFactory::set(
            _svcCtx, std::make_unique<CollectionShardingStateFactoryShard>(_svcCtx));

        MongoDSessionCatalog::set(
            _svcCtx,
            std::make_unique<MongoDSessionCatalog>(
                std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

        _oplogBuffer = std::make_unique<repl::OplogBufferBlockingQueue>(kOplogBufferSize);

        repl::replWriterThreadCount = numThreads;  // Repl worker thread count
        repl::replWriterMinThreadCount = numThreads;

        _oplogApplierThreadPool = repl::makeReplWriterPool();

        repl::OplogApplier::Options oplogApplierOptions(
            repl::OplogApplication::Mode::kSecondary,
            false /*allowNamespaceNotFoundErrorsOnCrudOps*/,
            false /*skipWritesToOplog*/);  // Write oplog
        _oplogApplier = std::make_unique<repl::OplogApplierImpl>(nullptr,
                                                                 _oplogBuffer.get(),
                                                                 &repl::noopOplogApplierObserver,
                                                                 _replCoord,
                                                                 &_consistencyMarkers,
                                                                 _storageInterface,
                                                                 oplogApplierOptions,
                                                                 _oplogApplierThreadPool.get());

        _svcCtx->notifyStorageStartupRecoveryComplete();
    }

    ~TestServiceContext() {
        shutDownStorageEngine();
        if (haveClient()) {
            Client::releaseCurrent();
        }
    }

    void shutDownStorageEngine() {
        ServiceContext::UniqueOperationContext uniqueOpCtx;
        auto opCtx = getClient()->getOperationContext();
        if (!opCtx) {
            uniqueOpCtx = getClient()->makeOperationContext();
            opCtx = uniqueOpCtx.get();
        }

        Lock::GlobalLock lk(opCtx, LockMode::MODE_X);

        SessionCatalog::get(_svcCtx)->reset_forTest();

        auto databaseHolder = DatabaseHolder::get(opCtx);
        databaseHolder->closeAll(opCtx);

        // Shut down storage engine.
        shutdownGlobalStorageEngineCleanly(_svcCtx);
    }

    // Shut down the storage engine, clear the dbpath, and restart the storage engine with empty
    // dbpath.
    void resetStorageEngine() {
        shutDownStorageEngine();

        // Clear dbpath.
        _tempDir.reset();

        // Restart storage engine.
        _tempDir.emplace("oplog_write_bm_data");
        storageGlobalParams.dbpath = _tempDir->path();
        storageGlobalParams.ephemeral = false;

        auto uniqueOpCtx = _svcCtx->makeOperationContext(&cc());
        shard_role_details::setRecoveryUnit(uniqueOpCtx.get(),
                                            std::make_unique<RecoveryUnitNoop>(),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

        initializeStorageEngine(uniqueOpCtx.get(),
                                StorageEngineInitFlags::kAllowNoLockFile |
                                    StorageEngineInitFlags::kSkipMetadataFile |
                                    StorageEngineInitFlags::kForRestart);
    }

    ServiceContext* getSvcCtx() {
        return _svcCtx;
    }

    Client* getClient() {
        return _client;
    }

    repl::ReplicationCoordinatorMock* getReplCoordMock() {
        return _replCoord;
    }

    repl::OplogApplier* getOplogApplier() {
        return _oplogApplier.get();
    }

    ThreadPool* getThreadPool() {
        return _oplogApplierThreadPool.get();
    }

    repl::StorageInterface* getStorageInterface() {
        return _storageInterface;
    }

private:
    ServiceContext* _svcCtx;
    Client* _client;
    repl::ReplicationCoordinatorMock* _replCoord;
    std::unique_ptr<repl::OplogApplier> _oplogApplier;
    repl::StorageInterface* _storageInterface;

    // This class also owns objects necessary for `_oplogApplier`.
    std::unique_ptr<repl::OplogBufferBlockingQueue> _oplogBuffer;
    repl::ReplicationConsistencyMarkersMock _consistencyMarkers;
    std::unique_ptr<ThreadPool> _oplogApplierThreadPool;
    boost::optional<unittest::TempDir> _tempDir;
};

BSONObj makeDoc(int idx, int size) {
    std::string data;
    data.resize(size);
    return BSON("_id" << OID::gen() << "value" << idx << "msg" << data);
}

class Fixture {
public:
    Fixture(TestServiceContext* testSvcCtx) : _testSvcCtx(testSvcCtx), _foobarUUID(UUID::gen()) {}

    void createBatch(int totalOps, int entrySize) {
        const long long term1 = 1;
        for (int idx = 0; idx < totalOps; ++idx) {
            auto x1 =
                BSON("op"
                     << "i"
                     << "ns"
                     << "foo.bar"
                     << "ui" << _foobarUUID << "o" << makeDoc(idx, entrySize) << "ts"
                     << Timestamp(1, idx) << "t" << term1 << "v" << 2 << "wall" << Date_t::now());
            // Size of the BSON obj will be 146 + entrySize bytes
            _oplogEntries.emplace_back(x1);
        }
    }

    void reset() {
        // Restart with an empty storage.
        _testSvcCtx->resetStorageEngine();

        _testSvcCtx->getReplCoordMock()->setFollowerMode(repl::MemberState::RS_PRIMARY).ignore();
        {
            auto opCtxRaii =
                _testSvcCtx->getSvcCtx()->makeOperationContext(_testSvcCtx->getClient());
            auto opCtx = opCtxRaii.get();
            repl::UnreplicatedWritesBlock noRep(opCtx);

            const bool allowRename = false;
            Lock::GlobalLock lk(opCtx, LockMode::MODE_X);
            auto storageInterface = repl::StorageInterface::get(opCtx);

            // Create collection 'foo.bar' with one secondary index `value_1` on an integer field.
            uassertStatusOK(createCollectionForApplyOps(opCtxRaii.get(),
                                                        _foobarNs.dbName(),
                                                        _foobarUUID,
                                                        BSON("create" << _foobarNs.coll()),
                                                        allowRename));
            uassertStatusOK(storageInterface->createIndexesOnEmptyCollection(
                opCtx,
                _foobarNs,
                {BSON("v" << 2 << "name"
                          << "value_1"
                          << "key" << BSON("value" << 1))}));

            // Create 'config.transactions' for transactions.
            uassertStatusOK(storageInterface->createCollection(
                opCtx, NamespaceString::kSessionTransactionsTableNamespace, CollectionOptions()));
            uassertStatusOK(storageInterface->createIndexesOnEmptyCollection(
                opCtx,
                NamespaceString::kSessionTransactionsTableNamespace,
                {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()}));
        }
    }

    void enqueueOplog(OperationContext* opCtx) {
        _testSvcCtx->getOplogApplier()->enqueue(opCtx, _oplogEntries.begin(), _oplogEntries.end());
    }

    void writeOplog(OperationContext* opCtx, size_t numEntriesPerBatch, size_t numBytesPerBatch) {
        while (!_testSvcCtx->getOplogApplier()->getBuffer()->isEmpty()) {
            auto oplogBatch = invariantStatusOK(_testSvcCtx->getOplogApplier()->getNextApplierBatch(
                                                    opCtx, {numBytesPerBatch, numEntriesPerBatch}))
                                  .releaseBatch();

            AutoGetDb autoDb(opCtx, _foobarNs.dbName(), MODE_X);

            _testSvcCtx->getOplogApplier()->scheduleWritesToOplogAndChangeCollection(
                opCtx,
                _testSvcCtx->getStorageInterface(),
                _testSvcCtx->getThreadPool(),
                std::move(oplogBatch),
                false);

            // Wait for writes to finish
            _testSvcCtx->getThreadPool()->waitForIdle();
        }
    }

private:
    TestServiceContext* _testSvcCtx;

    std::vector<BSONObj> _oplogEntries;
    UUID _foobarUUID;
    NamespaceString _foobarNs = NamespaceString::createNamespaceString_forTest("foo.bar"_sd);
};

void runBMTest(TestServiceContext& testSvcCtx, Fixture& fixture, benchmark::State& state) {
    for (auto _ : state) {
        fixture.reset();

        auto opCtxRaii = testSvcCtx.getSvcCtx()->makeOperationContext(testSvcCtx.getClient());
        auto opCtx = opCtxRaii.get();
        repl::createOplog(opCtx);
        fixture.enqueueOplog(opCtx);
        auto start = mongo::stdx::chrono::high_resolution_clock::now();
        fixture.writeOplog(opCtx, state.range(3), state.range(4));
        auto end = mongo::stdx::chrono::high_resolution_clock::now();
        auto elapsed_seconds =
            mongo::stdx::chrono::duration_cast<mongo::stdx::chrono::duration<double>>(end - start);
        state.SetIterationTime(elapsed_seconds.count());
    }
}

void BM_TestWriteOps(benchmark::State& state) {
    TestServiceContext testSvcCtx(state.range(0));
    Fixture fixture(&testSvcCtx);
    fixture.createBatch(state.range(1), state.range(2));
    runBMTest(testSvcCtx, fixture, state);
}

// Write oplog
// Input: Thread Count | totalOps | entrySize | numEntriesPerBatch | numBytesPerBatch
// the entry header over head is 146 bytes, fill the queue upto 240MB.

// Since Evergreen testing taking more than 2+ hours for Full test list, commenting out it for now.
// Only run Minimal test list.

// Full test list
#if 0
// Single Thread and various Batch and Entry sizes

// numEntriesPerBatch
// Since there is 146B overhead per entry, Reduce the number of entries to fit into 240MB
// queue 1KB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, (245760 - (32 * 1024)), 1024, 10, std::numeric_limits<long>::max()})
    ->Args({1, (245760 - (32 * 1024)), 1024, 100, std::numeric_limits<long>::max()})
    ->Args({1, (245760 - (32 * 1024)), 1024, 500, std::numeric_limits<long>::max()})
    ->Args({1, (245760 - (32 * 1024)), 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({1, (245760 - (32 * 1024)), 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({1, (245760 - (32 * 1024)), 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({1, (245760 - (32 * 1024)), 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({1, (245760 - (32 * 1024)), 1024, 100000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 4KB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, (61440 - 2048), 4 * 1024, 10, std::numeric_limits<long>::max()})
    ->Args({1, (61440 - 2048), 4 * 1024, 100, std::numeric_limits<long>::max()})
    ->Args({1, (61440 - 2048), 4 * 1024, 500, std::numeric_limits<long>::max()})
    ->Args({1, (61440 - 2048), 4 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({1, (61440 - 2048), 4 * 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({1, (61440 - 2048), 4 * 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({1, (61440 - 2048), 4 * 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({1, (61440 - 2048), 4 * 1024, 100000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 16KB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, (15360 - 128), 16 * 1024, 10, std::numeric_limits<long>::max()})
    ->Args({1, (15360 - 128), 16 * 1024, 100, std::numeric_limits<long>::max()})
    ->Args({1, (15360 - 128), 16 * 1024, 500, std::numeric_limits<long>::max()})
    ->Args({1, (15360 - 128), 16 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({1, (15360 - 128), 16 * 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({1, (15360 - 128), 16 * 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({1, (15360 - 128), 16 * 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({1, (15360 - 128), 16 * 1024, 100000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 64KB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, (3840 - 8), 64 * 1024, 10, std::numeric_limits<long>::max()})
    ->Args({1, (3840 - 8), 64 * 1024, 100, std::numeric_limits<long>::max()})
    ->Args({1, (3840 - 8), 64 * 1024, 500, std::numeric_limits<long>::max()})
    ->Args({1, (3840 - 8), 64 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({1, (3840 - 8), 64 * 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({1, (3840 - 8), 64 * 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({1, (3840 - 8), 64 * 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({1, (3840 - 8), 64 * 1024, 100000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 256KB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 960, 256 * 1024, 10, std::numeric_limits<long>::max()})
    ->Args({1, 960, 256 * 1024, 100, std::numeric_limits<long>::max()})
    ->Args({1, 960, 256 * 1024, 500, std::numeric_limits<long>::max()})
    ->Args({1, 960, 256 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({1, 960, 256 * 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({1, 960, 256 * 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({1, 960, 256 * 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({1, 960, 256 * 1024, 100000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 1MB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 240, 1024 * 1024, 10, std::numeric_limits<long>::max()})
    ->Args({1, 240, 1024 * 1024, 100, std::numeric_limits<long>::max()})
    ->Args({1, 240, 1024 * 1024, 500, std::numeric_limits<long>::max()})
    ->Args({1, 240, 1024 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({1, 240, 1024 * 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({1, 240, 1024 * 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({1, 240, 1024 * 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({1, 240, 1024 * 1024, 100000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 2MB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 120, 2 * 1024 * 1024, 10, std::numeric_limits<long>::max()})
    ->Args({1, 120, 2 * 1024 * 1024, 100, std::numeric_limits<long>::max()})
    ->Args({1, 120, 2 * 1024 * 1024, 500, std::numeric_limits<long>::max()})
    ->Args({1, 120, 2 * 1024 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({1, 120, 2 * 1024 * 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({1, 120, 2 * 1024 * 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({1, 120, 2 * 1024 * 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({1, 120, 2 * 1024 * 1024, 100000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 5MB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 48, 5 * 1024 * 1024, 10, std::numeric_limits<long>::max()})
    ->Args({1, 48, 5 * 1024 * 1024, 100, std::numeric_limits<long>::max()})
    ->Args({1, 48, 5 * 1024 * 1024, 500, std::numeric_limits<long>::max()})
    ->Args({1, 48, 5 * 1024 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({1, 48, 5 * 1024 * 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({1, 48, 5 * 1024 * 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({1, 48, 5 * 1024 * 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({1, 48, 5 * 1024 * 1024, 100000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 10MB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 24, 10 * 1024 * 1024, 10, std::numeric_limits<long>::max()})
    ->Args({1, 24, 10 * 1024 * 1024, 100, std::numeric_limits<long>::max()})
    ->Args({1, 24, 10 * 1024 * 1024, 500, std::numeric_limits<long>::max()})
    ->Args({1, 24, 10 * 1024 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({1, 24, 10 * 1024 * 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({1, 24, 10 * 1024 * 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({1, 24, 10 * 1024 * 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({1, 24, 10 * 1024 * 1024, 100000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 16MB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 15, 16 * 1024 * 1024, 10, std::numeric_limits<long>::max()})
    ->Args({1, 15, 16 * 1024 * 1024, 100, std::numeric_limits<long>::max()})
    ->Args({1, 15, 16 * 1024 * 1024, 500, std::numeric_limits<long>::max()})
    ->Args({1, 15, 16 * 1024 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({1, 15, 16 * 1024 * 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({1, 15, 16 * 1024 * 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({1, 15, 16 * 1024 * 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({1, 15, 16 * 1024 * 1024, 100000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// numBytesPerBatch

// 1KB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, (245760 - (32 * 1024)), 1024, std::numeric_limits<long>::max(), 1024 * 1024})
    ->Args({1, (245760 - (32 * 1024)), 1024, std::numeric_limits<long>::max(), 2 * 1024 * 1024})
    ->Args({1, (245760 - (32 * 1024)), 1024, std::numeric_limits<long>::max(), 5 * 1024 * 1024})
    ->Args({1, (245760 - (32 * 1024)), 1024, std::numeric_limits<long>::max(), 10 * 1024 * 1024})
    ->Args({1, (245760 - (32 * 1024)), 1024, std::numeric_limits<long>::max(), 16 * 1024 * 1024})
    ->Args({1, (245760 - (32 * 1024)), 1024, std::numeric_limits<long>::max(), 32 * 1024 * 1024})
    ->Args({1, (245760 - (32 * 1024)), 1024, std::numeric_limits<long>::max(), 64 * 1024 * 1024})
    ->Args({1, (245760 - (32 * 1024)), 1024, std::numeric_limits<long>::max(), 128 * 1024 * 1024})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 4KB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, (61440 - 2048), 4 * 1024, std::numeric_limits<long>::max(), 1024 * 1024})
    ->Args({1, (61440 - 2048), 4 * 1024, std::numeric_limits<long>::max(), 2 * 1024 * 1024})
    ->Args({1, (61440 - 2048), 4 * 1024, std::numeric_limits<long>::max(), 5 * 1024 * 1024})
    ->Args({1, (61440 - 2048), 4 * 1024, std::numeric_limits<long>::max(), 10 * 1024 * 1024})
    ->Args({1, (61440 - 2048), 4 * 1024, std::numeric_limits<long>::max(), 16 * 1024 * 1024})
    ->Args({1, (61440 - 2048), 4 * 1024, std::numeric_limits<long>::max(), 32 * 1024 * 1024})
    ->Args({1, (61440 - 2048), 4 * 1024, std::numeric_limits<long>::max(), 64 * 1024 * 1024})
    ->Args({1, (61440 - 2048), 4 * 1024, std::numeric_limits<long>::max(), 128 * 1024 * 1024})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 64KB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 1024 * 1024})
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 2 * 1024 * 1024})
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 5 * 1024 * 1024})
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 10 * 1024 * 1024})
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 16 * 1024 * 1024})
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 32 * 1024 * 1024})
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 64 * 1024 * 1024})
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 128 * 1024 * 1024})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 512KB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 480, 512 * 1024, std::numeric_limits<long>::max(), 1024 * 1024})
    ->Args({1, 480, 512 * 1024, std::numeric_limits<long>::max(), 2 * 1024 * 1024})
    ->Args({1, 480, 512 * 1024, std::numeric_limits<long>::max(), 5 * 1024 * 1024})
    ->Args({1, 480, 512 * 1024, std::numeric_limits<long>::max(), 10 * 1024 * 1024})
    ->Args({1, 480, 512 * 1024, std::numeric_limits<long>::max(), 16 * 1024 * 1024})
    ->Args({1, 480, 512 * 1024, std::numeric_limits<long>::max(), 32 * 1024 * 1024})
    ->Args({1, 480, 512 * 1024, std::numeric_limits<long>::max(), 64 * 1024 * 1024})
    ->Args({1, 480, 512 * 1024, std::numeric_limits<long>::max(), 128 * 1024 * 1024})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 1MB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 240, 1024 * 1024, std::numeric_limits<long>::max(), 1024 * 1024})
    ->Args({1, 240, 1024 * 1024, std::numeric_limits<long>::max(), 2 * 1024 * 1024})
    ->Args({1, 240, 1024 * 1024, std::numeric_limits<long>::max(), 5 * 1024 * 1024})
    ->Args({1, 240, 1024 * 1024, std::numeric_limits<long>::max(), 10 * 1024 * 1024})
    ->Args({1, 240, 1024 * 1024, std::numeric_limits<long>::max(), 16 * 1024 * 1024})
    ->Args({1, 240, 1024 * 1024, std::numeric_limits<long>::max(), 32 * 1024 * 1024})
    ->Args({1, 240, 1024 * 1024, std::numeric_limits<long>::max(), 64 * 1024 * 1024})
    ->Args({1, 240, 1024 * 1024, std::numeric_limits<long>::max(), 128 * 1024 * 1024})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 2MB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 120, 2 * 1024 * 1024, std::numeric_limits<long>::max(), 1024 * 1024})
    ->Args({1, 120, 2 * 1024 * 1024, std::numeric_limits<long>::max(), 2 * 1024 * 1024})
    ->Args({1, 120, 2 * 1024 * 1024, std::numeric_limits<long>::max(), 5 * 1024 * 1024})
    ->Args({1, 120, 2 * 1024 * 1024, std::numeric_limits<long>::max(), 10 * 1024 * 1024})
    ->Args({1, 120, 2 * 1024 * 1024, std::numeric_limits<long>::max(), 16 * 1024 * 1024})
    ->Args({1, 120, 2 * 1024 * 1024, std::numeric_limits<long>::max(), 32 * 1024 * 1024})
    ->Args({1, 120, 2 * 1024 * 1024, std::numeric_limits<long>::max(), 64 * 1024 * 1024})
    ->Args({1, 120, 2 * 1024 * 1024, std::numeric_limits<long>::max(), 128 * 1024 * 1024})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 5MB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 48, 5 * 1024 * 1024, std::numeric_limits<long>::max(), 1024 * 1024})
    ->Args({1, 48, 5 * 1024 * 1024, std::numeric_limits<long>::max(), 2 * 1024 * 1024})
    ->Args({1, 48, 5 * 1024 * 1024, std::numeric_limits<long>::max(), 5 * 1024 * 1024})
    ->Args({1, 48, 5 * 1024 * 1024, std::numeric_limits<long>::max(), 10 * 1024 * 1024})
    ->Args({1, 48, 5 * 1024 * 1024, std::numeric_limits<long>::max(), 16 * 1024 * 1024})
    ->Args({1, 48, 5 * 1024 * 1024, std::numeric_limits<long>::max(), 32 * 1024 * 1024})
    ->Args({1, 48, 5 * 1024 * 1024, std::numeric_limits<long>::max(), 64 * 1024 * 1024})
    ->Args({1, 48, 5 * 1024 * 1024, std::numeric_limits<long>::max(), 128 * 1024 * 1024})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 10MB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 24, 10 * 1024 * 1024, std::numeric_limits<long>::max(), 1024 * 1024})
    ->Args({1, 24, 10 * 1024 * 1024, std::numeric_limits<long>::max(), 2 * 1024 * 1024})
    ->Args({1, 24, 10 * 1024 * 1024, std::numeric_limits<long>::max(), 5 * 1024 * 1024})
    ->Args({1, 24, 10 * 1024 * 1024, std::numeric_limits<long>::max(), 10 * 1024 * 1024})
    ->Args({1, 24, 10 * 1024 * 1024, std::numeric_limits<long>::max(), 16 * 1024 * 1024})
    ->Args({1, 24, 10 * 1024 * 1024, std::numeric_limits<long>::max(), 32 * 1024 * 1024})
    ->Args({1, 24, 10 * 1024 * 1024, std::numeric_limits<long>::max(), 64 * 1024 * 1024})
    ->Args({1, 24, 10 * 1024 * 1024, std::numeric_limits<long>::max(), 128 * 1024 * 1024})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// 16MB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 15, 16 * 1024 * 1024, std::numeric_limits<long>::max(), 1024 * 1024})
    ->Args({1, 15, 16 * 1024 * 1024, std::numeric_limits<long>::max(), 2 * 1024 * 1024})
    ->Args({1, 15, 16 * 1024 * 1024, std::numeric_limits<long>::max(), 5 * 1024 * 1024})
    ->Args({1, 15, 16 * 1024 * 1024, std::numeric_limits<long>::max(), 10 * 1024 * 1024})
    ->Args({1, 15, 16 * 1024 * 1024, std::numeric_limits<long>::max(), 16 * 1024 * 1024})
    ->Args({1, 15, 16 * 1024 * 1024, std::numeric_limits<long>::max(), 32 * 1024 * 1024})
    ->Args({1, 15, 16 * 1024 * 1024, std::numeric_limits<long>::max(), 64 * 1024 * 1024})
    ->Args({1, 15, 16 * 1024 * 1024, std::numeric_limits<long>::max(), 128 * 1024 * 1024})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// Various Thread count and Batch sizes
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({2, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({4, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({5, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({8, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({10, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({12, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({15, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({20, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({25, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_TestWriteOps)
    ->Args({1, 200000, 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({2, 200000, 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({4, 200000, 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({5, 200000, 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({8, 200000, 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({10, 200000, 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({12, 200000, 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({15, 200000, 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({20, 200000, 1024, 2000, std::numeric_limits<long>::max()})
    ->Args({25, 200000, 1024, 2000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_TestWriteOps)
    ->Args({1, 200000, 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({2, 200000, 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({4, 200000, 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({5, 200000, 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({8, 200000, 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({10, 200000, 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({12, 200000, 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({15, 200000, 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({20, 200000, 1024, 5000, std::numeric_limits<long>::max()})
    ->Args({25, 200000, 1024, 5000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_TestWriteOps)
    ->Args({1, 200000, 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({2, 200000, 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({4, 200000, 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({5, 200000, 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({8, 200000, 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({10, 200000, 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({12, 200000, 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({15, 200000, 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({20, 200000, 1024, 10000, std::numeric_limits<long>::max()})
    ->Args({25, 200000, 1024, 10000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// Various Thread count and entrySize 100KB and numEntriesPerBatch 1000
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 2400, 100 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({2, 2400, 100 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({4, 2400, 100 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({5, 2400, 100 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({8, 2400, 100 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({10, 2400, 100 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({12, 2400, 100 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({15, 2400, 100 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({20, 2400, 100 * 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({25, 2400, 100 * 1024, 1000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

#endif

// Minimal test list

// the entry header over head is 146 bytes, fill the queue upto 240MB.
// numBytesPerBatch
// 64KB entrySize
BENCHMARK(BM_TestWriteOps)
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 1024 * 1024})
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 2 * 1024 * 1024})
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 5 * 1024 * 1024})
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 10 * 1024 * 1024})
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 16 * 1024 * 1024})
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 32 * 1024 * 1024})
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 64 * 1024 * 1024})
    ->Args({1, (3840 - 8), 64 * 1024, std::numeric_limits<long>::max(), 128 * 1024 * 1024})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

// Various Thread count and Batch sizes
BENCHMARK(BM_TestWriteOps)
    ->Args({1, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({2, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({5, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({10, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({15, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->Args({20, 200000, 1024, 1000, std::numeric_limits<long>::max()})
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace mongo
