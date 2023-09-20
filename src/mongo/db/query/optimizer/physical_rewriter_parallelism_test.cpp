/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <memory>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/ce/hinted_estimator.h"
#include "mongo/db/query/cost_model/cost_model_gen.h"
#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"


namespace mongo::optimizer {
namespace {

using namespace unit_test_abt_literals;

TEST(PhysRewriterParallel, ParallelScan) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT rootNode = NodeBuilder{}
                       .root("root")
                       .filter(_evalf(_get("a", _traverse1(_cmp("Eq", "1"_cint64))), "root"_var))
                       .finish(_scan("root", "c1"));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({}, {}, ConstEval::constFold, {DistributionType::UnknownPartitioning})}},
         5 /*numberOfPartitions*/},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: Centralized\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_0]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_0}, c1, parallel]\n",
        optimized);
}

TEST(PhysRewriterParallel, HashPartitioning) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT rootNode = NodeBuilder{}
                       .root("pc")
                       .gb(_varnames("pa"), _varnames("pc"), {"pb"_var})
                       .eval("pb", _evalp(_get("b", _id()), "root"_var))
                       .eval("pa", _evalp(_get("a", _id()), "root"_var))
                       .finish(_scan("root", "c1"));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {},
                         ConstEval::constFold,
                         {DistributionType::HashPartitioning,
                          makeSeq(make<PathGet>("a", make<PathIdentity>()))})}},
         5 /*numberOfPartitions*/},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(5, 10, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pc}]\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: Centralized\n"
        "GroupBy [{pa}]\n"
        "|   aggregations: \n"
        "|       [pc]\n"
        "|           Variable [pb]\n"
        "PhysicalScan [{'a': pa, 'b': pb}, c1]\n",
        optimized);
}

TEST(PhysRewriterParallel, IndexPartitioning0) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ce::PartialSchemaSelHints hints;
    hints.emplace(PartialSchemaKey{"root", make<PathGet>("a", make<PathIdentity>())},
                  kDefaultSelectivity);
    hints.emplace(PartialSchemaKey{"root", make<PathGet>("b", make<PathIdentity>())},
                  kDefaultSelectivity);

    ABT rootNode = NodeBuilder{}
                       .root("pc")
                       .gb(_varnames("pa"), _varnames("pc"), {"pb"_var})
                       .filter(_evalf(_cmp("Gt", "1"_cint64), "pb"_var))
                       .eval("pb", _evalp(_get("b", _id()), "root"_var))
                       .filter(_evalf(_cmp("Gt", "0"_cint64), "pa"_var))
                       .eval("pa", _evalp(_get("a", _id()), "root"_var))
                       .finish(_scan("root", "c1"));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{
                     {{makeNonMultikeyIndexPath("a"), CollationOp::Ascending}},
                     false /*isMultiKey*/,
                     {DistributionType::HashPartitioning, makeSeq(makeNonMultikeyIndexPath("a"))},
                     psr::makeNoOp()}}},
               ConstEval::constFold,
               {DistributionType::HashPartitioning, makeSeq(makeNonMultikeyIndexPath("b"))})}},
         5 /*numberOfPartitions*/},
        makeHintedCE(std::move(hints)),
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN_AUTO(20, 40, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pc}]\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: Centralized\n"
        "GroupBy [{pa}]\n"
        "|   aggregations: \n"
        "|       [pc]\n"
        "|           Variable [pb]\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: HashPartitioning\n"
        "|   |           projections: \n"
        "|   |               pa\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [pb]\n"
        "|   |   PathCompare [Gt]\n"
        "|   |   Const [1]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'b': pb}, c1]\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: RoundRobin\n"
        "IndexScan [{'<indexKey> 0': pa, '<rid>': rid_0}, scanDefName: c1, indexDefName: index1, "
        "interval: {>Const [0]}]\n",
        optimized);
}

