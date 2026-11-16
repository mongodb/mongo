// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/sbe_unittest_assert.h"
#include "mongo/db/exec/sbe/stages/exchange_gather.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/fail_point.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace mongo::sbe {
/**
 * This file contains tests for sbe::ExchangeGather.
 */
class ExchangeGatherTest : public PlanStageTestFixture {
public:
    std::unique_ptr<PlanStage> makeExchangeGather(std::unique_ptr<PlanStage> input,
                                                  value::SlotVector inputVals,
                                                  size_t dop) {
        return std::make_unique<ExchangeGather>(
            std::move(input), std::move(inputVals), dop, getYieldPolicy(), kEmptyPlanNodeId);
    }

    void setupAndCancelInfiniteQuery(const BSONArray& inputData) {
        auto [scanSlot, scanStage] = generateVirtualScan(inputData);
        auto stage = std::make_unique<ExchangeGather>(std::move(scanStage),
                                                      value::SlotVector{scanSlot},
                                                      4 /* dop */,
                                                      nullptr /* yieldPolicy */,
                                                      kEmptyPlanNodeId);
        // Make every virtual scan block until its operation is interrupted.
        FailPointEnableBlock hangVirtualScan("hangVirtualScan");

        auto* serviceContext = getServiceContext();
        // The query thread owns the OperationContext and resets its holder when done; the main
        // thread only uses this raw pointer, which is published before 'started' is set and stays
        // valid until the operation is killed.
        OperationContext* queryOpCtxPtr = nullptr;
        Status queryStatus = Status::OK();
        Notification<void> started;

        stdx::thread queryThread([&] {
            Client::initThread("ExchangeGatherQuery", serviceContext->getService());
            auto queryOpCtx = cc().makeOperationContext();
            queryOpCtxPtr = queryOpCtx.get();
            auto ctx = makeCompileCtx();
            stage->attachToOperationContext(queryOpCtx.get());
            try {
                stage->prepare(*ctx);
                stage->open(false);
                started.set();
                stage->getNext();
            } catch (const DBException& ex) {
                queryStatus = ex.toStatus();
            }
            stage->close();
            stage->detachFromOperationContext();
        });

        started.get(operationContext());
        sleepsecs(1);
        {
            auto* client = queryOpCtxPtr->getClient();
            ClientLock lk(client);
            serviceContext->killOperation(lk, queryOpCtxPtr, ErrorCodes::Interrupted);
        }

        // The query must notice the interrupt within a reasonable time.
        Timer timer;
        queryThread.join();
        ASSERT_LT(timer.elapsed(), Seconds(10));
        ASSERT_EQ(queryStatus, ErrorCodes::Interrupted);
    }

    // Runs a parallel query whose originating operation holds the global lock in 'queryMode', while
    // another operation queues a global lock request in 'conflictingMode'. Verifies that the
    // conflicting request can be granted, i.e. that the query doesn't deadlock against it.
    // 'phase' is the point of the query execution at which the conflicting request gets queued.
    enum class QueryPhase { kOpen, kGetNext, kClose, kIdleQuery };
    void runConflictingLockTest(LockMode queryMode, LockMode conflictingMode, QueryPhase phase);

