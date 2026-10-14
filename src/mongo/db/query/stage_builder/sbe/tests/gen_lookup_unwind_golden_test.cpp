// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Golden tests for $lookup + $unwind plans. Each test covers one cell of the grid
 * {flat or dotted 'as'} x {preserveNullAndEmptyArrays or not} x {includeArrayIndex or not} x
 * {no stage or one stage following the $unwind}, and runs it under every join strategy. The golden
 * output holds each strategy's plan and the results, which must agree across strategies.
 */

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/sbe_unittest_assert.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mongo {
namespace {

constexpr std::string_view kLocalDocs = R"([
    {_id: 0, lkey: 1, n: 5, a: {x: 10}},
    {_id: 1, lkey: 2, n: 7},
    {_id: 2, lkey: [3, 7], n: 9, a: 5},
    {_id: 3, lkey: 3, joined: "preexisting", a: {x: 1, b: "preexisting"}},
    {_id: 4, n: 1, a: {}},
    {_id: 5, lkey: null, joined: "preexisting"}
])";

constexpr std::string_view kForeignDocs = R"([
    {_id: 10, fkey: 1, c: 100},
    {_id: 11, fkey: 1, c: 200},
    {_id: 12, fkey: 3, c: 300},
    {_id: 13, fkey: null, c: 400}
])";

// The $lookup 'as' path, which is also the $unwind path: a top-level field or a dotted path.
constexpr std::string_view kFlat = "joined";
constexpr std::string_view kDotted = "a.b";

// The $unwind 'preserveNullAndEmptyArrays' option: whether a local document with no match is kept.
constexpr bool kPreserve = true;
constexpr bool kNoPreserve = false;

// The $unwind 'includeArrayIndex' option: whether the match's array index is written to "idx".
constexpr bool kIndex = true;
constexpr bool kNoIndex = false;

// The stage, if any, that follows the $unwind and consumes its output.
enum class Follower {
    kNone,
    kProject,
    kAddFields,
    kMatchOnIndex,
    kMatchAsPath,
    kMatchDeeper,
    kMatchPrefix,
};

using Strategy = EqLookupNode::LookupStrategy;

constexpr std::array kStrategies{Strategy::kHashJoin,
                                 Strategy::kNestedLoopJoin,
                                 Strategy::kIndexedLoopJoin,
                                 Strategy::kDynamicIndexedLoopJoin};

std::vector<BSONObj> arrayToVector(std::string_view json) {
    std::vector<BSONObj> objs;
    for (auto&& elem : fromjson(json)) {
        objs.push_back(elem.Obj().getOwned());
    }
    return objs;
}

std::vector<BSONObj> followerStages(Follower follower, std::string_view as) {
    const std::string deeper = std::string{as} + ".c";
    switch (follower) {
        case Follower::kNone:
            return {};
        case Follower::kProject:
            return {BSON("$project" << BSON("_id" << 0 << "lkey" << 1 << as << 1))};
        case Follower::kAddFields:
            return {BSON("$addFields"
                         << BSON("computed" << BSON("$add" << BSON_ARRAY("$n" << "$" + deeper))))};
        case Follower::kMatchOnIndex:
            return {BSON("$match" << BSON("idx" << BSON("$gt" << 0)))};
        case Follower::kMatchAsPath:
            return {BSON("$match" << BSON(as << BSON("$exists" << true)))};
        case Follower::kMatchDeeper:
            return {BSON("$match" << BSON(deeper << BSON("$gt" << 150)))};
        case Follower::kMatchPrefix:
            return {BSON("$match" << BSON(as.substr(0, as.find('.')) << BSON("$exists" << true)))};
    }
    MONGO_UNREACHABLE;
}

NamespaceString foreignNss(Strategy strategy) {
    return NamespaceString::createNamespaceString_forTest(
        "testdb." + std::string{EqLookupNode::serializeLookupStrategy(strategy)});
}

