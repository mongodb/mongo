/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/*
 * Utility class providing a range deletion task and the associated active queries completion future
 */
class RangeDeletionWithOngoingQueries {
public:
    RangeDeletionWithOngoingQueries(const RangeDeletionTask& t) : _task(t) {}

    RangeDeletionTask getTask() {
        return _task;
    }

    void drainOngoingQueries() {
        _ongoingQueries.setFrom(Status::OK());
    }

    auto getOngoingQueriesFuture() {
        return _ongoingQueries.getFuture().semi();
    }

private:
    RangeDeletionTask _task;
    SharedPromise<void> _ongoingQueries;
};

RangeDeletionWithOngoingQueries createRangeDeletionTaskForTesting(
    const UUID& collectionUUID,
    const BSONObj& min,
    const BSONObj& max,
    const CleanWhenEnum whenToClean = CleanWhenEnum::kNow) {
    RangeDeletionTask rdt;
    rdt.setCollectionUuid(collectionUUID);
    rdt.setRange(ChunkRange(min, max));
    rdt.setWhenToClean(whenToClean);
    return RangeDeletionWithOngoingQueries(rdt);
}

class RangeDeleterServiceTest : public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();
        opCtx = operationContext();
    }

    void tearDown() override {
        ShardServerTestFixture::tearDown();
    }

    OperationContext* opCtx;

    // Instantiate some collection UUIDs and tasks to be used for testing
    const UUID uuidCollA = UUID::gen();
    RangeDeletionWithOngoingQueries rangeDeletionTask0ForCollA =
        createRangeDeletionTaskForTesting(uuidCollA, BSON("a" << 0), BSON("a" << 10));
    RangeDeletionWithOngoingQueries rangeDeletionTask1ForCollA =
        createRangeDeletionTaskForTesting(uuidCollA, BSON("a" << 10), BSON("a" << 20));
    const UUID uuidCollB = UUID::gen();
    RangeDeletionWithOngoingQueries rangeDeletionTask0ForCollB =
        createRangeDeletionTaskForTesting(uuidCollB, BSON("a" << 0), BSON("a" << 10));
};

TEST_F(RangeDeleterServiceTest, RegisterAndProcessSingleTask) {
    RangeDeleterService rds;
    auto* taskWithOngoingQueries = &rangeDeletionTask0ForCollA;

    auto completionFuture = rds.registerTask(taskWithOngoingQueries->getTask(),
                                             taskWithOngoingQueries->getOngoingQueriesFuture());
    // The task can't be processed (hence completed) before ongoing queries drain
    ASSERT(!completionFuture.isReady());
    ASSERT_EQ(1, rds.getNumRangeDeletionTasksForCollection(uuidCollA));

    // Pretend ongoing queries have drained and check task is processed
    taskWithOngoingQueries->drainOngoingQueries();
    completionFuture.get(opCtx);
    ASSERT_EQ(0, rds.getNumRangeDeletionTasksForCollection(uuidCollA));
}

TEST_F(RangeDeleterServiceTest, RegisterDuplicateTaskForSameRangeReturnsOriginalFuture) {
    RangeDeleterService rds;
    auto* taskWithOngoingQueries = &rangeDeletionTask0ForCollA;

    auto originalTaskCompletionFuture = rds.registerTask(
        taskWithOngoingQueries->getTask(), taskWithOngoingQueries->getOngoingQueriesFuture());

    // Trying registering a duplicate task must return a future without throwing errors
    auto duplicateTaskCompletionFuture =
        rds.registerTask(taskWithOngoingQueries->getTask(), SemiFuture<void>::makeReady());

    // Check that the state of the original task is not affected
    ASSERT(!originalTaskCompletionFuture.isReady());

    // Check that no new task has been registered
    ASSERT_EQ(1, rds.getNumRangeDeletionTasksForCollection(uuidCollA));

    // Check that - upon range deletion completion - both original and "duplicate" futures are ready
    taskWithOngoingQueries->drainOngoingQueries();
    originalTaskCompletionFuture.get(opCtx);
    ASSERT(duplicateTaskCompletionFuture.isReady());
    ASSERT_EQ(0, rds.getNumRangeDeletionTasksForCollection(uuidCollA));
}

