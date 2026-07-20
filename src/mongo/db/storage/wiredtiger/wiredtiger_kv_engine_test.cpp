// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/rss/attached_storage/attached_persistence_provider.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/rss/stub_persistence_provider.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/checkpoint_schedule_policy.h"
#include "mongo/db/storage/checkpointer.h"
#include "mongo/db/storage/flush_all_files_observer.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine_test_harness.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/storage_engine_impl.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/version/releases.h"

#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <ostream>
#include <string_view>
#include <utility>

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

bool allowUntimestampedWrites(bool inStandaloneMode, bool shouldRecoverFromOplogAsStandalone);

namespace {

#if __has_feature(address_sanitizer)
constexpr bool kMemLeakAllowed = false;
#else
constexpr bool kMemLeakAllowed = true;
#endif

class WiredTigerKVHarnessHelper : public KVHarnessHelper {
public:
    WiredTigerKVHarnessHelper(ServiceContext* svcCtx,
                              bool forRepair = false,
                              bool preciseCheckpoints = false)
        : _svcCtx(svcCtx),
          _dbpath("wt-kv-harness"),
          _forRepair(forRepair),
          _preciseCheckpoints(preciseCheckpoints) {
        _svcCtx->setStorageEngine(makeEngine());
        getWiredTigerKVEngine()->notifyStorageStartupRecoveryComplete();
    }

    ~WiredTigerKVHarnessHelper() override {
        getWiredTigerKVEngine()->cleanShutdown(kMemLeakAllowed);
    }

    KVEngine* restartEngine() override {
        getEngine()->cleanShutdown(kMemLeakAllowed);
        _svcCtx->clearStorageEngine();
        _svcCtx->setStorageEngine(makeEngine());
        getEngine()->notifyStorageStartupRecoveryComplete();
        return getEngine();
    }

    KVEngine* getEngine() override {
        return _svcCtx->getStorageEngine()->getEngine();
    }

    virtual WiredTigerKVEngine* getWiredTigerKVEngine() {
        return static_cast<WiredTigerKVEngine*>(_svcCtx->getStorageEngine()->getEngine());
    }

private:
    std::unique_ptr<StorageEngine> makeEngine() {
        // Use a small journal for testing to account for the unlikely event that the underlying
        // filesystem does not support fast allocation of a file of zeros.
        auto& provider = rss::ReplicatedStorageService::get(_svcCtx).getPersistenceProvider();
        WiredTigerKVEngineBase::WiredTigerConfig wtConfig =
            getWiredTigerConfigFromStartupOptions(provider);
        wtConfig.cacheSizeMB = 1;
        if (_preciseCheckpoints) {
            // Precise checkpoints don't work with journaling in tests.
            wtConfig.extraOpenOptions = "precise_checkpoint=true,preserve_prepared=true,";
            wtConfig.logEnabled = false;
        } else {
            wtConfig.extraOpenOptions = "log=(file_max=1m,prealloc=false)";
        }
        // Faithfully simulate being in replica set mode for timestamping tests which requires
        // parity for journaling settings.
        auto isReplSet = true;
        auto shouldRecoverFromOplogAsStandalone = false;
        auto replSetMemberInStandaloneMode = false;
        auto kv = std::make_unique<WiredTigerKVEngine>(std::string{kWiredTigerEngineName},
                                                       _dbpath.path(),
                                                       _cs.get(),
                                                       std::move(wtConfig),
                                                       WiredTigerExtensions::get(_svcCtx),
                                                       provider,
                                                       _forRepair,
                                                       isReplSet,
                                                       shouldRecoverFromOplogAsStandalone,
                                                       replSetMemberInStandaloneMode);

        auto client = _svcCtx->getService()->makeClient("opCtx");
        auto opCtx = client->makeOperationContext();
        StorageEngineOptions options;
        return std::make_unique<StorageEngineImpl>(
            opCtx.get(), std::move(kv), std::unique_ptr<KVEngine>(), options);
    }

    ServiceContext* _svcCtx;
    const std::unique_ptr<ClockSource> _cs = std::make_unique<ClockSourceMock>();
    unittest::TempDir _dbpath;
    bool _forRepair;
    bool _preciseCheckpoints;
};

class WiredTigerKVEngineTest : public ServiceContextTest {
public:
    WiredTigerKVEngineTest(bool repair = false, bool preciseCheckpoints = false)
        : _repair(repair), _preciseCheckpoints(preciseCheckpoints) {}

    void setUp() override {
        _helper = std::make_unique<WiredTigerKVHarnessHelper>(
            getServiceContext(), _repair, _preciseCheckpoints);
    }

    void tearDown() override {
        _helper.reset();
    }

protected:
    using ClientAndCtx =
        std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>;

    ServiceContext::UniqueOperationContext _makeOperationContext() {
        auto opCtx = makeOperationContext();
        shard_role_details::setRecoveryUnit(opCtx.get(),
                                            _helper->getEngine()->newRecoveryUnit(),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        return opCtx;
    }

    ClientAndCtx _makeClientAndOperationContext(const std::string& clientName) {
        auto* sc = getServiceContext();
        auto client = sc->getService()->makeClient(clientName);
        auto opCtx = client->makeOperationContext();
        _helper->getEngine()->newRecoveryUnit();
        shard_role_details::setRecoveryUnit(opCtx.get(),
                                            _helper->getEngine()->newRecoveryUnit(),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        return std::make_pair(std::move(client), std::move(opCtx));
    }

    bool _repair;
    bool _preciseCheckpoints;
    std::unique_ptr<WiredTigerKVHarnessHelper> _helper;
};

class WiredTigerKVEngineRepairTest : public WiredTigerKVEngineTest {
public:
    WiredTigerKVEngineRepairTest() : WiredTigerKVEngineTest(true /* repair */) {}
};

TEST_F(WiredTigerKVEngineRepairTest, OrphanedDataFilesCanBeRecovered) {
    auto opCtxPtr = _makeOperationContext();

    std::string ident = "collection-1234";
    RecordStore::Options options;
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    auto& provider = rss::ReplicatedStorageService::get(opCtxPtr.get()).getPersistenceProvider();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());
    WriteUnitOfWork wuow(opCtxPtr.get());
    ASSERT_OK(
        _helper->getWiredTigerKVEngine()->createRecordStore(provider, ru, nss, ident, options));
    wuow.commit();
    auto rs = _helper->getWiredTigerKVEngine()->getRecordStore(
        opCtxPtr.get(), nss, ident, options, UUID::gen());
    ASSERT(rs);

    RecordId loc;
    std::string record = "abcd";
    {
        StorageWriteTransaction txn(ru);
        StatusWith<RecordId> res =
            rs->insertRecord(opCtxPtr.get(),
                             *shard_role_details::getRecoveryUnit(opCtxPtr.get()),
                             record.c_str(),
                             record.length() + 1,
                             Timestamp());
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        txn.commit();
    }

    const boost::optional<boost::filesystem::path> dataFilePath =
        _helper->getWiredTigerKVEngine()->getDataFilePathForIdent(ident);
    ASSERT(dataFilePath);

    ASSERT(boost::filesystem::exists(*dataFilePath));

    const boost::filesystem::path tmpFile{dataFilePath->string() + ".tmp"};
    ASSERT(!boost::filesystem::exists(tmpFile));

#ifdef _WIN32
    {
        WriteUnitOfWork wuow(opCtxPtr.get());
        auto status = _helper->getWiredTigerKVEngine()->recoverOrphanedIdent(
            provider, ru, nss, ident, options);
        ASSERT_EQ(ErrorCodes::CommandNotSupported, status.code());
    }
#else

    // Dropping a collection might fail if we haven't checkpointed the data.
    _helper->getWiredTigerKVEngine()->checkpoint();

    // Move the data file out of the way so the ident can be dropped. This not permitted on Windows
    // because the file cannot be moved while it is open. The implementation for orphan recovery is
    // also not implemented on Windows for this reason.
    boost::system::error_code err;
    boost::filesystem::rename(*dataFilePath, tmpFile, err);
    ASSERT(!err) << err.message();

    ASSERT_OK(_helper->getEngine()->dropIdent(
        *shard_role_details::getRecoveryUnit(opCtxPtr.get()), ident, /*identHasSizeInfo=*/true));

    // The data file is moved back in place so that it becomes an "orphan" of the storage
    // engine and the restoration process can be tested.
    boost::filesystem::rename(tmpFile, *dataFilePath, err);
    ASSERT(!err) << err.message();

    {
        WriteUnitOfWork wuow(opCtxPtr.get());
        auto status = _helper->getWiredTigerKVEngine()->recoverOrphanedIdent(
            provider, ru, nss, ident, options);
        ASSERT_EQ(ErrorCodes::DataModifiedByRepair, status.code());
        wuow.commit();
    }
#endif
}

TEST_F(WiredTigerKVEngineRepairTest, UnrecoverableOrphanedDataFilesAreRebuilt) {
    auto opCtxPtr = _makeOperationContext();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    std::string ident = "collection-1234";
    RecordStore::Options options;
    auto& provider = rss::ReplicatedStorageService::get(opCtxPtr.get()).getPersistenceProvider();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());
    StorageWriteTransaction swt(ru);
    ASSERT_OK(
        _helper->getWiredTigerKVEngine()->createRecordStore(provider, ru, nss, ident, options));
    swt.commit();

    UUID uuid = UUID::gen();
    auto rs =
        _helper->getWiredTigerKVEngine()->getRecordStore(opCtxPtr.get(), nss, ident, options, uuid);
    ASSERT(rs);

    RecordId loc;
    std::string record = "abcd";
    {
        StorageWriteTransaction txn(ru);
        StatusWith<RecordId> res =
            rs->insertRecord(opCtxPtr.get(),
                             *shard_role_details::getRecoveryUnit(opCtxPtr.get()),
                             record.c_str(),
                             record.length() + 1,
                             Timestamp());
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        txn.commit();
    }

    const boost::optional<boost::filesystem::path> dataFilePath =
        _helper->getWiredTigerKVEngine()->getDataFilePathForIdent(ident);
    ASSERT(dataFilePath);

    ASSERT(boost::filesystem::exists(*dataFilePath));

    // Dropping a collection might fail if we haven't checkpointed the data
    _helper->getWiredTigerKVEngine()->checkpoint();

    ASSERT_OK(_helper->getEngine()->dropIdent(
        *shard_role_details::getRecoveryUnit(opCtxPtr.get()), ident, /*identHasSizeInfo=*/true));

#ifdef _WIN32
    WriteUnitOfWork wuow(opCtxPtr.get());
    auto status =
        _helper->getWiredTigerKVEngine()->recoverOrphanedIdent(provider, ru, nss, ident, options);
    ASSERT_EQ(ErrorCodes::CommandNotSupported, status.code());
#else
    // The ident may not get immediately dropped, so ensure it is completely gone.
    boost::system::error_code err;
    boost::filesystem::remove(*dataFilePath, err);
    ASSERT(!err) << err.message();

    // Create an empty data file. The subsequent call to recreate the collection will fail because
    // it is unsalvageable.
    boost::filesystem::ofstream fileStream(*dataFilePath);
    fileStream << "";
    fileStream.close();

    ASSERT(boost::filesystem::exists(*dataFilePath));

    // This should recreate an empty data file successfully and move the old one to a name that ends
    // in ".corrupt".
    {
        WriteUnitOfWork wuow(opCtxPtr.get());
        auto status = _helper->getWiredTigerKVEngine()->recoverOrphanedIdent(
            provider, ru, nss, ident, options);
        ASSERT_EQ(ErrorCodes::DataModifiedByRepair, status.code()) << status.reason();
        wuow.commit();
    }

    boost::filesystem::path corruptFile = (dataFilePath->string() + ".corrupt");
    ASSERT(boost::filesystem::exists(corruptFile));

    rs =
        _helper->getWiredTigerKVEngine()->getRecordStore(opCtxPtr.get(), nss, ident, options, uuid);
    RecordData data;
    ASSERT_FALSE(rs->findRecord(
        opCtxPtr.get(), *shard_role_details::getRecoveryUnit(opCtxPtr.get()), loc, &data));
#endif
}

// The size storer buffers collection fast counts and only periodically writes them to its table, so
// that an unclean shutdown loses at most the updates since the last periodic flush.
TEST_F(WiredTigerKVEngineTest, SizeStorerFlushesAfterSyncPeriodElapses) {
    auto* engine = _helper->getWiredTigerKVEngine();
    auto opCtxPtr = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());
    auto& provider = rss::ReplicatedStorageService::get(opCtxPtr.get()).getPersistenceProvider();

