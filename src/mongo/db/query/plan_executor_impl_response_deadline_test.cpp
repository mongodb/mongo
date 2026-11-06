// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/mock_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.coll");

// Creates a WorkingSet and MockStage pre-loaded with the given documents. The first item in the
// stage can optionally be a NEED_YIELD state code to force a yield on the first getNext() call.
// Returns the WorkingSet and MockStage; both are needed so callers can pass them separately to
// plan_executor_factory::make(), which takes ownership of both.
std::pair<std::unique_ptr<WorkingSet>, std::unique_ptr<MockStage>> makeMockStageWithDocuments(
    ExpressionContext* expCtx,
    const std::vector<BSONObj>& docs,
    bool enqueueNeedYieldFirst = false) {
    auto ws = std::make_unique<WorkingSet>();
    auto mockStage = std::make_unique<MockStage>(expCtx, ws.get());

    if (enqueueNeedYieldFirst) {
        mockStage->enqueueStateCode(PlanStage::NEED_YIELD);
    }

    for (const auto& doc : docs) {
        WorkingSetID wsid = ws->allocate();
        WorkingSetMember* member = ws->get(wsid);
        member->doc = {SnapshotId{}, Document{doc}};
        member->transitionToOwnedObj();
        mockStage->enqueueAdvanced(wsid);
    }
    return {std::move(ws), std::move(mockStage)};
}

// Fixture for tests that only need a lightweight service context (no real collection). The yield
// policy falls back to INTERRUPT_ONLY when no collection is provided, so the deadline callback
// never fires. These tests verify that executor creation and basic document iteration work
// correctly across the different response deadline type configurations.
class PlanExecutorImplOplogScanNoCollectionTest : public ServiceContextMongoDTest {};

// Fixture for the deadline-interrupt test. Requires a real collection so that plan_executor_factory
// chooses YIELD_AUTO instead of falling back to INTERRUPT_ONLY, which means the
// afterSnapshotAndLocksRelinquishedCb actually fires during a yield.
//
// The auto-advancing mock clock (5000ms per now() call) ensures that the precise clock source
// always reports a time well past the configured 1ms timeout by the time the yield callback runs.
class PlanExecutorImplOplogScanWithCollectionTest : public CatalogTestFixture {
public:
    Options makeServiceContextOptions() override {
        return CatalogTestFixture::makeServiceContextOptions().useMockClock(true,
                                                                            Milliseconds{5000});
    }

    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions{}));
    }

    CollectionAcquisition acquireTestCollection() {
        auto opCtx = operationContext();
        return acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, kNss, AcquisitionPrerequisites::kRead),
            MODE_IS);
    }
};

// -----------------------------------------------------------------------------
// Tests using PlanExecutorImplOplogScanNoCollectionTest
// (no collection → INTERRUPT_ONLY yield policy → deadline callback never fires)
// These tests verify correct document delivery across the different response deadline type
// configurations when the timeout cannot be reached.
// -----------------------------------------------------------------------------

// A regular (non-change-stream) query produces all documents and never returns a premature IS_EOF,
// confirming that the kNone response deadline type does not interfere with execution.
TEST_F(PlanExecutorImplOplogScanNoCollectionTest, NonChangeStreamQuery_ReturnsAllDocuments) {
    auto opCtx = makeOperationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get(), kNss);

    auto docs = {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3)};
    auto [ws, mockStage] = makeMockStageWithDocuments(expCtx.get(), {docs.begin(), docs.end()});

    auto exec = plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(mockStage),
                                            /*collection=*/boost::none,
                                            PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                            QueryPlannerParams::DEFAULT,
                                            kNss);

    auto* implExec = dynamic_cast<PlanExecutorImpl*>(exec.get());
    ASSERT(implExec);

    // No change stream spec → kNone response deadline type.
    ASSERT_EQ(PlanExecutorImpl::ResponseDeadlineType::kNone,
              implExec->getResponseDeadlineType_forTest());

    BSONObj obj;
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), obj);
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), obj);
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 3), obj);
    ASSERT_EQ(PlanExecutor::IS_EOF, exec->getNext(&obj, nullptr));
}

