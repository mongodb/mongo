/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/vector_clock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/platform/random.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("foo", "bar");
const std::string kShardKey = "_id";
const BSONObj kShardKeyPattern = BSON(kShardKey << 1);

class RangeDeleterTest : public ShardServerTestFixture {
public:
    // Needed because UUID default constructor is private
    RangeDeleterTest() : _opCtx(nullptr), _uuid(UUID::gen()) {}

    void setUp() override {
        ShardServerTestFixture::setUp();
        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());
        _opCtx = operationContext();
        // Set up replication coordinator to be primary and have no replication delay.
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext());
        replCoord->setCanAcceptNonLocalWrites(true);
        std::ignore = replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY);
        // Make waitForWriteConcern return immediately.
        replCoord->setAwaitReplicationReturnValueFunction([this](const repl::OpTime& opTime) {
            return repl::ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
        });
        repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));

        {
            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(_opCtx);
            uassertStatusOK(createCollection(_opCtx, kNss.dbName(), BSON("create" << kNss.coll())));
        }

        AutoGetCollection autoColl(_opCtx, kNss, MODE_IX);
        _uuid = autoColl.getCollection()->uuid();
    }

    void tearDown() override {
        DBDirectClient client(_opCtx);
        client.dropCollection(kNss);

        while (migrationutil::getMigrationUtilExecutor(getServiceContext())->hasTasks()) {
            continue;
        }

        WaitForMajorityService::get(getServiceContext()).shutDown();
        ShardServerTestFixture::tearDown();
    }

    void setFilteringMetadataWithUUID(const UUID& uuid) {
        const OID epoch = OID::gen();

        auto rt = RoutingTableHistory::makeNew(
            kNss,
            uuid,
            kShardKeyPattern,
            false, /* unsplittable */
            nullptr,
            false,
            epoch,
            Timestamp(1, 1),
            boost::none /* timeseriesFields */,
            boost::none /* reshardingFields */,

            true,
            {ChunkType{uuid,
                       ChunkRange{BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)},
                       ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}),
                       ShardId("dummyShardId")}});
        ChunkManager cm(ShardId("dummyShardId"),
                        DatabaseVersion(UUID::gen(), Timestamp(1, 1)),
                        makeStandaloneRoutingTableHistory(std::move(rt)),
                        boost::none);
        AutoGetDb autoDb(_opCtx, kNss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(_opCtx, kNss, MODE_IX);
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(_opCtx, kNss)
            ->setFilteringMetadata(_opCtx,
                                   CollectionMetadata(std::move(cm), ShardId("dummyShardId")));
    }

    UUID uuid() const {
        return _uuid;
    }

protected:
    OperationContext* _opCtx;

private:
    UUID _uuid;
};

/**
 * Simple fixture for testing functions to rename range deletions.
 */
class RenameRangeDeletionsTest : public RangeDeleterTest {
public:
    const NamespaceString kToNss =
        NamespaceString::createNamespaceString_forTest(kNss.db_forTest(), "toColl");

    void setUp() override {
        RangeDeleterTest::setUp();

        // Suspending range deletions in order to rename tasks with "pending" set to false.
        // Otherwise, they could potentially complete before the rename.
        globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::alwaysOn);
    }

    void tearDown() override {
        DBDirectClient client(_opCtx);
        client.dropCollection(kToNss);
        // Re-enabling range deletions to drain tasks on the executor
        globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::off);
        RangeDeleterTest::tearDown();
    }
};

// Helper function to count number of documents in config.rangeDeletions.
int countDocsInConfigRangeDeletions(PersistentTaskStore<RangeDeletionTask>& store,
                                    OperationContext* opCtx) {
    auto numDocsInRangeDeletionsCollection = 0;
    store.forEach(opCtx, BSONObj(), [&](const RangeDeletionTask&) {
        ++numDocsInRangeDeletionsCollection;
        return true;
    });
    return numDocsInRangeDeletionsCollection;
};

