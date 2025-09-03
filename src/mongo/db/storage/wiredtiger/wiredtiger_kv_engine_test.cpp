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

#include <ostream>
#include <utility>

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/checkpointer.h"
#include "mongo/db/storage/kv/kv_engine_test_harness.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/storage_engine_impl.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/version/releases.h"

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

#if __has_feature(address_sanitizer)
constexpr bool kMemLeakAllowed = false;
#else
constexpr bool kMemLeakAllowed = true;
#endif

class WiredTigerKVHarnessHelper : public KVHarnessHelper {
public:
    WiredTigerKVHarnessHelper(ServiceContext* svcCtx, bool forRepair = false)
        : _svcCtx(svcCtx), _dbpath("wt-kv-harness"), _forRepair(forRepair) {
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
        wtConfig.extraOpenOptions = "log=(file_max=1m,prealloc=false)";
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
};

class WiredTigerKVEngineTest : public ServiceContextTest {
public:
    WiredTigerKVEngineTest(bool repair = false) : _helper(getServiceContext(), repair) {}

protected:
    ServiceContext::UniqueOperationContext _makeOperationContext() {
        auto opCtx = makeOperationContext();
        shard_role_details::setRecoveryUnit(opCtx.get(),
                                            _helper.getEngine()->newRecoveryUnit(),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        return opCtx;
    }

    WiredTigerKVHarnessHelper _helper;
};

class WiredTigerKVEngineRepairTest : public WiredTigerKVEngineTest {
public:
    WiredTigerKVEngineRepairTest() : WiredTigerKVEngineTest(true /* repair */) {}
};

TEST_F(WiredTigerKVEngineRepairTest, OrphanedDataFilesCanBeRecovered) {
    auto opCtxPtr = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());

    std::string ident = "collection-1234";
    RecordStore::Options options;
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    auto& provider = rss::ReplicatedStorageService::get(opCtxPtr.get()).getPersistenceProvider();
    ASSERT_OK(_helper.getWiredTigerKVEngine()->createRecordStore(provider, nss, ident, options));
    auto rs = _helper.getWiredTigerKVEngine()->getRecordStore(
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
        _helper.getWiredTigerKVEngine()->getDataFilePathForIdent(ident);
    ASSERT(dataFilePath);

    ASSERT(boost::filesystem::exists(*dataFilePath));

    const boost::filesystem::path tmpFile{dataFilePath->string() + ".tmp"};
    ASSERT(!boost::filesystem::exists(tmpFile));

#ifdef _WIN32
    auto status =
        _helper.getWiredTigerKVEngine()->recoverOrphanedIdent(provider, nss, ident, options);
    ASSERT_EQ(ErrorCodes::CommandNotSupported, status.code());
#else

    // Dropping a collection might fail if we haven't checkpointed the data.
    _helper.getWiredTigerKVEngine()->checkpoint();

    // Move the data file out of the way so the ident can be dropped. This not permitted on Windows
    // because the file cannot be moved while it is open. The implementation for orphan recovery is
    // also not implemented on Windows for this reason.
    boost::system::error_code err;
    boost::filesystem::rename(*dataFilePath, tmpFile, err);
    ASSERT(!err) << err.message();

    ASSERT_OK(_helper.getWiredTigerKVEngine()->dropIdent(
        *shard_role_details::getRecoveryUnit(opCtxPtr.get()), ident, /*identHasSizeInfo=*/true));

    // The data file is moved back in place so that it becomes an "orphan" of the storage
    // engine and the restoration process can be tested.
    boost::filesystem::rename(tmpFile, *dataFilePath, err);
    ASSERT(!err) << err.message();

    auto status =
        _helper.getWiredTigerKVEngine()->recoverOrphanedIdent(provider, nss, ident, options);
    ASSERT_EQ(ErrorCodes::DataModifiedByRepair, status.code());
#endif
}

TEST_F(WiredTigerKVEngineRepairTest, UnrecoverableOrphanedDataFilesAreRebuilt) {
    auto opCtxPtr = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxPtr.get());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    std::string ident = "collection-1234";
    RecordStore::Options options;
    auto& provider = rss::ReplicatedStorageService::get(opCtxPtr.get()).getPersistenceProvider();
    ASSERT_OK(_helper.getWiredTigerKVEngine()->createRecordStore(provider, nss, ident, options));