// A change-stream query with the knob at its default value (0, meaning no time limit) delivers all
// documents. The kLogInfoMessage response deadline type is selected; because the deadline is 90
// seconds and the test does not advance the clock, no log message is emitted and no premature
// IS_EOF is returned.
TEST_F(PlanExecutorImplOplogScanNoCollectionTest, ChangeStreamQuery_KnobZero_ReturnsAllDocuments) {
    auto opCtx = makeOperationContext();

    // Knob stays at its default (0).
    ASSERT_EQ(0, internalOperationResponseMaxMS.load());

    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get(), kNss);
    DocumentSourceChangeStreamSpec spec;
    expCtx->setChangeStreamSpec(spec);
    expCtx->setTailableMode(TailableModeEnum::kTailableAndAwaitData);

    auto docs = {BSON("_id" << 1), BSON("_id" << 2)};
    auto [ws, mockStage] = makeMockStageWithDocuments(expCtx.get(), {docs.begin(), docs.end()});

    auto exec = plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(mockStage),
                                            /*collection=*/boost::none,
                                            PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                            QueryPlannerParams::DEFAULT,
                                            kNss);

    auto* implExec = dynamic_cast<PlanExecutorImpl*>(exec.get());
    ASSERT(implExec);
    ASSERT_EQ(PlanExecutorImpl::ResponseDeadlineType::kLogInfoMessage,
              implExec->getResponseDeadlineType_forTest());

    BSONObj obj;
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), obj);
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), obj);
    ASSERT_EQ(PlanExecutor::IS_EOF, exec->getNext(&obj, nullptr));
}

// A change-stream query with a large knob value delivers all documents. The kInterruptWork
// response deadline type is selected, but the deadline (set to the knob value) is far in the future
// relative to test execution time, so no premature IS_EOF is returned.
TEST_F(PlanExecutorImplOplogScanNoCollectionTest,
       ChangeStreamQuery_LargeKnobValue_ReturnsAllDocuments) {
    auto opCtx = makeOperationContext();

    // 100 seconds – far enough in the future that the deadline will never fire in a unit test.
    unittest::ServerParameterGuard controller("internalOperationResponseMaxMS", 100000LL);

    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get(), kNss);
    DocumentSourceChangeStreamSpec spec;
    expCtx->setChangeStreamSpec(spec);
    expCtx->setTailableMode(TailableModeEnum::kTailableAndAwaitData);

    auto docs = {BSON("_id" << 1), BSON("_id" << 2)};
    auto [ws, mockStage] = makeMockStageWithDocuments(expCtx.get(), {docs.begin(), docs.end()});

    auto exec = plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(mockStage),
                                            /*collection=*/boost::none,
                                            PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                            QueryPlannerParams::DEFAULT,
                                            kNss);

    auto* implExec = dynamic_cast<PlanExecutorImpl*>(exec.get());
    ASSERT(implExec);
    ASSERT_EQ(PlanExecutorImpl::ResponseDeadlineType::kInterruptWork,
              implExec->getResponseDeadlineType_forTest());

    BSONObj obj;
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), obj);
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), obj);
    ASSERT_EQ(PlanExecutor::IS_EOF, exec->getNext(&obj, nullptr));

    // response deadline type shouldn't have changed.
    ASSERT_EQ(PlanExecutorImpl::ResponseDeadlineType::kInterruptWork,
              implExec->getResponseDeadlineType_forTest());
}

// -----------------------------------------------------------------------------
// Tests using PlanExecutorImplOplogScanWithCollectionTest
// (real collection → YIELD_AUTO yield policy → deadline callback fires on yields)
// -----------------------------------------------------------------------------

