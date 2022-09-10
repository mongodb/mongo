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

#include "mongo/db/s/range_deleter_service_test.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/operation_sharding_state.h"

namespace mongo {

/**
 *  RangeDeleterServiceTest implementation
 */
void RangeDeleterServiceTest::setUp() {
    ShardServerTestFixture::setUp();
    opCtx = operationContext();
    RangeDeleterService::get(opCtx)->onStepUpComplete(opCtx, 0L);

    {
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            opCtx);
        uassertStatusOK(
            createCollection(opCtx, nsCollA.dbName(), BSON("create" << nsCollA.coll())));
        uassertStatusOK(
            createCollection(opCtx, nsCollB.dbName(), BSON("create" << nsCollB.coll())));
    }

    {
        AutoGetCollection autoColl(opCtx, nsCollA, MODE_IX);
        uuidCollA = autoColl.getCollection()->uuid();
    }
    {
        AutoGetCollection autoColl(opCtx, nsCollB, MODE_IX);
        uuidCollB = autoColl.getCollection()->uuid();
    }

    rangeDeletionTask0ForCollA =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA, BSON("a" << 0), BSON("a" << 10));
    rangeDeletionTask1ForCollA =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA, BSON("a" << 10), BSON("a" << 20));
    rangeDeletionTask0ForCollB =
        createRangeDeletionTaskWithOngoingQueries(uuidCollB, BSON("a" << 0), BSON("a" << 10));
}

void RangeDeleterServiceTest::tearDown() {
    RangeDeleterService::get(opCtx)->onStepDown();
    ShardServerTestFixture::tearDown();
}

RangeDeletionTask RangeDeleterServiceTest::createRangeDeletionTask(const UUID& collectionUUID,
                                                                   const BSONObj& min,
                                                                   const BSONObj& max,
                                                                   CleanWhenEnum whenToClean,
                                                                   bool pending) {
    RangeDeletionTask rdt;
    rdt.setId(UUID::gen());
    rdt.setNss(NamespaceString("test.mock"));
    rdt.setDonorShardId(ShardId("shard0"));
    rdt.setCollectionUuid(collectionUUID);
    rdt.setRange(ChunkRange(min, max));
    rdt.setWhenToClean(whenToClean);
    rdt.setPending(pending);
    return rdt;
}

std::shared_ptr<RangeDeletionWithOngoingQueries>
RangeDeleterServiceTest::createRangeDeletionTaskWithOngoingQueries(const UUID& collectionUUID,
                                                                   const BSONObj& min,
                                                                   const BSONObj& max,
                                                                   CleanWhenEnum whenToClean,
                                                                   bool pending) {
    return std::make_shared<RangeDeletionWithOngoingQueries>(
        createRangeDeletionTask(collectionUUID, min, max, whenToClean, pending));
}

/**
 *  RangeDeletionWithOngoingQueries implementation
 */
RangeDeletionWithOngoingQueries::RangeDeletionWithOngoingQueries(const RangeDeletionTask& t)
    : _task(t) {}

RangeDeletionTask RangeDeletionWithOngoingQueries::getTask() {
    return _task;
}

void RangeDeletionWithOngoingQueries::drainOngoingQueries() {
    _ongoingQueries.setFrom(Status::OK());
}

auto RangeDeletionWithOngoingQueries::getOngoingQueriesFuture() {
    return _ongoingQueries.getFuture().semi();
}

/**
** TESTS
*/

TEST_F(RangeDeleterServiceTest, RegisterAndProcessSingleTask) {
    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries = rangeDeletionTask0ForCollA;

    auto completionFuture = rds->registerTask(taskWithOngoingQueries->getTask(),
                                              taskWithOngoingQueries->getOngoingQueriesFuture());
    // The task can't be processed (hence completed) before ongoing queries drain
    ASSERT(!completionFuture.isReady());
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollA));

    // Pretend ongoing queries have drained and check task is processed
    taskWithOngoingQueries->drainOngoingQueries();
    completionFuture.get(opCtx);
    ASSERT_EQ(0, rds->getNumRangeDeletionTasksForCollection(uuidCollA));
}