    // Create a collection and insert some records. This updates the in-memory fast count buffered
    // by the size storer, but does not write it to the size storer table.
    const std::string ident = "collection-sizestorer";
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.sizeStorer");
    const RecordStore::Options rsOptions;
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(engine->createRecordStore(provider, ru, nss, ident, rsOptions));
        txn.commit();
    }
    auto rs = engine->getRecordStore(opCtxPtr.get(), nss, ident, rsOptions, UUID::gen());
    ASSERT(rs);

    constexpr int64_t kNumRecords = 10;
    {
        StorageWriteTransaction txn(ru);
        for (int64_t i = 0; i < kNumRecords; ++i) {
            const std::string doc = "record";
            ASSERT_OK(rs->insertRecord(opCtxPtr.get(), ru, doc.c_str(), doc.size() + 1, Timestamp())
                          .getStatus());
        }
        txn.commit();
    }
    EXPECT_EQ(kNumRecords, rs->numRecords());

    // A fresh size storer reads persisted fast counts directly from the size storer table,
    // bypassing the engine's in-memory buffer.
    const std::string_view uri = checked_cast<WiredTigerRecordStore*>(rs.get())->getURI();
    WiredTigerSizeStorer reader(&engine->getConnection(),
                                WiredTigerUtil::buildTableUri(ident::kSizeStorer));
    WiredTigerSession session(&engine->getConnection());

    // The periodic sync period has not elapsed, so a periodic flush is a no-op: the updated fast
    // count is not yet durable and would be lost on an unclean shutdown.
    engine->sizeStorerPeriodicFlush();
    EXPECT_EQ(0, reader.load(session, uri)->numRecords.load());

    // Advancing the clock past the periodic sync period makes the next periodic flush write the
    // fast count to the size storer table, without relying on a clean shutdown.
    auto* clock = checked_cast<ClockSourceMock*>(engine->getClockSource());
    clock->advance(Milliseconds{gWiredTigerSizeStorerPeriodicSyncPeriodMillis} + Milliseconds{1});
    engine->sizeStorerPeriodicFlush();
    EXPECT_EQ(kNumRecords, reader.load(session, uri)->numRecords.load());
}

TEST_F(WiredTigerKVEngineTest, TestOplogTruncation) {
    // To diagnose any intermittent failures, maximize logging from WiredTigerKVEngine and friends.
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kStorage,
                                                              logv2::LogSeverity::Debug(3)};

    // Set syncdelay before starting the checkpoint thread, otherwise it can observe the default
    // checkpoint frequency of 60 seconds, causing the test to fail due to a 10 second timeout.
    storageGlobalParams.syncdelay.store(1);

    std::unique_ptr<Checkpointer> checkpointer =
        std::make_unique<Checkpointer>(createFixedIntervalPolicy());
    checkpointer->go();

    // If the test fails we want to ensure the checkpoint thread shuts down to avoid accessing the
    // storage engine during shutdown.
    ON_BLOCK_EXIT(
        [&] { checkpointer->shutdown({ErrorCodes::ShutdownInProgress, "Test finished"}); });

    auto opCtxPtr = _makeOperationContext();
    // The initial data timestamp has to be set to take stable checkpoints. The first stable
    // timestamp greater than this will also trigger a checkpoint. The following loop of the
    // CheckpointThread will observe the new `syncdelay` value.
    _helper->getWiredTigerKVEngine()->setInitialDataTimestamp(Timestamp(1, 1));

    // Simulate the callback that queries config.transactions for the oldest active transaction.
    boost::optional<Timestamp> oldestActiveTxnTimestamp;
    Atomic<bool> callbackShouldFail{false};
    auto callback = [&](Timestamp stableTimestamp) {
        using ResultType = StorageEngine::OldestActiveTransactionTimestampResult;
        if (callbackShouldFail.load()) {
            return ResultType(ErrorCodes::ExceededTimeLimit, "timeout");
        }

        return ResultType(oldestActiveTxnTimestamp);
    };

    _helper->getWiredTigerKVEngine()->setOldestActiveTransactionTimestampCallback(callback);

    // A method that will poll the WiredTigerKVEngine until it sees the amount of oplog necessary
    // for crash recovery exceeds the input.
    auto assertPinnedMovesSoon = [this](Timestamp newPinned) {
        // If the current oplog needed for rollback does not exceed the requested pinned out, we
        // cannot expect the CheckpointThread to eventually publish a sufficient crash recovery
        // value.
        auto needed = _helper->getWiredTigerKVEngine()->getOplogNeededForRollback();
        if (needed.isOK()) {
            ASSERT_TRUE(needed.getValue() >= newPinned);
        }

        // Do 100 iterations that sleep for 100 milliseconds between polls. This will wait for up
        // to 10 seconds to observe an asynchronous update that iterates once per second.
        for (auto iterations = 0; iterations < 100; ++iterations) {
            if (_helper->getWiredTigerKVEngine()->getPinnedOplog() >= newPinned) {
                ASSERT_TRUE(
                    _helper->getWiredTigerKVEngine()->getOplogNeededForCrashRecovery().value() >=
                    newPinned);
                return;
            }

            sleepmillis(100);
        }

        LOGV2(22367,
              "Expected the pinned oplog to advance.",
              "expectedValue"_attr = newPinned,
              "publishedValue"_attr =
                  _helper->getWiredTigerKVEngine()->getOplogNeededForCrashRecovery());
        FAIL("");
    };

    oldestActiveTxnTimestamp = boost::none;
    _helper->getWiredTigerKVEngine()->setStableTimestamp(Timestamp(10, 1), false);
    assertPinnedMovesSoon(Timestamp(10, 1));

    oldestActiveTxnTimestamp = Timestamp(15, 1);
    _helper->getWiredTigerKVEngine()->setStableTimestamp(Timestamp(20, 1), false);
    assertPinnedMovesSoon(Timestamp(15, 1));

    oldestActiveTxnTimestamp = Timestamp(19, 1);
    _helper->getWiredTigerKVEngine()->setStableTimestamp(Timestamp(30, 1), false);
    assertPinnedMovesSoon(Timestamp(19, 1));

    oldestActiveTxnTimestamp = boost::none;
    _helper->getWiredTigerKVEngine()->setStableTimestamp(Timestamp(30, 1), false);
    assertPinnedMovesSoon(Timestamp(30, 1));

    callbackShouldFail.store(true);
    ASSERT_NOT_OK(_helper->getWiredTigerKVEngine()->getOplogNeededForRollback());
    _helper->getWiredTigerKVEngine()->setStableTimestamp(Timestamp(40, 1), false);
    // Await a new checkpoint. Oplog needed for rollback does not advance.
    sleepmillis(1100);
    ASSERT_EQ(_helper->getWiredTigerKVEngine()->getOplogNeededForCrashRecovery().value(),
              Timestamp(30, 1));
    _helper->getWiredTigerKVEngine()->setStableTimestamp(Timestamp(30, 1), false);
    callbackShouldFail.store(false);
    assertPinnedMovesSoon(Timestamp(40, 1));
}

TEST_F(WiredTigerKVEngineTest, CreateRecordStoreFailsWithExistingIdent) {
    auto opCtxPtr = _makeOperationContext();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    std::string ident = "collection-1234";
    RecordStore::Options options;
    auto& provider = rss::ReplicatedStorageService::get(opCtxPtr.get()).getPersistenceProvider();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());
    StorageWriteTransaction swt(ru);
    ASSERT_OK(
        _helper->getWiredTigerKVEngine()->createRecordStore(provider, ru, nss, ident, options));

    // A new record store must always have its own storage table uniquely identified by the ident.
    // Otherwise, multiple record stores could point to the same storage resource and lead to data
    // corruption.
    //
    // Validate the server throws when trying to create a new record store with an ident already in
    // use.

    const auto status =
        _helper->getWiredTigerKVEngine()->createRecordStore(provider, ru, nss, ident, options);
    swt.commit();
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::ObjectAlreadyExists);
}

TEST_F(WiredTigerKVEngineTest, IdentDrop) {
#ifdef _WIN32
    // TODO SERVER-51595: to re-enable this test on Windows.
    return;
#endif

    auto opCtxPtr = _makeOperationContext();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    std::string ident = "collection-1234";
    RecordStore::Options options;

    auto& provider = rss::ReplicatedStorageService::get(opCtxPtr.get()).getPersistenceProvider();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());
    {
        StorageWriteTransaction swt(ru);
        ASSERT_OK(
            _helper->getWiredTigerKVEngine()->createRecordStore(provider, ru, nss, ident, options));
        swt.commit();
    }

    const boost::optional<boost::filesystem::path> dataFilePath =
        _helper->getWiredTigerKVEngine()->getDataFilePathForIdent(ident);
    ASSERT(dataFilePath);
    ASSERT(boost::filesystem::exists(*dataFilePath));

    _helper->getWiredTigerKVEngine()->dropIdentForImport(
        *opCtxPtr.get(), *shard_role_details::getRecoveryUnit(opCtxPtr.get()), ident);
    ASSERT(boost::filesystem::exists(*dataFilePath));

    // Because the underlying file was not removed, it will be renamed out of the way by WiredTiger
    // when creating a new table with the same ident.
    {
        StorageWriteTransaction swt(ru);
        ASSERT_OK(
            _helper->getWiredTigerKVEngine()->createRecordStore(provider, ru, nss, ident, options));
        swt.commit();
    }

    const boost::filesystem::path renamedFilePath = dataFilePath->generic_string() + ".1";
    ASSERT(boost::filesystem::exists(*dataFilePath));
    ASSERT(boost::filesystem::exists(renamedFilePath));

    ASSERT_OK(_helper->getEngine()->dropIdent(
        *shard_role_details::getRecoveryUnit(opCtxPtr.get()), ident, /*identHasSizeInfo=*/true));

    // WiredTiger drops files asynchronously.
    for (size_t check = 0; check < 30; check++) {
        if (!boost::filesystem::exists(*dataFilePath))
            break;
        sleepsecs(1);
    }

    ASSERT(!boost::filesystem::exists(*dataFilePath));
    ASSERT(boost::filesystem::exists(renamedFilePath));
}