std::vector<BSONObj> foreignIndexSpecs(Strategy strategy) {
    switch (strategy) {
        case Strategy::kIndexedLoopJoin:
            return {fromjson("{v: 2, name: 'fkey_1', key: {fkey: 1}}")};
        case Strategy::kDynamicIndexedLoopJoin:
            return {fromjson("{v: 2, name: 'fkey_1', key: {fkey: 1}, sparse: true}")};
        default:
            return {};
    }
}

const EqLookupNode* findEqLookup(const QuerySolutionNode* node) {
    if (auto* lookup = dynamic_cast<const EqLookupNode*>(node)) {
        return lookup;
    }
    for (auto&& child : node->children) {
        if (auto* lookup = findEqLookup(child.get())) {
            return lookup;
        }
    }
    return nullptr;
}

class LookupUnwindStrategyGoldenTest : public GoldenSbeStageBuilderTestFixture {
public:
    void runGrid(std::string_view as, bool preserve, bool index, Follower follower) {
        createCollection(arrayToVector(kLocalDocs), boost::none);
        for (auto strategy : kStrategies) {
            createSecondaryCollection(
                foreignNss(strategy), arrayToVector(kForeignDocs), foreignIndexSpecs(strategy));
        }

        BSONObjBuilder unwind;
        unwind.append("path", "$" + std::string{as});
        if (preserve) {
            unwind.append("preserveNullAndEmptyArrays", true);
        }
        if (index) {
            unwind.append("includeArrayIndex", "idx");
        }
        const BSONObj unwindStage = BSON("$unwind" << unwind.obj());
        const auto follow = followerStages(follower, as);
        auto pipeline = [&](std::string_view from) {
            std::vector<BSONObj> stages{
                BSON("$lookup" << BSON("from" << from << "localField" << "lkey" << "foreignField"
                                              << "fkey" << "as" << as)),
                unwindStage};
            stages.insert(stages.end(), follow.begin(), follow.end());
            return stages;
        };

        auto& out = _gctx->outStream();
        _gctx->printTestHeader(unittest::GoldenTestContext::HeaderFormat::Text);
        for (const auto& stage : pipeline("<strategy>")) {
            out << stage << std::endl;
        }

        boost::optional<sbe::value::TagValueOwned> reference;
        for (auto strategy : kStrategies) {
            out << std::endl
                << "## " << EqLookupNode::serializeLookupStrategy(strategy) << std::endl;
            auto run =
                runStrategy(strategy, pipeline(EqLookupNode::serializeLookupStrategy(strategy)));
            if (!run) {
                out << "Pipeline is not entirely pushed down to SBE." << std::endl;
                continue;
            }
            out << run->explain;
            auto sorted = sortArray(run->results.view());
            if (reference) {
                ASSERT_SBE_VALUE_EQ(sorted.view(), reference->view());
            } else {
                reference.emplace(std::move(sorted));
            }
        }

        if (reference) {
            out << std::endl << "## Results" << std::endl;
            BSONArrayBuilder resultsBuilder;
            sbe::bson::convertToBsonArr(resultsBuilder,
                                        sbe::value::getArrayView(reference->value()));
            for (auto&& doc : resultsBuilder.arr()) {
                out << doc.Obj() << std::endl;
            }
        }
        _gctx->verifyOutput();
    }

private:
    boost::optional<PlanRun> runStrategy(Strategy strategy, const std::vector<BSONObj>& stages) {
        const auto foreign = foreignNss(strategy);
        boost::intrusive_ptr<ExpressionContext> expCtx =
            make_intrusive<ExpressionContextForTest>(operationContext(), _nss);
        expCtx->setAllowDiskUse(strategy == Strategy::kHashJoin);
        expCtx->setResolvedNamespaces(
            ResolvedNamespaceMap{{foreign, {foreign, std::vector<BSONObj>()}}});

        auto pipeline =
            pipeline_factory::makePipeline(stages, expCtx, pipeline_factory::kOptionsMinimal);
        pipeline_optimization::optimizePipeline(*pipeline);

        auto colls = acquireCollections(_nss, {foreign});
        auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = expCtx,
            .parsedFind = ParsedFindCommandParams{std::make_unique<FindCommandRequest>(_nss)}});
        auto makePlannerParams = [&] {
            return std::make_unique<QueryPlannerParams>(
                QueryPlannerParams::ArgsForPushDownStagesDecision{.opCtx = operationContext(),
                                                                  .canonicalQuery = *cq,
                                                                  .collections = colls,
                                                                  .plannerOptions =
                                                                      QueryPlannerParams::DEFAULT});
        };
        attachPipelineStages(
            colls, pipeline.get(), /*needsMerge=*/false, cq.get(), makePlannerParams());
        if (!cq->containsEntirePipeline()) {
            return boost::none;
        }

        auto plannerParams = makePlannerParams();
        plannerParams->fillOutSecondaryCollectionsPlannerParams(operationContext(), *cq, colls);
        auto scan = std::make_unique<CollectionScanNode>(_nss);
        auto solution = QueryPlanner::extendWithAggPipeline(
            *cq, makeQuerySolution(std::move(scan)), plannerParams->secondaryCollectionsInfo);

        const auto* lookup = findEqLookup(solution->root());
        ASSERT_TRUE(lookup);
        ASSERT_EQ(EqLookupNode::serializeLookupStrategy(lookup->lookupStrategy),
                  EqLookupNode::serializeLookupStrategy(strategy));

        return runPlan(std::move(solution), {.expCtx = expCtx, .cqPipeline = cq->cqPipeline()});
    }

    unittest::ServerParameterGuard _hashJoin{"featureFlagSbeEqLookupUnwindHashJoin", true};
    unittest::ServerParameterGuard _localCollscan{"featureFlagSbeEqLookupUnwindLocalCollscan",
                                                  true};
    unittest::ServerParameterGuard _nestedLoopJoin{"featureFlagSbeEqLookupUnwindNestedLoopJoin",
                                                   true};
    unittest::ServerParameterGuard _indexedLoopJoin{"featureFlagSbeEqLookupUnwindIndexedLoopJoin",
                                                    true};
    unittest::ServerParameterGuard _dynamicIndexedLoopJoin{
        "featureFlagSbeEqLookupUnwindDynamicIndexedLoopJoin", true};
};

