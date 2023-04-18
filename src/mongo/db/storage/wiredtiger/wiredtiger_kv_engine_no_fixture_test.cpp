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

#include <fmt/format.h>
#include <wiredtiger.h>

#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/service_context.h"
#include "mongo/db/snapshot_window_options_gen.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"  // for WiredTigerSession
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

/**
 * This suite holds test cases that run against the WiredTiger KV Engine without the support
 * of the KVEngine test harness.
 *
 * The goal of this suite is to support test cases where the defaults provided by the test
 * harness are not required or desired. This suite is also intended to support a mix of
 * operations on both the KVEngine and the lower-level WiredTiger C interface.
 */

/**
 * This ClientObserver is registered with the ServiceContext to ensure that
 * the OperationContext is constructed with a WiredTigerRecoveryUnit rather than
 * the default RecoveryUnitNoop.
 */
class KVTestClientObserver final : public ServiceContext::ClientObserver {
public:
    KVTestClientObserver(KVEngine* kvEngine) : _kvEngine(kvEngine) {}
    void onCreateClient(Client* client) override {}
    void onDestroyClient(Client* client) override {}
    void onCreateOperationContext(OperationContext* opCtx) {
        opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(_kvEngine->newRecoveryUnit()),
                               WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    }
    void onDestroyOperationContext(OperationContext* opCtx) override {}

private:
    KVEngine* _kvEngine;
};

/**
 * Returns a new instance of the WiredTigerKVEngine.
 */
std::unique_ptr<WiredTigerKVEngine> makeKVEngine(ServiceContext* serviceContext,
                                                 const std::string& path,
                                                 ClockSource* clockSource) {
    auto client = serviceContext->makeClient("myclient");
    auto opCtx = serviceContext->makeOperationContext(client.get());
    return std::make_unique<WiredTigerKVEngine>(
        opCtx.get(),
        /*canonicalName=*/"",
        path,
        clockSource,
        /*extraOpenOptions=*/"",
        // Refer to config string in WiredTigerCApiTest::RollbackToStable40.
        /*cacheSizeMB=*/1,
        /*maxHistoryFileSizeMB=*/0,
        /*ephemeral=*/false,
        /*repair=*/false);
}

/**
 * Returns std::string stored in RecordData.
 */
std::string toString(const RecordData& recordData) {
    return std::string{recordData.data(), static_cast<std::size_t>(recordData.size())};
}

/**
 * Commits WriteUnitOfWork and checks timestamp of committed storage transaction.
 */
void commitWriteUnitOfWork(OperationContext* opCtx,
                           WriteUnitOfWork& wuow,
                           Timestamp expectedCommitTimestamp) {
    bool isCommitted = false;
    opCtx->recoveryUnit()->onCommit(
        [&](OperationContext*, boost::optional<Timestamp> commitTimestamp) {
            ASSERT(commitTimestamp) << "Storage transaction committed without timestamp";
            ASSERT_EQ(*commitTimestamp, expectedCommitTimestamp);
            isCommitted = true;
        });
    ASSERT_FALSE(isCommitted);
    wuow.commit();
    ASSERT(isCommitted);
}