    void setupAndTimeoutInfiniteQuery(const BSONArray& inputData) {
        auto [scanSlot, scanStage] = generateVirtualScan(inputData);
        auto stage = std::make_unique<ExchangeGather>(std::move(scanStage),
                                                      value::SlotVector{scanSlot},
                                                      4 /* dop */,
                                                      nullptr /* yieldPolicy */,
                                                      kEmptyPlanNodeId);
        // Make every virtual scan block until its operation is interrupted.
        FailPointEnableBlock hangVirtualScan("hangVirtualScan");

        auto* serviceContext = getServiceContext();
        Status queryStatus = Status::OK();
        Notification<void> started;

        stdx::thread queryThread([&] {
            Client::initThread("ExchangeGatherQuery", serviceContext->getService());
            ServiceContext::UniqueOperationContext queryOpCtx = cc().makeOperationContext();
            queryOpCtx->setDeadlineAfterNowBy(Seconds{1}, ErrorCodes::ExceededTimeLimit);
            auto ctx = makeCompileCtx();
            stage->attachToOperationContext(queryOpCtx.get());
            try {
                stage->prepare(*ctx);
                stage->open(false);
                started.set();
                stage->getNext();
            } catch (const DBException& ex) {
                queryStatus = ex.toStatus();
            }
            stage->close();
            stage->detachFromOperationContext();
        });

        started.get(operationContext());

        // The query must notice the interrupt within a reasonable time.
        Timer timer;
        queryThread.join();
        ASSERT_LT(timer.elapsed(), Seconds(5));
        ASSERT_EQ(queryStatus, ErrorCodes::ExceededTimeLimit);
    }
};

TEST_F(ExchangeGatherTest, DuplicateInputSlotsAreRejected) {
    auto [scanSlot, scanStage] = generateVirtualScan(BSON_ARRAY(1 << 2 << 3));

    ASSERT_THROWS_CODE(
        makeExchangeGather(std::move(scanStage), value::SlotVector{scanSlot, scanSlot}, 2),
        DBException,
        13436501);
}

TEST_F(ExchangeGatherTest, DegreeOfParallelismBelowTwoIsRejected) {
    auto [scanSlot, scanStage] = generateVirtualScan(BSON_ARRAY(1 << 2 << 3));

    ASSERT_THROWS_CODE(makeExchangeGather(std::move(scanStage), value::SlotVector{scanSlot}, 1),
                       DBException,
                       13436500);
}

TEST_F(ExchangeGatherTest, EmptyInputProducesNoResults) {
    auto makeStageFn = [this](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto stage = makeExchangeGather(std::move(scanStage), value::SlotVector{scanSlot}, 2);
        return std::make_pair(scanSlot, std::move(stage));
    };

    auto [inputTag, inputVal] = stage_builder::makeValue(BSONArray{});
    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSONArray{});
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(ExchangeGatherTest, CloneProducesAnIndependentlyUsableCopy) {
    auto [scanSlot, scanStage] = generateVirtualScan(BSON_ARRAY(1 << 2 << 3));
    // dop (4) exceeds the number of input elements (3), so this stays on the single-threaded
    // fallback path.
    auto original = makeExchangeGather(std::move(scanStage), value::SlotVector{scanSlot}, 4);

    auto clone = original->clone();

    auto ctx = makeCompileCtx();
    auto* accessor = prepareTree(ctx.get(), clone.get(), scanSlot);
    value::TagValueOwned results =
        value::TagValueOwned::fromRaw(getAllResults(clone.get(), accessor));

    value::TagValueOwned expected =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3)));

    ASSERT_SBE_VALUE_EQ(results.tag(), results.value(), expected.tag(), expected.value());

    clone->close();
}

TEST_F(ExchangeGatherTest, StatsReportRequestedAndActualDop) {
    auto [scanSlot, scanStage] = generateVirtualScan(BSON_ARRAY(1 << 2 << 3));
    // dop (4) exceeds the number of input elements (3), so this stays on the single-threaded
    // fallback path.
    auto stage = makeExchangeGather(std::move(scanStage), value::SlotVector{scanSlot}, 4);

    auto ctx = makeCompileCtx();
    auto accessor = prepareTree(ctx.get(), stage.get(), scanSlot);
    exhaustStage(stage.get(), accessor);

    auto stats = stage->getStats(true /* includeDebugInfo */);
    ASSERT_EQ(stats->debugInfo["dop"].numberLong(), 4);
    // The input is too small to split at the requested dop, so ExchangeGather must fall back to
    // a single producer.
    ASSERT_EQ(stats->debugInfo["actualDop"].numberLong(), 1);

    stage->close();
}