#define LOOKUP_UNWIND_TEST(As, Preserve, Index, Follow)                   \
    TEST_F(LookupUnwindStrategyGoldenTest, As##Preserve##Index##Follow) { \
        runGrid(k##As, k##Preserve, k##Index, Follower::k##Follow);       \
    }

#define LOOKUP_UNWIND_FOLLOWERS(As, Preserve, Index)      \
    LOOKUP_UNWIND_TEST(As, Preserve, Index, None)         \
    LOOKUP_UNWIND_TEST(As, Preserve, Index, Project)      \
    LOOKUP_UNWIND_TEST(As, Preserve, Index, AddFields)    \
    LOOKUP_UNWIND_TEST(As, Preserve, Index, MatchOnIndex) \
    LOOKUP_UNWIND_TEST(As, Preserve, Index, MatchAsPath)  \
    LOOKUP_UNWIND_TEST(As, Preserve, Index, MatchDeeper)  \
    LOOKUP_UNWIND_TEST(As, Preserve, Index, MatchPrefix)

#define LOOKUP_UNWIND_INDEX(As, Preserve)        \
    LOOKUP_UNWIND_FOLLOWERS(As, Preserve, Index) \
    LOOKUP_UNWIND_FOLLOWERS(As, Preserve, NoIndex)

#define LOOKUP_UNWIND_PRESERVE(As)    \
    LOOKUP_UNWIND_INDEX(As, Preserve) \
    LOOKUP_UNWIND_INDEX(As, NoPreserve)

LOOKUP_UNWIND_PRESERVE(Flat)
LOOKUP_UNWIND_PRESERVE(Dotted)

}  // namespace
}  // namespace mongo
