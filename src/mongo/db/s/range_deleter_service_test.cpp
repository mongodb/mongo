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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/range_deleter_service_test.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/random.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#include <ostream>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {

/**
 *  RangeDeleterServiceTest implementation
 */
void RangeDeleterServiceTest::setUp() {
    ShardServerTestFixture::setUp();
    WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());
    opCtx = operationContext();
    RangeDeleterService::get(opCtx)->onStartup(opCtx);
    RangeDeleterService::get(opCtx)->onStepUpComplete(opCtx, 0L);
    RangeDeleterService::get(opCtx)->getRangeDeleterServiceInitializationFuture().get(opCtx);

    createTestCollection(opCtx, nsCollA);
    createTestCollection(opCtx, nsCollB);

    {
        AutoGetCollection autoColl(opCtx, nsCollA, MODE_X);
        uuidCollA = autoColl->uuid();
        nssWithUuid[uuidCollA] = nsCollA;
        _setFilteringMetadataByUUID(opCtx, uuidCollA);
    }
    {
        AutoGetCollection autoColl(opCtx, nsCollB, MODE_X);
        uuidCollB = autoColl->uuid();
        nssWithUuid[uuidCollB] = nsCollB;
        _setFilteringMetadataByUUID(opCtx, uuidCollB);
    }

    rangeDeletionTask0ForCollA = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 0), BSON(kShardKey << 10));
    rangeDeletionTask1ForCollA = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 10), BSON(kShardKey << 20));
    rangeDeletionTask0ForCollB = createRangeDeletionTaskWithOngoingQueries(
        uuidCollB, BSON(kShardKey << 0), BSON(kShardKey << 10));
}

void RangeDeleterServiceTest::tearDown() {
    RangeDeleterService::get(opCtx)->onStepDown();
    RangeDeleterService::get(opCtx)->onShutdown();
    WaitForMajorityService::get(opCtx->getServiceContext()).shutDown();
    ShardServerTestFixture::tearDown();
}

void RangeDeleterServiceTest::_setFilteringMetadataByUUID(OperationContext* opCtx,
                                                          const UUID& uuid) {
    const OID epoch = OID::gen();
    NamespaceString nss = nssWithUuid[uuid];

    const CollectionMetadata metadata = [&]() {
        auto chunk = ChunkType(uuid,
                               ChunkRange{BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)},
                               ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}),
                               ShardId("this"));
        ChunkManager cm(makeStandaloneRoutingTableHistory(
                            RoutingTableHistory::makeNew(nss,
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
                                                         {std::move(chunk)})),
                        boost::none);

        return CollectionMetadata(std::move(cm), ShardId("this"));
    }();

    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss)
        ->setFilteringMetadata(opCtx, metadata);
}

/**
** TESTS
*/

TEST_F(RangeDeleterServiceTest, RegisterAndProcessSingleTask) {
    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries = rangeDeletionTask0ForCollA;

    auto completionFuture =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries->getTask(),
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

    auto originalTaskCompletionFuture =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries->getTask(),
                                        taskWithOngoingQueries->getOngoingQueriesFuture());

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
    auto completionFuture0 =
        registerAndCreatePersistentTask(opCtx,
                                        task0WithOngoingQueries->getTask(),
                                        task0WithOngoingQueries->getOngoingQueriesFuture());
    auto completionFuture1 =
        registerAndCreatePersistentTask(opCtx,
                                        task1WithOngoingQueries->getTask(),
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
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueriesCollA->getTask(),
                                        taskWithOngoingQueriesCollA->getOngoingQueriesFuture());
    auto completionFutureCollB =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueriesCollB->getTask(),
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

    auto taskWithOngoingQueries = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 0), BSON(kShardKey << 10), CleanWhenEnum::kDelayed);

    auto completionFuture =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries->getTask(),
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
        uuidCollA, BSON(kShardKey << 0), BSON(kShardKey << 10), CleanWhenEnum::kDelayed);
    // Mark ongoing queries as completed
    taskWithOngoingQueries->drainOngoingQueries();

    auto completionFuture =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries->getTask(),
                                        taskWithOngoingQueries->getOngoingQueriesFuture());

    // Manually trigger disabling of the service
    rds->onStepDown();
    try {
        completionFuture.get(opCtx);
    } catch (const ExceptionFor<ErrorCategory::Interruption>&) {
        // Expect an interruption error when the service gets disabled
    }
}

