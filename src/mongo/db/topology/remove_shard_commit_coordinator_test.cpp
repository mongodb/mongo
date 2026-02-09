/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/topology/remove_shard_draining_progress_gen.h"
#include "mongo/db/topology/remove_shard_exception.h"
#include "mongo/db/topology/topology_change_helpers.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class RemoveShardCommitCoordinatorTest : public ConfigServerTestFixture {
public:
    RemoveShardCommitCoordinatorTest() : ConfigServerTestFixture(Options{}.useMockClock(true)) {}

protected:
    void setUp() override {
        setUpAndInitializeConfigDb();

        // Advance the mock clock to a reasonable starting point
        // This ensures timestamps are valid and we have room to work with time differences
        clockSource()->reset(Date_t::fromMillisSinceEpoch(1000 * 1000));
    }

    void tearDown() override {
        ConfigServerTestFixture::tearDown();
    }

    void insertRangeDeletionTask(const UUID& migrationId,
                                 const NamespaceString& nss,
                                 const UUID& collUuid,
                                 const ChunkRange& range,
                                 CleanWhenEnum whenToClean,
                                 const Timestamp& timestamp,
                                 boost::optional<bool> pending = boost::none,
                                 boost::optional<bool> processing = boost::none) {
        RangeDeletionTask task(
            migrationId, nss, collUuid, ShardId("donorShard"), range, whenToClean);
        task.setTimestamp(timestamp);

        if (pending) {
            task.setPending(*pending);
        }
        if (processing) {
            task.setProcessing(*processing);
        }

        PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
        store.add(operationContext(), task);
    }

    /**
     * Returns the current time in seconds since epoch from the mock clock.
     */
    uint32_t getCurrentTimeSecs() {
        return durationCount<Seconds>(clockSource()->now().toDurationSinceEpoch());
    }

    int _originalOrphanCleanupDelaySecs;

    const NamespaceString kTestNss =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl");
    const ChunkRange kTestRange{BSON("_id" << 0), BSON("_id" << 100)};
};

TEST_F(RemoveShardCommitCoordinatorTest, OrphanCleanupDelayCheckNoTasks) {
    orphanCleanupDelaySecs.store(60);

    auto task =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_FALSE(task.has_value());
}

TEST_F(RemoveShardCommitCoordinatorTest, OrphanCleanupDelayCheckElapsed) {
    orphanCleanupDelaySecs.store(60);

    auto currentTimeSecs = getCurrentTimeSecs();
    Timestamp taskTimestamp(currentTimeSecs, 1);
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            kTestRange,
                            CleanWhenEnum::kDelayed,
                            taskTimestamp,
                            false /* pending */);

    clockSource()->advance(Seconds(100));

    auto task =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_TRUE(task.has_value());
    ASSERT_DOES_NOT_THROW(
        topology_change_helpers::checkOrphanCleanupDelayElapsed(operationContext(), *task));
}

TEST_F(RemoveShardCommitCoordinatorTest, OrphanCleanupDelayCheckNotElapsed) {
    orphanCleanupDelaySecs.store(60);

    auto currentTimeSecs = getCurrentTimeSecs();
    Timestamp taskTimestamp(currentTimeSecs, 1);
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            kTestRange,
                            CleanWhenEnum::kDelayed,
                            taskTimestamp,
                            false /* pending */);

    clockSource()->advance(Seconds(30));

    auto task =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_TRUE(task.has_value());
    ASSERT_THROWS_CODE(
        topology_change_helpers::checkOrphanCleanupDelayElapsed(operationContext(), *task),
        DBException,
        ErrorCodes::RemoveShardDrainingInProgress);
}

