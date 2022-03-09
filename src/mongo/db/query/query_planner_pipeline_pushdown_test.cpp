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
#include "mongo/db/query/query_planner_test_lib.h"

namespace {
using namespace mongo;

class QueryPlannerPipelinePushdownTest : public QueryPlannerTest {
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
        expCtx->addResolvedNamespaces({kSecondaryNamespace});
        return Pipeline::parse(rawPipeline, expCtx);
    }

    const NamespaceString kSecondaryNamespace{"test.other"};
    const std::map<NamespaceString, SecondaryCollectionInfo> secondaryCollMap{
        {kSecondaryNamespace, SecondaryCollectionInfo()}};
};

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfASingleGroup) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$group: {_id: '$_id', count: {$sum: '$x'}}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    // 'runQueryWithPipeline()' only does planning of the find subsystem.
    ASSERT_EQUALS(getNumSolutions(), 1U);
    ASSERT(QueryPlannerTestLib::solutionMatches("{cscan: {dir:1, filter: {x:1}}}", solns[0]->root())
               .isOK())
        << solns[0]->root()->toString();

    // Check the plan after lowering $group into the find subsystem.
    ASSERT(!cq->pipeline().empty());
    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), {} /* secondaryCollInfos */);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$_id'}, accs: [{count: {$sum: '$x'}}], node: "
        "{cscan: {dir:1, filter: {x:1}}}"
        "}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfTwoGroups) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$group: {_id: '$_id', count: {$sum: '$x'}}}"),
        fromjson("{$group: {_id: '$_id', count: {$min: '$count'}}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    // 'runQueryWithPipeline()' only does planning of the find subsystem.
    ASSERT_EQUALS(getNumSolutions(), 1U);
    ASSERT(QueryPlannerTestLib::solutionMatches("{cscan: {dir:1, filter: {x:1}}}", solns[0]->root())
               .isOK())
        << solns[0]->root()->toString();

    // Check the plan after lowering $group into the find subsystem.
    ASSERT(!cq->pipeline().empty());
    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), {} /* secondaryCollInfos */);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$_id'}, accs: [{count: {$min: '$count'}}], node: "
        "{group: {key: {_id: '$_id'}, accs: [{count: {$sum: '$x'}}], node: "
        "{cscan: {dir:1, filter: {x:1}}}"
        "}}"
        "}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfOneGroupWithMultipleAccumulators) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$group: {_id: '$_id', count: {$sum: '$x'}, m: {$min: '$y'}}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    // 'runQueryWithPipeline()' only does planning of the find subsystem.
    ASSERT_EQUALS(getNumSolutions(), 1U);
    ASSERT(QueryPlannerTestLib::solutionMatches("{cscan: {dir:1, filter: {x:1}}}", solns[0]->root())
               .isOK())
        << solns[0]->root()->toString();

    // Check the plan after lowering $group into the find subsystem.
    ASSERT(!cq->pipeline().empty());
    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), {} /* secondaryCollInfos */);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$_id'}, accs: [{count: {$sum: '$x'}}, {m: {$min: '$y'}}], node: "
        "{cscan: {dir:1, filter: {x:1}}}"
        "}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfASingleLookup) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$lookup: {from: '" + kSecondaryNamespace.coll().toString() +
                 "', localField: 'x', foreignField: 'y', as: 'out'}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    // 'runQueryWithPipeline()' only does planning of the find subsystem.
    ASSERT_EQUALS(getNumSolutions(), 1U);
    ASSERT(QueryPlannerTestLib::solutionMatches("{cscan: {dir:1, filter: {x:1}}}", solns[0]->root())
               .isOK())
        << solns[0]->root()->toString();

    // Check the plan after lowering $lookup into the find subsystem.
    ASSERT(!cq->pipeline().empty());
    auto solution = QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), secondaryCollMap);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{eq_lookup: {foreignCollection: '" + kSecondaryNamespace.toString() +
            "', joinFieldLocal: 'x', joinFieldForeign: 'y', joinField: 'out', "
            "strategy: 'NestedLoopJoin', node: "
            "{cscan: {dir:1, filter: {x:1}}}}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfTwoLookups) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$lookup: {from: '" + kSecondaryNamespace.coll().toString() +
                 "', localField: 'x', foreignField: 'y', as: 'out'}}"),
        fromjson("{$lookup: {from: '" + kSecondaryNamespace.coll().toString() +
                 "', localField: 'a', foreignField: 'b', as: 'c'}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    // 'runQueryWithPipeline()' only does planning of the find subsystem.
    ASSERT_EQUALS(getNumSolutions(), 1U);
    ASSERT(QueryPlannerTestLib::solutionMatches("{cscan: {dir:1, filter: {x:1}}}", solns[0]->root())
               .isOK())
        << solns[0]->root()->toString();

    // Check the plan after lowering both $lookups into the find subsystem.
    ASSERT(!cq->pipeline().empty());
    auto solution = QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), secondaryCollMap);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{eq_lookup: {foreignCollection: '" + kSecondaryNamespace.toString() +
            "', joinFieldLocal: 'a', joinFieldForeign: 'b', joinField: 'c', "
            "strategy: 'NestedLoopJoin', node: "
            "{eq_lookup: {foreignCollection: '" +
            kSecondaryNamespace.toString() +
            "', joinFieldLocal: 'x', joinFieldForeign: 'y', joinField: 'out',"
            "strategy: 'NestedLoopJoin', node: {cscan: {dir:1, filter: {x:1}}}}}}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfTwoLookupsAndTwoGroups) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$lookup: {from: '" + kSecondaryNamespace.coll().toString() +
                 "', localField: 'x', foreignField: 'y', as: 'out'}}"),
        fromjson("{$group: {_id: '$out', count: {$sum: '$x'}}}"),
        fromjson("{$lookup: {from: '" + kSecondaryNamespace.coll().toString() +
                 "', localField: 'a', foreignField: 'b', as: 'c'}}"),
        fromjson("{$group: {_id: '$c', count: {$min: '$count'}}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    // 'runQueryWithPipeline()' only does planning of the find subsystem.
    ASSERT_EQUALS(getNumSolutions(), 1U);
    ASSERT(QueryPlannerTestLib::solutionMatches("{cscan: {dir:1, filter: {x:1}}}", solns[0]->root())
               .isOK())
        << solns[0]->root()->toString();

    // Check the plan after lowering the $groups and $lookups into the find subsystem.
    ASSERT(!cq->pipeline().empty());
    auto solution = QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), secondaryCollMap);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$c'}, accs: [{count: {$min: '$count'}}], node: "
        "{eq_lookup: {foreignCollection: '" +
            kSecondaryNamespace.toString() +
            "', joinFieldLocal: 'a', joinFieldForeign: 'b', joinField: 'c', "
            "strategy: 'NestedLoopJoin', node: "
            "{group: {key: {_id: '$out'}, accs: [{count: {$sum: '$x'}}], node: "
            "{eq_lookup: {foreignCollection: '" +
            kSecondaryNamespace.toString() +
            "', joinFieldLocal: 'x', joinFieldForeign: 'y', joinField: 'out',"
            "strategy: 'NestedLoopJoin', node: "
            "{cscan: {dir:1, filter: {x:1}}}}}}}}}}}",
        solution->root()))
        << solution->root()->toString();
}
}  //  namespace