TEST_F(WiredTigerKVEngineTest, TestBasicPinOldestTimestamp) {
    auto opCtxRaii = _makeOperationContext();
    const Timestamp initTs = Timestamp(1, 0);

    // Initialize the oldest timestamp.
    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs, false);
    ASSERT_EQ(initTs, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    // Assert that advancing the oldest timestamp still succeeds.
    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs + 1, false);
    ASSERT_EQ(initTs + 1, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    // Error if there's a request to pin the oldest timestamp earlier than what it is already set
    // as. This error case is not exercised in this test.
    const bool roundUpIfTooOld = false;
    // Pin the oldest timestamp to "3".
    auto pinnedTs = unittest::assertGet(_helper->getWiredTigerKVEngine()->pinOldestTimestamp(
        *shard_role_details::getRecoveryUnit(opCtxRaii.get()), "A", initTs + 3, roundUpIfTooOld));
    // Assert that the pinning method returns the same timestamp as was requested.
    ASSERT_EQ(initTs + 3, pinnedTs);
    // Assert that pinning the oldest timestamp does not advance it.
    ASSERT_EQ(initTs + 1, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    // Attempt to advance the oldest timestamp to "5".
    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs + 5, false);
    // Observe the oldest timestamp was pinned at the requested "3".
    ASSERT_EQ(initTs + 3, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    // Unpin the oldest timestamp. Assert that unpinning does not advance the oldest timestamp.
    _helper->getWiredTigerKVEngine()->unpinOldestTimestamp("A");
    ASSERT_EQ(initTs + 3, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    // Now advancing the oldest timestamp to "5" succeeds.
    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs + 5, false);
    ASSERT_EQ(initTs + 5, _helper->getWiredTigerKVEngine()->getOldestTimestamp());
}

/**
 * Demonstrate that multiple actors can request different pins of the oldest timestamp. The minimum
 * of all active requests will be obeyed.
 */
TEST_F(WiredTigerKVEngineTest, TestMultiPinOldestTimestamp) {
    auto opCtxRaii = _makeOperationContext();
    const Timestamp initTs = Timestamp(1, 0);

    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs, false);
    ASSERT_EQ(initTs, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    // Error if there's a request to pin the oldest timestamp earlier than what it is already set
    // as. This error case is not exercised in this test.
    const bool roundUpIfTooOld = false;
    // Have "A" pin the timestamp to "1".
    auto pinnedTs = unittest::assertGet(_helper->getWiredTigerKVEngine()->pinOldestTimestamp(
        *shard_role_details::getRecoveryUnit(opCtxRaii.get()), "A", initTs + 1, roundUpIfTooOld));
    ASSERT_EQ(initTs + 1, pinnedTs);
    ASSERT_EQ(initTs, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    // Have "B" pin the timestamp to "2".
    pinnedTs = unittest::assertGet(_helper->getWiredTigerKVEngine()->pinOldestTimestamp(
        *shard_role_details::getRecoveryUnit(opCtxRaii.get()), "B", initTs + 2, roundUpIfTooOld));
    ASSERT_EQ(initTs + 2, pinnedTs);
    ASSERT_EQ(initTs, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    // Advancing the oldest timestamp to "5" will only succeed in advancing it to "1".
    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs + 5, false);
    ASSERT_EQ(initTs + 1, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    // After unpinning "A" at "1", advancing the oldest timestamp will be pinned to "2".
    _helper->getWiredTigerKVEngine()->unpinOldestTimestamp("A");
    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs + 5, false);
    ASSERT_EQ(initTs + 2, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    // Unpinning "B" at "2" allows the oldest timestamp to advance freely.
    _helper->getWiredTigerKVEngine()->unpinOldestTimestamp("B");
    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs + 5, false);
    ASSERT_EQ(initTs + 5, _helper->getWiredTigerKVEngine()->getOldestTimestamp());
}

/**
 * Test error cases where a request to pin the oldest timestamp uses a value that's too early
 * relative to the current oldest timestamp.
 */
TEST_F(WiredTigerKVEngineTest, TestPinOldestTimestampErrors) {
    auto opCtxRaii = _makeOperationContext();
    const Timestamp initTs = Timestamp(10, 0);

    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs, false);
    ASSERT_EQ(initTs, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    const bool roundUpIfTooOld = true;
    // The false value means using this variable will cause the method to fail on error.
    const bool failOnError = false;

    // When rounding on error, the pin will succeed, but the return value will be the current oldest
    // timestamp instead of the requested value.
    auto pinnedTs = unittest::assertGet(_helper->getWiredTigerKVEngine()->pinOldestTimestamp(
        *shard_role_details::getRecoveryUnit(opCtxRaii.get()), "A", initTs - 1, roundUpIfTooOld));
    ASSERT_EQ(initTs, pinnedTs);
    ASSERT_EQ(initTs, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    // Using "fail on error" will result in a not-OK return value.
    ASSERT_NOT_OK(_helper->getWiredTigerKVEngine()->pinOldestTimestamp(
        *shard_role_details::getRecoveryUnit(opCtxRaii.get()), "B", initTs - 1, failOnError));
    ASSERT_EQ(initTs, _helper->getWiredTigerKVEngine()->getOldestTimestamp());
}

/**
 * Test that setOldestTimestamp with force=false silently ignores backwards moves without crashing.
 * This is needed by the DSC checkpoint install path (SERVER-118879) where a checkpoint's
 * oldest_timestamp may be behind the standby's current oldest due to concurrent advancement.
 */
TEST_F(WiredTigerKVEngineTest, SetOldestTimestampBackwardsWithoutForceIsNoop) {
    const Timestamp initTs = Timestamp(10, 0);

    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs, false);
    ASSERT_EQ(initTs, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    // Moving backwards with force=false should be no-op.
    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs - 1, false);
    ASSERT_EQ(initTs, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    // Moving to the same value with force=false should also be no-op.
    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs, false);
    ASSERT_EQ(initTs, _helper->getWiredTigerKVEngine()->getOldestTimestamp());

    // Moving forwards with force=false should advance the timestamp.
    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs + 1, false);
    ASSERT_EQ(initTs + 1, _helper->getWiredTigerKVEngine()->getOldestTimestamp());
}

TEST_F(WiredTigerKVEngineTest, ForceStableTimestampBackwardsMovesOldestTimestampBack) {
    auto* engine = _helper->getWiredTigerKVEngine();

    // Advance the oldest timestamp to a high value.
    const Timestamp highTs = Timestamp(100, 0);
    engine->setOldestTimestamp(highTs, false);
    ASSERT_EQ(highTs, engine->getOldestTimestamp());

    // Force the stable timestamp backwards. This also sets WiredTiger's oldest_timestamp to the
    // lower value, so the cached oldest timestamp must follow.
    const Timestamp lowTs = Timestamp(50, 0);
    engine->setStableTimestamp(lowTs, true);
    ASSERT_EQ(lowTs, engine->getOldestTimestamp());
}

/**
 * Test the various cases for the relationship between oldestTimestamp and stableTimestamp at the
 * end of startup recovery.
 */
TEST_F(WiredTigerKVEngineTest, TestOldestStableTimestampEndOfStartupRecovery) {
    auto opCtxRaii = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxRaii.get());

    // oldest and stable are both null.
    ASSERT_DOES_NOT_THROW(_helper->getWiredTigerKVEngine()->notifyReplStartupRecoveryComplete(ru));

    // oldest is null, stable is not null.
    const Timestamp initTs = Timestamp(10, 0);
    _helper->getWiredTigerKVEngine()->setStableTimestamp(initTs, true);
    ASSERT_DOES_NOT_THROW(_helper->getWiredTigerKVEngine()->notifyReplStartupRecoveryComplete(ru));

    // oldest and stable equal.
    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs, true);
    ASSERT_DOES_NOT_THROW(_helper->getWiredTigerKVEngine()->notifyReplStartupRecoveryComplete(ru));

    // stable > oldest.
    Timestamp laterTs = Timestamp(15, 0);
    _helper->getWiredTigerKVEngine()->setStableTimestamp(laterTs, true);
    ASSERT_DOES_NOT_THROW(_helper->getWiredTigerKVEngine()->notifyReplStartupRecoveryComplete(ru));

    // oldest > stable.
    laterTs = Timestamp(20, 0);
    _helper->getWiredTigerKVEngine()->setOldestTimestamp(laterTs, true);
    ASSERT_THROWS_CODE(_helper->getWiredTigerKVEngine()->notifyReplStartupRecoveryComplete(ru),
                       AssertionException,
                       8470600);
}

/**
 * Test that oldestTimestamp is allowed to advance past stableTimestamp when we notify that
 * startup recovery is complete. This case happens when we complete logical initial sync.
 */
TEST_F(WiredTigerKVEngineTest, TestOldestStableTimestampEndOfStartupRecoveryStableNull) {
    auto opCtxRaii = _makeOperationContext();

    // oldest is not null, stable is null.
    const Timestamp initTs = Timestamp(10, 0);
    _helper->getWiredTigerKVEngine()->setOldestTimestamp(initTs, true);
    ASSERT_DOES_NOT_THROW(_helper->getWiredTigerKVEngine()->notifyReplStartupRecoveryComplete(
        *shard_role_details::getRecoveryUnit(opCtxRaii.get())));
}

TEST_F(WiredTigerKVEngineTest, ExtractIdentFromPath) {
    boost::filesystem::path dbpath = "/data/db";
    boost::filesystem::path identAbsolutePathDefault =
        "/data/db/collection-8a3a1418-4f05-44d6-aca7-59a2f7b30248.wt";
    std::string identDefault = "collection-8a3a1418-4f05-44d6-aca7-59a2f7b30248";

    ASSERT_EQ(extractIdentFromPath(dbpath, identAbsolutePathDefault), identDefault);

    boost::filesystem::path identAbsolutePathDirectoryPerDb =
        "/data/db/test/collection-8a3a1418-4f05-44d6-aca7-59a2f7b30248.wt";
    std::string identDirectoryPerDb = "test/collection-8a3a1418-4f05-44d6-aca7-59a2f7b30248";

    ASSERT_EQ(extractIdentFromPath(dbpath, identAbsolutePathDirectoryPerDb), identDirectoryPerDb);

    boost::filesystem::path identAbsolutePathWiredTigerDirectoryForIndexes =
        "/data/db/collection/8a3a1418-4f05-44d6-aca7-59a2f7b30248.wt";
    std::string identWiredTigerDirectoryForIndexes =
        "collection/8a3a1418-4f05-44d6-aca7-59a2f7b30248";

    ASSERT_EQ(extractIdentFromPath(dbpath, identAbsolutePathWiredTigerDirectoryForIndexes),
              identWiredTigerDirectoryForIndexes);

    boost::filesystem::path identAbsolutePathDirectoryPerDbAndWiredTigerDirectoryForIndexes =
        "/data/db/test/collection/8a3a1418-4f05-44d6-aca7-59a2f7b30248.wt";
    std::string identDirectoryPerDbWiredTigerDirectoryForIndexes =
        "test/collection/8a3a1418-4f05-44d6-aca7-59a2f7b30248";

    ASSERT_EQ(extractIdentFromPath(dbpath,
                                   identAbsolutePathDirectoryPerDbAndWiredTigerDirectoryForIndexes),
              identDirectoryPerDbWiredTigerDirectoryForIndexes);
}

// Prior to v8.2, idents were suffixed with a unique <counter> + <random number> combination. All
// future versions must also maintain compatibility with the legacy ident format.
TEST_F(WiredTigerKVEngineTest, ExtractLegacyIdentFromPath) {
    boost::filesystem::path dbpath = "/data/db";
    boost::filesystem::path identAbsolutePathDefault =
        "/data/db/collection-9-11733751379908443489.wt";
    std::string identDefault = "collection-9-11733751379908443489";

    ASSERT_EQ(extractIdentFromPath(dbpath, identAbsolutePathDefault), identDefault);

    boost::filesystem::path identAbsolutePathDirectoryPerDb =
        "/data/db/test/collection-9-11733751379908443489.wt";
    std::string identDirectoryPerDb = "test/collection-9-11733751379908443489";

    ASSERT_EQ(extractIdentFromPath(dbpath, identAbsolutePathDirectoryPerDb), identDirectoryPerDb);

    boost::filesystem::path identAbsolutePathWiredTigerDirectoryForIndexes =
        "/data/db/collection/9-11733751379908443489.wt";
    std::string identWiredTigerDirectoryForIndexes = "collection/9-11733751379908443489";

    ASSERT_EQ(extractIdentFromPath(dbpath, identAbsolutePathWiredTigerDirectoryForIndexes),
              identWiredTigerDirectoryForIndexes);

    boost::filesystem::path identAbsolutePathDirectoryPerDbAndWiredTigerDirectoryForIndexes =
        "/data/db/test/collection/9-11733751379908443489.wt";
    std::string identDirectoryPerDbWiredTigerDirectoryForIndexes =
        "test/collection/9-11733751379908443489";

    ASSERT_EQ(extractIdentFromPath(dbpath,
                                   identAbsolutePathDirectoryPerDbAndWiredTigerDirectoryForIndexes),
              identDirectoryPerDbWiredTigerDirectoryForIndexes);
}

TEST_F(WiredTigerKVEngineTest, WiredTigerDowngrade) {
    // Initializing this value to silence Coverity warning. Doesn't matter what value
    // _startupVersion is set to since shouldDowngrade() & getDowngradeString() only look at
    // _startupVersion when FCV is uninitialized. This test initializes FCV via setVersion().
    WiredTigerFileVersion version = {WiredTigerFileVersion::StartupVersion::IS_42};

    // (Generic FCV reference): When FCV is kLatest, no downgrade is necessary.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
    ASSERT_FALSE(version.shouldDowngrade(/*hasRecoveryTimestamp=*/false, /*isReplset=*/true));
    ASSERT_EQ(WiredTigerFileVersion::kLatestWTRelease, version.getDowngradeString());

    // (Generic FCV reference): When FCV is kLastContinuous or kLastLTS, a downgrade may be needed.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLastContinuous);
    ASSERT_TRUE(version.shouldDowngrade(/*hasRecoveryTimestamp=*/false, /*isReplset=*/true));
    ASSERT_EQ(WiredTigerFileVersion::kLastContinuousWTRelease, version.getDowngradeString());

    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLastLTS);
    ASSERT_TRUE(version.shouldDowngrade(/*hasRecoveryTimestamp=*/false, /*isReplset=*/true));
    ASSERT_EQ(WiredTigerFileVersion::kLastLTSWTRelease, version.getDowngradeString());

    // (Generic FCV reference): While we're in a semi-downgraded state, we shouldn't try downgrading
    // the WiredTiger compatibility version.
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::GenericFCV::kDowngradingFromLatestToLastContinuous);
    ASSERT_FALSE(version.shouldDowngrade(/*hasRecoveryTimestamp=*/false, /*isReplset=*/true));
    ASSERT_EQ(WiredTigerFileVersion::kLatestWTRelease, version.getDowngradeString());

    serverGlobalParams.mutableFCV.setVersion(
        multiversion::GenericFCV::kDowngradingFromLatestToLastLTS);
    ASSERT_FALSE(version.shouldDowngrade(/*hasRecoveryTimestamp=*/false, /*isReplset=*/true));
    ASSERT_EQ(WiredTigerFileVersion::kLatestWTRelease, version.getDowngradeString());
}

