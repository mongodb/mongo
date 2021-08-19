/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/json.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/inner_pipeline_stage_impl.h"
#include "mongo/db/pipeline/inner_pipeline_stage_interface.h"
#include "mongo/db/query/query_planner_test_fixture.h"

namespace {
using namespace mongo;

class QueryPlannerGroupPushdownTest : public QueryPlannerTest {
protected:
    /**
     * Makes a vector of InnerPipelineStageInterface that carries the input DocumentSources.
     */
    std::vector<std::unique_ptr<InnerPipelineStageInterface>> makeInnerPipelineStages(
        const Pipeline& pipeline) {
        std::vector<std::unique_ptr<InnerPipelineStageInterface>> stages;
        for (auto&& source : pipeline.getSources()) {
            stages.emplace_back(std::make_unique<InnerPipelineStageImpl>(source));
        }
        return stages;
    }

    /*
     * Builds a pipeline from raw input.
     */
    std::unique_ptr<Pipeline, PipelineDeleter> buildTestPipeline(
        const std::vector<BSONObj>& rawPipeline) {
        return Pipeline::parse(rawPipeline, expCtx);
    }
};

TEST_F(QueryPlannerGroupPushdownTest, PushdownOfASingleGroup) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$group: {_id: '$_id', count: {$sum: '$x'}}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertPostMultiPlanSolutionMatches(
        "{group: {key: {_id: '$_id'}, accs: [{count: {$sum: '$x'}}], node: {sentinel: "
        "{}}}}");
}

TEST_F(QueryPlannerGroupPushdownTest, PushdownOfTwoGroups) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$group: {_id: '$_id', count: {$sum: '$x'}}}"),
        fromjson("{$group: {_id: '$_id', count: {$min: '$count'}}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertPostMultiPlanSolutionMatches(
        "{group: {key: {_id: '$_id'}, accs: [{count: {$min: '$count'}}], node: {group: "
        "{key: {_id: "
        "'$_id'}, accs: [{count: {$sum: '$x'}}], node: {sentinel: {}}}}}}");
}

TEST_F(QueryPlannerGroupPushdownTest, PushdownOfOneGroupWithMultipleAccumulators) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$group: {_id: '$_id', count: {$sum: '$x'}, m: {$min: '$y'}}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    assertPostMultiPlanSolutionMatches(
        "{group: {key: {_id: '$_id'}, accs: [{count: {$sum: '$x'}}, {m: {$min: '$y'}}], node: "
        "{sentinel: "
        "{}}}}");
}
}  //  namespace