    UUID uuid = UUID::gen();
    auto rs =
        _helper.getWiredTigerKVEngine()->getRecordStore(opCtxPtr.get(), nss, ident, options, uuid);
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
        _helper.getWiredTigerKVEngine()->getDataFilePathForIdent(ident);
    ASSERT(dataFilePath);

    ASSERT(boost::filesystem::exists(*dataFilePath));

    // Dropping a collection might fail if we haven't checkpointed the data
    _helper.getWiredTigerKVEngine()->checkpoint();

    ASSERT_OK(_helper.getWiredTigerKVEngine()->dropIdent(
        *shard_role_details::getRecoveryUnit(opCtxPtr.get()), ident, /*identHasSizeInfo=*/true));

#ifdef _WIN32
    auto status =
        _helper.getWiredTigerKVEngine()->recoverOrphanedIdent(provider, nss, ident, options);
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
    auto status =
        _helper.getWiredTigerKVEngine()->recoverOrphanedIdent(provider, nss, ident, options);
    ASSERT_EQ(ErrorCodes::DataModifiedByRepair, status.code()) << status.reason();

    boost::filesystem::path corruptFile = (dataFilePath->string() + ".corrupt");
    ASSERT(boost::filesystem::exists(corruptFile));

    rs = _helper.getWiredTigerKVEngine()->getRecordStore(opCtxPtr.get(), nss, ident, options, uuid);
    RecordData data;
    ASSERT_FALSE(rs->findRecord(
        opCtxPtr.get(), *shard_role_details::getRecoveryUnit(opCtxPtr.get()), loc, &data));
#endif
}

TEST_F(WiredTigerKVEngineTest, TestOplogTruncation) {
    // To diagnose any intermittent failures, maximize logging from WiredTigerKVEngine and friends.
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kStorage,
                                                              logv2::LogSeverity::Debug(3)};

    // Set syncdelay before starting the checkpoint thread, otherwise it can observe the default
    // checkpoint frequency of 60 seconds, causing the test to fail due to a 10 second timeout.
    storageGlobalParams.syncdelay.store(1);

    std::unique_ptr<Checkpointer> checkpointer = std::make_unique<Checkpointer>();
    checkpointer->go();

    // If the test fails we want to ensure the checkpoint thread shuts down to avoid accessing the
    // storage engine during shutdown.
    ON_BLOCK_EXIT(
        [&] { checkpointer->shutdown({ErrorCodes::ShutdownInProgress, "Test finished"}); });

    auto opCtxPtr = _makeOperationContext();
    // The initial data timestamp has to be set to take stable checkpoints. The first stable
    // timestamp greater than this will also trigger a checkpoint. The following loop of the
    // CheckpointThread will observe the new `syncdelay` value.
    _helper.getWiredTigerKVEngine()->setInitialDataTimestamp(Timestamp(1, 1));

    // Simulate the callback that queries config.transactions for the oldest active transaction.
    boost::optional<Timestamp> oldestActiveTxnTimestamp;
    AtomicWord<bool> callbackShouldFail{false};
    auto callback = [&](Timestamp stableTimestamp) {
        using ResultType = StorageEngine::OldestActiveTransactionTimestampResult;
        if (callbackShouldFail.load()) {
            return ResultType(ErrorCodes::ExceededTimeLimit, "timeout");
        }

        return ResultType(oldestActiveTxnTimestamp);
    };

    _helper.getWiredTigerKVEngine()->setOldestActiveTransactionTimestampCallback(callback);