TEST_F(WiredTigerKVEngineTest, TestReconfigureLog) {
    // Perform each test in their own limited scope in order to establish different
    // severity levels.

    {
        auto opCtxRaii = _makeOperationContext();
        // Set the WiredTiger Checkpoint LOGV2 component severity to the Log level.
        auto severityGuard = unittest::MinimumLoggedSeverityGuard{
            logv2::LogComponent::kWiredTigerCheckpoint, logv2::LogSeverity::Log()};
        ASSERT_EQ(logv2::LogSeverity::Log(),
                  unittest::getMinimumLogSeverity(logv2::LogComponent::kWiredTigerCheckpoint));
        ASSERT_OK(_helper->getWiredTigerKVEngine()->reconfigureLogging());
        // Perform a checkpoint. The goal here is create some activity in WiredTiger in order
        // to generate verbose messages (we don't really care about the checkpoint itself).
        unittest::LogCaptureGuard logs;
        _helper->getWiredTigerKVEngine()->checkpoint();
        logs.stop();
        // In this initial case, we don't expect to capture any debug checkpoint messages. The
        // base severity for the checkpoint component should be at Log().
        bool foundWTCheckpointMessage = false;
        for (auto&& bson : logs.getBSON()) {
            if (bson["c"].String() == "WTCHKPT" &&
                bson["attr"]["message"]["verbose_level"].String() == "DEBUG_1" &&
                bson["attr"]["message"]["category"].String() == "WT_VERB_CHECKPOINT") {
                foundWTCheckpointMessage = true;
            }
        }
        ASSERT_FALSE(foundWTCheckpointMessage);
    }
    {
        auto opCtxRaii = _makeOperationContext();
        // Set the WiredTiger Checkpoint LOGV2 component severity to the Debug(2) level.
        auto severityGuard = unittest::MinimumLoggedSeverityGuard{
            logv2::LogComponent::kWiredTigerCheckpoint, logv2::LogSeverity::Debug(2)};
        ASSERT_OK(_helper->getWiredTigerKVEngine()->reconfigureLogging());
        ASSERT_EQ(logv2::LogSeverity::Debug(2),
                  unittest::getMinimumLogSeverity(logv2::LogComponent::kWiredTigerCheckpoint));

        // Perform another checkpoint.
        unittest::LogCaptureGuard logs;
        _helper->getWiredTigerKVEngine()->checkpoint();
        logs.stop();

        // This time we expect to detect WiredTiger checkpoint Debug() messages.
        bool foundWTCheckpointMessage = false;
        for (auto&& bson : logs.getBSON()) {
            if (bson["c"].String() == "WTCHKPT" &&
                bson["attr"]["message"]["verbose_level"].String() == "DEBUG_1" &&
                bson["attr"]["message"]["category"].String() == "WT_VERB_CHECKPOINT") {
                foundWTCheckpointMessage = true;
            }
        }
        ASSERT_TRUE(foundWTCheckpointMessage);
    }
}

TEST_F(WiredTigerKVEngineTest, RollbackToStableEBUSY) {
    auto opCtxPtr = _makeOperationContext();
    _helper->getWiredTigerKVEngine()->setInitialDataTimestamp(Timestamp(1, 1));
    _helper->getWiredTigerKVEngine()->setStableTimestamp(Timestamp(1, 1), false);

    // Get a session. This will open a transaction.
    WiredTigerSession* session =
        WiredTigerRecoveryUnit::get(shard_role_details::getRecoveryUnit(opCtxPtr.get()))
            ->getSession();
    invariant(session);

    // WT will return EBUSY due to the open transaction.
    FailPointEnableBlock failPoint("WTRollbackToStableReturnOnEBUSY");
    ASSERT_EQ(ErrorCodes::ObjectIsBusy,
              _helper->getWiredTigerKVEngine()
                  ->recoverToStableTimestamp(*opCtxPtr.get())
                  .getStatus()
                  .code());

    // Close the open transaction.
    WiredTigerRecoveryUnit::get(shard_role_details::getRecoveryUnit(opCtxPtr.get()))
        ->abandonSnapshot();

    // WT will no longer return EBUSY.
    ASSERT_OK(_helper->getWiredTigerKVEngine()->recoverToStableTimestamp(*opCtxPtr.get()));
}

TEST_F(WiredTigerKVEngineTest, GetIndexStorageSizeReturnsBusyWhenStableFileBusy) {
    auto opCtxPtr = _makeOperationContext();
    FailPointEnableBlock failPoint("WTIndexStorageSizeReturnBusy");
    // With the failpoint active, the stable-file statistics open is treated as EBUSY.
    // getIndexStorageSize must surface this as the retryable ObjectIsBusy rather than a hard error.
    EXPECT_EQ(ErrorCodes::ObjectIsBusy,
              _helper->getWiredTigerKVEngine()
                  ->getIndexStorageSize(opCtxPtr.get(), {"some-index-ident"})
                  .getStatus()
                  .code());
}

TEST_F(WiredTigerKVEngineTest, GetIndexStorageSizeAbsentStableFileContributesZero) {
    auto opCtxPtr = _makeOperationContext();
    // No .wt_stable checkpoint file exists for this ident, so the statistics open fails with
    // NoSuchKey and the ident contributes zero without erroring.
    const StatusWith<int64_t> swSize = _helper->getWiredTigerKVEngine()->getIndexStorageSize(
        opCtxPtr.get(), {"nonexistent-index-ident"});
    ASSERT_OK(swSize.getStatus());
    EXPECT_EQ(swSize.getValue(), 0);
}

// Background auto-compact reconfigures are applied asynchronously, so a reconfigure issued while a
// previous one is still being consumed is transiently rejected with ObjectIsBusy. Production wraps
// these calls in a retry loop (see StorageEngineImpl::pauseOrResumeAutoCompactForWriteBlock);
// mirror that here so back-to-back reconfigures in the tests below are not racy.
Status retryWhileAutoCompactBusy(const std::function<Status()>& op) {
    Status status = Status::OK();
    for (int attempt = 0; attempt < 600; ++attempt) {
        status = op();
        if (status != ErrorCodes::ObjectIsBusy) {
            break;
        }
        sleepmillis(50);
    }
    return status;
}

// Pausing auto-compaction for a write block stops it but saves the active configuration; resuming
// restarts compaction with the saved configuration.
TEST_F(WiredTigerKVEngineTest, AutoCompactPauseThenResumeRestoresConfig) {
    // canRunAutoCompact() requires checkpoints to be enabled.
    storageGlobalParams.syncdelay.store(1);
    ON_BLOCK_EXIT([] { storageGlobalParams.syncdelay.store(0); });

    auto* engine = _helper->getWiredTigerKVEngine();
    auto opCtx = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    ASSERT_OK(engine->autoCompact(ru,
                                  AutoCompactOptions{true /* enable */,
                                                     false /* runOnce */,
                                                     50 /* freeSpaceTargetMB */,
                                                     {} /* excludedIdents */}));
    ASSERT_TRUE(engine->getActiveAutoCompactOptions());

    ASSERT_OK(retryWhileAutoCompactBusy([&] {
        return engine->pauseOrResumeAutoCompactForWriteBlock(
            ru, true /* pause */, {} /* excludedIdents */);
    }));
    ASSERT_FALSE(engine->getActiveAutoCompactOptions());

    ASSERT_OK(retryWhileAutoCompactBusy([&] {
        return engine->pauseOrResumeAutoCompactForWriteBlock(
            ru, false /* pause */, {} /* excludedIdents */);
    }));
    auto active = engine->getActiveAutoCompactOptions();
    ASSERT_TRUE(active);
    ASSERT_TRUE(active->enable);
    ASSERT_TRUE(active->freeSpaceTargetMB);
    ASSERT_EQ(*active->freeSpaceTargetMB, 50);
}

// An explicit user disable discards the configuration saved for a write-block restore, so a
// subsequent resume does not resurrect the compaction the user turned off.
TEST_F(WiredTigerKVEngineTest, AutoCompactUserDisableDiscardsSavedRestore) {
    storageGlobalParams.syncdelay.store(1);
    ON_BLOCK_EXIT([] { storageGlobalParams.syncdelay.store(0); });

    auto* engine = _helper->getWiredTigerKVEngine();
    auto opCtx = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    ASSERT_OK(engine->autoCompact(ru,
                                  AutoCompactOptions{true /* enable */,
                                                     false /* runOnce */,
                                                     50 /* freeSpaceTargetMB */,
                                                     {} /* excludedIdents */}));
    ASSERT_OK(retryWhileAutoCompactBusy([&] {
        return engine->pauseOrResumeAutoCompactForWriteBlock(
            ru, true /* pause */, {} /* excludedIdents */);
    }));

    // The user explicitly disables auto-compaction while it is paused.
    ASSERT_OK(retryWhileAutoCompactBusy([&] {
        return engine->autoCompact(ru,
                                   AutoCompactOptions{false /* enable */,
                                                      false /* runOnce */,
                                                      boost::none /* freeSpaceTargetMB */,
                                                      {} /* excludedIdents */});
    }));

    // Resuming is a no-op because the saved configuration was discarded.
    ASSERT_OK(retryWhileAutoCompactBusy([&] {
        return engine->pauseOrResumeAutoCompactForWriteBlock(
            ru, false /* pause */, {} /* excludedIdents */);
    }));
    ASSERT_FALSE(engine->getActiveAutoCompactOptions());
}

// Pausing is idempotent: a second pause while nothing is active must not clobber the configuration
// saved by the first pause, so a later resume still restores it.
TEST_F(WiredTigerKVEngineTest, AutoCompactRepeatedPausePreservesSavedConfig) {
    storageGlobalParams.syncdelay.store(1);
    ON_BLOCK_EXIT([] { storageGlobalParams.syncdelay.store(0); });

    auto* engine = _helper->getWiredTigerKVEngine();
    auto opCtx = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    ASSERT_OK(engine->autoCompact(ru,
                                  AutoCompactOptions{true /* enable */,
                                                     false /* runOnce */,
                                                     50 /* freeSpaceTargetMB */,
                                                     {} /* excludedIdents */}));

    // First pause saves the active configuration; the second pause finds nothing active and must
    // leave the saved configuration untouched.
    ASSERT_OK(retryWhileAutoCompactBusy([&] {
        return engine->pauseOrResumeAutoCompactForWriteBlock(
            ru, true /* pause */, {} /* excludedIdents */);
    }));
    ASSERT_OK(retryWhileAutoCompactBusy([&] {
        return engine->pauseOrResumeAutoCompactForWriteBlock(
            ru, true /* pause */, {} /* excludedIdents */);
    }));

    ASSERT_OK(retryWhileAutoCompactBusy([&] {
        return engine->pauseOrResumeAutoCompactForWriteBlock(
            ru, false /* pause */, {} /* excludedIdents */);
    }));
    auto active = engine->getActiveAutoCompactOptions();
    ASSERT_TRUE(active);
    ASSERT_TRUE(active->freeSpaceTargetMB);
    ASSERT_EQ(*active->freeSpaceTargetMB, 50);
}

