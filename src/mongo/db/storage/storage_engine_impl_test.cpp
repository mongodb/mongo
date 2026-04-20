/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/storage/storage_engine_impl.h"

#include "mongo/db/rss/attached_storage/attached_persistence_provider.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/catalog_helper.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/periodic_runner_factory.h"

#include <algorithm>
#include <functional>

namespace mongo {
namespace {

/**
 * A test-only mock storage engine supporting timestamps.
 */
class TimestampMockKVEngine final : public DevNullKVEngine {
public:
    bool supportsRecoveryTimestamp() const override {
        return true;
    }

    // Increment the timestamps each time they are called for testing purposes.
    Timestamp getCheckpointTimestamp() const override {
        checkpointTimestamp = std::make_unique<Timestamp>(checkpointTimestamp->getSecs(),
                                                          checkpointTimestamp->getInc() + 1);
        return *checkpointTimestamp;
    }
    Timestamp getOldestTimestamp() const override {
        oldestTimestamp =
            std::make_unique<Timestamp>(oldestTimestamp->getSecs(), oldestTimestamp->getInc() + 1);
        return *oldestTimestamp;
    }
    Timestamp getStableTimestamp() const override {
        stableTimestamp =
            std::make_unique<Timestamp>(stableTimestamp->getSecs(), stableTimestamp->getInc() + 1);
        return *stableTimestamp;
    }

    // Mutable for testing purposes to increment the timestamp.
    mutable std::unique_ptr<Timestamp> checkpointTimestamp = std::make_unique<Timestamp>(1, 0);
    mutable std::unique_ptr<Timestamp> oldestTimestamp = std::make_unique<Timestamp>(1, 0);
    mutable std::unique_ptr<Timestamp> stableTimestamp = std::make_unique<Timestamp>(1, 0);
};

class TimestampKVEngineTest : public ServiceContextTest {
public:
    using TimestampListener = StorageEngine::TimestampMonitor::TimestampListener;
    using Timestamps = StorageEngine::TimestampMonitor::Timestamps;

    /**
     * Create an instance of the KV Storage Engine so that we have a timestamp monitor operating.
     */
    void setUp() override {
        ServiceContextTest::setUp();

        auto opCtx = makeOperationContext();

        auto runner = makePeriodicRunner(getServiceContext());
        getServiceContext()->setPeriodicRunner(std::move(runner));

        StorageEngineOptions options{/*directoryPerDB=*/false,
                                     /*directoryForIndexes=*/false,
                                     /*forRepair=*/false,
                                     /*lockFileCreatedByUncleanShutdown=*/false};
        _storageEngine =
            std::make_unique<StorageEngineImpl>(opCtx.get(),
                                                std::make_unique<TimestampMockKVEngine>(),
                                                std::unique_ptr<KVEngine>(),
                                                options);
        _storageEngine->startTimestampMonitor(
            {&catalog_helper::kCollectionCatalogCleanupTimestampListener});
    }

    void tearDown() override {
#if __has_feature(address_sanitizer)
        constexpr bool memLeakAllowed = false;
#else
        constexpr bool memLeakAllowed = true;
#endif
        _storageEngine->cleanShutdown(getServiceContext(), memLeakAllowed);
        _storageEngine.reset();

        ServiceContextTest::tearDown();
    }

    std::unique_ptr<StorageEngine> _storageEngine;
};

}  // namespace

TEST_F(TimestampKVEngineTest, TimestampMonitorRunning) {
    // The timestamp monitor should only be running if the storage engine supports timestamps.
    if (!_storageEngine->getEngine()->supportsRecoveryTimestamp())
        return;

    ASSERT_TRUE(_storageEngine->getTimestampMonitor()->isRunning_forTestOnly());
}

TEST_F(TimestampKVEngineTest, TimestampListeners) {
    TimestampListener first([](OperationContext* opCtx, auto) {});
    TimestampListener second([](OperationContext* opCtx, auto) {});
    TimestampListener third([](OperationContext* opCtx, auto) {});

    // Can only register the listener once.
    _storageEngine->getTimestampMonitor()->addListener(&first);

    _storageEngine->getTimestampMonitor()->removeListener(&first);
    _storageEngine->getTimestampMonitor()->addListener(&first);

    // Can register all three types of listeners.
    _storageEngine->getTimestampMonitor()->addListener(&second);
    _storageEngine->getTimestampMonitor()->addListener(&third);

    _storageEngine->getTimestampMonitor()->removeListener(&first);
    _storageEngine->getTimestampMonitor()->removeListener(&second);
    _storageEngine->getTimestampMonitor()->removeListener(&third);
}

TEST_F(TimestampKVEngineTest, TimestampMonitorNotifiesListeners) {
    stdx::mutex mutex;
    stdx::condition_variable cv;

    bool changes[] = {false, false, false};

    TimestampListener first([&](OperationContext* opCtx, auto& timestamps) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        if (!timestamps.checkpoint.isNull() && !changes[0]) {
            changes[0] = true;
            cv.notify_all();
        }
    });

    TimestampListener second([&](OperationContext* opCtx, auto& timestamps) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        if (!timestamps.oldest.isNull() && !changes[1]) {
            changes[1] = true;
            cv.notify_all();
        }
    });

    TimestampListener third([&](OperationContext* opCtx, auto& timestamps) {
        stdx::lock_guard<stdx::mutex> lock(mutex);
        if (!timestamps.stable.isNull() && !changes[2]) {
            changes[2] = true;
            cv.notify_all();
        }
    });

    _storageEngine->getTimestampMonitor()->addListener(&first);
    _storageEngine->getTimestampMonitor()->addListener(&second);
    _storageEngine->getTimestampMonitor()->addListener(&third);

    // Wait until all 3 listeners get notified at least once.
    {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        cv.wait(lk, [&] {
            return std::all_of(std::begin(changes), std::end(changes), std::identity());
        });
    };


    _storageEngine->getTimestampMonitor()->removeListener(&first);
    _storageEngine->getTimestampMonitor()->removeListener(&second);
    _storageEngine->getTimestampMonitor()->removeListener(&third);
}