TEST_F(RangeDeleterServiceTest, NoActionPossibleIfServiceIsDown) {
    auto rds = RangeDeleterService::get(opCtx);

    // Manually trigger disabling of the service
    rds->onStepDown();
    auto taskWithOngoingQueries = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 0), BSON(kShardKey << 10), CleanWhenEnum::kDelayed);

    ASSERT_THROWS_CODE(
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries->getTask(),
                                        taskWithOngoingQueries->getOngoingQueriesFuture()),
        DBException,
        ErrorCodes::NotYetInitialized);

    ASSERT_THROWS_CODE(rds->completeTask(taskWithOngoingQueries->getTask().getCollectionUuid(),
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
    ChunkRange inputRange(BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(fut.isReady());

    // Register a range deletion task
    auto taskWithOngoingQueries = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto completionFuture =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries->getTask(),
                                        taskWithOngoingQueries->getOngoingQueriesFuture());

    // Totally unrelated range
    inputRange = ChunkRange(BSON(kShardKey << -10), BSON(kShardKey << -3));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(fut.isReady());

    // Range "touching" lower bound
    inputRange = ChunkRange(BSON(kShardKey << -10), BSON(kShardKey << 0));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(fut.isReady());

    // Range "touching" upper bound
    inputRange = ChunkRange(BSON(kShardKey << 10), BSON(kShardKey << 20));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(fut.isReady());
}

TEST_F(RangeDeleterServiceTest, OneOverlappingRangeDeletionFuture) {
    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 0), BSON(kShardKey << 10));

    auto completionFuture =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries->getTask(),
                                        taskWithOngoingQueries->getOngoingQueriesFuture());

    std::vector<SharedSemiFuture<void>> waitForRangeToBeDeletedFutures;

    // Exact match
    ChunkRange inputRange(BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!fut.isReady());
    waitForRangeToBeDeletedFutures.push_back(fut);

    // Super-range
    inputRange = ChunkRange(BSON(kShardKey << -10), BSON(kShardKey << 20));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!fut.isReady());
    waitForRangeToBeDeletedFutures.push_back(fut);

    // Super range touching upper bound
    inputRange = ChunkRange(BSON(kShardKey << -10), BSON(kShardKey << 10));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!fut.isReady());
    waitForRangeToBeDeletedFutures.push_back(fut);

    // Super range touching lower bound
    inputRange = ChunkRange(BSON(kShardKey << 0), BSON(kShardKey << 20));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!fut.isReady());
    waitForRangeToBeDeletedFutures.push_back(fut);

    // Sub-range
    inputRange = ChunkRange(BSON(kShardKey << 3), BSON(kShardKey << 6));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!fut.isReady());
    waitForRangeToBeDeletedFutures.push_back(fut);

    // Sub-range touching upper bound
    inputRange = ChunkRange(BSON(kShardKey << 3), BSON(kShardKey << 10));
    fut = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!fut.isReady());
    waitForRangeToBeDeletedFutures.push_back(fut);

    // Sub-range touching lower bound
    inputRange = ChunkRange(BSON(kShardKey << 0), BSON(kShardKey << 6));
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
    auto taskWithOngoingQueries0 = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto completionFuture0 =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries0->getTask(),
                                        taskWithOngoingQueries0->getOngoingQueriesFuture());
    auto taskWithOngoingQueries10 = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 10), BSON(kShardKey << 20));
    auto completionFuture10 =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries10->getTask(),
                                        taskWithOngoingQueries10->getOngoingQueriesFuture());
    auto taskWithOngoingQueries30 = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 30), BSON(kShardKey << 40));
    auto completionFuture30 =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries30->getTask(),
                                        taskWithOngoingQueries30->getOngoingQueriesFuture());

    // Exact match with [0, 10)
    ChunkRange inputRange(BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto futureReadyWhenTask0Ready = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!futureReadyWhenTask0Ready.isReady());

    // Super-range spanning across [0, 10) and [10, 20)
    inputRange = ChunkRange(BSON(kShardKey << -10), BSON(kShardKey << 20));
    auto futureReadyWhenTasks0And10Ready =
        rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!futureReadyWhenTasks0And10Ready.isReady());

    // Super-range spanning across [0, 10), [10, 20) and [30, 40)
    inputRange = ChunkRange(BSON(kShardKey << -10), BSON(kShardKey << 50));
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
    auto taskWithOngoingQueries0 = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto completionFuture0 =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries0->getTask(),
                                        taskWithOngoingQueries0->getOngoingQueriesFuture());
    auto taskWithOngoingQueries10 = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 10), BSON(kShardKey << 20));
    auto completionFuture10 =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries10->getTask(),
                                        taskWithOngoingQueries10->getOngoingQueriesFuture());
    auto taskWithOngoingQueries30 = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 30), BSON(kShardKey << 40));
    auto completionFuture30 =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries30->getTask(),
                                        taskWithOngoingQueries30->getOngoingQueriesFuture());

    // Exact match with [0, 10)
    ChunkRange inputRange(BSON(kShardKey << 0 << "b"
                                         << "lol"),
                          BSON(kShardKey << 9 << "b"
                                         << "lol"));
    auto futureReadyWhenTask0Ready = rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!futureReadyWhenTask0Ready.isReady());

    // Super-range spanning across [0, 10) and [10, 20)
    inputRange = ChunkRange(BSON(kShardKey << -10 << "b"
                                           << "lol"),
                            BSON(kShardKey << 15 << "b"
                                           << "lol"));
    auto futureReadyWhenTasks0And10Ready =
        rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!futureReadyWhenTasks0And10Ready.isReady());

    // Super-range spanning across [0, 10), [10, 20) and [30, 40)
    inputRange = ChunkRange(BSON(kShardKey << -10 << "b"
                                           << "lol"),
                            BSON(kShardKey << 50 << "b"
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
        registerAndCreatePersistentTask(opCtx,
                                        task0WithOngoingQueriesCollA->getTask(),
                                        task0WithOngoingQueriesCollA->getOngoingQueriesFuture());
    auto completionFuture1CollA =
        registerAndCreatePersistentTask(opCtx,
                                        task1WithOngoingQueriesCollA->getTask(),
                                        task1WithOngoingQueriesCollA->getOngoingQueriesFuture());
    auto completionFutureCollB =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueriesCollB->getTask(),
                                        taskWithOngoingQueriesCollB->getOngoingQueriesFuture());

    // The tasks can't be processed (hence completed) before ongoing queries drain
    ASSERT(!completionFuture0CollA.isReady());
    ASSERT(!completionFuture1CollA.isReady());
    ASSERT(!completionFutureCollB.isReady());
    ASSERT_EQ(2, rds->getNumRangeDeletionTasksForCollection(uuidCollA));
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollB));

    auto state = dumpStateAsMap(opCtx);
    const auto& task0A = task0WithOngoingQueriesCollA->getTask();
    const auto& task1A = task1WithOngoingQueriesCollA->getTask();
    const auto& taskB = taskWithOngoingQueriesCollB->getTask();
    const auto& collAId = task0A.getCollectionUuid();
    const auto& collBId = taskB.getCollectionUuid();
    ASSERT_EQ(state[collAId].size(), 2);
    ASSERT_TRUE(state[collAId].contains(task0A.getRange()));
    ASSERT_TRUE(state[collAId].contains(task1A.getRange()));
    ASSERT_EQ(state[collBId].size(), 1);
    ASSERT_TRUE(state[collBId].contains(taskB.getRange()));
}

