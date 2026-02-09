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

#include "mongo/db/topology/topology_change_helpers.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class TopologyChangeHelpersTest : public ConfigServerTestFixture {
protected:
    void setUp() override {
        setUpAndInitializeConfigDb();
        setupShards({ShardType(kShardId.toString(), "host0:12345")});
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

    const NamespaceString kTestNss =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl");
    const ChunkRange kTestRange{BSON("_id" << 0), BSON("_id" << 100)};
    const ShardId kShardId{"shard0"};
};

TEST_F(TopologyChangeHelpersTest, NoRangeDeletionTasks) {
    auto result =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_FALSE(result.has_value());
}

TEST_F(TopologyChangeHelpersTest, OnlyPendingTasks) {
    Timestamp expectedTimestamp(100, 1);
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            kTestRange,
                            CleanWhenEnum::kDelayed,
                            expectedTimestamp,
                            true /* pending */);

    auto result =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result->getTimestamp(), expectedTimestamp);
}

TEST_F(TopologyChangeHelpersTest, OnlyProcessingTasks) {
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            kTestRange,
                            CleanWhenEnum::kDelayed,
                            Timestamp(100, 1),
                            boost::none /* pending not set */,
                            true /* processing */);

    auto result =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_FALSE(result.has_value());
}

TEST_F(TopologyChangeHelpersTest, SingleNonPendingTask) {
    Timestamp expectedTimestamp(100, 1);
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            kTestRange,
                            CleanWhenEnum::kDelayed,
                            expectedTimestamp,
                            false /* pending */);

    auto result =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result->getTimestamp(), expectedTimestamp);
}

TEST_F(TopologyChangeHelpersTest, TaskWithNoPendingField) {
    Timestamp expectedTimestamp(200, 1);
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            kTestRange,
                            CleanWhenEnum::kDelayed,
                            expectedTimestamp,
                            boost::none /* pending not set */);

    auto result =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result->getTimestamp(), expectedTimestamp);
}

TEST_F(TopologyChangeHelpersTest, MultipleTasksReturnsLatest) {
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            ChunkRange{BSON("_id" << 0), BSON("_id" << 50)},
                            CleanWhenEnum::kDelayed,
                            Timestamp(100, 1),
                            false /* pending */);

    Timestamp latestTimestamp(300, 1);
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            ChunkRange{BSON("_id" << 50), BSON("_id" << 100)},
                            CleanWhenEnum::kDelayed,
                            latestTimestamp,
                            false /* pending */);

    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            ChunkRange{BSON("_id" << 100), BSON("_id" << 150)},
                            CleanWhenEnum::kDelayed,
                            Timestamp(200, 1),
                            false /* pending */);

    auto result =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result->getTimestamp(), latestTimestamp);
}

TEST_F(TopologyChangeHelpersTest, MixedTasksFiltersCorrectly) {
    Timestamp expectedTimestamp(500, 1);
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            ChunkRange{BSON("_id" << 0), BSON("_id" << 10)},
                            CleanWhenEnum::kDelayed,
                            expectedTimestamp,
                            true /* pending */);

    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            ChunkRange{BSON("_id" << 10), BSON("_id" << 20)},
                            CleanWhenEnum::kNow,
                            Timestamp(400, 1),
                            boost::none /* pending not set */,
                            true /* processing */);

    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            ChunkRange{BSON("_id" << 30), BSON("_id" << 40)},
                            CleanWhenEnum::kDelayed,
                            Timestamp(350, 1),
                            false /* pending */);

    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            ChunkRange{BSON("_id" << 40), BSON("_id" << 50)},
                            CleanWhenEnum::kDelayed,
                            Timestamp(200, 1),
                            false /* pending */);

    auto result =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result->getTimestamp(), expectedTimestamp);
}

TEST_F(TopologyChangeHelpersTest, PendingFalseExplicitlySet) {
    Timestamp expectedTimestamp(150, 5);
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            kTestRange,
                            CleanWhenEnum::kDelayed,
                            expectedTimestamp,
                            false /* pending explicitly false */);

    auto result =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result->getTimestamp(), expectedTimestamp);
}

TEST_F(TopologyChangeHelpersTest, ProcessingFalseExplicitlySet) {
    Timestamp expectedTimestamp(175, 3);
    insertRangeDeletionTask(UUID::gen(),
                            kTestNss,
                            UUID::gen(),
                            kTestRange,
                            CleanWhenEnum::kDelayed,
                            expectedTimestamp,
                            false /* pending */,
                            false /* processing */);

    auto result =
        topology_change_helpers::getLatestNonProcessingRangeDeletionTask(operationContext());
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result->getTimestamp(), expectedTimestamp);
}

}  // namespace
}  // namespace mongo
