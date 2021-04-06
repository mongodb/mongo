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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/storage/kv/kv_engine_test_harness.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/checkpointer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class WiredTigerKVHarnessHelper : public KVHarnessHelper {
public:
    WiredTigerKVHarnessHelper(ServiceContext* svcCtx, bool forRepair = false)
        : _dbpath("wt-kv-harness"), _forRepair(forRepair), _engine(makeEngine()) {
        repl::ReplicationCoordinator::set(
            svcCtx,
            std::make_unique<repl::ReplicationCoordinatorMock>(svcCtx, repl::ReplSettings()));
        _engine->notifyStartupComplete();
    }

    virtual KVEngine* restartEngine() override {
        _engine.reset(nullptr);
        _engine = makeEngine();
        _engine->notifyStartupComplete();
        return _engine.get();
    }

    virtual KVEngine* getEngine() override {
        return _engine.get();
    }

    virtual WiredTigerKVEngine* getWiredTigerKVEngine() {
        return _engine.get();
    }

private:
    std::unique_ptr<WiredTigerKVEngine> makeEngine() {
        return std::make_unique<WiredTigerKVEngine>(kWiredTigerEngineName,
                                                    _dbpath.path(),
                                                    _cs.get(),
                                                    "",
                                                    1,
                                                    0,
                                                    false,
                                                    false,
                                                    _forRepair,
                                                    false);
    }

    const std::unique_ptr<ClockSource> _cs = std::make_unique<ClockSourceMock>();
    unittest::TempDir _dbpath;
    bool _forRepair;
    std::unique_ptr<WiredTigerKVEngine> _engine;
};