    // A method that will poll the WiredTigerKVEngine until it sees the amount of oplog necessary
    // for crash recovery exceeds the input.
    auto assertPinnedMovesSoon = [this](Timestamp newPinned) {
        // If the current oplog needed for rollback does not exceed the requested pinned out, we
        // cannot expect the CheckpointThread to eventually publish a sufficient crash recovery
        // value.
        auto needed = _helper.getWiredTigerKVEngine()->getOplogNeededForRollback();
        if (needed.isOK()) {
            ASSERT_TRUE(needed.getValue() >= newPinned);
        }

        // Do 100 iterations that sleep for 100 milliseconds between polls. This will wait for up
        // to 10 seconds to observe an asynchronous update that iterates once per second.
        for (auto iterations = 0; iterations < 100; ++iterations) {
            if (_helper.getWiredTigerKVEngine()->getPinnedOplog() >= newPinned) {
                ASSERT_TRUE(
                    _helper.getWiredTigerKVEngine()->getOplogNeededForCrashRecovery().value() >=
                    newPinned);
                return;
            }

            sleepmillis(100);
        }

        LOGV2(22367,
              "Expected the pinned oplog to advance.",
              "expectedValue"_attr = newPinned,
              "publishedValue"_attr =
                  _helper.getWiredTigerKVEngine()->getOplogNeededForCrashRecovery());
        FAIL("");
    };

    oldestActiveTxnTimestamp = boost::none;
    _helper.getWiredTigerKVEngine()->setStableTimestamp(Timestamp(10, 1), false);
    assertPinnedMovesSoon(Timestamp(10, 1));

    oldestActiveTxnTimestamp = Timestamp(15, 1);
    _helper.getWiredTigerKVEngine()->setStableTimestamp(Timestamp(20, 1), false);
    assertPinnedMovesSoon(Timestamp(15, 1));

    oldestActiveTxnTimestamp = Timestamp(19, 1);
    _helper.getWiredTigerKVEngine()->setStableTimestamp(Timestamp(30, 1), false);
    assertPinnedMovesSoon(Timestamp(19, 1));

    oldestActiveTxnTimestamp = boost::none;
    _helper.getWiredTigerKVEngine()->setStableTimestamp(Timestamp(30, 1), false);
    assertPinnedMovesSoon(Timestamp(30, 1));

    callbackShouldFail.store(true);
    ASSERT_NOT_OK(_helper.getWiredTigerKVEngine()->getOplogNeededForRollback());
    _helper.getWiredTigerKVEngine()->setStableTimestamp(Timestamp(40, 1), false);
    // Await a new checkpoint. Oplog needed for rollback does not advance.
    sleepmillis(1100);
    ASSERT_EQ(_helper.getWiredTigerKVEngine()->getOplogNeededForCrashRecovery().value(),
              Timestamp(30, 1));
    _helper.getWiredTigerKVEngine()->setStableTimestamp(Timestamp(30, 1), false);
    callbackShouldFail.store(false);
    assertPinnedMovesSoon(Timestamp(40, 1));
}