TEST(PhysRewriterParallel, IndexPartitioning1) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ce::PartialSchemaSelHints hints;
    hints.emplace(PartialSchemaKey{"root", make<PathGet>("a", make<PathIdentity>())},
                  SelectivityType{0.02});
    hints.emplace(PartialSchemaKey{"root", make<PathGet>("b", make<PathIdentity>())},
                  SelectivityType{0.01});

    ABT rootNode = NodeBuilder{}
                       .root("pc")
                       .gb(_varnames("pa"), _varnames("pc"), {"pb"_var})
                       .filter(_evalf(_cmp("Gt", "1"_cint64), "pb"_var))
                       .eval("pb", _evalp(_get("b", _id()), "root"_var))
                       .filter(_evalf(_cmp("Gt", "0"_cint64), "pa"_var))
                       .eval("pa", _evalp(_get("a", _id()), "root"_var))
                       .finish(_scan("root", "c1"));

    // TODO SERVER-71551 Follow up unit tests with overriden Cost Model.
    auto costModel = getTestCostModel();
    costModel.setNestedLoopJoinIncrementalCost(0.002);
    costModel.setHashJoinIncrementalCost(5e-5);

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase,
         OptPhase::ProjNormalize},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{
                     {{makeNonMultikeyIndexPath("a"), CollationOp::Ascending}},
                     false /*isMultiKey*/,
                     {DistributionType::HashPartitioning, makeSeq(makeNonMultikeyIndexPath("a"))},
                     psr::makeNoOp()}},
                {"index2",
                 IndexDefinition{
                     {{makeNonMultikeyIndexPath("b"), CollationOp::Ascending}},
                     false /*isMultiKey*/,
                     {DistributionType::HashPartitioning, makeSeq(makeNonMultikeyIndexPath("b"))},
                     psr::makeNoOp()}}},
               ConstEval::constFold,
               {DistributionType::HashPartitioning, makeSeq(makeNonMultikeyIndexPath("c"))})}},
         5 /*numberOfPartitions*/},
        makeHintedCE(std::move(hints)),
        std::move(costModel),
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN_AUTO(80, 140, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{renamed_7}]\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: Centralized\n"
        "GroupBy [{renamed_3}]\n"
        "|   aggregations: \n"
        "|       [renamed_7]\n"
        "|           Variable [renamed_1]\n"
        "HashJoin [joinType: Inner]\n"
        "|   |   Condition\n"
        "|   |       renamed_0 = renamed_4\n"
        "|   Union [{renamed_3, renamed_4}]\n"
        "|   Evaluation [{renamed_4} = Variable [renamed_0]]\n"
        "|   IndexScan [{'<indexKey> 0': renamed_3, '<rid>': renamed_0}, scanDefName: c1, "
        "indexDefName: index1, interval: {>Const [0]}]\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: Replicated\n"
        "IndexScan [{'<indexKey> 0': renamed_1, '<rid>': renamed_0}, scanDefName: c1, "
        "indexDefName: index2, interval: {>Const [1]}]\n",
        optimized);
}

TEST(PhysRewriterParallel, LocalGlobalAgg) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT rootNode = NodeBuilder{}
                       .root("pa", "pc")
                       .gb(_varnames("pa"), _varnames("pc"), {_fn("$sum", "pb"_var)})
                       .eval("pb", _evalp(_get("b", _id()), "root"_var))
                       .eval("pa", _evalp(_get("a", _id()), "root"_var))
                       .finish(_scan("root", "c1"));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({}, {}, ConstEval::constFold, {DistributionType::UnknownPartitioning})}},
         5 /*numberOfPartitions*/},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(15, 25, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pa, pc}]\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: Centralized\n"
        "GroupBy [{pa}, Global]\n"
        "|   aggregations: \n"
        "|       [pc]\n"
        "|           FunctionCall [$sum]\n"
        "|           Variable [preagg_0]\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: HashPartitioning\n"
        "|   |           projections: \n"
        "|   |               pa\n"
        "GroupBy [{pa}, Local]\n"
        "|   aggregations: \n"
        "|       [preagg_0]\n"
        "|           FunctionCall [$sum]\n"
        "|           Variable [pb]\n"
        "PhysicalScan [{'a': pa, 'b': pb}, c1, parallel]\n",
        optimized);
}

TEST(PhysRewriterParallel, LocalGlobalAgg1) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT rootNode = NodeBuilder{}
                       .root("pc")
                       .gb(_varnames(), _varnames("pc"), {_fn("$sum", "pb"_var)})
                       .eval("pb", _evalp(_get("b", _id()), "root"_var))
                       .finish(_scan("root", "c1"));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({}, {}, ConstEval::constFold, {DistributionType::UnknownPartitioning})}},
         5 /*numberOfPartitions*/},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(5, 15, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pc}]\n"
        "GroupBy [Global]\n"
        "|   aggregations: \n"
        "|       [pc]\n"
        "|           FunctionCall [$sum]\n"
        "|           Variable [preagg_0]\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: Centralized\n"
        "GroupBy [Local]\n"
        "|   aggregations: \n"
        "|       [preagg_0]\n"
        "|           FunctionCall [$sum]\n"
        "|           Variable [pb]\n"
        "PhysicalScan [{'b': pb}, c1, parallel]\n",
        optimized);
}