TEST_F(RangeDeleterServiceTest, RegisterDuplicateTaskForSameRangeReturnsOriginalFuture) {
    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries = rangeDeletionTask0ForCollA;

    auto originalTaskCompletionFuture = rds->registerTask(
        taskWithOngoingQueries->getTask(), taskWithOngoingQueries->getOngoingQueriesFuture());

    // Trying registering a duplicate task must return a future without throwing errors
    auto duplicateTaskCompletionFuture =
        rds->registerTask(taskWithOngoingQueries->getTask(), SemiFuture<void>::makeReady());

    // Check that the state of the original task is not affected
    ASSERT(!originalTaskCompletionFuture.isReady());

    // Check that no new task has been registered
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollA));

    // Check that - upon range deletion completion - both original and "duplicate" futures are ready
    taskWithOngoingQueries->drainOngoingQueries();
    originalTaskCompletionFuture.get(opCtx);
    ASSERT(duplicateTaskCompletionFuture.isReady());
    ASSERT_EQ(0, rds->getNumRangeDeletionTasksForCollection(uuidCollA));
}

TEST_F(RangeDeleterServiceTest, RegisterAndProcessMoreTasksForSameCollection) {
    auto rds = RangeDeleterService::get(opCtx);
    auto task0WithOngoingQueries = rangeDeletionTask0ForCollA;
    auto task1WithOngoingQueries = rangeDeletionTask1ForCollA;

    // Register 2 tasks for the same collection
    auto completionFuture0 = rds->registerTask(task0WithOngoingQueries->getTask(),
                                               task0WithOngoingQueries->getOngoingQueriesFuture());
    auto completionFuture1 = rds->registerTask(task1WithOngoingQueries->getTask(),
                                               task1WithOngoingQueries->getOngoingQueriesFuture());

    // The tasks can't be processed (hence completed) before ongoing queries drain
    ASSERT(!completionFuture0.isReady());
    ASSERT(!completionFuture1.isReady());
    ASSERT_EQ(2, rds->getNumRangeDeletionTasksForCollection(uuidCollA));

    // Pretend ongoing queries have drained for task 1 and check it gets processed
    task1WithOngoingQueries->drainOngoingQueries();
    completionFuture1.get(opCtx);
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollA));

    // Pretend ongoing queries have drained for task 0 and check it gets processed
    task0WithOngoingQueries->drainOngoingQueries();
    completionFuture0.get(opCtx);
    ASSERT_EQ(0, rds->getNumRangeDeletionTasksForCollection(uuidCollA));
}

TEST_F(RangeDeleterServiceTest, RegisterAndProcessTasksForDifferentCollections) {
    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueriesCollA = rangeDeletionTask0ForCollA;
    auto taskWithOngoingQueriesCollB = rangeDeletionTask0ForCollB;

    // Register 1 tasks for `collA` and 1 task for `collB`
    auto completionFutureCollA =
        rds->registerTask(taskWithOngoingQueriesCollA->getTask(),
                          taskWithOngoingQueriesCollA->getOngoingQueriesFuture());
    auto completionFutureCollB =
        rds->registerTask(taskWithOngoingQueriesCollB->getTask(),
                          taskWithOngoingQueriesCollB->getOngoingQueriesFuture());

    // The tasks can't be processed (hence completed) before ongoing queries drain
    ASSERT(!completionFutureCollA.isReady());
    ASSERT(!completionFutureCollB.isReady());
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollA));
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollB));

    // Pretend ongoing queries have drained for task on coll B and check that:
    // - Task for `collB` gets processed
    // - Task for `collA` is not affected
    taskWithOngoingQueriesCollB->drainOngoingQueries();
    completionFutureCollB.get(opCtx);
    ASSERT_EQ(0, rds->getNumRangeDeletionTasksForCollection(uuidCollB));
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollA));

    // Pretend ongoing queries have drained for task on coll A and check it gets processed
    taskWithOngoingQueriesCollA->drainOngoingQueries();
    completionFutureCollA.get(opCtx);
    ASSERT_EQ(0, rds->getNumRangeDeletionTasksForCollection(uuidCollA));
}