// When the executor for a change stream with a small timeout yields (here triggered by a
// NEED_YIELD result from the MockStage), and the auto-advancing precise clock reports a time past
// the deadline, the executor returns IS_EOF to signal that the scan should be restarted.
// This verifies the core kInterruptWork deadline behaviour.
TEST_F(PlanExecutorImplOplogScanWithCollectionTest,
       ChangeStreamQuery_SmallKnob_DeadlineExpiredOnYield_ReturnsEOF) {
    unittest::ServerParameterGuard controller("internalOperationResponseMaxMS", 1LL);

    auto opCtx = operationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);
    DocumentSourceChangeStreamSpec spec;
    expCtx->setChangeStreamSpec(spec);
    expCtx->setTailableMode(TailableModeEnum::kTailableAndAwaitData);

    // Enqueue NEED_YIELD first so that the MockStage causes the executor to force a yield on the
    // very first getNext() call. The auto-advancing clock ensures that by the time the yield
    // callback fires, the precise clock source has advanced well past the 1 ms deadline.
    auto [ws, mockStage] = makeMockStageWithDocuments(
        expCtx.get(), {BSON("_id" << 1), BSON("_id" << 2)}, /*enqueueNeedYieldFirst=*/true);

    auto coll = acquireTestCollection();
    auto exec = plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(mockStage),
                                            coll,
                                            PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                            QueryPlannerParams::DEFAULT);

    auto* implExec = dynamic_cast<PlanExecutorImpl*>(exec.get());
    ASSERT(implExec);
    ASSERT_EQ(PlanExecutorImpl::ResponseDeadlineType::kInterruptWork,
              implExec->getResponseDeadlineType_forTest());

    BSONObj obj;
    ASSERT_EQ(PlanExecutor::IS_EOF, exec->getNext(&obj, nullptr));

    // After the deadline fires and IS_EOF is returned, the type remains kInterruptWork
    // (only kLogInfoMessage resets to kNone after firing).
    ASSERT_EQ(PlanExecutorImpl::ResponseDeadlineType::kInterruptWork,
              implExec->getResponseDeadlineType_forTest());
}

// 'releaseAllAcquiredResources()' releases storage engine resources by detaching and reattaching to
// the same operation context. DocumentSourceCursor calls it between internal batches of a single
// getMore, so it must not reset the response deadline; otherwise a lengthy request whose downstream
// filter rejects change events would keep restarting its deadline on every batch and never fire.
TEST_F(PlanExecutorImplOplogScanWithCollectionTest,
       ChangeStreamQuery_ReleaseAllAcquiredResources_PreservesDeadline) {
    unittest::ServerParameterGuard controller("internalOperationResponseMaxMS", 1LL);

    auto opCtx = operationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);
    DocumentSourceChangeStreamSpec spec;
    expCtx->setChangeStreamSpec(spec);
    expCtx->setTailableMode(TailableModeEnum::kTailableAndAwaitData);

    auto [ws, mockStage] = makeMockStageWithDocuments(expCtx.get(), {BSON("_id" << 1)});

    auto coll = acquireTestCollection();
    auto exec = plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(mockStage),
                                            coll,
                                            PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                            QueryPlannerParams::DEFAULT);

    auto* implExec = dynamic_cast<PlanExecutorImpl*>(exec.get());
    ASSERT(implExec);
    ASSERT_EQ(PlanExecutorImpl::ResponseDeadlineType::kInterruptWork,
              implExec->getResponseDeadlineType_forTest());

    const auto deadlineBefore = implExec->getResponseDeadlineValue_forTest();
    ASSERT(deadlineBefore.has_value());

    // The auto-advancing clock moves forward on every now() call, so a recomputed deadline would
    // differ from the original one.
    exec->releaseAllAcquiredResources();

    const auto deadlineAfter = implExec->getResponseDeadlineValue_forTest();
    ASSERT(deadlineAfter.has_value());
    ASSERT_EQ(deadlineBefore->toMillisSinceEpoch(), deadlineAfter->toMillisSinceEpoch());
}