TEST(PhysRewriterParallel, LocalLimitSkip) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT rootNode = NodeBuilder{}.root("root").ls(20, 10).finish(_scan("root", "c1"));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({}, {}, ConstEval::constFold, {DistributionType::UnknownPartitioning})}},
         5 /*numberOfPartitions*/},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(5, 15, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_PROPS_V2_AUTO(
        "Properties [cost: 0.00930515, localCost: 0, adjustedCE: 20]\n"
        "|   |   Logical:\n"
        "|   |       cardinalityEstimate: \n"
        "|   |           ce: 20\n"
        "|   |       projections: \n"
        "|   |           root\n"
        "|   |       collectionAvailability: \n"
        "|   |           c1\n"
        "|   |       distributionAvailability: \n"
        "|   |           distribution: \n"
        "|   |               type: Centralized\n"
        "|   |           distribution: \n"
        "|   |               type: UnknownPartitioning\n"
        "|   Physical:\n"
        "|       distribution: \n"
        "|           type: Centralized\n"
        "Root [{root}]\n"
        "Properties [cost: 0.00930515, localCost: 0.00252964, adjustedCE: 30.03]\n"
        "|   |   Logical:\n"
        "|   |       cardinalityEstimate: \n"
        "|   |           ce: 1000\n"
        "|   |       projections: \n"
        "|   |           root\n"
        "|   |       indexingAvailability: \n"
        "|   |           [groupId: 0, scanProjection: root, scanDefName: c1, eqPredsOnly]\n"
        "|   |       collectionAvailability: \n"
        "|   |           c1\n"
        "|   |       distributionAvailability: \n"
        "|   |           distribution: \n"
        "|   |               type: UnknownPartitioning\n"
        "|   Physical:\n"
        "|       limitSkip:\n"
        "|           limit: 20\n"
        "|           skip: 10\n"
        "|       projections: \n"
        "|           root\n"
        "|       distribution: \n"
        "|           type: Centralized\n"
        "|       indexingRequirement: \n"
        "|           Complete, dedupRID\n"
        "|       removeOrphans: \n"
        "|           false\n"
        "LimitSkip [limit: 20, skip: 10]\n"
        "Properties [cost: 0.00677551, localCost: 0.003004, adjustedCE: 30.03]\n"
        "|   |   Logical:\n"
        "|   |       cardinalityEstimate: \n"
        "|   |           ce: 1000\n"
        "|   |       projections: \n"
        "|   |           root\n"
        "|   |       indexingAvailability: \n"
        "|   |           [groupId: 0, scanProjection: root, scanDefName: c1, eqPredsOnly]\n"
        "|   |       collectionAvailability: \n"
        "|   |           c1\n"
        "|   |       distributionAvailability: \n"
        "|   |           distribution: \n"
        "|   |               type: UnknownPartitioning\n"
        "|   Physical:\n"
        "|       projections: \n"
        "|           root\n"
        "|       distribution: \n"
        "|           type: Centralized\n"
        "|       indexingRequirement: \n"
        "|           Complete, dedupRID\n"
        "|       limitEstimate: \n"
        "|           30\n"
        "|       removeOrphans: \n"
        "|           false\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: Centralized\n"
        "Properties [cost: 0.00377151, localCost: 0.00377151, adjustedCE: 30.03]\n"
        "|   |   Logical:\n"
        "|   |       cardinalityEstimate: \n"
        "|   |           ce: 1000\n"
        "|   |       projections: \n"
        "|   |           root\n"
        "|   |       indexingAvailability: \n"
        "|   |           [groupId: 0, scanProjection: root, scanDefName: c1, eqPredsOnly]\n"
        "|   |       collectionAvailability: \n"
        "|   |           c1\n"
        "|   |       distributionAvailability: \n"
        "|   |           distribution: \n"
        "|   |               type: UnknownPartitioning\n"
        "|   Physical:\n"
        "|       projections: \n"
        "|           root\n"
        "|       distribution: \n"
        "|           type: UnknownPartitioning, disableExchanges\n"
        "|       indexingRequirement: \n"
        "|           Complete, dedupRID\n"
        "|       limitEstimate: \n"
        "|           30\n"
        "|       removeOrphans: \n"
        "|           false\n"
        "PhysicalScan [{'<root>': root}, c1, parallel]\n",
        phaseManager);
}

}  // namespace
}  // namespace mongo::optimizer
