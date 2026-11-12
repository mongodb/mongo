// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_test_optimizations.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/tick_source_mock.h"

#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class TimedDisposeDocumentSource final : public DocumentSourceTestOptimizations {
public:
    using DocumentSourceTestOptimizations::DocumentSourceTestOptimizations;
    static const Id& id;
    Id getId() const override {
        return id;
    }
};

ALLOCATE_DOCUMENT_SOURCE_ID(timedDisposeDocumentSource, TimedDisposeDocumentSource::id);

class TimedDisposeStage final : public exec::agg::Stage {
public:
    explicit TimedDisposeStage(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : Stage("$timedDispose", expCtx) {}

private:
    exec::agg::GetNextResult doGetNext() override {
        return exec::agg::GetNextResult::makeEOF();
    }

    void doDispose() override {
        auto* ticks = static_cast<TickSourceMock<>*>(
            pExpCtx->getOperationContext()->getServiceContext()->getTickSource());
        ticks->advance(Milliseconds{100});
    }
};

using exec::agg::registerDocumentSourceToStageFn;
REGISTER_AGG_STAGE_MAPPING(timedDisposeStage,
                           TimedDisposeDocumentSource::id,
                           [](const boost::intrusive_ptr<DocumentSource>& ds)
                               -> boost::intrusive_ptr<exec::agg::Stage> {
                               return make_intrusive<TimedDisposeStage>(ds->getExpCtx());
                           });

class PlanExecutorTest : public unittest::Test {
protected:
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> buildExecutor() {
        // Create a control event
        auto doc = Document{{"test", true}};
        MutableDocument docBuilder(doc);
        docBuilder.metadata().setChangeStreamControlEvent(
            DocumentMetadataFields::ChangeStreamControlEventType::kTopology);

        auto mock = DocumentSourceMock::createForTest(docBuilder.freeze(), _expCtx);
        auto pipeline = Pipeline::create({mock}, _expCtx);

        return plan_executor_factory::make(_expCtx, std::move(pipeline));
    }

    QueryTestServiceContext _serviceContext{};
    ServiceContext::UniqueOperationContext _opCtx = _serviceContext.makeOperationContext();

    boost::intrusive_ptr<ExpressionContext> _expCtx =
        make_intrusive<ExpressionContextForTest>(_opCtx.get());
};

/**
 * Test that the plan executor throws upon seeing a control event on a router.
 */
using PlanExecutorTestDeathTest = PlanExecutorTest;
DEATH_TEST_REGEX_F(PlanExecutorTestDeathTest,
                   AssertsOnControlEventOnRouter,
                   "Tripwire assertion.*10358906") {
    _expCtx->setInRouter(true);

    auto exec = buildExecutor();

    BSONObj objOut;
    ASSERT_THROWS_CODE(exec->getNext(&objOut, nullptr), AssertionException, 10358906);
}

/**
 * Test that the plan executor does not throw upon seeing a control event on a shard.
 */
TEST_F(PlanExecutorTest, DoesNotAssertsOnControlEventOnShard) {
    _expCtx->setInRouter(false);

    // Must be set to true to indicate we are on a shard. Otherwise document metadata is not going
    // to be serialized.
    _expCtx->setForPerShardCursor(true);

    auto exec = buildExecutor();

    BSONObj objOut;
    auto state = exec->getNext(&objOut, nullptr);
    ASSERT_EQ(PlanExecutorPipeline::ExecState::ADVANCED, state);
    ASSERT_TRUE(objOut.hasField(Document::metaFieldChangeStreamControlEvent));
}

TEST_F(PlanExecutorTest, ClassicPipelineDisposalExcludesCleanupAndAccountsForPriorWork) {
    unittest::ServerParameterGuard interval("overdueInterruptCheckIntervalMillis", 10);
    auto* ticks = _serviceContext.tickSource();
    auto source = make_intrusive<TimedDisposeDocumentSource>(_expCtx);
    auto pipeline = Pipeline::create({source}, _expCtx);
    auto exec = plan_executor_factory::make(_expCtx, std::move(pipeline));
    ASSERT(dynamic_cast<PlanExecutorPipeline*>(exec.get()));

    _opCtx->trackOverdueInterruptChecks(ticks->getTicks());
    const auto* stats = _opCtx->overdueInterruptCheckStats();
    ticks->advance(Milliseconds{20});
    exec->dispose(_opCtx.get());

    ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 1);
    ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), Milliseconds{10});
    _opCtx->updateInterruptCheckCounters();
    ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 1);
    ticks->advance(Milliseconds{20});
    _opCtx->updateInterruptCheckCounters();
    ASSERT_EQ(stats->overdueInterruptChecks.loadRelaxed(), 2);
    ASSERT_EQ(stats->overdueAccumulator.loadRelaxed(), Milliseconds{20});
}

}  // namespace
}  // namespace mongo