// This is adapted from WiredTigerCApiTest::RollbackToStable40 to use the KVEngine interface
// to substitute the WiredTiger C API calls wherever possible.
// See WT-9870 and src/third_party/wiredtiger/test/suite/test_rollback_to_stable40.py.
TEST(WiredTigerKVEngineNoFixtureTest, Basic) {
    std::size_t nrows = 3U;
    std::string valueA(500, 'a');
    std::string valueB(500, 'b');
    std::string valueC(500, 'c');
    std::string valueD(500, 'd');
    auto rid1 = RecordId{1LL};
    auto rid2 = RecordId{2LL};
    auto rid3 = RecordId{3LL};

    // Open the connection.
    setGlobalServiceContext(ServiceContext::make());
    ON_BLOCK_EXIT([] { setGlobalServiceContext({}); });
    auto serviceContext = getGlobalServiceContext();
    unittest::TempDir home("WiredTigerKVEngineNoFixtureTest_Basic_home");
    ClockSourceMock cs;
    auto kvEngine = makeKVEngine(serviceContext, home.path(), &cs);
    auto conn = kvEngine->getConnection();
    ASSERT(conn) << fmt::format("failed to open connection to source folder {}", home.path());

    // Open a session.
    WiredTigerSession session(conn);
    ASSERT(session.getSession()) << "failed to create session with config isolation=snapshot";

    // Create a table without logging.
    auto oldForceDisableTableLogging = storageGlobalParams.forceDisableTableLogging;
    storageGlobalParams.forceDisableTableLogging = true;
    ON_BLOCK_EXIT([oldForceDisableTableLogging] {
        storageGlobalParams.forceDisableTableLogging = oldForceDisableTableLogging;
    });

    // Create an OperationContext with the WiredTigetRecoveryUnit.
    serviceContext->registerClientObserver(std::make_unique<KVTestClientObserver>(kvEngine.get()));
    auto client = serviceContext->makeClient("myclient");
    auto opCtx = serviceContext->makeOperationContext(client.get());

    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.t");
    StringData ident("rollback_to_stable40");
    CollectionOptions collectionOptions;
    auto keyFormat = KeyFormat::Long;
    ASSERT_OK(kvEngine->createRecordStore(opCtx.get(), nss, ident, collectionOptions, keyFormat))
        << fmt::format("failed to create record store with namespace {}",
                       nss.toStringForErrorMsg());

    // Pin oldest and stable to timestamps (1,10).
    // The timestamps in the RollbackToStable40 in the C API test translate to MDB timestamps
    // a zero 'seconds' component, which are not acceptable to the KV engine and will be ignored.
    // To get past this restriction and to maintain  consistency with the C API test, we keep
    // the 'seconds' component constant at a positive value and used the C API test values for
    // the 'inc' component.
    // Additionally, there is a floor on the the 'seconds' component in order to set the oldest
    // timestamp imposed by the 'minSnapshotHistoryWindowInSeconds' setting.
    // Another consideration to setting the oldest timestamp is ensuring that we have a valid
    // initial timestamp.
    auto tsSecs = minSnapshotHistoryWindowInSeconds.load();
    {
        auto tsInc = 10U;
        auto ts = Timestamp(minSnapshotHistoryWindowInSeconds.load(), tsInc);
        LOGV2(7053900, "Setting initial data and stable timestamps", "ts"_attr = ts);
        kvEngine->setInitialDataTimestamp(ts);
        kvEngine->setStableTimestamp(
            ts, /*force=*/false);  // force=false sets both stable and oldest timestamps.
        ASSERT_EQ(kvEngine->getStableTimestamp(), ts)
            << fmt::format("failed to set stable timestamp to {} (hex: {:x})", tsInc, tsInc);
        ASSERT_EQ(kvEngine->getOldestTimestamp(), ts)
            << fmt::format("failed to set oldest timestamp to {} (hex: {:x})", tsInc, tsInc);
    }

    // Insert 3 keys with the value A.
    auto rs = kvEngine->getRecordStore(opCtx.get(), nss, ident, collectionOptions);
    ASSERT(rs) << fmt::format("failed to look up record store with namespace {}", nss.toString());
    {
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);
        WriteUnitOfWork wuow(opCtx.get());

        std::vector<Timestamp> timestamps(nrows, Timestamp(tsSecs, 20U));
        std::vector<Record> records;
        records.push_back(Record{rid1, RecordData(valueA.c_str(), valueA.size())});
        records.push_back(Record{rid2, RecordData(valueA.c_str(), valueA.size())});
        records.push_back(Record{rid3, RecordData(valueA.c_str(), valueA.size())});
        ASSERT_OK(rs->insertRecords(opCtx.get(), &records, timestamps));

        commitWriteUnitOfWork(opCtx.get(), wuow, /*expectedCommitTimestamp=*/timestamps[0]);
    }

    // Read data for 3 keys inserted.
    {
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT_EQ(toString(rs->dataFor(opCtx.get(), rid1)), valueA);
        ASSERT_EQ(toString(rs->dataFor(opCtx.get(), rid2)), valueA);
        ASSERT_EQ(toString(rs->dataFor(opCtx.get(), rid3)), valueA);
    }

    // Update the first and last keys with another value with a large timestamp.
    {
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);
        WriteUnitOfWork wuow(opCtx.get());

        Timestamp updateTimestamp(tsSecs, 1000U);
        ASSERT_OK(opCtx->recoveryUnit()->setTimestamp(updateTimestamp));
        ASSERT_OK(rs->updateRecord(opCtx.get(), rid1, valueD.c_str(), valueD.size()));
        ASSERT_OK(rs->updateRecord(opCtx.get(), rid3, valueD.c_str(), valueD.size()));

        bool isCommitted = false;
        opCtx->recoveryUnit()->onCommit(
            [expectedCommitTimestamp = updateTimestamp,
             &isCommitted](OperationContext*, boost::optional<Timestamp> commitTimestamp) {
                ASSERT(commitTimestamp) << "Storage transaction committed without timestamp";
                ASSERT_EQ(*commitTimestamp, expectedCommitTimestamp);
                isCommitted = true;
            });
        ASSERT_FALSE(isCommitted);
        wuow.commit();
        ASSERT(isCommitted);
    }

    // Confirm new values for two updated keys and also check that middle key is unchanged.
    {
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT_EQ(toString(rs->dataFor(opCtx.get(), rid1)), valueD);
        ASSERT_EQ(toString(rs->dataFor(opCtx.get(), rid2)), valueA);
        ASSERT_EQ(toString(rs->dataFor(opCtx.get(), rid3)), valueD);
    }

    // Update the middle key with lots of updates to generate more history.
    std::string expectedData2;
    for (unsigned int i = 21; i < 499; ++i) {
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);
        WriteUnitOfWork wuow(opCtx.get());

        Timestamp updateTimestamp(tsSecs, i);
        ASSERT_OK(opCtx->recoveryUnit()->setTimestamp(updateTimestamp));
        auto valueBWithISuffix = valueB + std::to_string(i);
        ASSERT_OK(rs->updateRecord(
            opCtx.get(), rid2, valueBWithISuffix.c_str(), valueBWithISuffix.size()));

        commitWriteUnitOfWork(opCtx.get(), wuow, /*expectedCommitTimestamp=*/updateTimestamp);

        expectedData2 = valueBWithISuffix;
    }

    // Check values in table after updating middle key.
    {
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT_EQ(toString(rs->dataFor(opCtx.get(), rid1)), valueD);
        ASSERT_EQ(toString(rs->dataFor(opCtx.get(), rid2)), expectedData2);
        ASSERT_EQ(toString(rs->dataFor(opCtx.get(), rid3)), valueD);
    }

    // With this checkpoint, all the updates in the history store are persisted to disk.
    LOGV2(7053901,
          "Stable timestamp after updating keys",
          "ts"_attr = kvEngine->getStableTimestamp());
    {
        // Increase log verbosity temporarily so that the KVEngine logs the timestamp for the
        // checkpoint.
        auto& logComponentSettings = logv2::LogManager::global().getGlobalSettings();
        logComponentSettings.setMinimumLoggedSeverity(logv2::LogComponent::kStorageRecovery,
                                                      logv2::LogSeverity::Debug(2));
        ON_BLOCK_EXIT([&] {
            logComponentSettings.clearMinimumLoggedSeverity(logv2::LogComponent::kStorageRecovery);
        });

        kvEngine->checkpoint(opCtx.get());
    }

    // Update the middle key with value C after taking checkpoint.
    {
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);
        WriteUnitOfWork wuow(opCtx.get());

        Timestamp updateTimestamp(tsSecs, 500U);
        ASSERT_OK(opCtx->recoveryUnit()->setTimestamp(updateTimestamp));
        ASSERT_OK(rs->updateRecord(opCtx.get(), rid2, valueC.c_str(), valueC.size()));

        commitWriteUnitOfWork(opCtx.get(), wuow, /*expectedCommitTimestamp=*/updateTimestamp);
    }

    // Check values in table after taking checkpoint and updating middle key again.
    {
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        ASSERT_EQ(toString(rs->dataFor(opCtx.get(), rid1)), valueD);
        ASSERT_EQ(toString(rs->dataFor(opCtx.get(), rid2)), valueC);
        ASSERT_EQ(toString(rs->dataFor(opCtx.get(), rid3)), valueD);
    }

    // Pin oldest and stable to timestamp 500.
    {
        auto tsInc = 500U;
        Timestamp ts(tsSecs, tsInc);
        LOGV2(7053902, "Setting initial data and stable timestamps", "ts"_attr = ts);
        kvEngine->setStableTimestamp(ts, /*force=*/false);
        kvEngine->setOldestTimestamp(ts, /*force=*/false);
        ASSERT_EQ(kvEngine->getStableTimestamp(), ts)
            << fmt::format("failed to set stable timestamp to {} (hex: {:x})", tsInc, tsInc);
        ASSERT_EQ(kvEngine->getOldestTimestamp(), ts)
            << fmt::format("failed to set oldest timestamp to {} (hex: {:x})", tsInc, tsInc);
    }

    // TODO(SERVER-72907): implement test-only function for evict cursor in KVEngine interface
    // porting the rest of the test.
}

}  // namespace
}  // namespace mongo