TEST_F(RangeDeleterServiceTest, TotalNumOfRegisteredTasks) {
    auto rds = RangeDeleterService::get(opCtx);
    auto task0WithOngoingQueriesCollA = rangeDeletionTask0ForCollA;
    auto task1WithOngoingQueriesCollA = rangeDeletionTask1ForCollA;
    auto taskWithOngoingQueriesCollB = rangeDeletionTask0ForCollB;

    // Register 2 tasks for `collA` and 1 task for `collB`
    auto completionFuture0CollA =
        registerAndCreatePersistentTask(opCtx,
                                        rangeDeletionTask0ForCollA->getTask(),
                                        rangeDeletionTask0ForCollA->getOngoingQueriesFuture());
    auto completionFuture1CollA =
        registerAndCreatePersistentTask(opCtx,
                                        rangeDeletionTask1ForCollA->getTask(),
                                        rangeDeletionTask1ForCollA->getOngoingQueriesFuture());
    auto completionFutureCollB =
        registerAndCreatePersistentTask(opCtx,
                                        rangeDeletionTask0ForCollB->getTask(),
                                        rangeDeletionTask0ForCollB->getOngoingQueriesFuture());

    ASSERT_EQ(3, rds->totalNumOfRegisteredTasks());
}

TEST_F(RangeDeleterServiceTest, RegisterTaskWithDisableResumableRangeDeleterFlagEnabled) {
    RAIIServerParameterControllerForTest disableRangeDeleter{"disableResumableRangeDeleter", true};

    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries = rangeDeletionTask0ForCollA;
    auto completionFuture =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries->getTask(),
                                        taskWithOngoingQueries->getOngoingQueriesFuture());
    ASSERT(!completionFuture.isReady());
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollA));

    auto overlappingRangeFuture = rds->getOverlappingRangeDeletionsFuture(
        uuidCollA, taskWithOngoingQueries->getTask().getRange());
    ASSERT(overlappingRangeFuture.isReady());
    ASSERT_THROWS_CODE(
        overlappingRangeFuture.get(opCtx), DBException, ErrorCodes::ResumableRangeDeleterDisabled);
}

