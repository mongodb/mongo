// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"

#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/exceptions.h"
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

TEST_F(WiredTigerPrepareConflictTest, IgnoreBehaviorClassification) {
    EXPECT_FALSE(ignoresPrepareConflicts(PrepareConflictBehavior::kEnforce));
    EXPECT_FALSE(ignoresPrepareConflicts(PrepareConflictBehavior::kThrow));
    EXPECT_TRUE(ignoresPrepareConflicts(PrepareConflictBehavior::kIgnoreConflicts));
    EXPECT_TRUE(ignoresPrepareConflicts(PrepareConflictBehavior::kIgnoreConflictsAllowWrites));
}

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

TEST_F(WiredTigerPrepareConflictTest, ThrowPrepareConflictCountsWithoutWaiting) {
    recoveryUnit->setPrepareConflictBehavior(PrepareConflictBehavior::kThrow);
    recoveryUnit->setBlockingAllowed(false);
    auto generation = recoveryUnit->getPrepareCommitOrAbortCount();
    auto conflictsBefore = PrepareConflictTracker::getGlobalNumPrepareConflicts();
    auto waitMicrosBefore = PrepareConflictTracker::getGlobalWaitingForPrepareConflictsMicros();
    PrepareConflictTracker tracker;
    try {
        wiredTigerPrepareConflictRetry(
            interruptible, tracker, *recoveryUnit, [] { return WT_PREPARE_CONFLICT; });
        FAIL("Expected a prepare conflict");
    } catch (const ExceptionFor<ErrorCodes::PrepareConflict>& ex) {
        EXPECT_EQ(ex->generation(), generation);
        EXPECT_EQ(ex.code(), ErrorCodes::PrepareConflict);
    }
    EXPECT_EQ(tracker.getThisOpPrepareConflictCount(), 1);
    EXPECT_EQ(PrepareConflictTracker::getGlobalNumPrepareConflicts(), conflictsBefore + 1);
    EXPECT_EQ(tracker.getThisOpPrepareConflictDuration(), Microseconds(0));
    EXPECT_EQ(PrepareConflictTracker::getGlobalWaitingForPrepareConflictsMicros(),
              waitMicrosBefore);
    EXPECT_FALSE(tracker.isWaitingOnPrepareConflict());
}

TEST_F(WiredTigerPrepareConflictTest, ResolutionBetweenReadAndWaitIsNotLost) {
    recoveryUnit->setPrepareConflictBehavior(PrepareConflictBehavior::kThrow);
    auto* connection = WiredTigerRecoveryUnit::get(*recoveryUnit).getConnection();
    PrepareConflictTracker tracker;
    try {
        wiredTigerPrepareConflictRetry(interruptible, tracker, *recoveryUnit, [&] {
            connection->notifyPreparedUnitOfWorkHasCommittedOrAborted();
            return WT_PREPARE_CONFLICT;
        });
        FAIL("Expected a prepare conflict");
    } catch (const ExceptionFor<ErrorCodes::PrepareConflict>& ex) {
        EXPECT_GT(recoveryUnit->getPrepareCommitOrAbortCount(), ex->generation());
        // An unbounded wait must return immediately even though notification preceded the wait.
        recoveryUnit->waitForPrepareConflictResolution(interruptible, ex->generation());
    }
    EXPECT_FALSE(tracker.isWaitingOnPrepareConflict());
}

TEST_F(WiredTigerPrepareConflictTest, NotificationDoesNotSuppressAnotherConflict) {
    recoveryUnit->setPrepareConflictBehavior(PrepareConflictBehavior::kThrow);
    auto* connection = WiredTigerRecoveryUnit::get(*recoveryUnit).getConnection();
    PrepareConflictTracker tracker;
    auto conflictsBefore = PrepareConflictTracker::getGlobalNumPrepareConflicts();
    auto waitMicrosBefore = PrepareConflictTracker::getGlobalWaitingForPrepareConflictsMicros();
    auto alwaysConflict = [] {
        return WT_PREPARE_CONFLICT;
    };
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            wiredTigerPrepareConflictRetry(interruptible, tracker, *recoveryUnit, alwaysConflict);
            FAIL("Expected a prepare conflict");
        } catch (const ExceptionFor<ErrorCodes::PrepareConflict>& ex) {
            connection->notifyPreparedUnitOfWorkHasCommittedOrAborted();
            recoveryUnit->waitForPrepareConflictResolution(interruptible, ex->generation());
        }
    }
    EXPECT_EQ(tracker.getThisOpPrepareConflictCount(), 2);
    EXPECT_EQ(PrepareConflictTracker::getGlobalNumPrepareConflicts(), conflictsBefore + 2);
    EXPECT_FALSE(tracker.isWaitingOnPrepareConflict());
    EXPECT_EQ(tracker.getThisOpPrepareConflictDuration(), Microseconds(0));
    EXPECT_EQ(PrepareConflictTracker::getGlobalWaitingForPrepareConflictsMicros(),
              waitMicrosBefore);
}

TEST_F(WiredTigerPrepareConflictTest, NotificationBeforeWaitIsNotLost) {
    auto generation = recoveryUnit->getPrepareCommitOrAbortCount();
    auto& connection = *WiredTigerRecoveryUnit::get(*recoveryUnit).getConnection();
    connection.notifyPreparedUnitOfWorkHasCommittedOrAborted();
    EXPECT_GT(recoveryUnit->getPrepareCommitOrAbortCount(), generation);
    recoveryUnit->waitForPrepareConflictResolution(interruptible, generation);
}

TEST_F(WiredTigerPrepareConflictTest, NotificationWaitThrowsWhenBlockingDisallowed) {
    auto generation = recoveryUnit->getPrepareCommitOrAbortCount();
    recoveryUnit->setBlockingAllowed(false);
    EXPECT_THAT([&] { recoveryUnit->waitForPrepareConflictResolution(interruptible, generation); },
                testing::Throws<StorageUnavailableException>(
                    unittest::match::CodeIs(ErrorCodes::WriteConflict)));
}

TEST_F(WiredTigerPrepareConflictTest, UnrelatedNotificationsAllowReadRecheck) {
    auto& connection = *WiredTigerRecoveryUnit::get(*recoveryUnit).getConnection();
    auto generation = recoveryUnit->getPrepareCommitOrAbortCount();
    for (int attempt = 0; attempt < 2; ++attempt) {
        connection.notifyPreparedUnitOfWorkHasCommittedOrAborted();
        recoveryUnit->waitForPrepareConflictResolution(interruptible, generation);
        auto nextGeneration = recoveryUnit->getPrepareCommitOrAbortCount();
        EXPECT_GT(nextGeneration, generation);
        generation = nextGeneration;
    }
}

TEST_F(WiredTigerPrepareConflictTest, NotificationWaitPropagatesInterruption) {
    EXPECT_THAT(
        [&] {
            recoveryUnit->waitForPrepareConflictResolution(
                interruptible, recoveryUnit->getPrepareCommitOrAbortCount());
        },
        testing::Throws<DBException>(unittest::match::CodeIs(ErrorCodes::Interrupted)));
}

}  // namespace
}  // namespace mongo
