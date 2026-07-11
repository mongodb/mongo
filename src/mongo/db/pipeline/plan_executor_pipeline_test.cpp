// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class PlanExecutorTest : public unittest::Test {
protected:
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> buildExecutor() {
        // Create a control event
        auto doc = Document{{"test", true}};
        MutableDocument docBuilder(doc);
        docBuilder.metadata().setChangeStreamControlEvent();

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

}  // namespace
}  // namespace mongo