TEST_F(RangeDeleterServiceTest, RegisterAndProcessMoreTasksForSameCollection) {
    RangeDeleterService rds;
    auto* task0WithOngoingQueries = &rangeDeletionTask0ForCollA;
    auto* task1WithOngoingQueries = &rangeDeletionTask1ForCollA;

    // Register 2 tasks for the same collection
    auto completionFuture0 = rds.registerTask(task0WithOngoingQueries->getTask(),
                                              task0WithOngoingQueries->getOngoingQueriesFuture());
    auto completionFuture1 = rds.registerTask(task1WithOngoingQueries->getTask(),
                                              task1WithOngoingQueries->getOngoingQueriesFuture());

    // The tasks can't be processed (hence completed) before ongoing queries drain
    ASSERT(!completionFuture0.isReady());
    ASSERT(!completionFuture1.isReady());
    ASSERT_EQ(2, rds.getNumRangeDeletionTasksForCollection(uuidCollA));

    // Pretend ongoing queries have drained for task 1 and check it gets processed
    task1WithOngoingQueries->drainOngoingQueries();
    completionFuture1.get(opCtx);
    ASSERT_EQ(1, rds.getNumRangeDeletionTasksForCollection(uuidCollA));

    // Pretend ongoing queries have drained for task 0 and check it gets processed
    task0WithOngoingQueries->drainOngoingQueries();
    completionFuture0.get(opCtx);
    ASSERT_EQ(0, rds.getNumRangeDeletionTasksForCollection(uuidCollA));
}

TEST_F(RangeDeleterServiceTest, RegisterAndProcessTasksForDifferentCollections) {
    RangeDeleterService rds;
    auto* taskWithOngoingQueriesCollA = &rangeDeletionTask0ForCollA;
    auto* taskWithOngoingQueriesCollB = &rangeDeletionTask0ForCollB;

    // Register 1 tasks for `collA` and 1 task for `collB`
    auto completionFutureCollA =
        rds.registerTask(taskWithOngoingQueriesCollA->getTask(),
                         taskWithOngoingQueriesCollA->getOngoingQueriesFuture());
    auto completionFutureCollB =
        rds.registerTask(taskWithOngoingQueriesCollB->getTask(),
                         taskWithOngoingQueriesCollB->getOngoingQueriesFuture());

    // The tasks can't be processed (hence completed) before ongoing queries drain
    ASSERT(!completionFutureCollA.isReady());
    ASSERT(!completionFutureCollB.isReady());
    ASSERT_EQ(1, rds.getNumRangeDeletionTasksForCollection(uuidCollA));
    ASSERT_EQ(1, rds.getNumRangeDeletionTasksForCollection(uuidCollB));

    // Pretend ongoing queries have drained for task on coll B and check that:
    // - Task for `collB` gets processed
    // - Task for `collA` is not affected
    taskWithOngoingQueriesCollB->drainOngoingQueries();
    completionFutureCollB.get(opCtx);
    ASSERT_EQ(0, rds.getNumRangeDeletionTasksForCollection(uuidCollB));
    ASSERT_EQ(1, rds.getNumRangeDeletionTasksForCollection(uuidCollA));

    // Pretend ongoing queries have drained for task on coll A and check it gets processed
    taskWithOngoingQueriesCollA->drainOngoingQueries();
    completionFutureCollA.get(opCtx);
    ASSERT_EQ(0, rds.getNumRangeDeletionTasksForCollection(uuidCollA));
}

TEST_F(RangeDeleterServiceTest, DelayForSecondaryQueriesIsHonored) {
    int defaultOrphanCleanupDelaySecs = orphanCleanupDelaySecs.load();
    ScopeGuard reset([=] { orphanCleanupDelaySecs.store(defaultOrphanCleanupDelaySecs); });

    // Set delay for waiting secondary queries to 2 seconds
    orphanCleanupDelaySecs.store(2);

    RangeDeleterService rds;
    auto taskWithOngoingQueries = createRangeDeletionTaskForTesting(
        uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed);

    auto completionFuture = rds.registerTask(taskWithOngoingQueries.getTask(),
                                             taskWithOngoingQueries.getOngoingQueriesFuture());

    // Check that the task lasts at least 2 seconds from the moment ongoing queries drain
    auto start = Date_t::now();
    taskWithOngoingQueries.drainOngoingQueries();
    completionFuture.get(opCtx);
    auto end = Date_t::now();

    auto elapsed = end.toDurationSinceEpoch() - start.toDurationSinceEpoch();
    ASSERT(elapsed >= Milliseconds(2000));
}

}  // namespace mongo
