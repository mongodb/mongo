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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/kv/kv_engine_test_harness.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>

#include "mongo/base/init.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class WiredTigerKVHarnessHelper : public KVHarnessHelper, public ScopedGlobalServiceContextForTest {
public:
    WiredTigerKVHarnessHelper(bool forRepair = false)
        : _dbpath("wt-kv-harness"), _forRepair(forRepair) {
        invariant(hasGlobalServiceContext());
        _engine.reset(makeEngine());
        repl::ReplicationCoordinator::set(
            getGlobalServiceContext(),
            std::unique_ptr<repl::ReplicationCoordinator>(new repl::ReplicationCoordinatorMock(
                getGlobalServiceContext(), repl::ReplSettings())));
    }

    virtual KVEngine* restartEngine() override {
        _engine.reset(nullptr);
        _engine.reset(makeEngine());
        return _engine.get();
    }

    virtual KVEngine* getEngine() override {
        return _engine.get();
    }

    virtual WiredTigerKVEngine* getWiredTigerKVEngine() {
        return _engine.get();
    }

private:
    WiredTigerKVEngine* makeEngine() {
        return new WiredTigerKVEngine(kWiredTigerEngineName,
                                      _dbpath.path(),
                                      _cs.get(),
                                      "",
                                      1,
                                      false,
                                      false,
                                      _forRepair,
                                      false);
    }

    const std::unique_ptr<ClockSource> _cs = stdx::make_unique<ClockSourceMock>();
    unittest::TempDir _dbpath;
    std::unique_ptr<WiredTigerKVEngine> _engine;
    bool _forRepair;
};

class WiredTigerKVEngineTest : public unittest::Test, public ScopedGlobalServiceContextForTest {
public:
    WiredTigerKVEngineTest(bool repair = false)
        : _helper(repair), _engine(_helper.getWiredTigerKVEngine()) {}

    std::unique_ptr<OperationContext> makeOperationContext() {
        return std::make_unique<OperationContextNoop>(_engine->newRecoveryUnit());
    }

protected:
    WiredTigerKVHarnessHelper _helper;
    WiredTigerKVEngine* _engine;
};

class WiredTigerKVEngineRepairTest : public WiredTigerKVEngineTest {
public:
    WiredTigerKVEngineRepairTest() : WiredTigerKVEngineTest(true /* repair */) {}
};

TEST_F(WiredTigerKVEngineRepairTest, OrphanedDataFilesCanBeRecovered) {
    auto opCtxPtr = makeOperationContext();

    std::string ns = "a.b";
    std::string ident = "collection-1234";
    std::string record = "abcd";
    CollectionOptions options;

    std::unique_ptr<RecordStore> rs;
    ASSERT_OK(_engine->createRecordStore(opCtxPtr.get(), ns, ident, options));
    rs = _engine->getRecordStore(opCtxPtr.get(), ns, ident, options);
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
    auto status = _engine->recoverOrphanedIdent(opCtxPtr.get(), ns, ident, options);
    ASSERT_EQ(ErrorCodes::CommandNotSupported, status.code());
#else
    // Move the data file out of the way so the ident can be dropped. This not permitted on Windows
    // because the file cannot be moved while it is open. The implementation for orphan recovery is
    // also not implemented on Windows for this reason.
    boost::system::error_code err;
    boost::filesystem::rename(*dataFilePath, tmpFile, err);
    ASSERT(!err) << err.message();

    ASSERT_OK(_engine->dropIdent(opCtxPtr.get(), ident));

    // The data file is moved back in place so that it becomes an "orphan" of the storage
    // engine and the restoration process can be tested.
    boost::filesystem::rename(tmpFile, *dataFilePath, err);
    ASSERT(!err) << err.message();

    auto status = _engine->recoverOrphanedIdent(opCtxPtr.get(), ns, ident, options);
    ASSERT_EQ(ErrorCodes::DataModifiedByRepair, status.code());
#endif
}

TEST_F(WiredTigerKVEngineRepairTest, UnrecoverableOrphanedDataFilesAreRebuilt) {
    auto opCtxPtr = makeOperationContext();

    std::string ns = "a.b";
    std::string ident = "collection-1234";
    std::string record = "abcd";
    CollectionOptions options;

    std::unique_ptr<RecordStore> rs;
    ASSERT_OK(_engine->createRecordStore(opCtxPtr.get(), ns, ident, options));
    rs = _engine->getRecordStore(opCtxPtr.get(), ns, ident, options);
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

    ASSERT_OK(_engine->dropIdent(opCtxPtr.get(), ident));

#ifdef _WIN32
    auto status = _engine->recoverOrphanedIdent(opCtxPtr.get(), ns, ident, options);
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
    auto status = _engine->recoverOrphanedIdent(opCtxPtr.get(), ns, ident, options);
    ASSERT_EQ(ErrorCodes::DataModifiedByRepair, status.code()) << status.reason();

    boost::filesystem::path corruptFile = (dataFilePath->string() + ".corrupt");
    ASSERT(boost::filesystem::exists(corruptFile));

    rs = _engine->getRecordStore(opCtxPtr.get(), ns, ident, options);
    RecordData data;
    ASSERT_FALSE(rs->findRecord(opCtxPtr.get(), loc, &data));
#endif
}

TEST_F(WiredTigerKVEngineTest, TestOplogTruncation) {
    auto opCtxPtr = makeOperationContext();
    // The initial data timestamp has to be set to take stable checkpoints. The first stable
    // timestamp greater than this will also trigger a checkpoint. The following loop of the
    // CheckpointThread will observe the new `checkpointDelaySecs` value.
    _engine->setInitialDataTimestamp(Timestamp(1, 1));
    wiredTigerGlobalOptions.checkpointDelaySecs = 1;

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

        unittest::log() << "Expected the pinned oplog to advance. Expected value: " << newPinned
                        << " Published value: " << _engine->getOplogNeededForCrashRecovery();
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
}

std::unique_ptr<KVHarnessHelper> makeHelper() {
    return stdx::make_unique<WiredTigerKVHarnessHelper>();
}

MONGO_INITIALIZER(RegisterKVHarnessFactory)(InitializerContext*) {
    KVHarnessHelper::registerFactory(makeHelper);
    return Status::OK();
}

}  // namespace
}  // namespace mongo