TEST_F(ExchangeGatherTest, GathersAllValuesSingleThreadedFallback) {
    auto makeStageFn = [this](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        // dop (6) exceeds the number of input elements (5), so this stays on the single-threaded
        // fallback path.
        auto stage = makeExchangeGather(std::move(scanStage), value::SlotVector{scanSlot}, 6);
        return std::make_pair(scanSlot, std::move(stage));
    };

    auto [inputTag, inputVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4 << 5));
    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4 << 5));
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(ExchangeGatherTest, GathersAllValuesSingleThreadedFallbackNoThreadsAvailable) {
    unittest::ServerParameterGuard maxWorkersZero("internalQuerySBEExchangeMaxWorkers", 0);

    auto makeStageFn = [this](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        // dop (3) is lower than the number of input elements (5), but parallel execution is
        // disabled by setting the thread pool to 0 capacity.
        auto stage = makeExchangeGather(std::move(scanStage), value::SlotVector{scanSlot}, 3);
        return std::make_pair(scanSlot, std::move(stage));
    };

    auto [inputTag, inputVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4 << 5));
    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4 << 5));
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(ExchangeGatherTest, GathersMultipleSlotsTogetherSingleThreadedFallback) {
    auto makeStageFn = [this](value::SlotVector scanSlots, std::unique_ptr<PlanStage> scanStage) {
        // dop (4) exceeds the number of input rows (3), so this stays on the single-threaded
        // fallback path.
        auto stage = makeExchangeGather(std::move(scanStage), scanSlots, 4);
        return std::make_pair(scanSlots, std::move(stage));
    };

    auto rows = BSON_ARRAY(BSON_ARRAY(1 << 10) << BSON_ARRAY(2 << 20) << BSON_ARRAY(3 << 30));
    auto [inputTag, inputVal] = stage_builder::makeValue(rows);
    auto [expectedTag, expectedVal] = stage_builder::makeValue(rows);

    // Each subarray must stay grouped together in the output: ExchangeGather must route every
    // exposed slot through the same producer index on every call to getNext().
    runTestMulti(2 /* numInputSlots */, inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(ExchangeGatherTest, GathersAllValues) {
    auto makeStageFn = [this](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        // dop (3) is lower than the number of input elements (5), so VirtualScanStage will split
        // the source data into a (2) + (2) + (1) parallel sources.
        auto stage = makeExchangeGather(std::move(scanStage), value::SlotVector{scanSlot}, 3);
        return std::make_pair(scanSlot, std::move(stage));
    };

    auto [inputTag, inputVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4 << 5));

    auto ctx = makeCompileCtx();
    value::TagValueOwned resultsOwned =
        value::TagValueOwned::fromRaw(runTest(ctx.get(), inputTag, inputVal, makeStageFn));

    // Order of values is non-deterministic, convert Array values into Set values.
    value::TagValueOwned resultsSetOwned = value::TagValueOwned::fromRaw(
        value::makeNewArraySet(resultsOwned.tag(), resultsOwned.value()));
    value::TagValueOwned expectedOwned =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4 << 5)));
    value::TagValueOwned expectedSetOwned = value::TagValueOwned::fromRaw(
        value::makeNewArraySet(expectedOwned.tag(), expectedOwned.value()));
    ASSERT_SBE_VALUE_EQ(resultsSetOwned.tag(),
                        resultsSetOwned.value(),
                        expectedSetOwned.tag(),
                        expectedSetOwned.value());
}

