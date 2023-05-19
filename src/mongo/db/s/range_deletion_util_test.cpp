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

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/metadata_manager.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/vector_clock.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/death_test.h"
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
        replCoord->setAwaitReplicationReturnValueFunction([this](OperationContext* opCtx,
                                                                 const repl::OpTime& opTime) {
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
        NamespaceString::createNamespaceString_forTest(kNss.db(), "toColl");

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
    snapshotRangeDeletionsForRename(_opCtx, kNss, kToNss);
    restoreRangeDeletionTasksForRename(_opCtx, kToNss);
    deleteRangeDeletionTasksForRename(_opCtx, kNss, kToNss);

    const auto targetRangeDeletionsQuery = BSON(RangeDeletionTask::kNssFieldName << kToNss.ns());

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
        snapshotRangeDeletionsForRename(_opCtx, kNss, kToNss);
    }
    randomLoopNTimes = generateRandomNumberFrom1To10();
    for (int i = 0; i < randomLoopNTimes; i++) {
        restoreRangeDeletionTasksForRename(_opCtx, kToNss);
    }
    randomLoopNTimes = generateRandomNumberFrom1To10();
    for (int i = 0; i < randomLoopNTimes; i++) {
        deleteRangeDeletionTasksForRename(_opCtx, kNss, kToNss);
    }

    const auto targetRangeDeletionsQuery = BSON(RangeDeletionTask::kNssFieldName << kToNss.ns());

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

}  // namespace
}  // namespace mongo
