// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_internal_projection.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/unittest/unittest.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace {
using namespace mongo;

class QueryPlannerPipelinePushdownTest : public QueryPlannerTest {
protected:
    QueryPlannerPipelinePushdownTest() : QueryPlannerTest() {
        secondaryCollMap.emplace(kSecondaryNamespace, CollectionInfo());
    }

    std::vector<boost::intrusive_ptr<DocumentSource>> makeInnerPipelineStages(
        const Pipeline& pipeline) {
        std::vector<boost::intrusive_ptr<DocumentSource>> stages;
        for (auto&& source : pipeline.getSources()) {
            stages.emplace_back(source);
        }
        return stages;
    }

    /*
     * Builds a pipeline from raw input.
     */
    std::unique_ptr<Pipeline> buildTestPipeline(const std::vector<BSONObj>& rawPipeline) {
        expCtx->addResolvedNamespaces({kSecondaryNamespace});
        return pipeline_factory::makePipeline(
            rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    }

    const NamespaceString kSecondaryNamespace =
        NamespaceString::createNamespaceString_forTest("test.other");
    std::map<NamespaceString, CollectionInfo> secondaryCollMap;
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
    ASSERT(!cq->cqPipeline().empty());
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
    ASSERT(!cq->cqPipeline().empty());
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
    ASSERT(!cq->cqPipeline().empty());
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
        fromjson("{$lookup: {from: '" + std::string{kSecondaryNamespace.coll()} +
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
    ASSERT(!cq->cqPipeline().empty());
    auto solution = QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), secondaryCollMap);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{eq_lookup: {foreignCollection: '" + kSecondaryNamespace.toString_forTest() +
            "', joinFieldLocal: 'x', joinFieldForeign: 'y', joinField: 'out', "
            "strategy: 'NestedLoopJoin', node: "
            "{cscan: {dir:1, filter: {x:1}}}}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfTwoLookups) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$lookup: {from: '" + std::string{kSecondaryNamespace.coll()} +
                 "', localField: 'x', foreignField: 'y', as: 'out'}}"),
        fromjson("{$lookup: {from: '" + std::string{kSecondaryNamespace.coll()} +
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
    ASSERT(!cq->cqPipeline().empty());
    auto solution = QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), secondaryCollMap);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{eq_lookup: {foreignCollection: '" + kSecondaryNamespace.toString_forTest() +
            "', joinFieldLocal: 'a', joinFieldForeign: 'b', joinField: 'c', "
            "strategy: 'NestedLoopJoin', node: "
            "{eq_lookup: {foreignCollection: '" +
            kSecondaryNamespace.toString_forTest() +
            "', joinFieldLocal: 'x', joinFieldForeign: 'y', joinField: 'out',"
            "strategy: 'NestedLoopJoin', node: {cscan: {dir:1, filter: {x:1}}}}}}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfTwoLookupsAndTwoGroups) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$lookup: {from: '" + std::string{kSecondaryNamespace.coll()} +
                 "', localField: 'x', foreignField: 'y', as: 'out'}}"),
        fromjson("{$group: {_id: '$out', count: {$sum: '$x'}}}"),
        fromjson("{$lookup: {from: '" + std::string{kSecondaryNamespace.coll()} +
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
    ASSERT(!cq->cqPipeline().empty());
    auto solution = QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), secondaryCollMap);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$c'}, accs: [{count: {$min: '$count'}}], node: "
        "{eq_lookup: {foreignCollection: '" +
            kSecondaryNamespace.toString_forTest() +
            "', joinFieldLocal: 'a', joinFieldForeign: 'b', joinField: 'c', "
            "strategy: 'NestedLoopJoin', node: "
            "{group: {key: {_id: '$out'}, accs: [{count: {$sum: '$x'}}], node: "
            "{eq_lookup: {foreignCollection: '" +
            kSecondaryNamespace.toString_forTest() +
            "', joinFieldLocal: 'x', joinFieldForeign: 'y', joinField: 'out',"
            "strategy: 'NestedLoopJoin', node: "
            "{cscan: {dir:1, filter: {x:1}}}}}}}}}}}",
        solution->root()))
        << solution->root()->toString();
}

// Tests for removeInclusionProjectionBelowGroup: the optimizer eliminates a $project that sits
// directly below a $group when the group doesn't actually read the projected fields.
//
// These tests construct a DocumentSourceInternalProjection directly (the stage type that
// extendWithAggPipeline recognises as a pushdown-able $project) so they can exercise the
// QSN-level optimisation without requiring trySbeEngine to be set.