TEST_F(RangeDeleterServiceTest,
       GetOverlappingRangeDeletionsFutureWithDisableResumableRangeDeleterFlagEnabled) {
    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries = rangeDeletionTask0ForCollA;
    auto completionFuture =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries->getTask(),
                                        taskWithOngoingQueries->getOngoingQueriesFuture());
    ASSERT(!completionFuture.isReady());
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollA));

    auto overlappingRangeFuture = rds->getOverlappingRangeDeletionsFuture(
        uuidCollA, taskWithOngoingQueries->getTask().getRange());
    ASSERT(!overlappingRangeFuture.isReady());

    RAIIServerParameterControllerForTest disableRangeDeleter{"disableResumableRangeDeleter", true};
    auto overlappingRangeFutureWhenDisabled = rds->getOverlappingRangeDeletionsFuture(
        uuidCollA, taskWithOngoingQueries->getTask().getRange());
    ASSERT(overlappingRangeFutureWhenDisabled.isReady());
}

TEST_F(RangeDeleterServiceTest, RescheduleRangeDeletionTasksOnStepUp) {
    PseudoRandom random(SecureRandom().nextInt64());
    auto rds = RangeDeleterService::get(opCtx);

    // Trigger step-down
    rds->onStepDown();

    // Random number of range deletion documents to generate (minimum 1, maximum 20).
    int nRangeDeletionTasks = random.nextInt32(20) + 1;

    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);

    // Generate and persist range deleter tasks (some pending, some non-pending, some non-pending &&
    // processing)
    int nNonPending = 0, nNonPendingAndProcessing = 0;
    int minBound = 0;
    for (int i = 0; i < nRangeDeletionTasks; i++) {
        auto rangeDeletionTask = createRangeDeletionTask(uuidCollA,
                                                         BSON(kShardKey << minBound),
                                                         BSON(kShardKey << minBound + 10),
                                                         CleanWhenEnum::kDelayed);
        minBound += 10;

        auto rand = random.nextInt32() % 3;
        if (rand == 0) {
            // Pending range deletion task
            rangeDeletionTask.setPending(true);
        } else if (rand == 1) {
            // Non-pending range deletion task
            rangeDeletionTask.setPending(false);
            nNonPending++;
        } else if (rand == 2) {
            // Non-pending and processing range deletion task
            rangeDeletionTask.setPending(false);
            rangeDeletionTask.setProcessing(true);
            nNonPendingAndProcessing++;
        }

        store.add(opCtx, rangeDeletionTask);
    }

    // Trigger step-up
    rds->onStepUpComplete(opCtx, 0L);
    rds->getRangeDeleterServiceInitializationFuture().get(opCtx);

    // Check that all non-pending tasks are being rescheduled
    ASSERT_EQ(nNonPending + nNonPendingAndProcessing,
              rds->getNumRangeDeletionTasksForCollection(uuidCollA));
}