// 'enforceResponseDeadline()' lets a batch-oriented driver (DocumentSourceCursor) observe the
// response deadline at a batch boundary. This is needed because each internal batch releases and
// reacquires resources, resetting the yield timer; a scan that produces many small batches would
// otherwise never yield and never check the deadline until it reaches the oplog tail.
//
// The deadline is deliberately NOT enforced until the executor has attempted to do work in the
// current request (getMore). Otherwise a very small 'operationResponseMaxMS' could fire before the
// $cursor stage has produced any result, and the getMore would return an empty batch without ever
// attempting to scan for a matching document.
TEST_F(PlanExecutorImplOplogScanWithCollectionTest,
       ChangeStreamQuery_EnforceResponseDeadline_AtBatchBoundary) {
    unittest::ServerParameterGuard controller("internalOperationResponseMaxMS", 1LL);

    auto opCtx = operationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);
    DocumentSourceChangeStreamSpec spec;
    expCtx->setChangeStreamSpec(spec);
    expCtx->setTailableMode(TailableModeEnum::kTailableAndAwaitData);

    auto [ws, mockStage] = makeMockStageWithDocuments(expCtx.get(), {BSON("_id" << 1)});

    auto coll = acquireTestCollection();
    auto exec = plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(mockStage),
                                            coll,
                                            PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                            QueryPlannerParams::DEFAULT);

    auto* implExec = dynamic_cast<PlanExecutorImpl*>(exec.get());
    ASSERT(implExec);

    // No work has been attempted in the current request yet, so the batch boundary does not enforce
    // the deadline even though the (auto-advancing) clock is already well past it. This guarantees
    // that every request attempts to make progress before the deadline can fire.
    ASSERT_FALSE(exec->enforceResponseDeadline());

    // Attempt to produce a result. The scan performs at least one unit of work, which arms the
    // batch-boundary check. Any yield that fires before this work does not observe the deadline.
    BSONObj obj;
    exec->getNext(&obj, nullptr);

    // Now that work has been attempted, the batch boundary enforces the deadline: the
    // auto-advancing clock (5000 ms per now() call) has moved past the 1 ms deadline, so
    // enforcement reports the executor as interrupted.
    ASSERT_TRUE(exec->enforceResponseDeadline());

    // The executor is now interrupted, so the next getNext() returns IS_EOF without scanning.
    ASSERT_EQ(PlanExecutor::IS_EOF, exec->getNext(&obj, nullptr));
}

// 'enforceResponseDeadline()' may fire between getNext() calls (for example at a batch boundary).
// The executor must honor that interruption on the next getNext() even when the yield timer has not
// elapsed, i.e. when no yield is due. The other tests use the auto-advancing clock, which makes a
// yield due on every loop iteration and therefore masks this case.
TEST_F(PlanExecutorImplOplogScanWithCollectionTest,
       ChangeStreamQuery_InterruptedWithoutYieldDue_ReturnsEOF) {
    unittest::ServerParameterGuard knobController("internalOperationResponseMaxMS", 1LL);
    // A yield period larger than the mock clock's per-call advance (5000 ms) ensures the yield
    // timer never elapses during the test, so no yield is due after the first loop iteration.
    unittest::ServerParameterGuard yieldPeriodController("internalQueryExecYieldPeriodMS",
                                                         100000LL);

    auto opCtx = operationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);
    DocumentSourceChangeStreamSpec spec;
    expCtx->setChangeStreamSpec(spec);
    expCtx->setTailableMode(TailableModeEnum::kTailableAndAwaitData);

    auto [ws, mockStage] =
        makeMockStageWithDocuments(expCtx.get(), {BSON("_id" << 1), BSON("_id" << 2)});

    auto coll = acquireTestCollection();
    auto exec = plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(mockStage),
                                            coll,
                                            PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                            QueryPlannerParams::DEFAULT);

    auto* implExec = dynamic_cast<PlanExecutorImpl*>(exec.get());
    ASSERT(implExec);
    ASSERT_EQ(PlanExecutorImpl::ResponseDeadlineType::kInterruptWork,
              implExec->getResponseDeadlineType_forTest());

    // Produce the first document; this also arms the batch-boundary check.
    BSONObj obj;
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), obj);

    // Simulate a batch-boundary check between getNext() calls: the deadline has expired, so the
    // executor is marked as interrupted.
    ASSERT_TRUE(exec->enforceResponseDeadline());

    // The next getNext() must return IS_EOF even though no yield is due.
    ASSERT_EQ(PlanExecutor::IS_EOF, exec->getNext(&obj, nullptr));
}

