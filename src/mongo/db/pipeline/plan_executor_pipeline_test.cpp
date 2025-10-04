/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
DEATH_TEST_REGEX_F(PlanExecutorTest,
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