TEST_F(RangeDeleterServiceTest, DelayForSecondaryQueriesIsHonored) {
    int defaultOrphanCleanupDelaySecs = orphanCleanupDelaySecs.load();
    ScopeGuard reset([=] { orphanCleanupDelaySecs.store(defaultOrphanCleanupDelaySecs); });

    // Set delay for waiting secondary queries to 2 seconds
    orphanCleanupDelaySecs.store(2);

    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed);

    auto completionFuture = rds->registerTask(taskWithOngoingQueries->getTask(),
                                              taskWithOngoingQueries->getOngoingQueriesFuture());

    // Check that the task lasts at least 2 seconds from the moment ongoing queries drain
    auto start = Date_t::now();
    taskWithOngoingQueries->drainOngoingQueries();
    completionFuture.get(opCtx);
    auto end = Date_t::now();

    auto elapsed = end.toDurationSinceEpoch() - start.toDurationSinceEpoch();
    ASSERT(elapsed >= Milliseconds(2000));
}

TEST_F(RangeDeleterServiceTest, ScheduledTaskInvalidatedOnStepDown) {
    auto rds = RangeDeleterService::get(opCtx);

    auto taskWithOngoingQueries = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed);
    // Mark ongoing queries as completed
    taskWithOngoingQueries->drainOngoingQueries();

    auto completionFuture = rds->registerTask(taskWithOngoingQueries->getTask(),
                                              taskWithOngoingQueries->getOngoingQueriesFuture());

    // Manually trigger disabling of the service
    rds->onStepDown();
    ON_BLOCK_EXIT([&] {
        rds->onStepUpComplete(opCtx, 0L);  // Re-enable the service for clean teardown
    });

    try {
        completionFuture.get(opCtx);
    } catch (const ExceptionForCat<ErrorCategory::CancellationError>&) {
        // Expect a cancellation error when the service gets disabled
    }
}

TEST_F(RangeDeleterServiceTest, NoActionPossibleIfServiceIsDown) {
    auto rds = RangeDeleterService::get(opCtx);

    // Manually trigger disabling of the service
    rds->onStepDown();
    ON_BLOCK_EXIT([&] {
        rds->onStepUpComplete(opCtx, 0L);  // Re-enable the service for clean teardown
    });

    auto taskWithOngoingQueries = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON("a" << 0), BSON("a" << 10), CleanWhenEnum::kDelayed);

    ASSERT_THROWS_CODE(rds->registerTask(taskWithOngoingQueries->getTask(),
                                         taskWithOngoingQueries->getOngoingQueriesFuture()),
                       DBException,
                       ErrorCodes::NotYetInitialized);

    ASSERT_THROWS_CODE(rds->deregisterTask(taskWithOngoingQueries->getTask().getCollectionUuid(),
                                           taskWithOngoingQueries->getTask().getRange()),
                       DBException,
                       ErrorCodes::NotYetInitialized);

    ASSERT_THROWS_CODE(rds->getNumRangeDeletionTasksForCollection(
                           taskWithOngoingQueries->getTask().getCollectionUuid()),
                       DBException,
                       ErrorCodes::NotYetInitialized);
}

TEST_F(RangeDeleterServiceTest, NoOverlappingRangeDeletionsFuture) {
    auto rds = RangeDeleterService::get(opCtx);

    // No range deletion task registered
    ChunkRange inputRange(BSON("a" << 0), BSON("a" << 10));
    auto fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(fut.isReady());

    // Register a range deletion task
    auto taskWithOngoingQueries =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA, BSON("a" << 0), BSON("a" << 10));
    auto completionFuture = rds->registerTask(taskWithOngoingQueries->getTask(),
                                              taskWithOngoingQueries->getOngoingQueriesFuture());

    // Totally unrelated range
    inputRange = ChunkRange(BSON("a" << -10), BSON("a" << -3));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(fut.isReady());

    // Range "touching" lower bound
    inputRange = ChunkRange(BSON("a" << -10), BSON("a" << 0));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(fut.isReady());

    // Range "touching" upper bound
    inputRange = ChunkRange(BSON("a" << 10), BSON("a" << 20));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(fut.isReady());
}