TEST_F(RangeDeleterServiceTest, PerformActualRangeDeletion) {
    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries = rangeDeletionTask0ForCollA;
    auto nss = nssWithUuid[uuidCollA];
    DBDirectClient dbclient(opCtx);

    insertDocsWithinRange(opCtx, nss, 0, 10, 10);
    ASSERT_EQUALS(dbclient.count(nss, BSONObj()), 10);

    auto completionFuture =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries->getTask(),
                                        taskWithOngoingQueries->getOngoingQueriesFuture());

    // The task can't be processed (hence completed) before ongoing queries drain
    ASSERT(!completionFuture.isReady());
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollA));
    ASSERT_EQUALS(dbclient.count(NamespaceString::kRangeDeletionNamespace), 1);
    verifyRangeDeletionTasks(opCtx, uuidCollA, {taskWithOngoingQueries->getTask().getRange()});

    // Pretend ongoing queries have drained and make sure the task completes
    taskWithOngoingQueries->drainOngoingQueries();
    completionFuture.get(opCtx);
    ASSERT_EQUALS(dbclient.count(nss), 0);
}

TEST_F(RangeDeleterServiceTest, OnlyRemoveDocumentsInRangeToDelete) {
    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries = rangeDeletionTask0ForCollA;
    auto nss = nssWithUuid[uuidCollA];
    DBDirectClient dbclient(opCtx);

    // Insert docs within the range targeted by the deletion task
    int numDocsToDelete = insertDocsWithinRange(opCtx, nss, 0, 10, 10);
    ASSERT_EQUALS(dbclient.count(nss), numDocsToDelete);

    // Insert docs in a different range
    int numDocsToKeep = insertDocsWithinRange(opCtx, nss, 20, 25, 5);
    numDocsToKeep += insertDocsWithinRange(opCtx, nss, 100, 105, 5);
    ASSERT_EQUALS(dbclient.count(nss), numDocsToKeep + numDocsToDelete);

    auto completionFuture =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries->getTask(),
                                        taskWithOngoingQueries->getOngoingQueriesFuture());

    // The task can't be processed (hence completed) before ongoing queries drain
    ASSERT(!completionFuture.isReady());
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollA));
    ASSERT_EQUALS(dbclient.count(NamespaceString::kRangeDeletionNamespace), 1);
    verifyRangeDeletionTasks(opCtx, uuidCollA, {taskWithOngoingQueries->getTask().getRange()});

    // Pretend ongoing queries have drained and make sure only orphans were cleared up
    taskWithOngoingQueries->drainOngoingQueries();
    completionFuture.get(opCtx);
    ASSERT_EQUALS(dbclient.count(nss), numDocsToKeep);
}