// Resuming without a prior pause has nothing saved to restore, so it is a no-op and leaves
// compaction off.
TEST_F(WiredTigerKVEngineTest, AutoCompactResumeWithoutPauseIsNoop) {
    storageGlobalParams.syncdelay.store(1);
    ON_BLOCK_EXIT([] { storageGlobalParams.syncdelay.store(0); });

    auto* engine = _helper->getWiredTigerKVEngine();
    auto opCtx = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    ASSERT_OK(retryWhileAutoCompactBusy([&] {
        return engine->pauseOrResumeAutoCompactForWriteBlock(
            ru, false /* pause */, {} /* excludedIdents */);
    }));
    ASSERT_FALSE(engine->getActiveAutoCompactOptions());
}

// A run-once compaction is not treated as active, so pausing does not save it and a later resume
// restores nothing.
TEST_F(WiredTigerKVEngineTest, AutoCompactRunOnceNotRestoredAcrossPause) {
    storageGlobalParams.syncdelay.store(1);
    ON_BLOCK_EXIT([] { storageGlobalParams.syncdelay.store(0); });

    auto* engine = _helper->getWiredTigerKVEngine();
    auto opCtx = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    ASSERT_OK(engine->autoCompact(ru,
                                  AutoCompactOptions{true /* enable */,
                                                     true /* runOnce */,
                                                     boost::none /* freeSpaceTargetMB */,
                                                     {} /* excludedIdents */}));

    ASSERT_OK(retryWhileAutoCompactBusy([&] {
        return engine->pauseOrResumeAutoCompactForWriteBlock(
            ru, true /* pause */, {} /* excludedIdents */);
    }));

    // Nothing was saved because run-once is never treated as active, so resume is a no-op.
    ASSERT_OK(retryWhileAutoCompactBusy([&] {
        return engine->pauseOrResumeAutoCompactForWriteBlock(
            ru, false /* pause */, {} /* excludedIdents */);
    }));
    ASSERT_FALSE(engine->getActiveAutoCompactOptions());
}

// Enabling continuous (non run-once) auto-compaction caches the active configuration.
TEST_F(WiredTigerKVEngineTest, AutoCompactCachesContinuousOptions) {
    // canRunAutoCompact() requires checkpoints to be enabled.
    storageGlobalParams.syncdelay.store(1);
    ON_BLOCK_EXIT([] { storageGlobalParams.syncdelay.store(0); });

    auto* engine = _helper->getWiredTigerKVEngine();
    auto opCtx = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    ASSERT_FALSE(engine->getActiveAutoCompactOptions());

    ASSERT_OK(engine->autoCompact(ru,
                                  AutoCompactOptions{true /* enable */,
                                                     false /* runOnce */,
                                                     100 /* freeSpaceTargetMB */,
                                                     {} /* excludedIdents */}));

    auto active = engine->getActiveAutoCompactOptions();
    ASSERT_TRUE(active);
    ASSERT_TRUE(active->enable);
    ASSERT_FALSE(active->runOnce);
    ASSERT_TRUE(active->freeSpaceTargetMB);
    ASSERT_EQ(*active->freeSpaceTargetMB, 100);
    // The cached options never retain excludedIdents: each enable
    // caller recomputes the oplog exclusion itself.
    ASSERT_TRUE(active->excludedIdents.empty());
}

// A run-once compaction is a transient one-shot, so it is not cached as the active configuration.
TEST_F(WiredTigerKVEngineTest, AutoCompactDoesNotCacheRunOnce) {
    storageGlobalParams.syncdelay.store(1);
    ON_BLOCK_EXIT([] { storageGlobalParams.syncdelay.store(0); });

    auto* engine = _helper->getWiredTigerKVEngine();
    auto opCtx = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    ASSERT_OK(engine->autoCompact(ru,
                                  AutoCompactOptions{true /* enable */,
                                                     true /* runOnce */,
                                                     boost::none /* freeSpaceTargetMB */,
                                                     {} /* excludedIdents */}));

    ASSERT_FALSE(engine->getActiveAutoCompactOptions());
}

std::unique_ptr<KVHarnessHelper> makeHelper(ServiceContext* svcCtx) {
    return std::make_unique<WiredTigerKVHarnessHelper>(svcCtx);
}

MONGO_INITIALIZER(RegisterKVHarnessFactory)(InitializerContext*) {
    KVHarnessHelper::registerFactory(makeHelper);
}

TEST_F(WiredTigerKVEngineTest, TestHandlerCleanShutdown) {
    auto* engine = _helper->getWiredTigerKVEngine();
    ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
    engine->cleanShutdown(kMemLeakAllowed);
    ASSERT(!engine->isWtConnReadyForStatsCollection_UNSAFE());
}

TEST_F(WiredTigerKVEngineTest, TestHandlerSingleActivityBeforeShutdownRAII) {
    auto* engine = _helper->getWiredTigerKVEngine();
    ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
    {
        auto permit = engine->tryGetStatsCollectionPermit();
        ASSERT(permit);
        ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
        ASSERT_EQ(engine->getActiveStatsReaders(), 1);
    }
    ASSERT_EQ(engine->getActiveStatsReaders(), 0);
    ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
    engine->cleanShutdown(kMemLeakAllowed);
    ASSERT(!engine->isWtConnReadyForStatsCollection_UNSAFE());
}

TEST_F(WiredTigerKVEngineTest, TestHandlerMultipleActivitiesBeforeShutdownRAII) {
    auto* engine = _helper->getWiredTigerKVEngine();
    ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
    {
        auto permit1 = engine->tryGetStatsCollectionPermit();
        ASSERT(permit1);
        ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
        ASSERT_EQ(engine->getActiveStatsReaders(), 1);
        {
            auto permit2 = engine->tryGetStatsCollectionPermit();
            ASSERT(permit2);
            ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
            ASSERT_EQ(engine->getActiveStatsReaders(), 2);
        }
        ASSERT_EQ(engine->getActiveStatsReaders(), 1);
    }
    ASSERT_EQ(engine->getActiveStatsReaders(), 0);
    ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
    engine->cleanShutdown(kMemLeakAllowed);
    ASSERT(!engine->isWtConnReadyForStatsCollection_UNSAFE());
}

TEST_F(WiredTigerKVEngineTest, TestHandlerCleanShutdownBeforeActivity) {
    auto* engine = _helper->getWiredTigerKVEngine();
    ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
    engine->cleanShutdown(kMemLeakAllowed);
    ASSERT(!engine->tryGetStatsCollectionPermit());
    ASSERT_EQ(engine->getActiveStatsReaders(), 0);
    ASSERT(!engine->isWtConnReadyForStatsCollection_UNSAFE());
}

TEST_F(WiredTigerKVEngineTest, TestHandlerCleanShutdownBeforeActivityReleaseRAII) {
    auto* engine = _helper->getWiredTigerKVEngine();
    ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
    {
        auto permit = engine->tryGetStatsCollectionPermit();
        ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
        ASSERT_EQ(engine->getActiveStatsReaders(), 1);
        stdx::thread shutdownThread([&]() { engine->cleanShutdown(kMemLeakAllowed); });
        ASSERT_EQ(engine->getActiveStatsReaders(), 1);
        while (engine->isWtConnReadyForStatsCollection_UNSAFE()) {
            std::this_thread::yield();
        }

        // Ensure that releasing the permit unblocks the shutdown
        permit.reset();
        shutdownThread.join();
    }
    ASSERT_EQ(engine->getActiveStatsReaders(), 0);
    ASSERT(!engine->isWtConnReadyForStatsCollection_UNSAFE());
}

TEST_F(WiredTigerKVEngineTest, TestRestartUsesNewConn) {
    auto* engine = _helper->getWiredTigerKVEngine();
    ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());

    {
        auto permit = engine->tryGetStatsCollectionPermit();
        ASSERT(permit);
        ASSERT_EQ(engine->getConn(), permit->conn());
    }

    _helper->restartEngine();
    engine = _helper->getWiredTigerKVEngine();

    auto permit = engine->tryGetStatsCollectionPermit();
    ASSERT(permit);
    ASSERT_EQ(engine->getConn(), permit->conn());
}

TEST_F(WiredTigerKVEngineTest, TestGetBackupCheckpointTimestampWithoutOpenBackupCursor) {
    auto* engine = _helper->getWiredTigerKVEngine();
    ASSERT_EQ(Timestamp::min(), engine->getBackupCheckpointTimestamp());
}

TEST_F(WiredTigerKVEngineTest, CollectStorageStatsReturnsNestedCategories) {
    auto* engine = _helper->getWiredTigerKVEngine();
    ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());

    auto stats = engine->collectStorageStats();
    ASSERT(stats);

    // Ensure all 3 categories we expect are present.
    for (std::string_view category : {"cache", "data-handle", "checkpoint"}) {
        auto elem = (*stats)[category];
        ASSERT(!elem.eoo()) << "missing category: " << category;
        ASSERT_EQ(elem.type(), BSONType::object) << "category not an object: " << category;
    }

    // Check that a representative sample of fields are present.
    const BSONObj cache = stats->getObjectField("cache");
    for (std::string_view measurement : {"bytes read into cache",
                                         "bytes written from cache",
                                         "bytes currently in the cache",
                                         "maximum bytes configured"}) {
        auto elem = cache[measurement];
        ASSERT(!elem.eoo()) << "missing cache measurement: " << measurement;
        ASSERT(elem.isNumber()) << "cache measurement not numeric: " << measurement;
    }

    ASSERT(stats->getObjectField("data-handle")["connection data handles currently active"]
               .isNumber());
    ASSERT(stats->getObjectField("checkpoint")["most recent time (msecs)"].isNumber());
}

TEST_F(WiredTigerKVEngineTest, CollectStorageStatsReturnsNoneWhenConnectionNotReady) {
    auto* engine = _helper->getWiredTigerKVEngine();
    ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
    ASSERT(engine->collectStorageStats());

    engine->cleanShutdown(kMemLeakAllowed);
    ASSERT(!engine->isWtConnReadyForStatsCollection_UNSAFE());
    ASSERT(!engine->collectStorageStats());
}

using WiredTigerKVEngineTestDeathTest = WiredTigerKVEngineTest;
DEATH_TEST_F(WiredTigerKVEngineTestDeathTest, WaitUntilDurableMustBeOutOfUnitOfWork, "invariant") {
    auto opCtx = _makeOperationContext();
    shard_role_details::getRecoveryUnit(opCtx.get())->beginUnitOfWork(opCtx->readOnly());
    opCtx->getServiceContext()->getStorageEngine()->waitUntilDurable(opCtx.get());
}