TEST_F(RemoveShardCommitCoordinatorTest, OrphanCleanupDelayCheckUsesLatestTimestamp) {
    orphanCleanupDelaySecs.store(60);

    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            ChunkRange{BSON("_id" << 0), BSON("_id" << 50)},
                            CleanWhenEnum::kDelayed,
                            Timestamp(900, 1),
                            false /* pending */);

    auto currentTimeSecs = getCurrentTimeSecs();
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            ChunkRange{BSON("_id" << 50), BSON("_id" << 100)},
                            CleanWhenEnum::kDelayed,
                            Timestamp(currentTimeSecs, 1),
                            false /* pending */);

    clockSource()->advance(Seconds(30));

    auto task =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_TRUE(task.has_value());
    ASSERT_THROWS_CODE(
        topology_change_helpers::checkOrphanCleanupDelayElapsed(operationContext(), *task),
        DBException,
        ErrorCodes::RemoveShardDrainingInProgress);

    clockSource()->advance(Seconds(40));

    // Re-fetch task since we need the same task reference
    task = topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_TRUE(task.has_value());
    ASSERT_DOES_NOT_THROW(
        topology_change_helpers::checkOrphanCleanupDelayElapsed(operationContext(), *task));
}

TEST_F(RemoveShardCommitCoordinatorTest, OrphanCleanupDelayCheckSmallDelay) {
    orphanCleanupDelaySecs.store(0);

    auto currentTimeSecs = getCurrentTimeSecs();
    Timestamp taskTimestamp(currentTimeSecs, 1);
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            kTestRange,
                            CleanWhenEnum::kDelayed,
                            taskTimestamp,
                            false /* pending */);

    auto task =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_TRUE(task.has_value());
    ASSERT_DOES_NOT_THROW(
        topology_change_helpers::checkOrphanCleanupDelayElapsed(operationContext(), *task));
}

TEST_F(RemoveShardCommitCoordinatorTest, OrphanCleanupDelayCheckIncludesPendingTasks) {
    orphanCleanupDelaySecs.store(60);

    auto currentTimeSecs = getCurrentTimeSecs();
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            kTestRange,
                            CleanWhenEnum::kDelayed,
                            Timestamp(currentTimeSecs, 1),
                            true /* pending */);

    auto task =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_TRUE(task.has_value());
    ASSERT_THROWS_CODE(
        topology_change_helpers::checkOrphanCleanupDelayElapsed(operationContext(), *task),
        DBException,
        ErrorCodes::RemoveShardDrainingInProgress);
}

TEST_F(RemoveShardCommitCoordinatorTest, OrphanCleanupDelayCheckIgnoresProcessingTasks) {
    orphanCleanupDelaySecs.store(60);

    auto currentTimeSecs = getCurrentTimeSecs();
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            kTestRange,
                            CleanWhenEnum::kNow,  // Processing tasks have kNow
                            Timestamp(currentTimeSecs, 1),
                            boost::none /* pending not set */,
                            true /* processing */);

    auto task =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_FALSE(task.has_value());
}

TEST_F(RemoveShardCommitCoordinatorTest, PendingDataCleanupStateUsedForOrphanCleanupDelay) {
    RemoveShardProgress progress(ShardDrainingStateEnum::kPendingDataCleanup);
    ASSERT_EQ(ShardDrainingStateEnum::kPendingDataCleanup, progress.getState());

    RemoveShardDrainingInfo info(progress);
    ASSERT_EQ(ShardDrainingStateEnum::kPendingDataCleanup, info.getProgress().getState());

    ASSERT_EQ(ErrorCodes::RemoveShardDrainingInProgress, RemoveShardDrainingInfo::code);
}

TEST_F(RemoveShardCommitCoordinatorTest, OrphanCleanupDelayThrowsWithPendingDataCleanupState) {
    orphanCleanupDelaySecs.store(60);

    auto currentTimeSecs = getCurrentTimeSecs();
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            kTestRange,
                            CleanWhenEnum::kDelayed,
                            Timestamp(currentTimeSecs, 1),
                            false /* pending */);

    auto task =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_TRUE(task.has_value());

    try {
        topology_change_helpers::checkOrphanCleanupDelayElapsed(operationContext(), *task);
        FAIL("Expected RemoveShardDrainingInfo exception");
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::RemoveShardDrainingInProgress, ex.code());
        auto info = ex.extraInfo<RemoveShardDrainingInfo>();
        ASSERT(info);
        ASSERT_EQ(ShardDrainingStateEnum::kPendingDataCleanup, info->getProgress().getState());
    }
}

}  // namespace
}  // namespace mongo