TEST_F(RangeDeleterServiceTest, RegisterAndProcessSingleTaskWithKeyPattern) {
    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA,
                                                  BSON(kShardKey << 0),
                                                  BSON(kShardKey << 10),
                                                  CleanWhenEnum::kNow,
                                                  false,
                                                  KeyPattern(kShardKeyPattern));

    auto completionFuture =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries->getTask(),
                                        taskWithOngoingQueries->getOngoingQueriesFuture());

    // The task can't be processed (hence completed) before ongoing queries drain
    ASSERT(!completionFuture.isReady());
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollA));

    // Make sure deletion can proceed even without filtering metadata
    _clearFilteringMetadataByUUID(opCtx, uuidCollA);

    // Pretend ongoing queries have drained and check task is processed
    taskWithOngoingQueries->drainOngoingQueries();
    completionFuture.get(opCtx);
    ASSERT_EQ(0, rds->getNumRangeDeletionTasksForCollection(uuidCollA));
}  // namespace mongo

TEST_F(RangeDeleterServiceTest, PerformActualRangeDeletionWithKeyPattern) {
    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA,
                                                  BSON(kShardKey << 0),
                                                  BSON(kShardKey << 10),
                                                  CleanWhenEnum::kNow,
                                                  false,
                                                  KeyPattern(kShardKeyPattern));
    auto nss = nssWithUuid[uuidCollA];
    DBDirectClient dbclient(opCtx);

    insertDocsWithinRange(opCtx, nss, 0, 10, 10);
    ASSERT_EQUALS(dbclient.count(nss, BSONObj()), 10);

    auto completionFuture =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries->getTask(),
                                        taskWithOngoingQueries->getOngoingQueriesFuture());

    // The task can't be processed (hence completed) before ongoing queries drain
    ASSERT(!completionFuture.isReady());
    ASSERT_EQ(1, rds->getNumRangeDeletionTasksForCollection(uuidCollA));
    ASSERT_EQUALS(dbclient.count(NamespaceString::kRangeDeletionNamespace), 1);
    verifyRangeDeletionTasks(opCtx, uuidCollA, {taskWithOngoingQueries->getTask().getRange()});

    // Make sure deletion can proceed even without filtering metadata
    _clearFilteringMetadataByUUID(opCtx, uuidCollA);

    // Pretend ongoing queries have drained and check task is processed
    taskWithOngoingQueries->drainOngoingQueries();
    completionFuture.get(opCtx);
    ASSERT_EQ(0, rds->getNumRangeDeletionTasksForCollection(uuidCollA));
    ASSERT_EQUALS(dbclient.count(nss), 0);
}

TEST_F(RangeDeleterServiceTest, GetOverlappingRangeDeletionsWithNonContiguousTasks) {
    auto rds = RangeDeleterService::get(opCtx);

    // Register range deletion tasks [0, 10) - [10, 20) - [30, 40).
    auto taskWithOngoingQueries0 = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 0), BSON(kShardKey << 10));
    auto completionFuture0 =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries0->getTask(),
                                        taskWithOngoingQueries0->getOngoingQueriesFuture());
    auto taskWithOngoingQueries10 = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 10), BSON(kShardKey << 20));
    auto completionFuture10 =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries10->getTask(),
                                        taskWithOngoingQueries10->getOngoingQueriesFuture());
    auto taskWithOngoingQueries30 = createRangeDeletionTaskWithOngoingQueries(
        uuidCollA, BSON(kShardKey << 30), BSON(kShardKey << 40));
    auto completionFuture30 =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries30->getTask(),
                                        taskWithOngoingQueries30->getOngoingQueriesFuture());

    // Range overlapping with [10, 20) and [30, 40).
    auto inputRange = ChunkRange(BSON(kShardKey << 25), BSON(kShardKey << 35));
    auto futureReadyWhenTask30Ready =
        rds->getOverlappingRangeDeletionsFuture(uuidCollA, inputRange);
    ASSERT(!futureReadyWhenTask30Ready.isReady());

    // Drain ongoing queries one task per time and check only expected futures get marked as ready.
    taskWithOngoingQueries0->drainOngoingQueries();
    ASSERT(!futureReadyWhenTask30Ready.isReady());

    taskWithOngoingQueries10->drainOngoingQueries();
    ASSERT(!futureReadyWhenTask30Ready.isReady());

    taskWithOngoingQueries30->drainOngoingQueries();
    ASSERT_OK(futureReadyWhenTask30Ready.getNoThrow(opCtx));
}