TEST_F(ExchangeGatherTest, GathersAllValuesWithFilter) {
    auto makeStageFn = [this](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        // Build a FilterStage whose filter expression is "mod(scanSlot, 2) == 0", so only the even
        // values reach the ExchangeGather.
        auto filter = makeS<FilterStage<false>>(
            std::move(scanStage),
            makeE<EPrimBinary>(
                EPrimBinary::eq,
                makeE<EFunction>(EFn::kMod,
                                 makeEs(makeE<EVariable>(scanSlot),
                                        makeE<EConstant>(value::TypeTags::NumberInt32,
                                                         value::bitcastFrom<int32_t>(2)))),
                makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0))),
            kEmptyPlanNodeId);

        // dop (3) is lower than the number of input elements (5), so VirtualScanStage will split
        // the source data into a (2) + (2) + (1) parallel sources.
        auto stage = makeExchangeGather(std::move(filter), value::SlotVector{scanSlot}, 3);
        return std::make_pair(scanSlot, std::move(stage));
    };

    auto [inputTag, inputVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4 << 5));

    auto ctx = makeCompileCtx();
    value::TagValueOwned resultsOwned =
        value::TagValueOwned::fromRaw(runTest(ctx.get(), inputTag, inputVal, makeStageFn));

    // Order of values is non-deterministic, convert Array values into Set values.
    value::TagValueOwned resultsSetOwned = value::TagValueOwned::fromRaw(
        value::makeNewArraySet(resultsOwned.tag(), resultsOwned.value()));
    value::TagValueOwned expectedOwned =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(2 << 4)));
    value::TagValueOwned expectedSetOwned = value::TagValueOwned::fromRaw(
        value::makeNewArraySet(expectedOwned.tag(), expectedOwned.value()));
    ASSERT_SBE_VALUE_EQ(resultsSetOwned.tag(),
                        resultsSetOwned.value(),
                        expectedSetOwned.tag(),
                        expectedSetOwned.value());
}

TEST_F(ExchangeGatherTest, GathersMultipleSlotsTogether) {
    auto makeStageFn = [this](value::SlotVector scanSlots, std::unique_ptr<PlanStage> scanStage) {
        // dop (2) is lower than the number of input elements (3), so VirtualScanStage will split
        // the source data into a (2) + (1) parallel sources.
        auto stage = makeExchangeGather(std::move(scanStage), scanSlots, 2);
        return std::make_pair(scanSlots, std::move(stage));
    };

    auto rows = BSON_ARRAY(BSON_ARRAY(1 << 10) << BSON_ARRAY(2 << 20) << BSON_ARRAY(3 << 30));
    auto [inputTag, inputVal] = stage_builder::makeValue(rows);

    // Each subarray must stay grouped together in the output: ExchangeGather must route every
    // exposed slot through the same producer index on every call to getNext().
    // Order of values is non-deterministic, convert Array values into Set values.
    value::TagValueOwned resultsOwned = value::TagValueOwned::fromRaw(
        runTestMulti(2 /* numInputSlots */, inputTag, inputVal, makeStageFn));

    value::TagValueOwned resultsSetOwned = value::TagValueOwned::fromRaw(
        value::makeNewArraySet(resultsOwned.tag(), resultsOwned.value()));
    value::TagValueOwned expectedOwned =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(rows));
    value::TagValueOwned expectedSetOwned = value::TagValueOwned::fromRaw(
        value::makeNewArraySet(expectedOwned.tag(), expectedOwned.value()));
    ASSERT_SBE_VALUE_EQ(resultsSetOwned.tag(),
                        resultsSetOwned.value(),
                        expectedSetOwned.tag(),
                        expectedSetOwned.value());
}

TEST_F(ExchangeGatherTest, WorkerThreadsAreReusedAcrossQueries) {
    auto makeStageFn = [this](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto stage = makeExchangeGather(std::move(scanStage), value::SlotVector{scanSlot}, 3);
        return std::make_pair(scanSlot, std::move(stage));
    };

    auto runOnce = [&] {
        auto [inputTag, inputVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 4 << 5));
        auto ctx = makeCompileCtx();
        value::TagValueOwned results =
            value::TagValueOwned::fromRaw(runTest(ctx.get(), inputTag, inputVal, makeStageFn));
    };

    auto& pool = ExchangeWorkerPool::get(operationContext()->getServiceContext());

    runOnce();
    // The three producers gave their threads back to the pool once the plan was destroyed.
    size_t createdAfterFirstRun = pool.createdCount();
    ASSERT_GTE(createdAfterFirstRun, 3u);
    ASSERT_GTE(pool.idleCount(), 3u);

    // The second query reuses those threads instead of creating new ones.
    runOnce();
    ASSERT_EQ(pool.createdCount(), createdAfterFirstRun);
}