TEST_F(RangeDeleterServiceTest, OneOverlappingRangeDeletionFuture) {
    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA, BSON("a" << 0), BSON("a" << 10));

    auto completionFuture = rds->registerTask(taskWithOngoingQueries->getTask(),
                                              taskWithOngoingQueries->getOngoingQueriesFuture());

    std::vector<SharedSemiFuture<void>> waitForRangeToBeDeletedFutures;

    // Exact match
    ChunkRange inputRange(BSON("a" << 0), BSON("a" << 10));
    auto fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!fut.isReady());
    waitForRangeToBeDeletedFutures.push_back(fut);

    // Super-range
    inputRange = ChunkRange(BSON("a" << -10), BSON("a" << 20));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!fut.isReady());
    waitForRangeToBeDeletedFutures.push_back(fut);

    // Super range touching upper bound
    inputRange = ChunkRange(BSON("a" << -10), BSON("a" << 10));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!fut.isReady());
    waitForRangeToBeDeletedFutures.push_back(fut);

    // Super range touching lower bound
    inputRange = ChunkRange(BSON("a" << 0), BSON("a" << 20));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!fut.isReady());
    waitForRangeToBeDeletedFutures.push_back(fut);

    // Sub-range
    inputRange = ChunkRange(BSON("a" << 3), BSON("a" << 6));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!fut.isReady());
    waitForRangeToBeDeletedFutures.push_back(fut);

    // Sub-range touching upper bound
    inputRange = ChunkRange(BSON("a" << 3), BSON("a" << 10));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!fut.isReady());
    waitForRangeToBeDeletedFutures.push_back(fut);

    // Sub-range touching lower bound
    inputRange = ChunkRange(BSON("a" << 0), BSON("a" << 6));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!fut.isReady());
    waitForRangeToBeDeletedFutures.push_back(fut);

    // Drain ongoing queries to start the task and check futures get marked as ready
    taskWithOngoingQueries->drainOngoingQueries();
    for (const auto& future : waitForRangeToBeDeletedFutures) {
        future.get(opCtx);
    }
}

TEST_F(RangeDeleterServiceTest, MultipleOverlappingRangeDeletionsFuture) {
    auto rds = RangeDeleterService::get(opCtx);

    // Register range deletion tasks [0, 10) - [10, 20) - [20, 30)
    auto taskWithOngoingQueries0 =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA, BSON("a" << 0), BSON("a" << 10));
    auto completionFuture0 = rds->registerTask(taskWithOngoingQueries0->getTask(),
                                               taskWithOngoingQueries0->getOngoingQueriesFuture());
    auto taskWithOngoingQueries10 =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA, BSON("a" << 10), BSON("a" << 20));
    auto completionFuture10 = rds->registerTask(
        taskWithOngoingQueries10->getTask(), taskWithOngoingQueries10->getOngoingQueriesFuture());
    auto taskWithOngoingQueries30 =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA, BSON("a" << 30), BSON("a" << 40));
    auto completionFuture30 = rds->registerTask(
        taskWithOngoingQueries30->getTask(), taskWithOngoingQueries30->getOngoingQueriesFuture());

    // Exact match with [0, 10)
    ChunkRange inputRange(BSON("a" << 0), BSON("a" << 10));
    auto futureReadyWhenTask0Ready = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!futureReadyWhenTask0Ready.isReady());

    // Super-range spanning across [0, 10) and [10, 20)
    inputRange = ChunkRange(BSON("a" << -10), BSON("a" << 20));
    auto futureReadyWhenTasks0And10Ready =
        rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!futureReadyWhenTasks0And10Ready.isReady());

    // Super-range spanning across [0, 10), [10, 20) and [30, 40)
    inputRange = ChunkRange(BSON("a" << -10), BSON("a" << 50));
    auto futureReadyWhenTasks0And10And30Ready =
        rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!futureReadyWhenTasks0And10And30Ready.isReady());

    // Drain ongoing queries one task per time and check only expected futures get marked as ready
    taskWithOngoingQueries0->drainOngoingQueries();
    futureReadyWhenTask0Ready.get(opCtx);
    ASSERT(!futureReadyWhenTasks0And10Ready.isReady());
    ASSERT(!futureReadyWhenTasks0And10And30Ready.isReady());

    taskWithOngoingQueries10->drainOngoingQueries();
    futureReadyWhenTasks0And10Ready.get(opCtx);
    ASSERT(!futureReadyWhenTasks0And10And30Ready.isReady());

    taskWithOngoingQueries30->drainOngoingQueries();
    futureReadyWhenTasks0And10And30Ready.get(opCtx);
}

