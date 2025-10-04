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

#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"

#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/prepare_conflict_tracker.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/future_test_utils.h"

#include <memory>
#include <string>

#include <wiredtiger.h>

namespace mongo {

namespace {

/**
 * Returns a new instance of the WiredTigerKVEngine.
 */
std::unique_ptr<WiredTigerKVEngine> makeKVEngine(ServiceContext* serviceContext,
                                                 const std::string& path,
                                                 ClockSource* clockSource) {
    auto& provider = rss::ReplicatedStorageService::get(serviceContext).getPersistenceProvider();
    WiredTigerKVEngineBase::WiredTigerConfig wtConfig =
        getWiredTigerConfigFromStartupOptions(provider);
    wtConfig.cacheSizeMB = 1;
    return std::make_unique<WiredTigerKVEngine>(
        /*canonicalName=*/"",
        path,
        clockSource,
        std::move(wtConfig),
        WiredTigerExtensions::get(serviceContext),
        provider,
        /*repair=*/false,
        /*isReplSet=*/false,
        /*shouldRecoverFromOplogAsStandalone=*/false,
        /*inStandaloneMode=*/false);
}

class WiredTigerPrepareConflictTest : public unittest::Test {
public:
    void setUp() override {
        setGlobalServiceContext(ServiceContext::make());
        auto serviceContext = getGlobalServiceContext();
        kvEngine = makeKVEngine(serviceContext, home.path(), &cs);
        recoveryUnit = std::unique_ptr<RecoveryUnit>(kvEngine->newRecoveryUnit());
    }

    ~WiredTigerPrepareConflictTest() override {
#if __has_feature(address_sanitizer)
        constexpr bool memLeakAllowed = false;
#else
        constexpr bool memLeakAllowed = true;
#endif
        kvEngine->cleanShutdown(memLeakAllowed);
    }

    unittest::TempDir home{"temp"};
    ClockSourceMock cs;
    std::unique_ptr<WiredTigerKVEngine> kvEngine;
    std::unique_ptr<RecoveryUnit> recoveryUnit;
    DummyInterruptible interruptible;
};

TEST_F(WiredTigerPrepareConflictTest, SuccessWithNoConflict) {
    auto successOnFirstTry = []() {
        return 0;
    };

    PrepareConflictTracker tracker;
    ASSERT_EQ(
        wiredTigerPrepareConflictRetry(interruptible, tracker, *recoveryUnit, successOnFirstTry),
        0);
    ASSERT_EQ(tracker.getThisOpPrepareConflictCount(), 0);
}

TEST_F(WiredTigerPrepareConflictTest, HandleWTPrepareConflictOnce) {
    auto attempt = 0;
    auto throwWTPrepareConflictOnce = [&attempt]() {
        return attempt++ < 1 ? WT_PREPARE_CONFLICT : 0;
    };

    PrepareConflictTracker tracker;
    ASSERT_EQ(wiredTigerPrepareConflictRetry(
                  interruptible, tracker, *recoveryUnit, throwWTPrepareConflictOnce),
              0);
    ASSERT_EQ(tracker.getThisOpPrepareConflictCount(), 1);
}

TEST_F(WiredTigerPrepareConflictTest, HandleWTPrepareConflictMultipleTimes) {
    auto attempt = 0;
    auto ru = recoveryUnit.get();
    auto connection = static_cast<WiredTigerRecoveryUnit*>(ru)->getConnection();

    auto throwWTPrepareConflictMultipleTimes = [&connection, &attempt]() {
        // Manually increments '_prepareCommitOrAbortCounter' to simulate a unit of work has been
        // committed/aborted to continue retrying.
        connection->notifyPreparedUnitOfWorkHasCommittedOrAborted();
        return attempt++ < 100 ? WT_PREPARE_CONFLICT : 0;
    };

    PrepareConflictTracker tracker;
    ASSERT_EQ(wiredTigerPrepareConflictRetry(
                  interruptible, tracker, *ru, throwWTPrepareConflictMultipleTimes),
              0);
    // Multiple retries are still considered to be one prepare conflict.
    ASSERT_EQ(tracker.getThisOpPrepareConflictCount(), 1);
}

TEST_F(WiredTigerPrepareConflictTest, ThrowNonBlocking) {
    auto alwaysFail = []() {
        return WT_PREPARE_CONFLICT;
    };
    auto ru = recoveryUnit.get();
    ru->setBlockingAllowed(false);

    PrepareConflictTracker tracker;
    ASSERT_THROWS_CODE(wiredTigerPrepareConflictRetry(interruptible, tracker, *ru, alwaysFail),
                       StorageUnavailableException,
                       ErrorCodes::WriteConflict);
    ASSERT_EQ(tracker.getThisOpPrepareConflictCount(), 0);
}

}  // namespace
}  // namespace mongo
