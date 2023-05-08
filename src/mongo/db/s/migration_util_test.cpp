/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/collection_sharding_runtime_test.cpp"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/shard_server_catalog_cache_loader.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/vector_clock.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/s/database_version.h"
#include "mongo/util/future.h"

namespace mongo {
namespace {

using MigrationUtilsTest = ShardServerTestFixture;

UUID getCollectionUuid(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollection autoColl(opCtx, nss, MODE_IS);

    ASSERT(autoColl.getCollection());

    return autoColl.getCollection()->uuid();
}

template <typename ShardKey>
RangeDeletionTask createDeletionTask(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const UUID& uuid,
                                     ShardKey min,
                                     ShardKey max,
                                     ShardId donorShard = ShardId("donorShard"),
                                     bool pending = true) {
    auto task = RangeDeletionTask(UUID::gen(),
                                  nss,
                                  uuid,
                                  donorShard,
                                  ChunkRange{BSON("_id" << min), BSON("_id" << max)},
                                  CleanWhenEnum::kNow);
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    task.setTimestamp(currentTime.clusterTime().asTimestamp());

    if (pending)
        task.setPending(true);

    return task;
}

// Test that overlappingRangeQuery() can handle the cases that we expect to encounter.
//           1    1    2    2    3    3    4    4    5
// 0----5----0----5----0----5----0----5----0----5----0
//                          |---------O                Range 1 [25, 35)
//      |---------O                                    Range 2 [5, 15)
//           |---------O                               Range 4 [10, 20)
// |----O                                              Range 5 [0, 5)
//             |-----O                                 Range 7 [12, 18)
//                               |---------O           Range 8 [30, 40)
// Ranges in store
// |---------O                                         [0, 10)
//           |---------O                               [10, 20)
//                                         |---------O [40 50)
//           1    1    2    2    3    3    4    4    5
// 0----5----0----5----0----5----0----5----0----5----0
TEST_F(MigrationUtilsTest, TestOverlappingRangeQueryWithIntegerShardKey) {
    auto opCtx = operationContext();
    const auto uuid = UUID::gen();
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);

    store.add(opCtx,
              createDeletionTask(
                  opCtx, NamespaceString::createNamespaceString_forTest("one"), uuid, 0, 10));
    store.add(opCtx,
              createDeletionTask(
                  opCtx, NamespaceString::createNamespaceString_forTest("two"), uuid, 10, 20));
    store.add(opCtx,
              createDeletionTask(
                  opCtx, NamespaceString::createNamespaceString_forTest("three"), uuid, 40, 50));

    ASSERT_EQ(store.count(opCtx), 3);

    // 1. Non-overlapping range
    auto range1 = ChunkRange{BSON("_id" << 25), BSON("_id" << 35)};
    auto results = store.count(opCtx, migrationutil::overlappingRangeQuery(range1, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range1, uuid));

    // 2, 3. Find overlapping ranges, either direction.
    auto range2 = ChunkRange{BSON("_id" << 5), BSON("_id" << 15)};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range2, uuid));
    ASSERT_EQ(results, 2);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range2, uuid));

    // 4. Identical range
    auto range4 = ChunkRange{BSON("_id" << 10), BSON("_id" << 20)};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range4, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range4, uuid));

    // 5, 6. Find overlapping edge, either direction.
    auto range5 = ChunkRange{BSON("_id" << 0), BSON("_id" << 5)};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range5, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range5, uuid));
    auto range6 = ChunkRange{BSON("_id" << 5), BSON("_id" << 10)};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range6, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range6, uuid));

    // 7. Find fully enclosed range
    auto range7 = ChunkRange{BSON("_id" << 12), BSON("_id" << 18)};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range7, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range7, uuid));

    // 8, 9. Open max doesn't overlap closed min, either direction.
    auto range8 = ChunkRange{BSON("_id" << 30), BSON("_id" << 40)};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range8, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range8, uuid));
    auto range9 = ChunkRange{BSON("_id" << 20), BSON("_id" << 30)};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range9, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range9, uuid));
}

