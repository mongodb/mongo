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
    PlanExecutorImplOplogScanWithCollectionTest()
        : CatalogTestFixture(Options{}.useMockClock(true, Milliseconds{5000})) {}

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
                                            boost::none /* collection */,
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

    auto docs = {BSON("_id" << 1), BSON("_id" << 2)};
    auto [ws, mockStage] = makeMockStageWithDocuments(expCtx.get(), {docs.begin(), docs.end()});

    auto exec = plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(mockStage),
                                            boost::none /* collection */,
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

    auto docs = {BSON("_id" << 1), BSON("_id" << 2)};
    auto [ws, mockStage] = makeMockStageWithDocuments(expCtx.get(), {docs.begin(), docs.end()});

    auto exec = plan_executor_factory::make(expCtx,
                                            std::move(ws),
                                            std::move(mockStage),
                                            boost::none /* collection */,
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

    // Enqueue NEED_YIELD first so that the MockStage causes the executor to force a yield on the
    // very first getNext() call. The auto-advancing clock ensures that by the time the yield
    // callback fires, the precise clock source has advanced well past the 1 ms deadline.
    auto [ws, mockStage] = makeMockStageWithDocuments(
        expCtx.get(), {BSON("_id" << 1), BSON("_id" << 2)}, true /* enqueueNeedYieldFirst */);

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

    auto [ws, mockStage] = makeMockStageWithDocuments(
        expCtx.get(), {BSON("_id" << 1), BSON("_id" << 2)}, false /* enqueueNeedYieldFirst */);

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
    PlanExecutorImplOplogScanLogInfoTest()
        : CatalogTestFixture(Options{}.useMockClock(true, Milliseconds{100000})) {}

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

    // Enqueue NEED_YIELD first so the stage forces a yield on the first getNext() call. The
    // auto-advancing precise clock (100 000 ms per now() call) guarantees the callback sees a
    // time well past kLongOplogScanLogInfoDeadline (90 s).
    auto [ws, mockStage] = makeMockStageWithDocuments(
        expCtx.get(), {BSON("_id" << 1), BSON("_id" << 2)}, true /* enqueueNeedYieldFirst */);

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