// Reopening after a partial consumption must not expose rows (or EOF markers) left in the ready
// queue by the previous execution, which would show up as duplicated results.
TEST_F(ExchangeGatherTest, ReopenAfterPartialConsumptionDoesNotReturnStaleRows) {
    for (bool closeBeforeReopen : {true, false}) {
        auto [scanSlot, scanStage] =
            generateVirtualScan(BSON_ARRAY(1 << 2 << 3 << 4 << 5 << 6 << 7 << 8));
        // dop (4) is lower than the number of input elements (8): every producer gets 2 rows.
        auto stage = makeExchangeGather(std::move(scanStage), value::SlotVector{scanSlot}, 4);

        auto ctx = makeCompileCtx();
        auto* accessor = prepareTree(ctx.get(), stage.get(), scanSlot);

        // Consume a single row, leaving the producers' results in the ready queue.
        ASSERT_EQ(stage->getNext(), PlanState::ADVANCED);
        // Give every producer the time to push its (now stale) result.
        sleepmillis(100);

        if (closeBeforeReopen) {
            stage->close();
            stage->open(false);
        } else {
            stage->open(true);
        }

        std::vector<int32_t> results;
        while (stage->getNext() == PlanState::ADVANCED) {
            auto [tag, val] = accessor->getViewOfValue();
            ASSERT_EQ(tag, value::TypeTags::NumberInt32);
            results.push_back(value::bitcastTo<int32_t>(val));
        }
        std::sort(results.begin(), results.end());

        ASSERT_EQ(results, (std::vector<int32_t>{1, 2, 3, 4, 5, 6, 7, 8}))
            << "closeBeforeReopen: " << closeBeforeReopen;

        stage->close();
    }
}

// Destroying a running parallel plan without calling close() must not crash, hang or leak worker
// threads: the destructor is responsible for stopping the producers and returning them to the pool.
TEST_F(ExchangeGatherTest, DestroyWithoutCloseAfterOpen) {
    auto& pool = ExchangeWorkerPool::get(operationContext()->getServiceContext());

    {
        auto [scanSlot, scanStage] =
            generateVirtualScan(BSON_ARRAY(1 << 2 << 3 << 4 << 5 << 6 << 7 << 8));
        // dop (4) is lower than the number of input elements (8), so the query runs in parallel.
        auto stage = makeExchangeGather(std::move(scanStage), value::SlotVector{scanSlot}, 4);

        auto ctx = makeCompileCtx();
        prepareTree(ctx.get(), stage.get(), scanSlot);
        // The producers have been started by open(); destroy the plan right away, without
        // consuming any row and without calling close().
    }

    // All the producers must have given their threads back to the pool.
    ASSERT_GTE(pool.createdCount(), 4u);
    ASSERT_GTE(pool.idleCount(), 4u);
}

TEST_F(ExchangeGatherTest, DestroyWithoutCloseAfterPartialConsumption) {
    {
        auto [scanSlot, scanStage] =
            generateVirtualScan(BSON_ARRAY(1 << 2 << 3 << 4 << 5 << 6 << 7 << 8));
        auto stage = makeExchangeGather(std::move(scanStage), value::SlotVector{scanSlot}, 4);

        auto ctx = makeCompileCtx();
        prepareTree(ctx.get(), stage.get(), scanSlot);
        ASSERT_EQ(stage->getNext(), PlanState::ADVANCED);
        // Let the producers fill the ready queue, then destroy without close().
        sleepmillis(100);
    }
}

TEST_F(ExchangeGatherTest, MultithreadedQueryCanBeCancelled) {
    // The virtual scan input has enough entries to support a 4-level DoP.
    setupAndCancelInfiniteQuery(BSON_ARRAY(1 << 2 << 3 << 4 << 5 << 6 << 7 << 8));
}

