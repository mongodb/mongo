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

#include <memory>
#include <string>

#include <wiredtiger.h>

#include "mongo/db/curop.h"
#include "mongo/db/prepare_conflict_tracker.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {

namespace {

/**
 * Returns a new instance of the WiredTigerKVEngine.
 */
std::unique_ptr<WiredTigerKVEngine> makeKVEngine(ServiceContext* serviceContext,
                                                 const std::string& path,
                                                 ClockSource* clockSource) {
    return std::make_unique<WiredTigerKVEngine>(
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

class WiredTigerPrepareConflictTest : public unittest::Test {
public:
    void setUp() override {
        setGlobalServiceContext(ServiceContext::make());
        auto serviceContext = getGlobalServiceContext();
        client = serviceContext->getService()->makeClient("myClient");
        opCtx = serviceContext->makeOperationContext(client.get());
        kvEngine = makeKVEngine(serviceContext, home.path(), &cs);
        recoveryUnit = std::unique_ptr<RecoveryUnit>(kvEngine->newRecoveryUnit());
    }

    unittest::TempDir home{"temp"};
    ClockSourceMock cs;
    std::unique_ptr<WiredTigerKVEngine> kvEngine;
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
    std::unique_ptr<RecoveryUnit> recoveryUnit;
};

TEST_F(WiredTigerPrepareConflictTest, SuccessWithNoConflict) {
    auto successOnFirstTry = []() {
        return 0;
    };

    ASSERT_EQ(wiredTigerPrepareConflictRetry(opCtx.get(), *recoveryUnit.get(), successOnFirstTry),
              0);
    ASSERT_EQ(CurOp::get(opCtx.get())->debug().additiveMetrics.prepareReadConflicts.load(), 0);
    ASSERT_EQ(PrepareConflictTracker::get(opCtx.get()).getThisOpPrepareConflictCount(), 0);
}

TEST_F(WiredTigerPrepareConflictTest, HandleWTPrepareConflictOnce) {
    auto attempt = 0;
    auto throwWTPrepareConflictOnce = [&attempt]() {
        return attempt++ < 1 ? WT_PREPARE_CONFLICT : 0;
    };

    ASSERT_EQ(wiredTigerPrepareConflictRetry(
                  opCtx.get(), *recoveryUnit.get(), throwWTPrepareConflictOnce),
              0);
    ASSERT_EQ(CurOp::get(opCtx.get())->debug().additiveMetrics.prepareReadConflicts.load(), 1);
    ASSERT_EQ(PrepareConflictTracker::get(opCtx.get()).getThisOpPrepareConflictCount(), 1);
}

TEST_F(WiredTigerPrepareConflictTest, HandleWTPrepareConflictMultipleTimes) {
    auto attempt = 0;
    auto ru = recoveryUnit.get();
    auto sessionCache = static_cast<WiredTigerRecoveryUnit*>(ru)->getSessionCache();

    auto throwWTPrepareConflictMultipleTimes = [&sessionCache, &attempt]() {
        // Manually increments '_prepareCommitOrAbortCounter' to simulate a unit of work has been
        // committed/aborted to continue retrying.
        sessionCache->notifyPreparedUnitOfWorkHasCommittedOrAborted();
        return attempt++ < 100 ? WT_PREPARE_CONFLICT : 0;
    };

    ASSERT_EQ(wiredTigerPrepareConflictRetry(opCtx.get(), *ru, throwWTPrepareConflictMultipleTimes),
              0);
    // Multiple retries are still considered to be one prepare conflict.
    ASSERT_EQ(CurOp::get(opCtx.get())->debug().additiveMetrics.prepareReadConflicts.load(), 1);
    ASSERT_EQ(PrepareConflictTracker::get(opCtx.get()).getThisOpPrepareConflictCount(), 1);
}

TEST_F(WiredTigerPrepareConflictTest, ThrowNonBlocking) {
    auto alwaysFail = []() {
        return WT_PREPARE_CONFLICT;
    };
    auto ru = recoveryUnit.get();
    ru->setBlockingAllowed(false);

    ASSERT_THROWS_CODE(wiredTigerPrepareConflictRetry(opCtx.get(), *ru, alwaysFail),
                       StorageUnavailableException,
                       ErrorCodes::WriteConflict);
    ASSERT_EQ(CurOp::get(opCtx.get())->debug().additiveMetrics.prepareReadConflicts.load(), 1);
}

}  // namespace
}  // namespace mongo