TEST_F(QueryPlannerPipelinePushdownTest, ComputedProjectionBeforeZeroDependencyGroupIsEliminated) {
    // Pipeline: [$_internalProjection({k0: {$add: ['$a', '$b']}}), $group({_id: null, total: {$sum:
    // 1}})] The $group has no field dependencies (_id is constant null, accumulator is constant 1).
    // Even though the $project computes an expression, its output is entirely unused by the $group,
    // so the optimizer should eliminate the intermediate projection node.
    auto projStage = make_intrusive<DocumentSourceInternalProjection>(
        expCtx, fromjson("{k0: {$add: ['$a', '$b']}}"), InternalProjectionPolicyEnum::kAggregate);

    const std::vector<BSONObj> rawGroupPipeline = {
        fromjson("{$group: {_id: null, total: {$sum: 1}}}"),
    };
    auto groupPipeline = buildTestPipeline(rawGroupPipeline);
    auto stages = makeInnerPipelineStages(*groupPipeline);
    stages.insert(stages.begin(), projStage);

    runQueryWithPipeline(BSONObj(), std::move(stages));
    ASSERT_EQUALS(getNumSolutions(), 1U);

    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), {} /* secondaryCollInfos */);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: {$const: null}}, accs: [{total: {$sum: {$const: 1}}}], "
        "node: {cscan: {dir: 1}}}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest,
       ComputedProjectionBeforeGroupWithFieldDepsIsNotEliminated) {
    // Pipeline: [$_internalProjection({k0: {$add: ['$a', '$b']}}), $group({_id: null, total: {$sum:
    // '$k0'}})] The $group accumulates over field 'k0' (computed by the $project), so
    // requiredFields is non-empty.  The projection must be preserved.
    auto projStage = make_intrusive<DocumentSourceInternalProjection>(
        expCtx, fromjson("{k0: {$add: ['$a', '$b']}}"), InternalProjectionPolicyEnum::kAggregate);

    const std::vector<BSONObj> rawGroupPipeline = {
        fromjson("{$group: {_id: null, total: {$sum: '$k0'}}}"),
    };
    auto groupPipeline = buildTestPipeline(rawGroupPipeline);
    auto stages = makeInnerPipelineStages(*groupPipeline);
    stages.insert(stages.begin(), projStage);

    runQueryWithPipeline(BSONObj(), std::move(stages));
    ASSERT_EQUALS(getNumSolutions(), 1U);

    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), {} /* secondaryCollInfos */);
    // The direct-to-cscan pattern must NOT match — a proj node must still be present between the
    // group and the cscan.
    ASSERT_NOT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: {$const: null}}, accs: [{total: {$sum: '$k0'}}], "
        "node: {cscan: {dir: 1}}}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest,
       MixedProjectionBeforeGroupReadingOnlyPassthroughFieldIsEliminated) {
    // Pipeline: [$_internalProjection({k0: {$add: ['$a', '$b']}, b: 1}), $group({_id: '$b',
    // cnt: {$sum: 1}})] The projection is mixed: 'k0' is computed, 'b' is a plain pass-through.
    // The $group only reads 'b', which isFieldRetainedExactly returns true for, so the
    // projection should be eliminated.
    auto projStage = make_intrusive<DocumentSourceInternalProjection>(
        expCtx,
        fromjson("{k0: {$add: ['$a', '$b']}, b: 1}"),
        InternalProjectionPolicyEnum::kAggregate);

    const std::vector<BSONObj> rawGroupPipeline = {
        fromjson("{$group: {_id: '$b', cnt: {$sum: 1}}}"),
    };
    auto groupPipeline = buildTestPipeline(rawGroupPipeline);
    auto stages = makeInnerPipelineStages(*groupPipeline);
    stages.insert(stages.begin(), projStage);

    runQueryWithPipeline(BSONObj(), std::move(stages));
    ASSERT_EQUALS(getNumSolutions(), 1U);

    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), {} /* secondaryCollInfos */);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$b'}, accs: [{cnt: {$sum: {$const: 1}}}], "
        "node: {cscan: {dir: 1}}}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest,
       MixedProjectionBeforeGroupReadingComputedFieldIsNotEliminated) {
    // Pipeline: [$_internalProjection({k0: {$add: ['$a', '$b']}, b: 1}), $group({_id: '$k0'})]
    // The $group reads 'k0', which is a computed field in the projection.
    // isFieldRetainedExactly('k0') is false, so the projection must be preserved.
    auto projStage = make_intrusive<DocumentSourceInternalProjection>(
        expCtx,
        fromjson("{k0: {$add: ['$a', '$b']}, b: 1}"),
        InternalProjectionPolicyEnum::kAggregate);

    const std::vector<BSONObj> rawGroupPipeline = {
        fromjson("{$group: {_id: '$k0'}}"),
    };
    auto groupPipeline = buildTestPipeline(rawGroupPipeline);
    auto stages = makeInnerPipelineStages(*groupPipeline);
    stages.insert(stages.begin(), projStage);

    runQueryWithPipeline(BSONObj(), std::move(stages));
    ASSERT_EQUALS(getNumSolutions(), 1U);

    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), {} /* secondaryCollInfos */);
    // The direct-to-cscan pattern must NOT match — a proj node must still be present between the
    // group and the cscan.
    ASSERT_NOT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$k0'}, accs: [], node: {cscan: {dir: 1}}}}", solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest,
       ExclusionProjectionNotExcludingRequiredFieldsIsEliminatedBeforeGroup) {
    // Pipeline: [$_internalProjection({c: 0}), $group({_id: '$a', total: {$sum: '$b'}})]
    // {c: 0} retains 'a' and 'b' exactly (isFieldRetainedExactly returns true for fields not
    // mentioned in an exclusion), so the optimizer should eliminate the projection.
    auto projStage = make_intrusive<DocumentSourceInternalProjection>(
        expCtx, fromjson("{c: 0}"), InternalProjectionPolicyEnum::kAggregate);

    const std::vector<BSONObj> rawGroupPipeline = {
        fromjson("{$group: {_id: '$a', total: {$sum: '$b'}}}"),
    };
    auto groupPipeline = buildTestPipeline(rawGroupPipeline);
    auto stages = makeInnerPipelineStages(*groupPipeline);
    stages.insert(stages.begin(), projStage);

    runQueryWithPipeline(BSONObj(), std::move(stages));
    ASSERT_EQUALS(getNumSolutions(), 1U);

    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), {} /* secondaryCollInfos */);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$a'}, accs: [{total: {$sum: '$b'}}], "
        "node: {cscan: {dir: 1}}}}",
        solution->root()))
        << solution->root()->toString();
}

}  //  namespace