TEST_F(ExchangeGatherTest, SingleThreadedFallbackQueryCanBeCancelled) {
    // The virtual scan input has less entries than the requested DoP, so this query will run in a
    // single thread.
    setupAndCancelInfiniteQuery(BSON_ARRAY(1 << 2));
}

TEST_F(ExchangeGatherTest, MultithreadedQueryCanTimeOut) {
    // The virtual scan input has enough entries to support a 4-level DoP.
    setupAndTimeoutInfiniteQuery(BSON_ARRAY(1 << 2 << 3 << 4 << 5 << 6 << 7 << 8));
}

TEST_F(ExchangeGatherTest, SingleThreadedFallbackQueryCanTimeout) {
    // The virtual scan input has less entries than the requested DoP, so this query will run in a
    // single thread.
    setupAndTimeoutInfiniteQuery(BSON_ARRAY(1 << 2));
}

// The consumer thread holds the global lock, and the producers need to take it in the same mode. If
// another operation queues a conflicting request in the meantime, new requests from the producers
// are queued behind it. The consumer must then release its own lock while waiting for the
// producers, otherwise the query, its producers and the other operation would deadlock.
void ExchangeGatherTest::runConflictingLockTest(LockMode queryMode,
                                                LockMode conflictingMode,
                                                QueryPhase phase) {
    auto [scanSlot, scanStage] =
        generateVirtualScan(BSON_ARRAY(1 << 2 << 3 << 4 << 5 << 6 << 7 << 8));
    // dop (4) is lower than the number of input elements (8), so the query runs in parallel.
    auto stage = std::make_unique<ExchangeGather>(std::move(scanStage),
                                                  value::SlotVector{scanSlot},
                                                  /*dop=*/4,
                                                  getYieldPolicy(),
                                                  kEmptyPlanNodeId);

    auto* serviceContext = getServiceContext();
    // The raw OperationContext pointers are published before the matching notification is set, and
    // stay valid until the owning thread terminates.
    OperationContext* queryOpCtxPtr = nullptr;
    OperationContext* lockerOpCtxPtr = nullptr;
    // The query thread reaches each of these points, and then waits for the matching 'go' signal.
    Notification<void> prepared, goOpen, opened, goGetNext, drained, goClose;
    Notification<void> lockAcquired;
    Status queryStatus = Status::OK();
    std::vector<int32_t> results;

    stdx::thread queryThread([&] {
        Client::initThread("ExchangeGatherQuery", serviceContext->getService());
        auto queryOpCtx = cc().makeOperationContext();
        queryOpCtxPtr = queryOpCtx.get();
        // The originating operation holds the locks through a collection acquisition, like a
        // regular query would. This is what allows the query to yield them.
        const bool isWrite = queryMode != MODE_IS;
        auto acquisition = acquireCollection(
            queryOpCtx.get(),
            CollectionAcquisitionRequest::fromOpCtx(
                queryOpCtx.get(),
                NamespaceString::createNamespaceString_forTest("test.exchangeGather"),
                isWrite ? AcquisitionPrerequisites::kWrite : AcquisitionPrerequisites::kRead),
            queryMode);
        // The yield policy of the fixture was created when no lock was held, so it can't yield.
        auto yieldPolicy = PlanYieldPolicySBE::make(queryOpCtx.get(),
                                                    PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                    serviceContext->getFastClockSource(),
                                                    0,
                                                    Milliseconds::zero());
        auto ctx = makeCompileCtx();
        stage->attachNewYieldPolicy(yieldPolicy.get());
        yieldPolicy->registerPlan(stage.get());
        stage->attachToOperationContext(queryOpCtx.get());
        try {
            stage->prepare(*ctx);
            auto* accessor = stage->getAccessor(*ctx, scanSlot);
            prepared.set();
            goOpen.get();
            stage->open(false);
            opened.set();
            goGetNext.get();
            while (stage->getNext() == PlanState::ADVANCED) {
                auto [tag, val] = accessor->getViewOfValue();
                ASSERT_EQ(tag, value::TypeTags::NumberInt32);
                results.push_back(value::bitcastTo<int32_t>(val));
            }
        } catch (const DBException& ex) {
            queryStatus = ex.toStatus();
        }
        // Make sure the main thread never waits forever for a query that failed early.
        for (auto* n : {&prepared, &opened, &drained}) {
            if (!*n) {
                n->set();
            }
        }
        goClose.get();
        stage->close();
    });

    // Waits until the query reaches 'point', which must be reached promptly.
    auto waitFor = [&](Notification<void>& point) {
        ASSERT_TRUE(point.waitFor(operationContext(), Seconds(10)));
    };

    stdx::thread lockerThread;
    // Stands in for an operation that needs a lock conflicting with the one held by the query.
    auto startLocker = [&] {
        Notification<void> started;
        lockerThread = stdx::thread([&] {
            Client::initThread("ConflictingLocker", serviceContext->getService());
            auto opCtx = cc().makeOperationContext();
            lockerOpCtxPtr = opCtx.get();
            started.set();
            try {
                Lock::GlobalLock lock(opCtx.get(), conflictingMode);
                lockAcquired.set();
            } catch (const DBException&) {
                // Interrupted because the query never released its lock: reported below.
            }
        });
        started.get(operationContext());
        // Wait for the request to be queued behind the lock held by the query.
        Timer queueTimer;
        while (!shard_role_details::getLocker(lockerOpCtxPtr)->getWaitingResource().isValid()) {
            ASSERT_LT(queueTimer.elapsed(), Seconds(10));
            sleepmillis(1);
        }
        ASSERT_FALSE(lockAcquired);
    };

    // Runs the query up to the point where the conflicting request must be queued, queues it, and
    // then lets the query go on.
    waitFor(prepared);
    if (phase == QueryPhase::kOpen) {
        startLocker();
    }
    goOpen.set();
    if (phase != QueryPhase::kOpen) {
        waitFor(opened);
    }
    if (phase == QueryPhase::kGetNext) {
        startLocker();
    }
    goGetNext.set();
    if (phase == QueryPhase::kClose || phase == QueryPhase::kIdleQuery) {
        waitFor(drained);
        startLocker();
        if (phase == QueryPhase::kIdleQuery) {
            stage->detachFromOperationContext();
        }
    }
    goClose.set();

    // Must get the lock within a reasonable time. If not, a deadlock is the most likely cause.
    const bool acquired = lockAcquired.waitFor(operationContext(), Seconds(10));
    if (!acquired) {
        // Unblock every thread so that we can join them.
        for (auto* opCtx : {queryOpCtxPtr, lockerOpCtxPtr}) {
            ClientLock lk(opCtx->getClient());
            serviceContext->killOperation(lk, opCtx, ErrorCodes::Interrupted);
        }
    }
    lockerThread.join();
    queryThread.join();

    ASSERT_TRUE(acquired)
        << "the conflicting lock request could not be granted: deadlock? query mode "
        << modeName(queryMode) << ", conflicting mode " << modeName(conflictingMode) << ", phase "
        << static_cast<int>(phase);
    ASSERT_OK(queryStatus);
    std::sort(results.begin(), results.end());
    ASSERT_EQ(results, (std::vector<int32_t>{1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST_F(ExchangeGatherTest, ConflictingLockRequestDoesNotDeadlockQuery) {
    using Phase = QueryPhase;
    for (auto phase : {Phase::kOpen, Phase::kGetNext, Phase::kClose, Phase::kIdleQuery}) {
        for (auto [queryMode, conflictingMode] :
             {std::pair{MODE_IX, MODE_S}, std::pair{MODE_IX, MODE_X}, std::pair{MODE_IS, MODE_X}}) {
            runConflictingLockTest(queryMode, conflictingMode, phase);
        }
    }
}

}  // namespace mongo::sbe