TEST_F(WiredTigerKVEngineTest, CreateRecordStoreFailsWithExistingIdent) {
    auto opCtxPtr = _makeOperationContext();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("a.b");
    std::string ident = "collection-1234";
    RecordStore::Options options;
    auto& provider = rss::ReplicatedStorageService::get(opCtxPtr.get()).getPersistenceProvider();
    ASSERT_OK(_helper.getWiredTigerKVEngine()->createRecordStore(provider, nss, ident, options));

    // A new record store must always have its own storage table uniquely identified by the ident.
    // Otherwise, multiple record stores could point to the same storage resource and lead to data
    // corruption.
    //
    // Validate the server throws when trying to create a new record store with an ident already in
    // use.

    const auto status =
        _helper.getWiredTigerKVEngine()->createRecordStore(provider, nss, ident, options);
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
    ASSERT_OK(_helper.getWiredTigerKVEngine()->createRecordStore(provider, nss, ident, options));

    const boost::optional<boost::filesystem::path> dataFilePath =
        _helper.getWiredTigerKVEngine()->getDataFilePathForIdent(ident);
    ASSERT(dataFilePath);
    ASSERT(boost::filesystem::exists(*dataFilePath));

    _helper.getWiredTigerKVEngine()->dropIdentForImport(
        *opCtxPtr.get(), *shard_role_details::getRecoveryUnit(opCtxPtr.get()), ident);
    ASSERT(boost::filesystem::exists(*dataFilePath));

    // Because the underlying file was not removed, it will be renamed out of the way by WiredTiger
    // when creating a new table with the same ident.
    ASSERT_OK(_helper.getWiredTigerKVEngine()->createRecordStore(provider, nss, ident, options));

    const boost::filesystem::path renamedFilePath = dataFilePath->generic_string() + ".1";
    ASSERT(boost::filesystem::exists(*dataFilePath));
    ASSERT(boost::filesystem::exists(renamedFilePath));

    ASSERT_OK(_helper.getWiredTigerKVEngine()->dropIdent(
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
    _helper.getWiredTigerKVEngine()->setOldestTimestamp(initTs, false);
    ASSERT_EQ(initTs, _helper.getWiredTigerKVEngine()->getOldestTimestamp());

    // Assert that advancing the oldest timestamp still succeeds.
    _helper.getWiredTigerKVEngine()->setOldestTimestamp(initTs + 1, false);
    ASSERT_EQ(initTs + 1, _helper.getWiredTigerKVEngine()->getOldestTimestamp());

    // Error if there's a request to pin the oldest timestamp earlier than what it is already set
    // as. This error case is not exercised in this test.
    const bool roundUpIfTooOld = false;
    // Pin the oldest timestamp to "3".
    auto pinnedTs = unittest::assertGet(_helper.getWiredTigerKVEngine()->pinOldestTimestamp(
        *shard_role_details::getRecoveryUnit(opCtxRaii.get()), "A", initTs + 3, roundUpIfTooOld));
    // Assert that the pinning method returns the same timestamp as was requested.
    ASSERT_EQ(initTs + 3, pinnedTs);
    // Assert that pinning the oldest timestamp does not advance it.
    ASSERT_EQ(initTs + 1, _helper.getWiredTigerKVEngine()->getOldestTimestamp());

    // Attempt to advance the oldest timestamp to "5".
    _helper.getWiredTigerKVEngine()->setOldestTimestamp(initTs + 5, false);
    // Observe the oldest timestamp was pinned at the requested "3".
    ASSERT_EQ(initTs + 3, _helper.getWiredTigerKVEngine()->getOldestTimestamp());

    // Unpin the oldest timestamp. Assert that unpinning does not advance the oldest timestamp.
    _helper.getWiredTigerKVEngine()->unpinOldestTimestamp("A");
    ASSERT_EQ(initTs + 3, _helper.getWiredTigerKVEngine()->getOldestTimestamp());

    // Now advancing the oldest timestamp to "5" succeeds.
    _helper.getWiredTigerKVEngine()->setOldestTimestamp(initTs + 5, false);
    ASSERT_EQ(initTs + 5, _helper.getWiredTigerKVEngine()->getOldestTimestamp());
}

/**
 * Demonstrate that multiple actors can request different pins of the oldest timestamp. The minimum
 * of all active requests will be obeyed.
 */
TEST_F(WiredTigerKVEngineTest, TestMultiPinOldestTimestamp) {
    auto opCtxRaii = _makeOperationContext();
    const Timestamp initTs = Timestamp(1, 0);

    _helper.getWiredTigerKVEngine()->setOldestTimestamp(initTs, false);
    ASSERT_EQ(initTs, _helper.getWiredTigerKVEngine()->getOldestTimestamp());

    // Error if there's a request to pin the oldest timestamp earlier than what it is already set
    // as. This error case is not exercised in this test.
    const bool roundUpIfTooOld = false;
    // Have "A" pin the timestamp to "1".
    auto pinnedTs = unittest::assertGet(_helper.getWiredTigerKVEngine()->pinOldestTimestamp(
        *shard_role_details::getRecoveryUnit(opCtxRaii.get()), "A", initTs + 1, roundUpIfTooOld));
    ASSERT_EQ(initTs + 1, pinnedTs);
    ASSERT_EQ(initTs, _helper.getWiredTigerKVEngine()->getOldestTimestamp());

    // Have "B" pin the timestamp to "2".
    pinnedTs = unittest::assertGet(_helper.getWiredTigerKVEngine()->pinOldestTimestamp(
        *shard_role_details::getRecoveryUnit(opCtxRaii.get()), "B", initTs + 2, roundUpIfTooOld));
    ASSERT_EQ(initTs + 2, pinnedTs);
    ASSERT_EQ(initTs, _helper.getWiredTigerKVEngine()->getOldestTimestamp());

    // Advancing the oldest timestamp to "5" will only succeed in advancing it to "1".
    _helper.getWiredTigerKVEngine()->setOldestTimestamp(initTs + 5, false);
    ASSERT_EQ(initTs + 1, _helper.getWiredTigerKVEngine()->getOldestTimestamp());

    // After unpinning "A" at "1", advancing the oldest timestamp will be pinned to "2".
    _helper.getWiredTigerKVEngine()->unpinOldestTimestamp("A");
    _helper.getWiredTigerKVEngine()->setOldestTimestamp(initTs + 5, false);
    ASSERT_EQ(initTs + 2, _helper.getWiredTigerKVEngine()->getOldestTimestamp());

    // Unpinning "B" at "2" allows the oldest timestamp to advance freely.
    _helper.getWiredTigerKVEngine()->unpinOldestTimestamp("B");
    _helper.getWiredTigerKVEngine()->setOldestTimestamp(initTs + 5, false);
    ASSERT_EQ(initTs + 5, _helper.getWiredTigerKVEngine()->getOldestTimestamp());
}

/**
 * Test error cases where a request to pin the oldest timestamp uses a value that's too early
 * relative to the current oldest timestamp.
 */
TEST_F(WiredTigerKVEngineTest, TestPinOldestTimestampErrors) {
    auto opCtxRaii = _makeOperationContext();
    const Timestamp initTs = Timestamp(10, 0);

    _helper.getWiredTigerKVEngine()->setOldestTimestamp(initTs, false);
    ASSERT_EQ(initTs, _helper.getWiredTigerKVEngine()->getOldestTimestamp());

    const bool roundUpIfTooOld = true;
    // The false value means using this variable will cause the method to fail on error.
    const bool failOnError = false;

    // When rounding on error, the pin will succeed, but the return value will be the current oldest
    // timestamp instead of the requested value.
    auto pinnedTs = unittest::assertGet(_helper.getWiredTigerKVEngine()->pinOldestTimestamp(
        *shard_role_details::getRecoveryUnit(opCtxRaii.get()), "A", initTs - 1, roundUpIfTooOld));
    ASSERT_EQ(initTs, pinnedTs);
    ASSERT_EQ(initTs, _helper.getWiredTigerKVEngine()->getOldestTimestamp());

    // Using "fail on error" will result in a not-OK return value.
    ASSERT_NOT_OK(_helper.getWiredTigerKVEngine()->pinOldestTimestamp(
        *shard_role_details::getRecoveryUnit(opCtxRaii.get()), "B", initTs - 1, failOnError));
    ASSERT_EQ(initTs, _helper.getWiredTigerKVEngine()->getOldestTimestamp());
}

/**
 * Test the various cases for the relationship between oldestTimestamp and stableTimestamp at the
 * end of startup recovery.
 */
TEST_F(WiredTigerKVEngineTest, TestOldestStableTimestampEndOfStartupRecovery) {
    auto opCtxRaii = _makeOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtxRaii.get());

    // oldest and stable are both null.
    ASSERT_DOES_NOT_THROW(_helper.getWiredTigerKVEngine()->notifyReplStartupRecoveryComplete(ru));

    // oldest is null, stable is not null.
    const Timestamp initTs = Timestamp(10, 0);
    _helper.getWiredTigerKVEngine()->setStableTimestamp(initTs, true);
    ASSERT_DOES_NOT_THROW(_helper.getWiredTigerKVEngine()->notifyReplStartupRecoveryComplete(ru));

    // oldest and stable equal.
    _helper.getWiredTigerKVEngine()->setOldestTimestamp(initTs, true);
    ASSERT_DOES_NOT_THROW(_helper.getWiredTigerKVEngine()->notifyReplStartupRecoveryComplete(ru));

    // stable > oldest.
    Timestamp laterTs = Timestamp(15, 0);
    _helper.getWiredTigerKVEngine()->setStableTimestamp(laterTs, true);
    ASSERT_DOES_NOT_THROW(_helper.getWiredTigerKVEngine()->notifyReplStartupRecoveryComplete(ru));

    // oldest > stable.
    laterTs = Timestamp(20, 0);
    _helper.getWiredTigerKVEngine()->setOldestTimestamp(laterTs, true);
    ASSERT_THROWS_CODE(_helper.getWiredTigerKVEngine()->notifyReplStartupRecoveryComplete(ru),
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
    _helper.getWiredTigerKVEngine()->setOldestTimestamp(initTs, true);
    ASSERT_DOES_NOT_THROW(_helper.getWiredTigerKVEngine()->notifyReplStartupRecoveryComplete(
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
        ASSERT_OK(_helper.getWiredTigerKVEngine()->reconfigureLogging());
        // Perform a checkpoint. The goal here is create some activity in WiredTiger in order
        // to generate verbose messages (we don't really care about the checkpoint itself).
        unittest::LogCaptureGuard logs;
        _helper.getWiredTigerKVEngine()->checkpoint();
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
        ASSERT_OK(_helper.getWiredTigerKVEngine()->reconfigureLogging());
        ASSERT_EQ(logv2::LogSeverity::Debug(2),
                  unittest::getMinimumLogSeverity(logv2::LogComponent::kWiredTigerCheckpoint));

        // Perform another checkpoint.
        unittest::LogCaptureGuard logs;
        _helper.getWiredTigerKVEngine()->checkpoint();
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
    _helper.getWiredTigerKVEngine()->setInitialDataTimestamp(Timestamp(1, 1));
    _helper.getWiredTigerKVEngine()->setStableTimestamp(Timestamp(1, 1), false);

    // Get a session. This will open a transaction.
    WiredTigerSession* session =
        WiredTigerRecoveryUnit::get(shard_role_details::getRecoveryUnit(opCtxPtr.get()))
            ->getSession();
    invariant(session);

    // WT will return EBUSY due to the open transaction.
    FailPointEnableBlock failPoint("WTRollbackToStableReturnOnEBUSY");
    ASSERT_EQ(ErrorCodes::ObjectIsBusy,
              _helper.getWiredTigerKVEngine()
                  ->recoverToStableTimestamp(*opCtxPtr.get())
                  .getStatus()
                  .code());

    // Close the open transaction.
    WiredTigerRecoveryUnit::get(shard_role_details::getRecoveryUnit(opCtxPtr.get()))
        ->abandonSnapshot();

    // WT will no longer return EBUSY.
    ASSERT_OK(_helper.getWiredTigerKVEngine()->recoverToStableTimestamp(*opCtxPtr.get()));
}

std::unique_ptr<KVHarnessHelper> makeHelper(ServiceContext* svcCtx) {
    return std::make_unique<WiredTigerKVHarnessHelper>(svcCtx);
}

MONGO_INITIALIZER(RegisterKVHarnessFactory)(InitializerContext*) {
    KVHarnessHelper::registerFactory(makeHelper);
}

TEST_F(WiredTigerKVEngineTest, TestHandlerCleanShutdown) {
    auto* engine = _helper.getWiredTigerKVEngine();
    ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
    engine->cleanShutdown(kMemLeakAllowed);
    ASSERT(!engine->isWtConnReadyForStatsCollection_UNSAFE());
}

TEST_F(WiredTigerKVEngineTest, TestHandlerSingleActivityBeforeShutdownRAII) {
    auto* engine = _helper.getWiredTigerKVEngine();
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
    auto* engine = _helper.getWiredTigerKVEngine();
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
    auto* engine = _helper.getWiredTigerKVEngine();
    ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
    engine->cleanShutdown(kMemLeakAllowed);
    ASSERT(!engine->tryGetStatsCollectionPermit());
    ASSERT_EQ(engine->getActiveStatsReaders(), 0);
    ASSERT(!engine->isWtConnReadyForStatsCollection_UNSAFE());
}

TEST_F(WiredTigerKVEngineTest, TestHandlerCleanShutdownBeforeActivityReleaseRAII) {
    auto* engine = _helper.getWiredTigerKVEngine();
    ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
    {
        auto permit = engine->tryGetStatsCollectionPermit();
        ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());
        ASSERT_EQ(engine->getActiveStatsReaders(), 1);
        stdx::thread shutdownThread([&]() { engine->cleanShutdown(kMemLeakAllowed); });
        ASSERT_EQ(engine->getActiveStatsReaders(), 1);
        while (engine->isWtConnReadyForStatsCollection_UNSAFE()) {
            stdx::this_thread::yield();
        }

        // Ensure that releasing the permit unblocks the shutdown
        permit.reset();
        shutdownThread.join();
    }
    ASSERT_EQ(engine->getActiveStatsReaders(), 0);
    ASSERT(!engine->isWtConnReadyForStatsCollection_UNSAFE());
}

TEST_F(WiredTigerKVEngineTest, TestRestartUsesNewConn) {
    auto* engine = _helper.getWiredTigerKVEngine();
    ASSERT(engine->isWtConnReadyForStatsCollection_UNSAFE());

    {
        auto permit = engine->tryGetStatsCollectionPermit();
        ASSERT(permit);
        ASSERT_EQ(engine->getConn(), permit->conn());
    }

    _helper.restartEngine();
    engine = _helper.getWiredTigerKVEngine();

    auto permit = engine->tryGetStatsCollectionPermit();
    ASSERT(permit);
    ASSERT_EQ(engine->getConn(), permit->conn());
}

TEST_F(WiredTigerKVEngineTest, TestGetBackupCheckpointTimestampWithoutOpenBackupCursor) {
    auto* engine = _helper.getWiredTigerKVEngine();
    ASSERT_EQ(Timestamp::min(), engine->getBackupCheckpointTimestamp());
}

DEATH_TEST_F(WiredTigerKVEngineTest, WaitUntilDurableMustBeOutOfUnitOfWork, "invariant") {
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
    StatusWith<boost::filesystem::path> createIdent(StringData ns, StringData ident) {
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);
        RecordStore::Options options;
        auto& provider =
            rss::ReplicatedStorageService::get(getGlobalServiceContext()).getPersistenceProvider();
        Status stat =
            _helper.getWiredTigerKVEngine()->createRecordStore(provider, nss, ident, options);
        if (!stat.isOK()) {
            return stat;
        }
        boost::optional<boost::filesystem::path> path =
            _helper.getWiredTigerKVEngine()->getDataFilePathForIdent(ident);
        ASSERT_TRUE(path.has_value());
        return *path;
    }

    Status removeIdent(StringData ident) {
        return _helper.getWiredTigerKVEngine()->dropIdent(
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

    RAIIServerParameterControllerForTest sessionCacheMax{"wiredTigerSessionCacheMaxPercentage", 20};
    RAIIServerParameterControllerForTest sessionMax{"wiredTigerSessionMax", 150};
    _helper.restartEngine();

    auto* engine = _helper.getWiredTigerKVEngine();
    auto& connection = engine->getConnection();

    // Check that the configured session cache max is derived correctly.
    ASSERT_EQ(connection.getSessionCacheMax(), 30);
}

}  // namespace
}  // namespace mongo