class WiredTigerKVEngineTest : public ServiceContextTest {
public:
    WiredTigerKVEngineTest(bool repair = false)
        : _helper(getServiceContext(), repair), _engine(_helper.getWiredTigerKVEngine()) {}

protected:
    ServiceContext::UniqueOperationContext _makeOperationContext() {
        auto opCtx = makeOperationContext();
        opCtx->setRecoveryUnit(
            std::unique_ptr<RecoveryUnit>(_helper.getEngine()->newRecoveryUnit()),
            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        opCtx->swapLockState(std::make_unique<LockerNoop>(), WithLock::withoutLock());
        return opCtx;
    }

    WiredTigerKVHarnessHelper _helper;
    WiredTigerKVEngine* _engine;
};

class WiredTigerKVEngineRepairTest : public WiredTigerKVEngineTest {
public:
    WiredTigerKVEngineRepairTest() : WiredTigerKVEngineTest(true /* repair */) {}
};

TEST_F(WiredTigerKVEngineRepairTest, OrphanedDataFilesCanBeRecovered) {
    auto opCtxPtr = _makeOperationContext();

    NamespaceString nss("a.b");
    std::string ident = "collection-1234";
    std::string record = "abcd";
    CollectionOptions defaultCollectionOptions;

    std::unique_ptr<RecordStore> rs;
    ASSERT_OK(
        _engine->createRecordStore(opCtxPtr.get(), nss.ns(), ident, defaultCollectionOptions));
    rs = _engine->getRecordStore(opCtxPtr.get(), nss.ns(), ident, defaultCollectionOptions);
    ASSERT(rs);

    RecordId loc;
    {
        WriteUnitOfWork uow(opCtxPtr.get());
        StatusWith<RecordId> res =
            rs->insertRecord(opCtxPtr.get(), record.c_str(), record.length() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        uow.commit();
    }

    const boost::optional<boost::filesystem::path> dataFilePath =
        _engine->getDataFilePathForIdent(ident);
    ASSERT(dataFilePath);

    ASSERT(boost::filesystem::exists(*dataFilePath));

    const boost::filesystem::path tmpFile{dataFilePath->string() + ".tmp"};
    ASSERT(!boost::filesystem::exists(tmpFile));

#ifdef _WIN32
    auto status =
        _engine->recoverOrphanedIdent(opCtxPtr.get(), nss, ident, defaultCollectionOptions);
    ASSERT_EQ(ErrorCodes::CommandNotSupported, status.code());
#else
    // Move the data file out of the way so the ident can be dropped. This not permitted on Windows
    // because the file cannot be moved while it is open. The implementation for orphan recovery is
    // also not implemented on Windows for this reason.
    boost::system::error_code err;
    boost::filesystem::rename(*dataFilePath, tmpFile, err);
    ASSERT(!err) << err.message();

    ASSERT_OK(_engine->dropIdent(opCtxPtr.get()->recoveryUnit(), ident));

    // The data file is moved back in place so that it becomes an "orphan" of the storage
    // engine and the restoration process can be tested.
    boost::filesystem::rename(tmpFile, *dataFilePath, err);
    ASSERT(!err) << err.message();

    auto status =
        _engine->recoverOrphanedIdent(opCtxPtr.get(), nss, ident, defaultCollectionOptions);
    ASSERT_EQ(ErrorCodes::DataModifiedByRepair, status.code());
#endif
}

TEST_F(WiredTigerKVEngineRepairTest, UnrecoverableOrphanedDataFilesAreRebuilt) {
    auto opCtxPtr = _makeOperationContext();

    NamespaceString nss("a.b");
    std::string ident = "collection-1234";
    std::string record = "abcd";
    CollectionOptions defaultCollectionOptions;

    std::unique_ptr<RecordStore> rs;
    ASSERT_OK(
        _engine->createRecordStore(opCtxPtr.get(), nss.ns(), ident, defaultCollectionOptions));
    rs = _engine->getRecordStore(opCtxPtr.get(), nss.ns(), ident, defaultCollectionOptions);
    ASSERT(rs);

    RecordId loc;
    {
        WriteUnitOfWork uow(opCtxPtr.get());
        StatusWith<RecordId> res =
            rs->insertRecord(opCtxPtr.get(), record.c_str(), record.length() + 1, Timestamp());
        ASSERT_OK(res.getStatus());
        loc = res.getValue();
        uow.commit();
    }

    const boost::optional<boost::filesystem::path> dataFilePath =
        _engine->getDataFilePathForIdent(ident);
    ASSERT(dataFilePath);

    ASSERT(boost::filesystem::exists(*dataFilePath));

    ASSERT_OK(_engine->dropIdent(opCtxPtr.get()->recoveryUnit(), ident));

#ifdef _WIN32
    auto status =
        _engine->recoverOrphanedIdent(opCtxPtr.get(), nss, ident, defaultCollectionOptions);
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
        _engine->recoverOrphanedIdent(opCtxPtr.get(), nss, ident, defaultCollectionOptions);
    ASSERT_EQ(ErrorCodes::DataModifiedByRepair, status.code()) << status.reason();

    boost::filesystem::path corruptFile = (dataFilePath->string() + ".corrupt");
    ASSERT(boost::filesystem::exists(corruptFile));

    rs = _engine->getRecordStore(opCtxPtr.get(), nss.ns(), ident, defaultCollectionOptions);
    RecordData data;
    ASSERT_FALSE(rs->findRecord(opCtxPtr.get(), loc, &data));
#endif
}

TEST_F(WiredTigerKVEngineTest, TestOplogTruncation) {
    std::unique_ptr<Checkpointer> checkpointer = std::make_unique<Checkpointer>(_engine);
    checkpointer->go();

    auto opCtxPtr = _makeOperationContext();
    // The initial data timestamp has to be set to take stable checkpoints. The first stable
    // timestamp greater than this will also trigger a checkpoint. The following loop of the
    // CheckpointThread will observe the new `checkpointDelaySecs` value.
    _engine->setInitialDataTimestamp(Timestamp(1, 1));


    // Ignore data race on this variable when running with TSAN, this is only an issue in this
    // unittest and not in mongod
    []()
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
        __attribute__((no_sanitize("thread")))
#endif
#endif
    {
        storageGlobalParams.checkpointDelaySecs = 1;
    }
    ();


    // To diagnose any intermittent failures, maximize logging from WiredTigerKVEngine and friends.
    auto severityGuard = unittest::MinimumLoggedSeverityGuard{logv2::LogComponent::kStorage,
                                                              logv2::LogSeverity::Debug(3)};

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

    _engine->setOldestActiveTransactionTimestampCallback(callback);

    // A method that will poll the WiredTigerKVEngine until it sees the amount of oplog necessary
    // for crash recovery exceeds the input.
    auto assertPinnedMovesSoon = [this](Timestamp newPinned) {
        // If the current oplog needed for rollback does not exceed the requested pinned out, we
        // cannot expect the CheckpointThread to eventually publish a sufficient crash recovery
        // value.
        auto needed = _engine->getOplogNeededForRollback();
        if (needed.isOK()) {
            ASSERT_TRUE(needed.getValue() >= newPinned);
        }

        // Do 100 iterations that sleep for 100 milliseconds between polls. This will wait for up
        // to 10 seconds to observe an asynchronous update that iterates once per second.
        for (auto iterations = 0; iterations < 100; ++iterations) {
            if (_engine->getPinnedOplog() >= newPinned) {
                ASSERT_TRUE(_engine->getOplogNeededForCrashRecovery().get() >= newPinned);
                return;
            }

            sleepmillis(100);
        }

        LOGV2(22367,
              "Expected the pinned oplog to advance. Expected value: {newPinned} Published value: "
              "{engine_getOplogNeededForCrashRecovery}",
              "newPinned"_attr = newPinned,
              "engine_getOplogNeededForCrashRecovery"_attr =
                  _engine->getOplogNeededForCrashRecovery());
        FAIL("");
    };

    oldestActiveTxnTimestamp = boost::none;
    _engine->setStableTimestamp(Timestamp(10, 1), false);
    assertPinnedMovesSoon(Timestamp(10, 1));

    oldestActiveTxnTimestamp = Timestamp(15, 1);
    _engine->setStableTimestamp(Timestamp(20, 1), false);
    assertPinnedMovesSoon(Timestamp(15, 1));

    oldestActiveTxnTimestamp = Timestamp(19, 1);
    _engine->setStableTimestamp(Timestamp(30, 1), false);
    assertPinnedMovesSoon(Timestamp(19, 1));

    oldestActiveTxnTimestamp = boost::none;
    _engine->setStableTimestamp(Timestamp(30, 1), false);
    assertPinnedMovesSoon(Timestamp(30, 1));

    callbackShouldFail.store(true);
    ASSERT_NOT_OK(_engine->getOplogNeededForRollback());
    _engine->setStableTimestamp(Timestamp(40, 1), false);
    // Await a new checkpoint. Oplog needed for rollback does not advance.
    sleepmillis(1100);
    ASSERT_EQ(_engine->getOplogNeededForCrashRecovery().get(), Timestamp(30, 1));
    _engine->setStableTimestamp(Timestamp(30, 1), false);
    callbackShouldFail.store(false);
    assertPinnedMovesSoon(Timestamp(40, 1));

    checkpointer->shutdown({ErrorCodes::ShutdownInProgress, "Test finished"});
}

TEST_F(WiredTigerKVEngineTest, IdentDrop) {
#ifdef _WIN32
    // TODO SERVER-51595: to re-enable this test on Windows.
    return;
#endif

    auto opCtxPtr = _makeOperationContext();

    NamespaceString nss("a.b");
    std::string ident = "collection-1234";
    CollectionOptions defaultCollectionOptions;

    std::unique_ptr<RecordStore> rs;
    ASSERT_OK(
        _engine->createRecordStore(opCtxPtr.get(), nss.ns(), ident, defaultCollectionOptions));

    const boost::optional<boost::filesystem::path> dataFilePath =
        _engine->getDataFilePathForIdent(ident);
    ASSERT(dataFilePath);
    ASSERT(boost::filesystem::exists(*dataFilePath));

    _engine->dropIdentForImport(opCtxPtr.get(), ident);
    ASSERT(boost::filesystem::exists(*dataFilePath));

    // Because the underlying file was not removed, it will be renamed out of the way by WiredTiger
    // when creating a new table with the same ident.
    ASSERT_OK(
        _engine->createRecordStore(opCtxPtr.get(), nss.ns(), ident, defaultCollectionOptions));

    const boost::filesystem::path renamedFilePath = dataFilePath->generic_string() + ".1";
    ASSERT(boost::filesystem::exists(*dataFilePath));
    ASSERT(boost::filesystem::exists(renamedFilePath));

    ASSERT_OK(_engine->dropIdent(opCtxPtr.get()->recoveryUnit(), ident));

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
    _engine->setOldestTimestamp(initTs, false);
    ASSERT_EQ(initTs, _engine->getOldestTimestamp());

    // Assert that advancing the oldest timestamp still succeeds.
    _engine->setOldestTimestamp(initTs + 1, false);
    ASSERT_EQ(initTs + 1, _engine->getOldestTimestamp());

    // Error if there's a request to pin the oldest timestamp earlier than what it is already set
    // as. This error case is not exercised in this test.
    const bool roundUpIfTooOld = false;
    // Pin the oldest timestamp to "3".
    auto pinnedTs = unittest::assertGet(
        _engine->pinOldestTimestamp(opCtxRaii.get(), "A", initTs + 3, roundUpIfTooOld));
    // Assert that the pinning method returns the same timestamp as was requested.
    ASSERT_EQ(initTs + 3, pinnedTs);
    // Assert that pinning the oldest timestamp does not advance it.
    ASSERT_EQ(initTs + 1, _engine->getOldestTimestamp());

    // Attempt to advance the oldest timestamp to "5".
    _engine->setOldestTimestamp(initTs + 5, false);
    // Observe the oldest timestamp was pinned at the requested "3".
    ASSERT_EQ(initTs + 3, _engine->getOldestTimestamp());

    // Unpin the oldest timestamp. Assert that unpinning does not advance the oldest timestamp.
    _engine->unpinOldestTimestamp("A");
    ASSERT_EQ(initTs + 3, _engine->getOldestTimestamp());

    // Now advancing the oldest timestamp to "5" succeeds.
    _engine->setOldestTimestamp(initTs + 5, false);
    ASSERT_EQ(initTs + 5, _engine->getOldestTimestamp());
}

/**
 * Demonstrate that multiple actors can request different pins of the oldest timestamp. The minimum
 * of all active requests will be obeyed.
 */
TEST_F(WiredTigerKVEngineTest, TestMultiPinOldestTimestamp) {
    auto opCtxRaii = _makeOperationContext();
    const Timestamp initTs = Timestamp(1, 0);

    _engine->setOldestTimestamp(initTs, false);
    ASSERT_EQ(initTs, _engine->getOldestTimestamp());

    // Error if there's a request to pin the oldest timestamp earlier than what it is already set
    // as. This error case is not exercised in this test.
    const bool roundUpIfTooOld = false;
    // Have "A" pin the timestamp to "1".
    auto pinnedTs = unittest::assertGet(
        _engine->pinOldestTimestamp(opCtxRaii.get(), "A", initTs + 1, roundUpIfTooOld));
    ASSERT_EQ(initTs + 1, pinnedTs);
    ASSERT_EQ(initTs, _engine->getOldestTimestamp());

    // Have "B" pin the timestamp to "2".
    pinnedTs = unittest::assertGet(
        _engine->pinOldestTimestamp(opCtxRaii.get(), "B", initTs + 2, roundUpIfTooOld));
    ASSERT_EQ(initTs + 2, pinnedTs);
    ASSERT_EQ(initTs, _engine->getOldestTimestamp());

    // Advancing the oldest timestamp to "5" will only succeed in advancing it to "1".
    _engine->setOldestTimestamp(initTs + 5, false);
    ASSERT_EQ(initTs + 1, _engine->getOldestTimestamp());

    // After unpinning "A" at "1", advancing the oldest timestamp will be pinned to "2".
    _engine->unpinOldestTimestamp("A");
    _engine->setOldestTimestamp(initTs + 5, false);
    ASSERT_EQ(initTs + 2, _engine->getOldestTimestamp());

    // Unpinning "B" at "2" allows the oldest timestamp to advance freely.
    _engine->unpinOldestTimestamp("B");
    _engine->setOldestTimestamp(initTs + 5, false);
    ASSERT_EQ(initTs + 5, _engine->getOldestTimestamp());
}

/**
 * Test error cases where a request to pin the oldest timestamp uses a value that's too early
 * relative to the current oldest timestamp.
 */
TEST_F(WiredTigerKVEngineTest, TestPinOldestTimestampErrors) {
    auto opCtxRaii = _makeOperationContext();
    const Timestamp initTs = Timestamp(10, 0);

    _engine->setOldestTimestamp(initTs, false);
    ASSERT_EQ(initTs, _engine->getOldestTimestamp());

    const bool roundUpIfTooOld = true;
    // The false value means using this variable will cause the method to fail on error.
    const bool failOnError = false;

    // When rounding on error, the pin will succeed, but the return value will be the current oldest
    // timestamp instead of the requested value.
    auto pinnedTs = unittest::assertGet(
        _engine->pinOldestTimestamp(opCtxRaii.get(), "A", initTs - 1, roundUpIfTooOld));
    ASSERT_EQ(initTs, pinnedTs);
    ASSERT_EQ(initTs, _engine->getOldestTimestamp());

    // Using "fail on error" will result in a not-OK return value.
    ASSERT_NOT_OK(_engine->pinOldestTimestamp(opCtxRaii.get(), "B", initTs - 1, failOnError));
    ASSERT_EQ(initTs, _engine->getOldestTimestamp());
}


std::unique_ptr<KVHarnessHelper> makeHelper(ServiceContext* svcCtx) {
    return std::make_unique<WiredTigerKVHarnessHelper>(svcCtx);
}

MONGO_INITIALIZER(RegisterKVHarnessFactory)(InitializerContext*) {
    KVHarnessHelper::registerFactory(makeHelper);
}

}  // namespace
}  // namespace mongo