TEST_F(RangeDeleterServiceTest, RegisterPendingTaskAndMarkItNonPending) {
    // Set delay for waiting secondary queries to 0
    int defaultOrphanCleanupDelaySecs = orphanCleanupDelaySecs.load();
    ScopeGuard reset([=] { orphanCleanupDelaySecs.store(defaultOrphanCleanupDelaySecs); });
    orphanCleanupDelaySecs.store(0);

    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries = rangeDeletionTask0ForCollA;

    // Drain queries since the beginning
    taskWithOngoingQueries->drainOngoingQueries();

    // Register task as pending (will not be processed until someone registers it again as !pending)
    auto completionFuture = rds->registerTask(taskWithOngoingQueries->getTask(),
                                              taskWithOngoingQueries->getOngoingQueriesFuture(),
                                              false /* from step up*/,
                                              true /* pending */);

    ASSERT(!completionFuture.isReady());

    // Re-registering the task as non-pending must unblock the range deletion
    registerAndCreatePersistentTask(
        opCtx, taskWithOngoingQueries->getTask(), taskWithOngoingQueries->getOngoingQueriesFuture())
        .get(opCtx);

    completionFuture.get();
    ASSERT(completionFuture.isReady());
}

TEST_F(RangeDeleterServiceTest, WaitForOngoingQueriesInvalidatedOnStepDown) {
    auto rds = RangeDeleterService::get(opCtx);

    auto taskWithOngoingQueries =
        createRangeDeletionTaskWithOngoingQueries(uuidCollA, BSON("a" << 0), BSON("a" << 10));

    auto completionFuture = rds->registerTask(taskWithOngoingQueries->getTask(),
                                              taskWithOngoingQueries->getOngoingQueriesFuture());

    // Manually trigger disabling of the service
    rds->onStepDown();
    try {
        completionFuture.get(opCtx);
    } catch (const ExceptionFor<ErrorCategory::Interruption>&) {
        // Future must have been set to an interruption error because the service was disabled
    }
}

TEST_F(RangeDeleterServiceTest, ProcessingFlagIsSetWhenRangeDeletionExecutionStarts) {
    auto rds = RangeDeleterService::get(opCtx);
    auto taskWithOngoingQueries = rangeDeletionTask0ForCollA;

    auto completionFuture =
        registerAndCreatePersistentTask(opCtx,
                                        taskWithOngoingQueries->getTask(),
                                        taskWithOngoingQueries->getOngoingQueriesFuture());

    // Check the `ongoing` flag is still not present since the range deletion hasn't started yet.
    verifyProcessingFlag(opCtx,
                         uuidCollA,
                         rangeDeletionTask0ForCollA->getTask().getRange(),
                         /*processingExpected=*/false);

    {
        // Add a failpoint to pause the range deletion execution after setting the `ongoing` flag.
        FailPointEnableBlock hangBeforeDoingDeletionFp("hangBeforeDoingDeletion");

        // Mark ongoing queries as drained and check the `ongoing` flag is present
        taskWithOngoingQueries->drainOngoingQueries();

        hangBeforeDoingDeletionFp->waitForTimesEntered(
            hangBeforeDoingDeletionFp.initialTimesEntered() + 1);
        verifyProcessingFlag(opCtx,
                             uuidCollA,
                             rangeDeletionTask0ForCollA->getTask().getRange(),
                             /*processingExpected=*/true);
    }

    // Complete the range deletion execution
    completionFuture.get(opCtx);
    ASSERT_EQ(0, rds->getNumRangeDeletionTasksForCollection(uuidCollA));
}

}  // namespace mongo