TEST_F(TimestampKVEngineTest, TimestampAdvancesOnNotification) {
    Timestamp previous = Timestamp();
    AtomicWord<int> timesNotified{0};

    TimestampListener listener([&](OperationContext* opCtx, const Timestamps& timestamps) {
        ASSERT_TRUE(previous < timestamps.stable);
        previous = timestamps.stable;
        timesNotified.fetchAndAdd(1);
    });
    _storageEngine->getTimestampMonitor()->addListener(&listener);

    // Let three rounds of notifications happen while ensuring that each new notification produces
    // an increasing timestamp.
    while (timesNotified.load() < 3) {
        sleepmillis(100);
    }

    _storageEngine->getTimestampMonitor()->removeListener(&listener);
}

namespace {
class MockKVEngine : public DevNullKVEngine {
public:
    MOCK_METHOD(Status,
                dropIdent,
                (RecoveryUnit & ru,
                 StringData ident,
                 bool identHasSizeInfo,
                 const StorageEngine::DropIdentCallback& onDrop,
                 boost::optional<uint64_t> schemaEpoch),
                (override));
};

class MockPersistenceProvider : public rss::AttachedPersistenceProvider {
public:
    MOCK_METHOD(uint64_t, getSchemaEpochForTimestamp, (Timestamp ts), (const, override));
};

class StorageEngineImplTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();

        auto provider = std::make_unique<testing::NiceMock<MockPersistenceProvider>>();
        _mockProvider = provider.get();
        rss::ReplicatedStorageService::get(getServiceContext())
            .setPersistenceProvider(std::move(provider));

        auto opCtx = makeOperationContext();

        auto runner = makePeriodicRunner(getServiceContext());
        getServiceContext()->setPeriodicRunner(std::move(runner));

        StorageEngineOptions options{/*directoryPerDB=*/false,
                                     /*directoryForIndexes=*/false,
                                     /*forRepair=*/false,
                                     /*lockFileCreatedByUncleanShutdown=*/false};
        _storageEngine = std::make_unique<StorageEngineImpl>(
            opCtx.get(), std::make_unique<MockKVEngine>(), std::unique_ptr<KVEngine>(), options);
        _mockKVEngine = static_cast<MockKVEngine*>(_storageEngine->getEngine());
    }

    void tearDown() override {
        ServiceContextTest::tearDown();
    }

    std::unique_ptr<StorageEngine> _storageEngine;
    MockKVEngine* _mockKVEngine;
    MockPersistenceProvider* _mockProvider = nullptr;
};
}  // namespace

TEST_F(StorageEngineImplTest, DropIdentTimestampedPassesTimestampToKVEngine) {
    auto uniqueOpCtx = makeOperationContext();
    auto opCtx = uniqueOpCtx.get();
    std::string ident{"my-ident"};
    Timestamp dropIdentTs(10, 20);
    Timestamp dropCollectionTs(5, 0);

    // Assert that the replicated ident drop schema epoch is passed to the KVEngine.
    const uint64_t expectedSchemaEpoch = 42;
    ON_CALL(*_mockProvider, getSchemaEpochForTimestamp(dropIdentTs))
        .WillByDefault(testing::Return(expectedSchemaEpoch));

    _storageEngine->addDropPendingIdent(StorageEngine::OldestTimestamp{dropCollectionTs},
                                        std::make_shared<Ident>(ident));

    EXPECT_CALL(*_mockKVEngine, dropIdent)
        .WillOnce([&](RecoveryUnit& calledRu,
                      StringData calledIdent,
                      bool identHasSizeInfo,
                      const StorageEngine::DropIdentCallback& onDrop,
                      boost::optional<uint64_t> schemaEpoch) {
            ASSERT_EQ(calledIdent, StringData{ident});
            ASSERT_EQ(identHasSizeInfo, ident::isCollectionIdent(calledIdent));
            ASSERT_FALSE(static_cast<bool>(onDrop));
            ASSERT_EQ(schemaEpoch, expectedSchemaEpoch);
            return Status::OK();
        });

    ASSERT_DOES_NOT_THROW(_storageEngine->dropIdentTimestamped(opCtx, ident, dropIdentTs));

    // Assert that if dropIdentTimestamped returns a failure, it is propagated.
    _storageEngine->addDropPendingIdent(StorageEngine::OldestTimestamp{dropCollectionTs},
                                        std::make_shared<Ident>(ident));
    EXPECT_CALL(*_mockKVEngine, dropIdent)
        .WillOnce([&](RecoveryUnit& calledRu,
                      StringData calledIdent,
                      bool identHasSizeInfo,
                      const StorageEngine::DropIdentCallback& onDrop,
                      boost::optional<uint64_t> schemaEpoch) {
            ASSERT_EQ(calledIdent, StringData{ident});
            ASSERT_EQ(identHasSizeInfo, ident::isCollectionIdent(calledIdent));
            ASSERT_FALSE(static_cast<bool>(onDrop));
            ASSERT_EQ(schemaEpoch, expectedSchemaEpoch);
            return Status(ErrorCodes::OperationFailed, "Mock KV engine dropIdent failed.");
        });

    ASSERT_THROWS_CODE(_storageEngine->dropIdentTimestamped(opCtx, ident, dropIdentTs),
                       DBException,
                       ErrorCodes::OperationFailed);
}

}  // namespace mongo