TEST_F(MigrationUtilsTest, TestOverlappingRangeQueryWithCompoundShardKeyWhereFirstValueIsConstant) {
    auto opCtx = operationContext();
    const auto uuid = UUID::gen();
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);

    auto deletionTasks = {
        createDeletionTask(opCtx,
                           NamespaceString::createNamespaceString_forTest("one"),
                           uuid,
                           BSON("a" << 0 << "b" << 0),
                           BSON("a" << 0 << "b" << 10)),
        createDeletionTask(opCtx,
                           NamespaceString::createNamespaceString_forTest("two"),
                           uuid,
                           BSON("a" << 0 << "b" << 10),
                           BSON("a" << 0 << "b" << 20)),
        createDeletionTask(opCtx,
                           NamespaceString::createNamespaceString_forTest("one"),
                           uuid,
                           BSON("a" << 0 << "b" << 40),
                           BSON("a" << 0 << "b" << 50)),
    };

    for (auto&& task : deletionTasks) {
        store.add(opCtx, task);
    }

    ASSERT_EQ(store.count(opCtx), 3);

    // 1. Non-overlapping range
    auto range1 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 25)),
                             BSON("_id" << BSON("a" << 0 << "b" << 35))};
    auto results = store.count(opCtx, migrationutil::overlappingRangeQuery(range1, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range1, uuid));

    // 2, 3. Find overlapping ranges, either direction.
    auto range2 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 5)),
                             BSON("_id" << BSON("a" << 0 << "b" << 15))};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range2, uuid));
    ASSERT_EQ(results, 2);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range2, uuid));

    // 4. Identical range
    auto range4 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 10)),
                             BSON("_id" << BSON("a" << 0 << "b" << 20))};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range4, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range4, uuid));

    // 5, 6. Find overlapping edge, either direction.
    auto range5 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 0)),
                             BSON("_id" << BSON("a" << 0 << "b" << 5))};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range5, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range5, uuid));
    auto range6 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 5)),
                             BSON("_id" << BSON("a" << 0 << "b" << 10))};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range6, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range6, uuid));

    // 7. Find fully enclosed range
    auto range7 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 12)),
                             BSON("_id" << BSON("a" << 0 << "b" << 18))};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range7, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range7, uuid));

    // 8, 9. Open max doesn't overlap closed min, either direction.
    auto range8 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 30)),
                             BSON("_id" << BSON("a" << 0 << "b" << 40))};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range8, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range8, uuid));
    auto range9 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 20)),
                             BSON("_id" << BSON("a" << 0 << "b" << 30))};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range9, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range9, uuid));
}