class WiredTigerKVEngineDirectoryTest : public WiredTigerKVEngineTest {
public:
    void setUp() override {
        WiredTigerKVEngineTest::setUp();
        _opCtx = _makeOperationContext();
    }

protected:
    // Creates the given ident, returning the path to it.
    StatusWith<boost::filesystem::path> createIdent(std::string_view ns, std::string_view ident) {
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);
        RecordStore::Options options;
        auto& provider =
            rss::ReplicatedStorageService::get(getGlobalServiceContext()).getPersistenceProvider();
        auto& ru = *shard_role_details::getRecoveryUnit(_opCtx.get());
        StorageWriteTransaction swt(ru);
        Status stat =
            _helper->getWiredTigerKVEngine()->createRecordStore(provider, ru, nss, ident, options);
        swt.commit();
        if (!stat.isOK()) {
            return stat;
        }
        boost::optional<boost::filesystem::path> path =
            _helper->getWiredTigerKVEngine()->getDataFilePathForIdent(ident);
        ASSERT_TRUE(path.has_value());
        return *path;
    }

    Status removeIdent(std::string_view ident) {
        return _helper->getEngine()->dropIdent(
            *shard_role_details::getRecoveryUnit(_opCtx.get()), ident, /*identHasSizeInfo=*/true);
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(WiredTigerKVEngineDirectoryTest, TopLevelIdentsDontRemoveDirectories) {
    std::string ident = "collection-ident-without-directory";
    StatusWith<boost::filesystem::path> path = createIdent("name.space", ident);
    ASSERT_OK(path);
    ASSERT_TRUE(boost::filesystem::exists(path.getValue().parent_path()));

    // Since this ident isn't in a directory, we'd better not delete it's parent (i.e. the dbpath).
    ASSERT_OK(removeIdent(ident));
    ASSERT_TRUE(boost::filesystem::exists(path.getValue().parent_path()));
}

TEST_F(WiredTigerKVEngineDirectoryTest, RemovingLastIdentPromptsDirectoryRemoval) {
    std::string apple = "fruit/collection-apple";
    StatusWith<boost::filesystem::path> applePath = createIdent("fruit.apple", apple);
    ASSERT_OK(applePath);
    boost::filesystem::path fruitDir = applePath.getValue().parent_path();

    // Same directory.
    std::string orange = "fruit/collection-orange";
    StatusWith<boost::filesystem::path> orangePath = createIdent("fruit.orange", orange);
    ASSERT_OK(orangePath);
    ASSERT_EQ(fruitDir, orangePath.getValue().parent_path());

    // Different directory.
    std::string potato = "veg/collection-potato";
    StatusWith<boost::filesystem::path> potatoPath = createIdent("veg.potato", potato);
    ASSERT_OK(potatoPath);
    ASSERT_NE(fruitDir, potatoPath.getValue().parent_path());

    // Not the last ident, so directory still exists.
    ASSERT_OK(removeIdent(apple));
    ASSERT_TRUE(boost::filesystem::exists(fruitDir));

    // Remove a different directory, doesn't touch the original one.
    ASSERT_OK(removeIdent(potato));
    ASSERT_FALSE(boost::filesystem::exists(potatoPath.getValue().parent_path()));
    ASSERT_TRUE(boost::filesystem::exists(fruitDir));

    // Now its gone.
    ASSERT_OK(removeIdent(orange));
    ASSERT_FALSE(boost::filesystem::exists(fruitDir));
}

TEST_F(WiredTigerKVEngineDirectoryTest, HandlesNestedDirectories) {
    std::string ident = "dbname/collection/ident";
    StatusWith<boost::filesystem::path> path = createIdent("name.space", ident);
    ASSERT_OK(path);
    ASSERT_TRUE(boost::filesystem::exists(path.getValue()));

    ASSERT_OK(removeIdent(ident));
    // ident
    ASSERT_FALSE(boost::filesystem::exists(path.getValue()));
    // collection
    ASSERT_FALSE(boost::filesystem::exists(path.getValue().parent_path()));
    // dbname
    ASSERT_FALSE(boost::filesystem::exists(path.getValue().parent_path().parent_path()));
    // the test parent directory
    ASSERT_TRUE(boost::filesystem::exists(
        path.getValue().parent_path().parent_path().parent_path().parent_path()));
}

TEST_F(WiredTigerKVEngineTest, CheckSessionCacheMax) {

    unittest::ServerParameterGuard sessionCacheMax{"wiredTigerSessionCacheMaxPercentage", 20};
    unittest::ServerParameterGuard sessionMax{"wiredTigerSessionMax", 150};
    unittest::ServerParameterGuard reservedSession{"wiredTigerReservedSessionMax", 10};
    _helper->restartEngine();

    auto* engine = _helper->getWiredTigerKVEngine();
    auto& connection = engine->getConnection();

    // Check that the configured session cache max is derived correctly.
    ASSERT_EQ(connection.getSessionCacheMax(), 30);
}

class WiredTigerKVEngineTestWithPreciseCheckpoints : public WiredTigerKVEngineTest {
public:
    WiredTigerKVEngineTestWithPreciseCheckpoints()
        : WiredTigerKVEngineTest(false /* repair */, true /* preciseCheckpoints */) {}

protected:
    void createPreparedTransaction(OperationContext* opCtx,
                                   RecoveryUnit& ru,
                                   Timestamp prepareTimestamp,
                                   uint64_t preparedId) {
        auto& wtRu = WiredTigerRecoveryUnit::get(ru);
        WiredTigerSession* session = wtRu.getSession();

        ru.beginUnitOfWork(opCtx->readOnly());

        WT_CURSOR* cursor = nullptr;
        const char* wt_uri = "table:test_table";
        const char* wt_config = "key_format=S,value_format=S,log=(enabled=false)";
        ASSERT_OK(wtRCToStatus(session->create(wt_uri, wt_config), *session));
        ASSERT_OK(wtRCToStatus(session->open_cursor(wt_uri, nullptr, nullptr, &cursor), *session));
        // We need to insert unique values into the table otherwise the insert could conflict
        // with the insert of a previously created prepared transaction.
        const std::string key = "key" + std::to_string(preparedId);
        const std::string value = "value" + std::to_string(preparedId);
        cursor->set_key(cursor, key.c_str());
        cursor->set_value(cursor, value.c_str());
        ASSERT_OK(wtRCToStatus(wiredTigerCursorInsert(wtRu, cursor), *session));

        ru.setPrepareTimestamp(prepareTimestamp);
        ru.setPreparedId(preparedId);
        ru.prepareUnitOfWork();
    }
};

TEST_F(WiredTigerKVEngineTestWithPreciseCheckpoints,
       UnresolvedPreparedTransactionIsVisibleOnStartupRecovery) {
    // Create an unresolved prepared transaction.
    auto opCtxPtr = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());
    const auto prepareTimestamp = Timestamp(2, 0);
    const auto preparedId = prepareTimestamp.asULL();
    createPreparedTransaction(opCtxPtr.get(), ru, prepareTimestamp, preparedId);
    ASSERT_EQ(ru.getPrepareTimestamp(), prepareTimestamp);
    ASSERT_EQ(ru.getPreparedId().value(), preparedId);

    // Create a checkpoint that includes the prepared transaction.
    auto* engine = _helper->getWiredTigerKVEngine();
    engine->setInitialDataTimestamp(Timestamp(1, 0));
    engine->setStableTimestamp(prepareTimestamp, /*force=*/false);
    engine->checkpoint();

    // This is necessary to satisfy the destructor of the recovery unit which expects to not be in a
    // unit of work when the storage engine is restarted. This does not affect the results of the
    // prepared transaction iterator since the transaction rollback is not in the checkpoint.
    auto rollbackTimestamp = Timestamp(3, 0);
    ru.setRollbackTimestamp(rollbackTimestamp);
    ASSERT_EQ(ru.getRollbackTimestamp(), rollbackTimestamp);
    ru.abortUnitOfWork();

    // Release the opCtx to prevent memory issues when the storage engine is restarted.
    opCtxPtr.reset();

    // Simulate startup recovery by restarting the storage engine.
    _helper->restartEngine();
    engine = _helper->getWiredTigerKVEngine();
    auto opCtxPtr2 = _makeOperationContext();

    // Verify that we see the prepared transaction on startup recovery and reclaim it.
    int count = 0;
    auto iterator = engine->getUnclaimedPreparedTransactionsForStartupRecovery(opCtxPtr2.get());
    auto& ru2 = *checked_cast<WiredTigerRecoveryUnit*>(
        shard_role_details::getRecoveryUnit(opCtxPtr2.get()));

    while (auto recoveredPreparedId = iterator->next()) {
        ASSERT_EQ(*recoveredPreparedId, preparedId);
        ru2.beginUnitOfWork(false);
        ru2.setPrepareTimestamp(prepareTimestamp);
        ru2.setPreparedId(*recoveredPreparedId);
        ru2.getSession();  // Note this starts the storage transaction.

        ru2.setDurableTimestamp(Timestamp(3, 0));
        ru2.setCommitTimestamp(prepareTimestamp);
        ru2.commitUnitOfWork();
        count++;
    }
    ASSERT_EQ(count, 1);
}

TEST_F(WiredTigerKVEngineTestWithPreciseCheckpoints,
       MultipleUnresolvedPreparedTransactionsAreVisibleOnStartupRecovery) {
    // Create two unresolved prepared transactions on two separate clients/operation contexts.
    //
    // This must be done on separate clients because a client may only own a single
    // OperationContext, which in turn has a single RecoveryUnit and thus at most one prepared
    // transaction.
    auto clientAndCtx1 = _makeClientAndOperationContext("preparedTxnClient1");
    auto* opCtx1 = clientAndCtx1.second.get();
    auto& ru1 = *shard_role_details::getRecoveryUnit(opCtx1);
    const auto prepareTimestamp1 = Timestamp(2, 0);
    const auto preparedId1 = prepareTimestamp1.asULL();
    createPreparedTransaction(opCtx1, ru1, prepareTimestamp1, preparedId1);
    ASSERT_EQ(ru1.getPrepareTimestamp(), prepareTimestamp1);
    ASSERT_EQ(ru1.getPreparedId().value(), preparedId1);

    auto clientAndCtx2 = _makeClientAndOperationContext("preparedTxnClient2");
    auto* opCtx2 = clientAndCtx2.second.get();
    auto& ru2 = *shard_role_details::getRecoveryUnit(opCtx2);
    const auto prepareTimestamp2 = Timestamp(3, 0);
    const auto preparedId2 = prepareTimestamp2.asULL();
    createPreparedTransaction(opCtx2, ru2, prepareTimestamp2, preparedId2);
    ASSERT_EQ(ru2.getPrepareTimestamp(), prepareTimestamp2);
    ASSERT_EQ(ru2.getPreparedId().value(), preparedId2);

    // Create a checkpoint that includes both prepared transactions.
    auto* engine = _helper->getWiredTigerKVEngine();
    engine->setInitialDataTimestamp(Timestamp(1, 0));
    engine->setStableTimestamp(prepareTimestamp2, /*force=*/false);
    engine->checkpoint();

    // This is necessary to satisfy the destructor of the recovery units which expect to not be in
    // a unit of work when the storage engine is restarted. This does not affect the results of the
    // prepared transaction iterator since the transaction rollbacks are not in the checkpoint.
    auto rollbackTimestamp = Timestamp(4, 0);
    ru1.setRollbackTimestamp(rollbackTimestamp);
    ASSERT_EQ(ru1.getRollbackTimestamp(), rollbackTimestamp);
    ru1.abortUnitOfWork();
    ru2.setRollbackTimestamp(rollbackTimestamp);
    ASSERT_EQ(ru2.getRollbackTimestamp(), rollbackTimestamp);
    ru2.abortUnitOfWork();

    // Release the clients and opCtxs to prevent memory issues when the storage engine is restarted.
    clientAndCtx1.second.reset();
    clientAndCtx1.first.reset();
    clientAndCtx2.second.reset();
    clientAndCtx2.first.reset();

    // Simulate startup recovery by restarting the storage engine.
    _helper->restartEngine();
    engine = _helper->getWiredTigerKVEngine();
    auto opCtxPtr = _makeOperationContext();

    // Verify that we see both prepared transactions on startup recovery.
    auto iterator = engine->getUnclaimedPreparedTransactionsForStartupRecovery(opCtxPtr.get());
    uint64_t firstId = *iterator->next();
    ASSERT_TRUE(firstId == preparedId1 || firstId == preparedId2);
    uint64_t secondId = *iterator->next();
    ASSERT_TRUE(secondId == preparedId1 || secondId == preparedId2);
    ASSERT_NE(firstId, secondId);
    ASSERT_TRUE(!iterator->next());

    // Reclaim the prepared transactions and abort/commit them.
    auto& ru3 =
        *checked_cast<WiredTigerRecoveryUnit*>(shard_role_details::getRecoveryUnit(opCtxPtr.get()));
    ru3.beginUnitOfWork(false);
    ru3.setPrepareTimestamp(prepareTimestamp1);
    ru3.setPreparedId(firstId);
    ru3.getSession();  // Note this starts the storage transaction.
    ru3.setRollbackTimestamp(rollbackTimestamp);
    ru3.abortUnitOfWork();

    ru3.beginUnitOfWork(false);
    ru3.setPrepareTimestamp(prepareTimestamp2);
    ru3.setPreparedId(secondId);
    ru3.getSession();  // Note this starts the storage transaction.
    ru3.setDurableTimestamp(Timestamp(8, 0));
    ru3.setCommitTimestamp(prepareTimestamp2);
    ru3.commitUnitOfWork();
}

TEST_F(WiredTigerKVEngineTestWithPreciseCheckpoints,
       AbortedPreparedTransactionIsNotVisibleOnStartupRecovery) {
    // Create a prepared transaction and abort it at a timestamp that makes it part of the
    // checkpoint.
    auto opCtxPtr = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());
    const auto prepareTimestamp = Timestamp(2, 0);
    const auto preparedId = prepareTimestamp.asULL();
    createPreparedTransaction(opCtxPtr.get(), ru, prepareTimestamp, preparedId);
    ASSERT_EQ(ru.getPrepareTimestamp(), prepareTimestamp);
    ASSERT_EQ(ru.getPreparedId().value(), preparedId);

    auto rollbackTimestamp = Timestamp(4, 0);
    ru.setRollbackTimestamp(rollbackTimestamp);
    ASSERT_EQ(ru.getRollbackTimestamp(), rollbackTimestamp);
    ru.abortUnitOfWork();

    // Create a checkpoint that includes the aborted transaction.
    auto* engine = _helper->getWiredTigerKVEngine();
    engine->setInitialDataTimestamp(Timestamp(1, 0));
    engine->setStableTimestamp(Timestamp(5, 0), /*force=*/false);
    engine->checkpoint();

    // Release the opCtx to prevent memory issues when the storage engine is restarted.
    opCtxPtr.reset();

    // Simulate startup recovery by restarting the storage engine.
    _helper->restartEngine();
    engine = _helper->getWiredTigerKVEngine();
    auto opCtxPtr2 = _makeOperationContext();

    // Verify that we do not see any prepared transactions on startup recovery.
    auto iterator = engine->getUnclaimedPreparedTransactionsForStartupRecovery(opCtxPtr2.get());
    ASSERT_TRUE(!iterator->next());
}