// The 'pending' field must not be set in order for a range deletion task to succeed, but the
// ShardServerOpObserver will submit the task for deletion upon seeing an insert without the
// 'pending' field. The tests call removeDocumentsFromRange directly, so we want to avoid having
// the op observer also submit the task. The ShardServerOpObserver will ignore replacement
//  updates on the range deletions namespace though, so we can get around the issue by inserting
// the task with the 'pending' field set, and then remove the field using a replacement update
// after.
RangeDeletionTask insertRangeDeletionTask(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const UUID& uuid,
                                          const ChunkRange& range,
                                          int64_t numOrphans) {
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    auto migrationId = UUID::gen();
    RangeDeletionTask t(migrationId, nss, uuid, ShardId("donor"), range, CleanWhenEnum::kDelayed);
    t.setPending(true);
    t.setNumOrphanDocs(numOrphans);
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    t.setTimestamp(currentTime.clusterTime().asTimestamp());
    store.add(opCtx, t);

    // Document should be in the store.
    ASSERT_GTE(countDocsInConfigRangeDeletions(store, opCtx), 1);

    auto query = BSON(RangeDeletionTask::kIdFieldName << migrationId);
    t.setPending(boost::none);
    auto update = t.toBSON();
    store.update(opCtx, query, update);

    return t;
}

RangeDeletionTask insertRangeDeletionTask(OperationContext* opCtx,
                                          const UUID& uuid,
                                          const ChunkRange& range,
                                          int64_t numOrphans = 0) {
    return insertRangeDeletionTask(opCtx, kNss, uuid, range, numOrphans);
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
}  // namespace

/**
 *  Tests that the rename range deletion flow:
 *  - Renames range deletions from source to target collection
 *  - Doesn't leave garbage
 */
TEST_F(RenameRangeDeletionsTest, BasicRenameRangeDeletionsTest) {
    const auto numTasks = 10;
    std::vector<RangeDeletionTask> tasks;

    // Insert initial range deletions associated to the FROM collection
    PersistentTaskStore<RangeDeletionTask> rangeDeletionsStore(
        NamespaceString::kRangeDeletionNamespace);
    for (int i = 0; i < numTasks; i++) {
        const auto range = ChunkRange(BSON(kShardKey << 0), BSON(kShardKey << 1));
        RangeDeletionTask task(
            UUID::gen(), kNss, UUID::gen(), ShardId("donor"), range, CleanWhenEnum::kDelayed);
        task.setPending(false);
        tasks.push_back(task);
        rangeDeletionsStore.add(_opCtx, task);
    }

    // Rename range deletions
    rangedeletionutil::snapshotRangeDeletionsForRename(_opCtx, kNss, kToNss);
    rangedeletionutil::restoreRangeDeletionTasksForRename(_opCtx, kToNss);
    rangedeletionutil::deleteRangeDeletionTasksForRename(_opCtx, kNss, kToNss);

    const auto targetRangeDeletionsQuery =
        BSON(RangeDeletionTask::kNssFieldName << kToNss.ns_forTest());

    // Make sure range deletions for the TO collection are found
    ASSERT_EQ(10, rangeDeletionsStore.count(_opCtx, targetRangeDeletionsQuery));
    int foundTasks = 0;
    rangeDeletionsStore.forEach(
        _opCtx, targetRangeDeletionsQuery, [&](const RangeDeletionTask& newTask) {
            auto task = tasks.at(foundTasks++);
            ASSERT_EQ(newTask.getNss(), kToNss);
            ASSERT_EQ(newTask.getCollectionUuid(), task.getCollectionUuid());
            ASSERT_EQ(newTask.getDonorShardId(), task.getDonorShardId());
            ASSERT(SimpleBSONObjComparator::kInstance.evaluate(newTask.getRange().toBSON() ==
                                                               task.getRange().toBSON()));
            ASSERT(newTask.getWhenToClean() == task.getWhenToClean());
            return true;
        });
    ASSERT_EQ(foundTasks, numTasks);

    // Make sure no garbage is left in intermediate collection
    PersistentTaskStore<RangeDeletionTask> forRenameStore(
        NamespaceString::kRangeDeletionForRenameNamespace);
    ASSERT_EQ(0, forRenameStore.count(_opCtx, BSONObj()));
}

/**
 *  Same as BasicRenameRangeDeletionsTest, but also tests idempotency of single utility functions
 */