TEST_F(MigrationUtilsTest,
       TestOverlappingRangeQueryWithCompoundShardKeyWhereSecondValueIsConstant) {
    auto opCtx = operationContext();
    const auto uuid = UUID::gen();
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);

    auto deletionTasks = {
        createDeletionTask(opCtx,
                           NamespaceString::createNamespaceString_forTest("one"),
                           uuid,
                           BSON("a" << 0 << "b" << 0),
                           BSON("a" << 10 << "b" << 0)),
        createDeletionTask(opCtx,
                           NamespaceString::createNamespaceString_forTest("two"),
                           uuid,
                           BSON("a" << 10 << "b" << 0),
                           BSON("a" << 20 << "b" << 0)),
        createDeletionTask(opCtx,
                           NamespaceString::createNamespaceString_forTest("one"),
                           uuid,
                           BSON("a" << 40 << "b" << 0),
                           BSON("a" << 50 << "b" << 0)),
    };

    for (auto&& task : deletionTasks) {
        store.add(opCtx, task);
    }

    ASSERT_EQ(store.count(opCtx), 3);

    // 1. Non-overlapping range
    auto range1 = ChunkRange{BSON("_id" << BSON("a" << 25 << "b" << 0)),
                             BSON("_id" << BSON("a" << 35 << "b" << 0))};
    auto results = store.count(opCtx, migrationutil::overlappingRangeQuery(range1, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range1, uuid));

    // 2, 3. Find overlapping ranges, either direction.
    auto range2 = ChunkRange{BSON("_id" << BSON("a" << 5 << "b" << 0)),
                             BSON("_id" << BSON("a" << 15 << "b" << 0))};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range2, uuid));
    ASSERT_EQ(results, 2);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range2, uuid));

    // 4. Identical range
    auto range4 = ChunkRange{BSON("_id" << BSON("a" << 10 << "b" << 0)),
                             BSON("_id" << BSON("a" << 20 << "b" << 0))};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range4, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range4, uuid));

    // 5, 6. Find overlapping edge, either direction.
    auto range5 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 0)),
                             BSON("_id" << BSON("a" << 5 << "b" << 0))};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range5, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range5, uuid));
    auto range6 = ChunkRange{BSON("_id" << BSON("a" << 5 << "b" << 0)),
                             BSON("_id" << BSON("a" << 10 << "b" << 0))};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range6, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range6, uuid));

    // 7. Find fully enclosed range
    auto range7 = ChunkRange{BSON("_id" << BSON("a" << 12 << "b" << 0)),
                             BSON("_id" << BSON("a" << 18 << "b" << 0))};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range7, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(migrationutil::checkForConflictingDeletions(opCtx, range7, uuid));

    // 8, 9. Open max doesn't overlap closed min, either direction.
    auto range8 = ChunkRange{BSON("_id" << BSON("a" << 30 << "b" << 0)),
                             BSON("_id" << BSON("a" << 40 << "b" << 0))};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range8, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range8, uuid));
    auto range9 = ChunkRange{BSON("_id" << BSON("a" << 20 << "b" << 0)),
                             BSON("_id" << BSON("a" << 30 << "b" << 0))};
    results = store.count(opCtx, migrationutil::overlappingRangeQuery(range9, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range9, uuid));
}

TEST_F(MigrationUtilsTest, TestInvalidUUID) {
    auto opCtx = operationContext();
    const auto uuid = UUID::gen();
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);

    store.add(opCtx,
              createDeletionTask(
                  opCtx, NamespaceString::createNamespaceString_forTest("one"), uuid, 0, 10));
    store.add(opCtx,
              createDeletionTask(
                  opCtx, NamespaceString::createNamespaceString_forTest("two"), uuid, 10, 20));
    store.add(opCtx,
              createDeletionTask(
                  opCtx, NamespaceString::createNamespaceString_forTest("three"), uuid, 40, 50));

    ASSERT_EQ(store.count(opCtx), 3);

    const auto wrongUuid = UUID::gen();
    auto range = ChunkRange{BSON("_id" << 5), BSON("_id" << 15)};
    auto results = store.count(opCtx, migrationutil::overlappingRangeQuery(range, wrongUuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(migrationutil::checkForConflictingDeletions(opCtx, range, wrongUuid));
}

TEST_F(MigrationUtilsTest, TestUpdateNumberOfOrphans) {
    auto opCtx = operationContext();
    const auto collectionUuid = UUID::gen();
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    auto rangeDeletionDoc = createDeletionTask(opCtx, kTestNss, collectionUuid, 0, 10);
    store.add(opCtx, rangeDeletionDoc);

    persistUpdatedNumOrphans(opCtx, collectionUuid, rangeDeletionDoc.getRange(), 5);
    rangeDeletionDoc.setNumOrphanDocs(5);
    ASSERT_EQ(store.count(opCtx, rangeDeletionDoc.toBSON().removeField("timestamp")), 1);

    persistUpdatedNumOrphans(opCtx, collectionUuid, rangeDeletionDoc.getRange(), -5);
    rangeDeletionDoc.setNumOrphanDocs(0);
    ASSERT_EQ(store.count(opCtx, rangeDeletionDoc.toBSON().removeField("timestamp")), 1);
}

}  // namespace
}  // namespace mongo