TEST_F(RangeDeleterServiceTest, GetOverlappingRangeDeletionsResilientToRefineShardKey) {
    auto rds = RangeDeleterService::get(opCtx);

    // Register range deletion tasks [0, 10) - [10, 20) - [20, 30)
    auto taskWithOngoingQueries0 =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA, BSON("a" << 0), BSON("a" << 10));
    auto completionFuture0 = rds->registerTask(taskWithOngoingQueries0->getTask(),
                                               taskWithOngoingQueries0->getOngoingQueriesFuture());
    auto taskWithOngoingQueries10 =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA, BSON("a" << 10), BSON("a" << 20));
    auto completionFuture10 = rds->registerTask(
        taskWithOngoingQueries10->getTask(), taskWithOngoingQueries10->getOngoingQueriesFuture());
    auto taskWithOngoingQueries30 =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA, BSON("a" << 30), BSON("a" << 40));
    auto completionFuture30 = rds->registerTask(
        taskWithOngoingQueries30->getTask(), taskWithOngoingQueries30->getOngoingQueriesFuture());

    // Exact match with [0, 10)
    ChunkRange inputRange(BSON("a" << 0 << "b"
                                   << "lol"),
                          BSON("a" << 9 << "b"
                                   << "lol"));
    auto futureReadyWhenTask0Ready = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!futureReadyWhenTask0Ready.isReady());

    // Super-range spanning across [0, 10) and [10, 20)
    inputRange = ChunkRange(BSON("a" << -10 << "b"
                                     << "lol"),
                            BSON("a" << 15 << "b"
                                     << "lol"));
    auto futureReadyWhenTasks0And10Ready =
        rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!futureReadyWhenTasks0And10Ready.isReady());

    // Super-range spanning across [0, 10), [10, 20) and [30, 40)
    inputRange = ChunkRange(BSON("a" << -10 << "b"
                                     << "lol"),
                            BSON("a" << 50 << "b"
                                     << "lol"));
    auto futureReadyWhenTasks0And10And30Ready =
        rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!futureReadyWhenTasks0And10And30Ready.isReady());

    // Drain ongoing queries one task per time and check only expected futures get marked as ready
    taskWithOngoingQueries0->drainOngoingQueries();
    futureReadyWhenTask0Ready.get(opCtx);
    ASSERT(!futureReadyWhenTasks0And10Ready.isReady());
    ASSERT(!futureReadyWhenTasks0And10And30Ready.isReady());

    taskWithOngoingQueries10->drainOngoingQueries();
    futureReadyWhenTasks0And10Ready.get(opCtx);
    ASSERT(!futureReadyWhenTasks0And10And30Ready.isReady());

    taskWithOngoingQueries30->drainOngoingQueries();
    futureReadyWhenTasks0And10And30Ready.get(opCtx);
}