// A yield that fires before the executor has attempted any work must not softly interrupt the
// request. Setup, lock acquisition, or scheduling can consume the entire response budget (and the
// yield period) before the first '_root->work()' call; if the yield path interrupted there, every
// getMore would return an empty batch with an unchanged PBRT despite an oplog backlog. The deadline
// may only be enforced once the scan has attempted at least one unit of work.
TEST_F(PlanExecutorImplOplogScanWithCollectionTest,
       ChangeStreamQuery_DeadlineExpiredBeforeFirstWork_YieldDoesNotInterrupt) {
    unittest::ServerParameterGuard controller("internalOperationResponseMaxMS", 1LL);

    auto opCtx = operationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);
    DocumentSourceChangeStreamSpec spec;
    expCtx->setChangeStreamSpec(spec);
    expCtx->setTailableMode(TailableModeEnum::kTailableAndAwaitData);

    // The auto-advancing clock (5000 ms per now() call) makes the yield policy request a yield on
    // the very first loop iteration, i.e. before '_root->work()' has run.
    auto [ws, mockStage] = makeMockStageWithDocuments(expCtx.get(), {BSON("_id" << 1)});

    auto coll = acquireTestCollection();
    auto exec = plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(mockStage),
                                            coll,
                                            PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                            QueryPlannerParams::DEFAULT);

    auto* implExec = dynamic_cast<PlanExecutorImpl*>(exec.get());
    ASSERT(implExec);
    ASSERT_EQ(PlanExecutorImpl::ResponseDeadlineType::kInterruptWork,
              implExec->getResponseDeadlineType_forTest());

    // Even though the (auto-advancing) clock is already well past the 1 ms deadline and a yield
    // fires before any work, the executor must still scan and return the first document rather than
    // a synthetic IS_EOF.
    BSONObj obj;
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), obj);

    // Once work has been attempted, the deadline is enforced and the next getNext() returns IS_EOF.
    ASSERT_EQ(PlanExecutor::IS_EOF, exec->getNext(&obj, nullptr));
}

// A non-change-stream query has no response deadline, so batch-boundary enforcement is a no-op.
TEST_F(PlanExecutorImplOplogScanNoCollectionTest,
       NonChangeStreamQuery_EnforceResponseDeadline_IsNoOp) {
    unittest::ServerParameterGuard controller("internalOperationResponseMaxMS", 1LL);

    auto opCtx = makeOperationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get(), kNss);

    auto [ws, mockStage] = makeMockStageWithDocuments(expCtx.get(), {BSON("_id" << 1)});

    auto exec = plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(mockStage),
                                            /*collection=*/boost::none,
                                            PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                            QueryPlannerParams::DEFAULT,
                                            kNss);

    ASSERT_FALSE(exec->enforceResponseDeadline());

    BSONObj obj;
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), obj);
}