TEST_F(WiredTigerKVEngineTestWithPreciseCheckpoints,
       PreparedTransactionWithAbortPastCheckpointIsVisibleOnStartupRecovery) {
    // Create a prepared transaction and abort it at a timestamp that makes it not a part of the
    // checkpoint.
    auto opCtxPtr = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());
    const auto prepareTimestamp = Timestamp(2, 0);
    const auto preparedId = prepareTimestamp.asULL();
    createPreparedTransaction(opCtxPtr.get(), ru, prepareTimestamp, preparedId);
    ASSERT_EQ(ru.getPrepareTimestamp(), prepareTimestamp);
    ASSERT_EQ(ru.getPreparedId().value(), preparedId);

    auto rollbackTimestamp = Timestamp(8, 0);
    ru.setRollbackTimestamp(rollbackTimestamp);
    ASSERT_EQ(ru.getRollbackTimestamp(), rollbackTimestamp);
    ru.abortUnitOfWork();

    // Create a checkpoint that includes the prepared transaction but not the abort.
    auto* engine = _helper->getWiredTigerKVEngine();
    engine->setInitialDataTimestamp(Timestamp(1, 0));
    engine->setStableTimestamp(Timestamp(5, 0), /*force=*/false);
    engine->checkpoint();

    // Release the opCtx to prevent memory issues when the storage engine is restarted.
    opCtxPtr.reset();

    // Simulate startup recovery by restarting the storage engine.
    _helper->restartEngine();
    engine = _helper->getWiredTigerKVEngine();
    auto opCtxPtr2 = _makeOperationContext();
    auto& ru2 = *checked_cast<WiredTigerRecoveryUnit*>(
        shard_role_details::getRecoveryUnit(opCtxPtr2.get()));

    // Verify that we see the prepared transaction on startup recovery and reclaim it.
    int count = 0;
    auto iterator = engine->getUnclaimedPreparedTransactionsForStartupRecovery(opCtxPtr2.get());
    while (auto recoveredPreparedId = iterator->next()) {
        ASSERT_EQ(*recoveredPreparedId, preparedId);
        ru2.beginUnitOfWork(false);
        // Not strictly necessary to begin a transaction, but used to verify that starting a
        // transaction can handle extra configuration options when claim_prepared_id is set.
        ru2.setPrepareConflictBehavior(PrepareConflictBehavior::kIgnoreConflicts);
        ru2.setPrepareTimestamp(prepareTimestamp);
        ru2.setPreparedId(*recoveredPreparedId);
        ru2.getSession();  // Note this starts the storage transaction.

        ru2.setRollbackTimestamp(rollbackTimestamp);
        ru2.abortUnitOfWork();
        count++;
    }
    ASSERT_EQ(count, 1);
}
using WiredTigerKVEngineTestWithPreciseCheckpointsDeathTest =
    WiredTigerKVEngineTestWithPreciseCheckpoints;
DEATH_TEST_F(WiredTigerKVEngineTestWithPreciseCheckpointsDeathTest,
             UnresolvedPreparedTransactionsMustBeClaimed,
             "Found 1 unclaimed prepared transactions") {
    // Create an unresolved prepared transaction.
    auto opCtxPtr = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());
    const auto prepareTimestamp = Timestamp(2, 0);
    const auto preparedId = prepareTimestamp.asULL();
    createPreparedTransaction(opCtxPtr.get(), ru, prepareTimestamp, preparedId);
    ASSERT_EQ(ru.getPrepareTimestamp(), prepareTimestamp);
    ASSERT_EQ(ru.getPreparedId().value(), preparedId);

    // Create a checkpoint that includes the prepared transaction.
    auto* engine = _helper->getWiredTigerKVEngine();
    engine->setInitialDataTimestamp(Timestamp(1, 0));
    engine->setStableTimestamp(prepareTimestamp, /*force=*/false);
    engine->checkpoint();

    // This is necessary to satisfy the destructor of the recovery unit which expects to not be
    // in a unit of work when the storage engine is restarted. This does not affect the results of
    // the prepared transaction iterator since the transaction rollback is not in the
    // checkpoint.
    auto rollbackTimestamp = Timestamp(3, 0);
    ru.setRollbackTimestamp(rollbackTimestamp);
    ASSERT_EQ(ru.getRollbackTimestamp(), rollbackTimestamp);
    ru.abortUnitOfWork();

    // Release the opCtx to prevent memory issues when the storage engine is restarted.
    opCtxPtr.reset();

    // Simulate startup recovery by restarting the storage engine.
    _helper->restartEngine();
    engine = _helper->getWiredTigerKVEngine();
    auto opCtxPtr2 = _makeOperationContext();

    // Verify that we see the prepared transaction on startup recovery.
    auto iterator = engine->getUnclaimedPreparedTransactionsForStartupRecovery(opCtxPtr2.get());
    auto recoveredPreparedId = iterator->next();
    ASSERT_EQ(*recoveredPreparedId, preparedId);
    ASSERT_TRUE(!iterator->next());

    // Purposely don't reclaim the transaction to verify that destroying the iterator without
    // claiming the prepared transaction results in a crash.
}

TEST_F(WiredTigerKVEngineTest, PinAllDurableTimestamp) {
    auto* engine = _helper->getWiredTigerKVEngine();

    engine->setInitialDataTimestamp(Timestamp(1, 0));
    engine->setOldestTimestamp(Timestamp(1, 0), /*force=*/false);
    engine->setStableTimestamp(Timestamp(5, 0), /*force=*/false);

    auto opCtx = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    // Do a timestamped write so all_durable advances past 0.
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(ru.setTimestamp(Timestamp(100, 0)));
        txn.commit();
    }

    // Pin at the current all_durable.
    auto pinnedTs = engine->getRawAllDurableTimestamp();
    engine->pinAllDurableTimestamp(pinnedTs);

    // Advance the raw all_durable with another timestamped write.
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(ru.setTimestamp(Timestamp(200, 0)));
        txn.commit();
    }

    // The raw value advanced, but getAllDurableTimestamp() should still reflect the pin.
    ASSERT_GTE(engine->getRawAllDurableTimestamp(), Timestamp(200, 0).asULL());
    ASSERT_EQ(engine->getAllDurableTimestamp(), Timestamp(pinnedTs));

    // A second pin at a higher value; the minimum pin should still win.
    auto secondPinTs = Timestamp(200, 0).asULL();
    engine->pinAllDurableTimestamp(secondPinTs);
    ASSERT_EQ(engine->getAllDurableTimestamp(), Timestamp(pinnedTs));

    // Removing the lower pin leaves the higher one in effect.
    engine->unpinAllDurableTimestamp(pinnedTs);
    ASSERT_EQ(engine->getAllDurableTimestamp(), Timestamp(200, 0));

    // Duplicate pin at the same value requires two unpins.
    engine->pinAllDurableTimestamp(secondPinTs);
    engine->unpinAllDurableTimestamp(secondPinTs);
    ASSERT_EQ(engine->getAllDurableTimestamp(), Timestamp(200, 0));

    // Advance again past the remaining pin.
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(ru.setTimestamp(Timestamp(300, 0)));
        txn.commit();
    }
    ASSERT_EQ(engine->getAllDurableTimestamp(), Timestamp(200, 0));

    // Removing the last pin lets getAllDurableTimestamp() catch up.
    engine->unpinAllDurableTimestamp(secondPinTs);
    ASSERT_GTE(engine->getAllDurableTimestamp(), Timestamp(300, 0));
}

TEST_F(WiredTigerKVEngineTest, AllowUntimestampedWritesWithServerParam) {
    auto initialStorageGlobalParamsDotMagicRestore = storageGlobalParams.magicRestore;
    ON_BLOCK_EXIT([initialStorageGlobalParamsDotMagicRestore] {
        storageGlobalParams.magicRestore = initialStorageGlobalParamsDotMagicRestore;
    });
    unittest::ServerParameterGuard controller("allowUnsafeUntimestampedWrites", true);

    storageGlobalParams.magicRestore = false;
    // Standalone mode without oplog recovery is the only case that permits untimestamped writes.
    ASSERT_FALSE(allowUntimestampedWrites(true, true));
    ASSERT_TRUE(allowUntimestampedWrites(true, false));
    ASSERT_FALSE(allowUntimestampedWrites(false, true));
    ASSERT_FALSE(allowUntimestampedWrites(false, false));
    // Classic magic restore unconditionally allows untimestamped writes.
    storageGlobalParams.magicRestore = true;
    ASSERT_TRUE(allowUntimestampedWrites(true, true));
    ASSERT_TRUE(allowUntimestampedWrites(true, false));
    ASSERT_TRUE(allowUntimestampedWrites(false, true));
    ASSERT_TRUE(allowUntimestampedWrites(false, false));

    // DSC magic restore is not classic, so it falls through to the normal standalone logic.
    class DSCMagicRestoreStub : public rss::StubPersistenceProvider {
    public:
        bool supportsClassicMagicRestore() const override {
            return false;
        }
    };
    rss::ReplicatedStorageService::get(getServiceContext())
        .setPersistenceProvider(std::make_unique<DSCMagicRestoreStub>());
    storageGlobalParams.magicRestore = false;
    ASSERT_FALSE(allowUntimestampedWrites(true, true));
    ASSERT_TRUE(allowUntimestampedWrites(true, false));
    ASSERT_FALSE(allowUntimestampedWrites(false, true));
    ASSERT_FALSE(allowUntimestampedWrites(false, false));
    storageGlobalParams.magicRestore = true;
    ASSERT_FALSE(allowUntimestampedWrites(true, true));
    ASSERT_TRUE(allowUntimestampedWrites(true, false));
    ASSERT_FALSE(allowUntimestampedWrites(false, true));
    ASSERT_FALSE(allowUntimestampedWrites(false, false));
}

TEST_F(WiredTigerKVEngineTest, AllowUntimestampedWritesWithoutServerParam) {
    auto initialStorageGlobalParamsDotMagicRestore = storageGlobalParams.magicRestore;
    ON_BLOCK_EXIT([initialStorageGlobalParamsDotMagicRestore] {
        storageGlobalParams.magicRestore = initialStorageGlobalParamsDotMagicRestore;
    });
    unittest::ServerParameterGuard controller("allowUnsafeUntimestampedWrites", false);

    storageGlobalParams.magicRestore = false;
    // Standalone mode without oplog recovery is the only case that permits untimestamped writes.
    ASSERT_FALSE(allowUntimestampedWrites(true, true));
    ASSERT_FALSE(allowUntimestampedWrites(true, false));
    ASSERT_FALSE(allowUntimestampedWrites(false, true));
    ASSERT_FALSE(allowUntimestampedWrites(false, false));
    // Classic magic restore unconditionally allows untimestamped writes.
    storageGlobalParams.magicRestore = true;
    ASSERT_TRUE(allowUntimestampedWrites(true, true));
    ASSERT_TRUE(allowUntimestampedWrites(true, false));
    ASSERT_TRUE(allowUntimestampedWrites(false, true));
    ASSERT_TRUE(allowUntimestampedWrites(false, false));

    // DSC magic restore is not classic, so it falls through to the normal standalone logic.
    class DSCMagicRestoreStub : public rss::StubPersistenceProvider {
    public:
        bool supportsClassicMagicRestore() const override {
            return false;
        }
    };
    rss::ReplicatedStorageService::get(getServiceContext())
        .setPersistenceProvider(std::make_unique<DSCMagicRestoreStub>());
    storageGlobalParams.magicRestore = false;
    ASSERT_FALSE(allowUntimestampedWrites(true, true));
    ASSERT_FALSE(allowUntimestampedWrites(true, false));
    ASSERT_FALSE(allowUntimestampedWrites(false, true));
    ASSERT_FALSE(allowUntimestampedWrites(false, false));
    storageGlobalParams.magicRestore = true;
    ASSERT_FALSE(allowUntimestampedWrites(true, true));
    ASSERT_FALSE(allowUntimestampedWrites(true, false));
    ASSERT_FALSE(allowUntimestampedWrites(false, true));
    ASSERT_FALSE(allowUntimestampedWrites(false, false));
}