TEST_F(RenameRangeDeletionsTest, IdempotentRenameRangeDeletionsTest) {
    PseudoRandom random(SecureRandom().nextInt64());
    auto generateRandomNumberFrom1To10 = [&random]() {
        return random.nextInt32(9) + 1;
    };

    const auto numTasks = 10;
    std::vector<RangeDeletionTask> tasks;

    // Insert initial range deletions associated to the FROM collection
    PersistentTaskStore<RangeDeletionTask> rangeDeletionsStore(
        NamespaceString::kRangeDeletionNamespace);
    for (int i = 0; i < numTasks; i++) {
        const auto range = ChunkRange(BSON(kShardKey << 0), BSON(kShardKey << 1));
        RangeDeletionTask task(
            UUID::gen(), kNss, UUID::gen(), ShardId("donor"), range, CleanWhenEnum::kDelayed);
        tasks.push_back(task);
        task.setPending(false);
        rangeDeletionsStore.add(_opCtx, task);
    }

    // Rename range deletions, repeating idempotent steps several times
    auto randomLoopNTimes = generateRandomNumberFrom1To10();
    for (int i = 0; i < randomLoopNTimes; i++) {
        rangedeletionutil::snapshotRangeDeletionsForRename(_opCtx, kNss, kToNss);
    }
    randomLoopNTimes = generateRandomNumberFrom1To10();
    for (int i = 0; i < randomLoopNTimes; i++) {
        rangedeletionutil::restoreRangeDeletionTasksForRename(_opCtx, kToNss);
    }
    randomLoopNTimes = generateRandomNumberFrom1To10();
    for (int i = 0; i < randomLoopNTimes; i++) {
        rangedeletionutil::deleteRangeDeletionTasksForRename(_opCtx, kNss, kToNss);
    }

    const auto targetRangeDeletionsQuery =
        BSON(RangeDeletionTask::kNssFieldName << kToNss.ns_forTest());

    // Make sure range deletions for the TO collection are found
    ASSERT_EQ(10, rangeDeletionsStore.count(_opCtx, targetRangeDeletionsQuery));
    int foundTasks = 0;
    rangeDeletionsStore.forEach(
        _opCtx, targetRangeDeletionsQuery, [&](const RangeDeletionTask& newTask) {
            auto task = tasks.at(foundTasks++);
            ASSERT_EQ(newTask.getNss(), kToNss);
            ASSERT_EQ(newTask.getCollectionUuid(), task.getCollectionUuid());
            ASSERT_EQ(newTask.getDonorShardId(), task.getDonorShardId());
            ASSERT(SimpleBSONObjComparator::kInstance.evaluate(newTask.getRange().toBSON() ==
                                                               task.getRange().toBSON()));
            ASSERT(newTask.getWhenToClean() == task.getWhenToClean());
            return true;
        });
    ASSERT_EQ(foundTasks, numTasks);

    // Make sure no garbage is left in intermediate collection
    PersistentTaskStore<RangeDeletionTask> forRenameStore(
        NamespaceString::kRangeDeletionForRenameNamespace);
    ASSERT_EQ(0, forRenameStore.count(_opCtx, BSONObj()));
}

// Test that rangedeletionutil::overlappingRangeDeletionsQuery() can handle the cases that we expect
// to encounter.
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
TEST_F(RenameRangeDeletionsTest, overlappingRangeDeletionsQueryWithIntegerShardKey) {
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
    auto results =
        store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range1, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(rangedeletionutil::checkForConflictingDeletions(opCtx, range1, uuid));

    // 2, 3. Find overlapping ranges, either direction.
    auto range2 = ChunkRange{BSON("_id" << 5), BSON("_id" << 15)};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range2, uuid));
    ASSERT_EQ(results, 2);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range2, uuid));

    // 4. Identical range
    auto range4 = ChunkRange{BSON("_id" << 10), BSON("_id" << 20)};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range4, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range4, uuid));

    // 5, 6. Find overlapping edge, either direction.
    auto range5 = ChunkRange{BSON("_id" << 0), BSON("_id" << 5)};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range5, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range5, uuid));
    auto range6 = ChunkRange{BSON("_id" << 5), BSON("_id" << 10)};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range6, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range6, uuid));

    // 7. Find fully enclosed range
    auto range7 = ChunkRange{BSON("_id" << 12), BSON("_id" << 18)};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range7, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range7, uuid));

    // 8, 9. Open max doesn't overlap closed min, either direction.
    auto range8 = ChunkRange{BSON("_id" << 30), BSON("_id" << 40)};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range8, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(rangedeletionutil::checkForConflictingDeletions(opCtx, range8, uuid));
    auto range9 = ChunkRange{BSON("_id" << 20), BSON("_id" << 30)};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range9, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(rangedeletionutil::checkForConflictingDeletions(opCtx, range9, uuid));
}