// When the deadline has not yet expired the executor returns documents normally. This is an
// end-to-end sanity-check for kInterruptWork under normal (non-expiring) conditions using a real
// collection and YIELD_AUTO policy. The knob is set to 100 000 ms (100 s); even though the
// auto-advancing clock fires the yield policy timer immediately (5000 ms >> 10 ms threshold), the
// deadline callback sees the clock at only ~5000 ms past the start, which is well within the
// 100 000 ms deadline, so IS_EOF is never triggered.
TEST_F(PlanExecutorImplOplogScanWithCollectionTest,
       ChangeStreamQuery_LargeKnob_DeadlineNotExpired_ReturnsAllDocuments) {
    unittest::ServerParameterGuard controller("internalOperationResponseMaxMS", 100000LL);

    auto opCtx = operationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);
    DocumentSourceChangeStreamSpec spec;
    expCtx->setChangeStreamSpec(spec);
    expCtx->setTailableMode(TailableModeEnum::kTailableAndAwaitData);

    auto [ws, mockStage] = makeMockStageWithDocuments(
        expCtx.get(), {BSON("_id" << 1), BSON("_id" << 2)}, /*enqueueNeedYieldFirst=*/false);

    auto coll = acquireTestCollection();
    auto exec = plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(mockStage),
                                            coll,
                                            PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                            QueryPlannerParams::DEFAULT);

    auto* implExec = dynamic_cast<PlanExecutorImpl*>(exec.get());
    ASSERT(implExec);
    ASSERT_EQ(PlanExecutorImpl::ResponseDeadlineType::kInterruptWork,
              implExec->getResponseDeadlineType_forTest());

    BSONObj obj;
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), obj);
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), obj);
    ASSERT_EQ(PlanExecutor::IS_EOF, exec->getNext(&obj, nullptr));

    // response deadline type shouldn't have changed.
    ASSERT_EQ(PlanExecutorImpl::ResponseDeadlineType::kInterruptWork,
              implExec->getResponseDeadlineType_forTest());
}

// Regression test for the internal 'fullDocument: "updateLookup"' query. Copying an expression
// context propagates 'changeStreamSpec' to the derived context used for the post-image lookup, but
// that context is not tailable (only the resumable oplog/change-collection scan is). It must
// therefore not get a response deadline: otherwise a yield past the deadline would return a
// synthetic IS_EOF that the lookup interprets as "no matching document", producing an incorrect
// 'fullDocument: null' even though the document exists.
TEST_F(PlanExecutorImplOplogScanWithCollectionTest,
       DerivedLookupQuery_InheritsChangeStreamSpec_NoResponseDeadline) {
    unittest::ServerParameterGuard controller("internalOperationResponseMaxMS", 1LL);

    auto opCtx = operationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);
    // 'changeStreamSpec' is inherited by derived contexts such as the updateLookup query, but the
    // derived context is not tailable.
    DocumentSourceChangeStreamSpec spec;
    expCtx->setChangeStreamSpec(spec);

    // Enqueue NEED_YIELD first so a yield occurs on the first getNext() call. The auto-advancing
    // clock would move past a 1 ms deadline if one were installed, which would return a premature
    // IS_EOF.
    auto [ws, mockStage] = makeMockStageWithDocuments(
        expCtx.get(), {BSON("_id" << 1), BSON("_id" << 2)}, /*enqueueNeedYieldFirst=*/true);

    auto coll = acquireTestCollection();
    auto exec = plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(mockStage),
                                            coll,
                                            PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                            QueryPlannerParams::DEFAULT);

    auto* implExec = dynamic_cast<PlanExecutorImpl*>(exec.get());
    ASSERT(implExec);

    // Not tailable -> no response deadline, despite 'changeStreamSpec' being present.
    ASSERT_EQ(PlanExecutorImpl::ResponseDeadlineType::kNone,
              implExec->getResponseDeadlineType_forTest());

    // All documents are returned; no premature IS_EOF.
    BSONObj obj;
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), obj);
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), obj);
    ASSERT_EQ(PlanExecutor::IS_EOF, exec->getNext(&obj, nullptr));
}

// -----------------------------------------------------------------------------
// Tests using PlanExecutorImplOplogScanLogInfoTest
// (real collection → YIELD_AUTO → deadline callback fires, 100 000 ms clock advance ensures the
// precise clock exceeds kLongOplogScanLogInfoDeadline (90 s) on the first yield callback)
// -----------------------------------------------------------------------------