TEST_F(WiredTigerKVEngineTest, SetStorageTierCold) {
    auto* engine = _helper->getWiredTigerKVEngine();
    auto result = engine->setStorageTierToStorageOptions(BSONObj(), StorageTierLevelEnum::cold);
    // Verify the returned storage options contain the cold tier WT config.
    auto wtObj = result.getObjectField("wiredTiger");
    auto configString = wtObj.getStringField("configString");
    ASSERT_STRING_CONTAINS(configString, "storage_tier=cold");
    ASSERT_STRING_CONTAINS(configString, "leaf_page_max=128KB");
}

TEST_F(WiredTigerKVEngineTest, IsColdCollectionRecordStore) {
    auto opCtxPtr = _makeOperationContext();
    auto* engine = _helper->getWiredTigerKVEngine();
    auto& provider = rss::ReplicatedStorageService::get(opCtxPtr.get()).getPersistenceProvider();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());

    // Create a normal (non-cold) collection and verify isColdCollection is false.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.hot");
    std::string ident = "collection-hot";
    RecordStore::Options options;
    {
        StorageWriteTransaction swt(ru);
        ASSERT_OK(engine->createRecordStore(provider, ru, nss, ident, options));
        swt.commit();
    }

    auto hotRs = engine->getRecordStore(opCtxPtr.get(), nss, ident, options, UUID::gen());
    ASSERT(hotRs);
    ASSERT_FALSE(hotRs->isColdCollection());

    // Verify that constructing a WiredTigerRecordStore with isColdCollection=true in Params
    // results in isColdCollection() returning true.
    WiredTigerRecordStore::Params params;
    params.uuid = UUID::gen();
    params.ident = ident;
    params.engineName = std::string{kWiredTigerEngineName};
    params.keyFormat = KeyFormat::Long;
    params.overwrite = true;
    params.isLogged = false;
    params.forceUpdateWithFullDocument = false;
    params.inMemory = false;
    params.sizeStorer = nullptr;
    params.tracksSizeAdjustments = false;
    params.isColdCollection = true;

    auto coldRs = std::make_unique<WiredTigerRecordStore>(
        engine,
        WiredTigerRecoveryUnit::get(*shard_role_details::getRecoveryUnit(opCtxPtr.get())),
        params);
    ASSERT_TRUE(coldRs->isColdCollection());
}

// Creates a record store via getRecordStore(), inserts a few records, and reads the in-memory
// _sizeInfo, which _changeNumRecordsAndDataSize only mutates when the store was created with
// tracksSizeAdjustments=true.
int64_t insertRecordsAndGetSizeInfoCount(WiredTigerKVEngine* engine,
                                         OperationContext* opCtx,
                                         RecoveryUnit& ru,
                                         const NamespaceString& nss,
                                         const std::string& ident) {
    auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
    const RecordStore::Options rsOptions;
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(engine->createRecordStore(provider, ru, nss, ident, rsOptions));
        txn.commit();
    }
    auto rs = engine->getRecordStore(opCtx, nss, ident, rsOptions, UUID::gen());
    ASSERT(rs);

    constexpr int64_t kNumRecords = 3;
    {
        StorageWriteTransaction txn(ru);
        for (int64_t i = 0; i < kNumRecords; ++i) {
            const std::string doc = "record";
            ASSERT_OK(
                rs->insertRecord(opCtx, ru, doc.c_str(), doc.size() + 1, Timestamp()).getStatus());
        }
        txn.commit();
    }
    return rs->numRecords();
}

TEST_F(WiredTigerKVEngineTest, GetRecordStoreTracksSizeAdjustmentsWhenNotUsingReplicatedFastCount) {
    auto* engine = _helper->getWiredTigerKVEngine();
    auto opCtxPtr = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());

    // The attached provider does not use replicated fast count, so getRecordStore() sets
    // tracksSizeAdjustments=true and inserts are reflected in the sizeInfo.
    ASSERT_FALSE(rss::ReplicatedStorageService::get(opCtxPtr.get())
                     .getPersistenceProvider()
                     .shouldUseReplicatedFastCount());
    ASSERT_EQ(3,
              insertRecordsAndGetSizeInfoCount(
                  engine,
                  opCtxPtr.get(),
                  ru,
                  NamespaceString::createNamespaceString_forTest("test.tracked"),
                  "collection-tracked"));
}

TEST_F(WiredTigerKVEngineTest,
       GetRecordStoreDoesNotTrackSizeAdjustmentsWhenUsingReplicatedFastCount) {
    auto* engine = _helper->getWiredTigerKVEngine();
    auto opCtxPtr = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());

    // A provider that uses replicated fast count makes getRecordStore() set
    // tracksSizeAdjustments=false, so _changeNumRecordsAndDataSize is a no-op and the sizeInfo
    // stays zero even though records were inserted.
    class ReplicatedFastCountProvider : public rss::AttachedPersistenceProvider {
    public:
        bool shouldUseReplicatedFastCount() const override {
            return true;
        }
    };
    rss::ReplicatedStorageService::get(getServiceContext())
        .setPersistenceProvider(std::make_unique<ReplicatedFastCountProvider>());
    ASSERT_EQ(0,
              insertRecordsAndGetSizeInfoCount(
                  engine,
                  opCtxPtr.get(),
                  ru,
                  NamespaceString::createNamespaceString_forTest("test.untracked"),
                  "collection-untracked"));
}

TEST_F(WiredTigerKVEngineTest, GetStorageTierFromStorageOptionsNone) {
    auto* engine = _helper->getWiredTigerKVEngine();
    // WiredTiger's default config string uses storage_tier=none, which should be treated as unset.
    auto options =
        BSON("wiredTiger" << BSON("configString" << "disaggregated=(storage_tier=none)"));
    ASSERT_EQ(engine->getStorageTierFromStorageOptions(options), boost::none);
}

TEST_F(WiredTigerKVEngineTest, GetStorageTierFromStorageOptionsCold) {
    auto* engine = _helper->getWiredTigerKVEngine();
    auto options =
        BSON("wiredTiger" << BSON("configString" << "disaggregated=(storage_tier=cold)"));
    auto result = engine->getStorageTierFromStorageOptions(options);
    ASSERT(result);
    ASSERT_EQ(*result, StorageTierLevelEnum::cold);
}

TEST_F(WiredTigerKVEngineTest, GetStorageTierFromStorageOptionsEmpty) {
    auto* engine = _helper->getWiredTigerKVEngine();
    ASSERT_EQ(engine->getStorageTierFromStorageOptions(BSONObj()), boost::none);
}

// Minimal FlushAllFilesObserver that records how many times it was notified.
class CountingFlushAllFilesObserver : public FlushAllFilesObserver {
public:
    void onFlushAllFiles() override {
        ++timesNotified;
    }

    int timesNotified = 0;
};

TEST_F(WiredTigerKVEngineTest, FlushAllFilesObserverRoundTrip) {
    auto* engine = _helper->getWiredTigerKVEngine();
    ASSERT_EQ(nullptr, engine->getFlushAllFilesObserver());

    CountingFlushAllFilesObserver observer;
    engine->setFlushAllFilesObserver(&observer);
    ASSERT_EQ(&observer, engine->getFlushAllFilesObserver());

    engine->setFlushAllFilesObserver(nullptr);
    ASSERT_EQ(nullptr, engine->getFlushAllFilesObserver());
}

TEST_F(WiredTigerKVEngineTest, FlushAllFilesNotifiesRegisteredObserver) {
    auto* engine = _helper->getWiredTigerKVEngine();
    auto opCtx = _makeOperationContext();

    // Give flushAllFiles() a coherent stable timestamp so its checkpoint is well-defined.
    engine->setInitialDataTimestamp(Timestamp(1, 1));
    engine->setStableTimestamp(Timestamp(1, 1), false);

    CountingFlushAllFilesObserver observer;
    engine->setFlushAllFilesObserver(&observer);
    engine->flushAllFiles(opCtx.get(), /*callerHoldsReadLock=*/false);
    ASSERT_EQ(1, observer.timesNotified);

    // After the observer is cleared, flushAllFiles() must neither notify nor crash.
    engine->setFlushAllFilesObserver(nullptr);
    engine->flushAllFiles(opCtx.get(), /*callerHoldsReadLock=*/false);
    ASSERT_EQ(1, observer.timesNotified);
}

/**
 * A test helper which uses a custom collator to lock and unlock the WiredTiger schema lock. This
 * takes advantage of that the customize callback is invoked with the locks held to enable us to
 * test lock_wait=false behavior without relying on timing.
 *
 * As WiredTiger does not have a remove_collator() function, the WiredTigerKVEngine passed to this
 * must be destroyed *before* this object.
 */
class BlockingCollator : WT_COLLATOR {
public:
    BlockingCollator(WiredTigerKVEngine& engine) : _engine(engine) {
        compare = [](auto...) {
            return 0;
        };
        customize = [](WT_COLLATOR* collator, auto...) {
            auto* self = static_cast<BlockingCollator*>(collator);
            std::unique_lock lk(self->_mutex);
            self->_started = true;
            self->_cv.notify_all();
            self->_cv.wait(lk, [&] { return self->_released; });
            return 0;
        };
        terminate = nullptr;

        WT_CONNECTION* conn = engine.getConn();
        ASSERT_EQ(conn->add_collator(conn, "lockBusyTestCollator", this, nullptr), 0);
    }

    /**
     * Acquires the WiredTiger schema lock. Must not be called while already locked.
     */
    void lock() {
        std::unique_lock lk(_mutex);
        invariant(!_thread.joinable());
        _thread = unittest::JoinThread([&] {
            WiredTigerSession session(&_engine.getConnection());
            ASSERT_EQ(session.create("table:lockBusyTestCollatorTable",
                                     "key_format=S,value_format=S,collator=lockBusyTestCollator"),
                      0);
        });
        _cv.wait(lk, [&] { return _started; });
    }

    /**
     * Releases the WiredTiger schema lock. Must not be called when not locked.
     */
    void unlock() {
        {
            std::lock_guard lk(_mutex);
            _released = true;
        }
        _cv.notify_all();
        _thread.join();
    }

private:
    WiredTigerKVEngine& _engine;
    std::mutex _mutex;
    std::condition_variable _cv;
    unittest::JoinThread _thread;
    bool _started = false;
    bool _released = false;
};

TEST_F(WiredTigerKVEngineTest, DropIdentReturnsLockBusyWhenSchemaLockHeld) {
    auto opCtxPtr = _makeOperationContext();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    std::string ident = "collection-1234";
    RecordStore::Options options;

    auto& provider = rss::ReplicatedStorageService::get(opCtxPtr.get()).getPersistenceProvider();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());
    {
        StorageWriteTransaction swt(ru);
        ASSERT_OK(
            _helper->getWiredTigerKVEngine()->createRecordStore(provider, ru, nss, ident, options));
        swt.commit();
    }

    // Dropping a collection might fail if we haven't checkpointed the data.
    _helper->getWiredTigerKVEngine()->checkpoint();

    auto* engine = _helper->getWiredTigerKVEngine();

    BlockingCollator blockingCollator(*engine);
    auto status = [&] {
        std::lock_guard lock(blockingCollator);
        return engine->dropIdent(*shard_role_details::getRecoveryUnit(opCtxPtr.get()),
                                 ident,
                                 /*identHasSizeInfo=*/true,
                                 /*onDrop=*/nullptr,
                                 /*schemaEpoch=*/boost::none,
                                 /*waitForLocks=*/false);
    }();
    EXPECT_EQ(status, ErrorCodes::LockBusy);

    // The WiredTigerKVEngine must be destroyed before the test collator as WiredTiger holds a
    // pointer to it and will call terminate() on teardown
    opCtxPtr.reset();
    _helper.reset();
}

}  // namespace
}  // namespace mongo