TEST_F(RangeDeleterServiceTest, DumpState) {
    auto rds = RangeDeleterService::get(opCtx);
    auto task0WithOngoingQueriesCollA = rangeDeletionTask0ForCollA;
    auto task1WithOngoingQueriesCollA = rangeDeletionTask1ForCollA;
    auto taskWithOngoingQueriesCollB = rangeDeletionTask0ForCollB;

    // Register 2 tasks for `collA` and 1 task for `collB`
    auto completionFuture0CollA =
        rds->registerTask(task0WithOngoingQueriesCollA->getTask(),
                          task0WithOngoingQueriesCollA->getOngoingQueriesFuture());
    auto completionFuture1CollA =
        rds->registerTask(task1WithOngoingQueriesCollA->getTask(),
                          task1WithOngoingQueriesCollA->getOngoingQueriesFuture());
    auto completionFutureCollB =
        rds->registerTask(taskWithOngoingQueriesCollB->getTask(),
                          taskWithOngoingQueriesCollB->getOngoingQueriesFuture());

    // The tasks can't be processed (hence completed) before ongoing queries drain
    ASSERT(!completionFuture0CollA.isReady());
    ASSERT(!completionFuture1CollA.isReady());
    ASSERT(!completionFutureCollB.isReady());
    ASSERT_EQ(2, rds->getNumRangeDeletionTasksForCollection(uuidCollA));
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollB));

    // Build expected state and compare it with the returned one from
    // RangeDeleterService::dumpState()
    BSONArrayBuilder builderArrCollA;
    builderArrCollA.append(task0WithOngoingQueriesCollA->getTask().getRange().toBSON());
    builderArrCollA.append(task1WithOngoingQueriesCollA->getTask().getRange().toBSON());

    BSONArrayBuilder builderArrCollB;
    builderArrCollB.append(taskWithOngoingQueriesCollB->getTask().getRange().toBSON());

    BSONObj state = rds->dumpState();
    BSONObj expectedState =
        BSON(uuidCollA.toString() << builderArrCollA.arr() << uuidCollB.toString()
                                  << builderArrCollB.arr());

    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(kComparator.compare(state, expectedState), 0)
        << "Expected " << state << " == " << expectedState;
}

TEST_F(RangeDeleterServiceTest, TotalNumOfRegisteredTasks) {
    auto rds = RangeDeleterService::get(opCtx);
    auto task0WithOngoingQueriesCollA = rangeDeletionTask0ForCollA;
    auto task1WithOngoingQueriesCollA = rangeDeletionTask1ForCollA;
    auto taskWithOngoingQueriesCollB = rangeDeletionTask0ForCollB;

    // Register 2 tasks for `collA` and 1 task for `collB`
    auto completionFuture0CollA =
        rds->registerTask(rangeDeletionTask0ForCollA->getTask(),
                          rangeDeletionTask0ForCollA->getOngoingQueriesFuture());
    auto completionFuture1CollA =
        rds->registerTask(rangeDeletionTask1ForCollA->getTask(),
                          rangeDeletionTask1ForCollA->getOngoingQueriesFuture());
    auto completionFutureCollB =
        rds->registerTask(rangeDeletionTask0ForCollB->getTask(),
                          rangeDeletionTask0ForCollB->getOngoingQueriesFuture());

    ASSERT_EQ(3, rds->totalNumOfRegisteredTasks());
}

TEST_F(RangeDeleterServiceTest, RegisterTaskWithDisableResumableRangeDeleterFlagEnabled) {

    RAIIServerParameterControllerForTest enableFeatureFlag{"disableResumableRangeDeleter", true};

    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries = rangeDeletionTask0ForCollA;
    auto completionFuture = rds->registerTask(taskWithOngoingQueries->getTask(),
                                              taskWithOngoingQueries->getOngoingQueriesFuture());
    ASSERT(completionFuture.isReady());
    ASSERT_EQ(0, rds->getNumRangeDeletionTasksForCollection(uuidCollA));
}

TEST_F(RangeDeleterServiceTest,
       GetOverlappingRangeDeletionsFutureWithDisableResumableRangeDeleterFlagEnabled) {

    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries = rangeDeletionTask0ForCollA;
    auto completionFuture = rds->registerTask(taskWithOngoingQueries->getTask(),
                                              taskWithOngoingQueries->getOngoingQueriesFuture());
    ASSERT(!completionFuture.isReady());
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollA));

    auto overlappingRangeFuture = rds->getOverlappingRangeDeletionsFuture(
        uuidCollA, taskWithOngoingQueries->getTask().getRange());
    ASSERT(!overlappingRangeFuture.isReady());

    RAIIServerParameterControllerForTest enableFeatureFlag{"disableResumableRangeDeleter", true};
    auto overlappingRangeFutureWhenDisabled = rds->getOverlappingRangeDeletionsFuture(
        uuidCollA, taskWithOngoingQueries->getTask().getRange());
    ASSERT(overlappingRangeFutureWhenDisabled.isReady());
}

}  // namespace mongo