// Fixture identical to PlanExecutorImplOplogScanWithCollectionTest except the mock clock advances
// 100 000 ms per now() call. This guarantees that a single precise-clock read inside the yield
// callback lands past kLongOplogScanLogInfoDeadline (90 s), which is what the kLogInfoMessage
// response deadline type checks.
class PlanExecutorImplOplogScanLogInfoTest : public CatalogTestFixture {
public:
    Options makeServiceContextOptions() override {
        return CatalogTestFixture::makeServiceContextOptions().useMockClock(true,
                                                                            Milliseconds{100000});
    }

    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions{}));
    }

    CollectionAcquisition acquireTestCollection() {
        auto opCtx = operationContext();
        return acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, kNss, AcquisitionPrerequisites::kRead),
            MODE_IS);
    }
};

// Verifies that once the kLongOperationResponseLogInfoDeadline elapses and the executor yields, it
// emits log message 10290001 exactly once and transitions _deadlineType to kNone, so that
// subsequent yields (which also exceed the elapsed deadline relative to the new now()) do not
// re-log.
TEST_F(PlanExecutorImplOplogScanLogInfoTest,
       ChangeStreamQuery_KnobZero_DeadlineExpired_LogsMessageOnce) {
    // Knob stays at 0 so the executor picks kLogInfoMessage, not kInterruptWork.
    ASSERT_EQ(0, internalOperationResponseMaxMS.load());

    auto opCtx = operationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);
    DocumentSourceChangeStreamSpec spec;
    expCtx->setChangeStreamSpec(spec);
    expCtx->setTailableMode(TailableModeEnum::kTailableAndAwaitData);

    // Enqueue NEED_YIELD first so the stage forces a yield on the first getNext() call. The
    // auto-advancing precise clock (100 000 ms per now() call) guarantees the callback sees a
    // time well past kLongOplogScanLogInfoDeadline (90 s).
    auto [ws, mockStage] = makeMockStageWithDocuments(
        expCtx.get(), {BSON("_id" << 1), BSON("_id" << 2)}, /*enqueueNeedYieldFirst=*/true);

    auto coll = acquireTestCollection();
    auto exec = plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(mockStage),
                                            coll,
                                            PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                            QueryPlannerParams::DEFAULT);

    auto* implExec = dynamic_cast<PlanExecutorImpl*>(exec.get());
    ASSERT(implExec);
    ASSERT_EQ(PlanExecutorImpl::ResponseDeadlineType::kLogInfoMessage,
              implExec->getResponseDeadlineType_forTest());

    // Raise the minimum logged severity for kQuery to Debug(3) so the message is captured even
    // when longOplogScanSeveritySuppressor has downgraded it from Info to Debug(3).
    unittest::MinimumLoggedSeverityGuard logSeverityGuard{logv2::LogComponent::kQuery,
                                                          logv2::LogSeverity::Debug(3)};
    unittest::LogCaptureGuard logs;

    // First getNext: a yield fires (triggered by NEED_YIELD from the stage or the yield-policy
    // timer), the deadline callback detects the clock is past kLongOplogScanLogInfoDeadline, logs
    // message 10290001, and resets _deadlineType to kNone.
    BSONObj obj;
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), obj);

    // After the first yield the response deadline type must have been cleared.
    ASSERT_EQ(PlanExecutorImpl::ResponseDeadlineType::kNone,
              implExec->getResponseDeadlineType_forTest());

    // The message must have been logged exactly once so far.
    ASSERT_EQ(1, logs.countBSONContainingSubset(BSON("id" << 10290001)));

    // Drain the remaining results. Each getNext may trigger additional yields (the auto-advancing
    // clock fires the yield-policy timer), but _deadlineType is kNone so the callback exits early
    // without logging again.
    ASSERT_EQ(PlanExecutor::ADVANCED, exec->getNext(&obj, nullptr));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), obj);
    ASSERT_EQ(PlanExecutor::IS_EOF, exec->getNext(&obj, nullptr));

    logs.stop();

    // Still exactly one log message - it was not re-emitted on subsequent yields.
    ASSERT_EQ(1, logs.countBSONContainingSubset(BSON("id" << 10290001)));
}

}  // namespace
}  // namespace mongo