TEST_F(RenameRangeDeletionsTest,
       overlappingRangeDeletionsQueryWithCompoundShardKeyWhereFirstValueIsConstant) {
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
    auto results =
        store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range1, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(rangedeletionutil::checkForConflictingDeletions(opCtx, range1, uuid));

    // 2, 3. Find overlapping ranges, either direction.
    auto range2 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 5)),
                             BSON("_id" << BSON("a" << 0 << "b" << 15))};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range2, uuid));
    ASSERT_EQ(results, 2);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range2, uuid));

    // 4. Identical range
    auto range4 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 10)),
                             BSON("_id" << BSON("a" << 0 << "b" << 20))};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range4, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range4, uuid));

    // 5, 6. Find overlapping edge, either direction.
    auto range5 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 0)),
                             BSON("_id" << BSON("a" << 0 << "b" << 5))};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range5, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range5, uuid));
    auto range6 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 5)),
                             BSON("_id" << BSON("a" << 0 << "b" << 10))};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range6, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range6, uuid));

    // 7. Find fully enclosed range
    auto range7 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 12)),
                             BSON("_id" << BSON("a" << 0 << "b" << 18))};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range7, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range7, uuid));

    // 8, 9. Open max doesn't overlap closed min, either direction.
    auto range8 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 30)),
                             BSON("_id" << BSON("a" << 0 << "b" << 40))};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range8, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(rangedeletionutil::checkForConflictingDeletions(opCtx, range8, uuid));
    auto range9 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 20)),
                             BSON("_id" << BSON("a" << 0 << "b" << 30))};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range9, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(rangedeletionutil::checkForConflictingDeletions(opCtx, range9, uuid));
}

TEST_F(RenameRangeDeletionsTest,
       overlappingRangeDeletionsQueryWithCompoundShardKeyWhereSecondValueIsConstant) {
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
    auto results =
        store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range1, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(rangedeletionutil::checkForConflictingDeletions(opCtx, range1, uuid));

    // 2, 3. Find overlapping ranges, either direction.
    auto range2 = ChunkRange{BSON("_id" << BSON("a" << 5 << "b" << 0)),
                             BSON("_id" << BSON("a" << 15 << "b" << 0))};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range2, uuid));
    ASSERT_EQ(results, 2);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range2, uuid));

    // 4. Identical range
    auto range4 = ChunkRange{BSON("_id" << BSON("a" << 10 << "b" << 0)),
                             BSON("_id" << BSON("a" << 20 << "b" << 0))};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range4, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range4, uuid));

    // 5, 6. Find overlapping edge, either direction.
    auto range5 = ChunkRange{BSON("_id" << BSON("a" << 0 << "b" << 0)),
                             BSON("_id" << BSON("a" << 5 << "b" << 0))};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range5, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range5, uuid));
    auto range6 = ChunkRange{BSON("_id" << BSON("a" << 5 << "b" << 0)),
                             BSON("_id" << BSON("a" << 10 << "b" << 0))};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range6, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range6, uuid));

    // 7. Find fully enclosed range
    auto range7 = ChunkRange{BSON("_id" << BSON("a" << 12 << "b" << 0)),
                             BSON("_id" << BSON("a" << 18 << "b" << 0))};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range7, uuid));
    ASSERT_EQ(results, 1);
    ASSERT(rangedeletionutil::checkForConflictingDeletions(opCtx, range7, uuid));

    // 8, 9. Open max doesn't overlap closed min, either direction.
    auto range8 = ChunkRange{BSON("_id" << BSON("a" << 30 << "b" << 0)),
                             BSON("_id" << BSON("a" << 40 << "b" << 0))};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range8, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(rangedeletionutil::checkForConflictingDeletions(opCtx, range8, uuid));
    auto range9 = ChunkRange{BSON("_id" << BSON("a" << 20 << "b" << 0)),
                             BSON("_id" << BSON("a" << 30 << "b" << 0))};
    results = store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range9, uuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(rangedeletionutil::checkForConflictingDeletions(opCtx, range9, uuid));
}

TEST_F(RenameRangeDeletionsTest, TestInvalidUUID) {
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
    auto results =
        store.count(opCtx, rangedeletionutil::overlappingRangeDeletionsQuery(range, wrongUuid));
    ASSERT_EQ(results, 0);
    ASSERT_FALSE(rangedeletionutil::checkForConflictingDeletions(opCtx, range, wrongUuid));
}
}  // namespace mongo
