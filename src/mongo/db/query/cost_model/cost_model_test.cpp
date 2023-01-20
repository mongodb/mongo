/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/cost_model/cost_model_manager.h"
#include "mongo/db/query/cost_model/cost_model_utils.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo::cost_model {

using namespace optimizer;
using namespace properties;

TEST(CostModel, IncreaseIndexScanCost) {
    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(1)))),
                         make<Variable>("root")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode));

    {
        CostModelCoefficients costCoefs{};
        initializeTestCostModel(costCoefs, 100.0);
        auto prefixId = PrefixId::createForTests();
        auto phaseManager = makePhaseManager(
            {OptPhase::MemoSubstitutionPhase,
             OptPhase::MemoExplorationPhase,
             OptPhase::MemoImplementationPhase},
            prefixId,
            {{{"c1",
               createScanDef({}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}})}}},
            costCoefs,
            {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

        ABT optimized = rootNode;
        phaseManager.optimize(optimized);

        ASSERT_EXPLAIN_V2_AUTO(
            "Root []\n"
            "|   |   projections: \n"
            "|   |       root\n"
            "|   RefBlock: \n"
            "|       Variable [root]\n"
            "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
            "|   |   Const [true]\n"
            "|   LimitSkip []\n"
            "|   |   limitSkip:\n"
            "|   |       limit: 1\n"
            "|   |       skip: 0\n"
            "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
            "|   RefBlock: \n"
            "|       Variable [rid_0]\n"
            "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Const "
            "[1]}]\n",
            optimized);
    }

    {
        CostModelCoefficients costCoefs{};
        initializeTestCostModel(costCoefs, 100.0);
        // Increasing the cost of IndexScan should result in a PhysicalScan plan.
        costCoefs.setIndexScanIncrementalCost(10000.0);

        auto prefixId = PrefixId::createForTests();
        auto phaseManager = makePhaseManager(
            {OptPhase::MemoSubstitutionPhase,
             OptPhase::MemoExplorationPhase,
             OptPhase::MemoImplementationPhase},
            prefixId,
            {{{"c1",
               createScanDef({}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}})}}},
            costCoefs,
            {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

        ABT optimized = rootNode;
        phaseManager.optimize(optimized);

        ASSERT_EXPLAIN_V2_AUTO(
            "Root []\n"
            "|   |   projections: \n"
            "|   |       root\n"
            "|   RefBlock: \n"
            "|       Variable [root]\n"
            "Filter []\n"
            "|   EvalFilter []\n"
            "|   |   Variable [evalTemp_0]\n"
            "|   PathTraverse [1]\n"
            "|   PathCompare [Eq]\n"
            "|   Const [1]\n"
            "PhysicalScan [{'<root>': root, 'a': evalTemp_0}, c1]\n",
            optimized);
    }
}

TEST(CostModel, IncreaseJoinsCost) {
    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));
    ABT filterNode1 = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Eq, Constant::int64(1))),
                         make<Variable>("pa")),
        std::move(evalNode));

    ABT filterNode2 = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("b",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(2)))),
                         make<Variable>("root")),
        std::move(filterNode1));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pa"}}, std::move(filterNode2));

    {
        auto prefixId = PrefixId::createForTests();
        CostModelCoefficients costCoefs{};
        initializeTestCostModel(costCoefs, 100.0);
        auto phaseManager = makePhaseManager(
            {OptPhase::MemoSubstitutionPhase,
             OptPhase::MemoExplorationPhase,
             OptPhase::MemoImplementationPhase},
            prefixId,
            {{{"c1",
               createScanDef(
                   {},
                   {{"index1",
                     makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)},
                    {"index2",
                     makeIndexDefinition("b", CollationOp::Ascending, false /*isMultiKey*/)}})}}},
            costCoefs,
            {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

        ABT optimized = rootNode;
        phaseManager.optimize(optimized);
        ASSERT_EXPLAIN_V2_AUTO(
            "Root []\n"
            "|   |   projections: \n"
            "|   |       pa\n"
            "|   RefBlock: \n"
            "|       Variable [pa]\n"
            "MergeJoin []\n"
            "|   |   |   Condition\n"
            "|   |   |       rid_0 = rid_1\n"
            "|   |   Collation\n"
            "|   |       Ascending\n"
            "|   Union [{rid_1}]\n"
            "|   Evaluation [{rid_1}]\n"
            "|   |   Variable [rid_0]\n"
            "|   IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index2, interval: "
            "{=Const [2]}]\n"
            "IndexScan [{'<indexKey> 0': pa, '<rid>': rid_0}, scanDefName: c1, indexDefName: "
            "index1, interval: {=Const [1]}]\n",
            optimized);
    }

    {
        auto prefixId = PrefixId::createForTests();
        CostModelCoefficients costCoefs{};
        initializeTestCostModel(costCoefs, 100.0);
        // Decreasing the cost of NestedLoopJoin should result in a NestedLoopJoin plan.
        costCoefs.setNestedLoopJoinIncrementalCost(10.0);

        auto phaseManager = makePhaseManager(
            {OptPhase::MemoSubstitutionPhase,
             OptPhase::MemoExplorationPhase,
             OptPhase::MemoImplementationPhase},
            prefixId,
            {{{"c1",
               createScanDef(
                   {},
                   {{"index1",
                     makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)},
                    {"index2",
                     makeIndexDefinition("b", CollationOp::Ascending, false /*isMultiKey*/)}})}}},
            costCoefs,
            {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

        ABT optimized = rootNode;
        phaseManager.optimize(optimized);

        auto optimizedExplain = ExplainGenerator::explainV2(optimized);

        // Verifies that a NestedLoopJoin plan is generated not a MergeJoin plan.
        ASSERT_NE(optimizedExplain.find("NestedLoopJoin"), std::string::npos);
        ASSERT_EQ(optimizedExplain.find("MergeJoin"), std::string::npos);
    }

    {
        auto prefixId = PrefixId::createForTests();
        CostModelCoefficients costCoefs{};
        initializeTestCostModel(costCoefs, 100.0);
        // Increasing the cost of both MergeJoin and NestedLoopJoin should use neither of the two
        // joins but a PhysicalScan plan.
        costCoefs.setMergeJoinIncrementalCost(10000.0);
        costCoefs.setNestedLoopJoinIncrementalCost(10000.0);

        auto phaseManager = makePhaseManager(
            {OptPhase::MemoSubstitutionPhase,
             OptPhase::MemoExplorationPhase,
             OptPhase::MemoImplementationPhase},
            prefixId,
            {{{"c1",
               createScanDef(
                   {},
                   {{"index1",
                     makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)},
                    {"index2",
                     makeIndexDefinition("b", CollationOp::Ascending, false /*isMultiKey*/)}})}}},
            costCoefs,
            {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

        ABT optimized = rootNode;
        phaseManager.optimize(optimized);

        ASSERT_EXPLAIN_V2_AUTO(
            "Root []\n"
            "|   |   projections: \n"
            "|   |       pa\n"
            "|   RefBlock: \n"
            "|       Variable [pa]\n"
            "Filter []\n"
            "|   EvalFilter []\n"
            "|   |   Variable [evalTemp_1]\n"
            "|   PathCompare [Eq]\n"
            "|   Const [2]\n"
            "Filter []\n"
            "|   EvalFilter []\n"
            "|   |   Variable [pa]\n"
            "|   PathCompare [Eq]\n"
            "|   Const [1]\n"
            "PhysicalScan [{'a': pa, 'b': evalTemp_1}, c1]\n",
            optimized);
    }
}
}  // namespace mongo::cost_model
