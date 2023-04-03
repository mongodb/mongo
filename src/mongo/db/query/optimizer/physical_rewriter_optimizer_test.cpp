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

#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/query/optimizer/cascades/rewriter_rules.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/unittest.h"


namespace mongo::optimizer {
namespace {

using namespace unit_test_abt_literals;

TEST(PhysRewriter, PhysicalRewriterBasic) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("p1", "test");

    ABT projectionNode1 = make<EvaluationNode>(
        "p2", make<EvalPath>(make<PathIdentity>(), make<Variable>("p1")), std::move(scanNode));

    ABT filter1Node = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("p1")),
                                       std::move(projectionNode1));

    ABT filter2Node = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a", make<PathCompare>(Operations::Eq, Constant::int64(1))),
                         make<Variable>("p2")),
        std::move(filter1Node));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"p2"}}, std::move(filter2Node));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"test", createScanDef({}, {})}}},
        /*costModel*/ boost::none,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    {
        auto env = VariableEnvironment::build(optimized);
        ProjectionNameSet expSet = {"p1", "p2"};
        ASSERT_TRUE(expSet == env.topLevelProjections());
    }
    ASSERT_EQ(5, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{p2}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [p2]\n"
        "|   PathGet [a]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Evaluation [{p2} = Variable [p1]]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [p1]\n"
        "|   PathIdentity []\n"
        "PhysicalScan [{'<root>': p1}, test]\n",
        optimized);

    // Plan output with properties.
    ASSERT_EXPLAIN_PROPS_V2_AUTO(
        "Properties [cost: 0.438321, localCost: 0, adjustedCE: 10]\n"
        "|   |   Logical:\n"
        "|   |       cardinalityEstimate: \n"
        "|   |           ce: 10\n"
        "|   |       projections: \n"
        "|   |           p1\n"
        "|   |           p2\n"
        "|   |       indexingAvailability: \n"
        "|   |           [groupId: 0, scanProjection: p1, scanDefName: test, hasProperInterval]\n"
        "|   |       collectionAvailability: \n"
        "|   |           test\n"
        "|   |       distributionAvailability: \n"
        "|   |           distribution: \n"
        "|   |               type: Centralized\n"
        "|   Physical:\n"
        "|       distribution: \n"
        "|           type: Centralized\n"
        "|       indexingRequirement: \n"
        "|           Complete, dedupRID\n"
        "Root [{p2}]\n"
        "Properties [cost: 0.438321, localCost: 0.00983406, adjustedCE: 10]\n"
        "|   |   Logical:\n"
        "|   |       cardinalityEstimate: \n"
        "|   |           ce: 10\n"
        "|   |           requirementCEs: \n"
        "|   |               refProjection: p2, path: 'PathGet [a] PathIdentity []', ce: 10\n"
        "|   |       projections: \n"
        "|   |           p1\n"
        "|   |           p2\n"
        "|   |       indexingAvailability: \n"
        "|   |           [groupId: 0, scanProjection: p1, scanDefName: test, hasProperInterval]\n"
        "|   |       collectionAvailability: \n"
        "|   |           test\n"
        "|   |       distributionAvailability: \n"
        "|   |           distribution: \n"
        "|   |               type: Centralized\n"
        "|   Physical:\n"
        "|       projections: \n"
        "|           p2\n"
        "|       distribution: \n"
        "|           type: Centralized\n"
        "|       indexingRequirement: \n"
        "|           Complete, dedupRID\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [p2]\n"
        "|   PathGet [a]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Properties [cost: 0.428487, localCost: 0, adjustedCE: 100]\n"
        "|   |   Logical:\n"
        "|   |       cardinalityEstimate: \n"
        "|   |           ce: 100\n"
        "|   |       projections: \n"
        "|   |           p1\n"
        "|   |           p2\n"
        "|   |       indexingAvailability: \n"
        "|   |           [groupId: 0, scanProjection: p1, scanDefName: test]\n"
        "|   |       collectionAvailability: \n"
        "|   |           test\n"
        "|   |       distributionAvailability: \n"
        "|   |           distribution: \n"
        "|   |               type: Centralized\n"
        "|   Physical:\n"
        "|       projections: \n"
        "|           p2\n"
        "|       distribution: \n"
        "|           type: Centralized, disableExchanges\n"
        "|       indexingRequirement: \n"
        "|           Complete, dedupRID\n"
        "Evaluation [{p2} = Variable [p1]]\n"
        "Properties [cost: 0.428487, localCost: 0, adjustedCE: 100]\n"
        "|   |   Logical:\n"
        "|   |       cardinalityEstimate: \n"
        "|   |           ce: 100\n"
        "|   |       projections: \n"
        "|   |           p1\n"
        "|   |       indexingAvailability: \n"
        "|   |           [groupId: 0, scanProjection: p1, scanDefName: test]\n"
        "|   |       collectionAvailability: \n"
        "|   |           test\n"
        "|   |       distributionAvailability: \n"
        "|   |           distribution: \n"
        "|   |               type: Centralized\n"
        "|   Physical:\n"
        "|       projections: \n"
        "|           p1\n"
        "|       distribution: \n"
        "|           type: Centralized, disableExchanges\n"
        "|       indexingRequirement: \n"
        "|           Complete, dedupRID\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [p1]\n"
        "|   PathIdentity []\n"
        "Properties [cost: 0.428487, localCost: 0.428487, adjustedCE: 1000]\n"
        "|   |   Logical:\n"
        "|   |       cardinalityEstimate: \n"
        "|   |           ce: 1000\n"
        "|   |       projections: \n"
        "|   |           p1\n"
        "|   |       indexingAvailability: \n"
        "|   |           [groupId: 0, scanProjection: p1, scanDefName: test, eqPredsOnly]\n"
        "|   |       collectionAvailability: \n"
        "|   |           test\n"
        "|   |       distributionAvailability: \n"
        "|   |           distribution: \n"
        "|   |               type: Centralized\n"
        "|   Physical:\n"
        "|       projections: \n"
        "|           p1\n"
        "|       distribution: \n"
        "|           type: Centralized, disableExchanges\n"
        "|       indexingRequirement: \n"
        "|           Complete, dedupRID\n"
        "PhysicalScan [{'<root>': p1}, test]\n",
        phaseManager);
}

TEST(PhysRewriter, GroupBy) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT projectionANode = make<EvaluationNode>(
        "a", make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")), std::move(scanNode));
    ABT projectionBNode =
        make<EvaluationNode>("b",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")),
                             std::move(projectionANode));

    ABT groupByNode = make<GroupByNode>(ProjectionNameVector{"a"},
                                        ProjectionNameVector{"c"},
                                        makeSeq(make<Variable>("b")),
                                        std::move(projectionBNode));

    ABT filterCNode = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("c")),
                                       std::move(groupByNode));

    ABT filterANode = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("a")),
                                       std::move(filterCNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"c"}}, std::move(filterANode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"test", createScanDef({}, {})}}},
        /*costModel*/ boost::none,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_EQ(7, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{c}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [a]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [c]\n"
        "|   PathIdentity []\n"
        "GroupBy [{a}]\n"
        "|   aggregations: \n"
        "|       [c]\n"
        "|           Variable [b]\n"
        "Evaluation [{b} = Variable [ptest]]\n"
        "Evaluation [{a} = Variable [ptest]]\n"
        "PhysicalScan [{'<root>': ptest}, test]\n",
        optimized);
}

TEST(PhysRewriter, GroupBy1) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT projectionANode = make<EvaluationNode>("pa", Constant::null(), std::move(scanNode));
    ABT projectionA1Node =
        make<EvaluationNode>("pa1", Constant::null(), std::move(projectionANode));

    ABT groupByNode = make<GroupByNode>(ProjectionNameVector{},
                                        ProjectionNameVector{"pb", "pb1"},
                                        makeSeq(make<Variable>("pa"), make<Variable>("pa1")),
                                        std::move(projectionA1Node));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pb"}}, std::move(groupByNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"test", createScanDef({}, {})}}},
        /*costModel*/ boost::none,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_EQ(5, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Assert we have specific CE details at certain nodes.
    std::vector<std::string> cePaths = {"Memo.0.physicalNodes.0.nodeInfo.node.ce",
                                        "Memo.1.physicalNodes.0.nodeInfo.node.ce",
                                        "Memo.3.physicalNodes.0.nodeInfo.node.ce",
                                        "Memo.4.physicalNodes.0.nodeInfo.node.ce"};
    BSONObj bsonMemo = ExplainGenerator::explainMemoBSONObj(phaseManager.getMemo());
    for (const auto& cePath : cePaths) {
        BSONElement ce = dotted_path_support::extractElementAtPath(bsonMemo, cePath);
        ASSERT(!ce.eoo());
    }

    // Projection "pb1" is unused and we do not generate an aggregation expression for it.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pb}]\n"
        "GroupBy []\n"
        "|   aggregations: \n"
        "|       [pb]\n"
        "|           Variable [pa]\n"
        "Evaluation [{pa} = Const [null]]\n"
        "PhysicalScan [{}, test]\n",
        optimized);
}

TEST(PhysRewriter, Unwind) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT projectionANode = make<EvaluationNode>(
        "a", make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")), std::move(scanNode));
    ABT projectionBNode =
        make<EvaluationNode>("b",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")),
                             std::move(projectionANode));

    ABT unwindNode =
        make<UnwindNode>("a", "a_pid", false /*retainNonArrays*/, std::move(projectionBNode));

    // This filter should stay above the unwind.
    ABT filterANode = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("a")),
                                       std::move(unwindNode));

    // This filter should be pushed down below the unwind.
    ABT filterBNode = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("b")),
                                       std::move(filterANode));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"a", "b"}},
                                  std::move(filterBNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"test", createScanDef({}, {})}}},
        /*costModel*/ boost::none,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_EQ(7, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{a, b}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [b]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [a]\n"
        "|   PathIdentity []\n"
        "Unwind [{a, a_pid}]\n"
        "Evaluation [{b} = Variable [ptest]]\n"
        "Evaluation [{a} = Variable [ptest]]\n"
        "PhysicalScan [{'<root>': ptest}, test]\n",
        optimized);
}

TEST(PhysRewriter, DuplicateFilter) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterNode1 = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(0)))),
                         make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode2 = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(0)))),
                         make<Variable>("root")),
        std::move(filterNode1));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode2));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        /*costModel*/ boost::none,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_EQ(2, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Only one copy of the filter.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_2]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [0]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_2}, c1]\n",
        optimized);
}

TEST(PhysRewriter, FilterCollation) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode = make<EvaluationNode>(
        "pb",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(1)))),
                         make<Variable>("root")),
        std::move(evalNode));

    ABT collationNode = make<CollationNode>(CollationRequirement({{"pb", CollationOp::Ascending}}),
                                            std::move(filterNode));

    ABT limitSkipNode = make<LimitSkipNode>(LimitSkipRequirement{10, 0}, std::move(collationNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pb"}}, std::move(limitSkipNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        /*costModel*/ boost::none,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(9, 11, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Limit-skip is attached to the collation node by virtue of physical props.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pb}]\n"
        "Collation [{pb: Ascending}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_1]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'a': evalTemp_1, 'b': pb}, c1]\n",
        optimized);
}

TEST(PhysRewriter, EvalCollation) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT collationNode = make<CollationNode>(CollationRequirement({{"pa", CollationOp::Ascending}}),
                                            std::move(evalNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pa"}}, std::move(collationNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        /*costModel*/ boost::none,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pa}]\n"
        "Collation [{pa: Ascending}]\n"
        "PhysicalScan [{'a': pa}, c1]\n",
        optimized);
}

TEST(PhysRewriter, FilterEvalCollation) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(10)))),
                         make<Variable>("root")),
        std::move(scanNode));

    ABT evalNode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(filterNode));

    ABT collationNode = make<CollationNode>(CollationRequirement({{"pa", CollationOp::Ascending}}),
                                            std::move(evalNode));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}},
                                  std::move(collationNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        /*costModel*/ boost::none,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Collation [{pa: Ascending}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pa]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [10]\n"
        "PhysicalScan [{'<root>': root, 'a': pa}, c1]\n",
        optimized);
}

TEST(PhysRewriter, FilterIndexing) {
    using namespace properties;

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
        auto prefixId = PrefixId::createForTests();
        auto phaseManager = makePhaseManager(
            {OptPhase::MemoSubstitutionPhase, OptPhase::MemoExplorationPhase},
            prefixId,
            {{{"c1",
               createScanDef({}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}})}}},
            /*costModel*/ boost::none,
            {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

        // Demonstrate sargable node is rewritten from filter node.
        // Note: SargableNodes cannot be lowered and by default are not created unless we have
        // indexes.

        ABT optimized = rootNode;
        phaseManager.optimize(optimized);

        ASSERT_EXPLAIN_V2_AUTO(
            "Root [{root}]\n"
            "RIDIntersect [root]\n"
            "|   Scan [c1, {root}]\n"
            "Sargable [Index]\n"
            "|   |   requirements: \n"
            "|   |       {{{root, 'PathGet [a] PathTraverse [1] PathIdentity []', {{{=Const "
            "[1]}}}}}}\n"
            "|   candidateIndexes: \n"
            "|       candidateId: 1, index1, {}, {SimpleEquality}, {{{=Const [1]}}}\n"
            "Scan [c1, {root}]\n",
            optimized);
    }

    {
        auto prefixId = PrefixId::createForTests();
        auto phaseManager = makePhaseManager(
            {OptPhase::MemoSubstitutionPhase,
             OptPhase::MemoExplorationPhase,
             OptPhase::MemoImplementationPhase},
            prefixId,
            {{{"c1",
               createScanDef({}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}})}}},
            /*costModel*/ boost::none,
            {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

        ABT optimized = rootNode;
        phaseManager.optimize(optimized);
        ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

        // Test sargable filter is satisfied with an index scan.
        ASSERT_EXPLAIN_V2_AUTO(
            "Root [{root}]\n"
            "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
            "|   |   Const [true]\n"
            "|   LimitSkip [limit: 1, skip: 0]\n"
            "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
            "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Const "
            "[1]}]\n",
            optimized);
    }

    {
        auto prefixId = PrefixId::createForTests();
        auto phaseManager = makePhaseManager(
            {OptPhase::MemoSubstitutionPhase,
             OptPhase::MemoExplorationPhase,
             OptPhase::MemoImplementationPhase},
            prefixId,
            {{{"c1", createScanDef({}, {})}}},
            /*costModel*/ boost::none,
            {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

        ABT optimized = rootNode;
        phaseManager.optimize(optimized);
        ASSERT_EQ(2, phaseManager.getMemo().getStats()._physPlanExplorationCount);

        // Test we can optimize sargable filter nodes even without an index.
        ASSERT_EXPLAIN_V2_AUTO(
            "Root [{root}]\n"
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

TEST(PhysRewriter, FilterIndexing1) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("root", "c1");

    // This node will not be converted to Sargable.
    ABT evalNode = make<EvaluationNode>(
        "p1",
        make<EvalPath>(
            make<PathGet>(
                "b",
                make<PathLambda>(make<LambdaAbstraction>(
                    "t",
                    make<BinaryOp>(Operations::Add, make<Variable>("t"), Constant::int64(1))))),
            make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(1)))),
                         make<Variable>("p1")),
        std::move(evalNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"p1"}}, std::move(filterNode));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}})}}},
        /*costModel*/ boost::none,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(7, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{p1}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [p1]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Evaluation [{p1}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathGet [b]\n"
        "|   PathLambda []\n"
        "|   LambdaAbstraction [t]\n"
        "|   BinaryOp [Add]\n"
        "|   |   Const [1]\n"
        "|   Variable [t]\n"
        "PhysicalScan [{'<root>': root}, c1]\n",
        optimized);
}

TEST(PhysRewriter, FilterIndexing2) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>(
                "a",
                make<PathTraverse>(
                    PathTraverse::kSingleLevel,
                    make<PathGet>("b",
                                  make<PathTraverse>(
                                      PathTraverse::kSingleLevel,
                                      make<PathCompare>(Operations::Eq, Constant::int64(1)))))),
            make<Variable>("root")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {{"index1",
                           {{{make<PathGet>("a", make<PathGet>("b", make<PathIdentity>())),
                              CollationOp::Ascending}},
                            false /*isMultiKey*/}}})}}},
        /*costModel*/ boost::none,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Const [1"
        "]}]\n",
        optimized);
}

TEST(PhysRewriter, FilterIndexing2NonSarg) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode1 = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode1 = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Eq, Constant::int64(1))),
                         make<Variable>("pa")),
        std::move(evalNode1));

    // Dependent eval node.
    ABT evalNode2 = make<EvaluationNode>(
        "pb",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("pa")),
        std::move(filterNode1));

    // Non-sargable filter.
    ABT filterNode2 = make<FilterNode>(
        make<EvalFilter>(
            make<PathTraverse>(
                PathTraverse::kSingleLevel,
                make<PathLambda>(make<LambdaAbstraction>(
                    "var", make<FunctionCall>("someFunction", makeSeq(make<Variable>("var")))))),
            make<Variable>("pb")),
        std::move(evalNode2));


    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode2));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}})}}},
        /*costModel*/ boost::none,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(10, 15, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate non-sargable evaluation and filter are moved under the NLJ+seek,
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pb]\n"
        "|   PathTraverse [1]\n"
        "|   PathLambda []\n"
        "|   LambdaAbstraction [var]\n"
        "|   FunctionCall [someFunction]\n"
        "|   Variable [var]\n"
        "Evaluation [{pb}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [pa]\n"
        "|   PathGet [b]\n"
        "|   PathIdentity []\n"
        "IndexScan [{'<indexKey> 0': pa, '<rid>': rid_0}, scanDefName: c1, indexDefName: index1, "
        "interval: {=Const [1]}]\n",
        optimized);

    LogicalRewriteType logicalRules[] = {LogicalRewriteType::Root,
                                         LogicalRewriteType::Root,
                                         LogicalRewriteType::SargableSplit,
                                         LogicalRewriteType::SargableSplit,
                                         LogicalRewriteType::EvaluationRIDIntersectReorder,
                                         LogicalRewriteType::Root,
                                         LogicalRewriteType::FilterRIDIntersectReorder,
                                         LogicalRewriteType::Root,
                                         LogicalRewriteType::SargableSplit,
                                         LogicalRewriteType::EvaluationRIDIntersectReorder,
                                         LogicalRewriteType::FilterRIDIntersectReorder};
    PhysicalRewriteType physicalRules[] = {PhysicalRewriteType::Seek,
                                           PhysicalRewriteType::Seek,
                                           PhysicalRewriteType::IndexFetch,
                                           PhysicalRewriteType::Evaluation,
                                           PhysicalRewriteType::IndexFetch,
                                           PhysicalRewriteType::Root,
                                           PhysicalRewriteType::SargableToIndex,
                                           PhysicalRewriteType::SargableToIndex,
                                           PhysicalRewriteType::Evaluation,
                                           PhysicalRewriteType::Evaluation,
                                           PhysicalRewriteType::Filter};

    int logicalRuleIndex = 0;
    int physicalRuleIndex = 0;
    const Memo& memo = phaseManager.getMemo();
    for (size_t groupId = 0; groupId < memo.getGroupCount(); groupId++) {
        for (const auto rule : memo.getRules(groupId)) {
            ASSERT(rule == logicalRules[logicalRuleIndex]);
            logicalRuleIndex++;
        }
        for (const auto& physOptResult : memo.getPhysicalNodes(groupId)) {
            const auto rule = physOptResult->_nodeInfo->_rule;
            ASSERT(rule == physicalRules[physicalRuleIndex]);
            physicalRuleIndex++;
        }
    }
}

TEST(PhysRewriter, FilterIndexing3) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Eq, Constant::int64(1))),
                         make<Variable>("pa")),
        std::move(evalNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pa"}}, std::move(filterNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("b"), CollationOp::Ascending}},
                                 false /*isMultiKey*/,
                                 {DistributionType::Centralized},
                                 {}}}})}}},
        /*costModel*/ boost::none,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // We dont need a Seek if we dont have multi-key paths.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pa}]\n"
        "IndexScan [{'<indexKey> 0': pa}, scanDefName: c1, indexDefName: index1, interval: {[Const "
        "[1 | minKey], Const [1 | maxKey]]}]\n",
        optimized);
}

TEST(PhysRewriter, FilterIndexing3MultiKey) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Eq, Constant::int64(1))),
                         make<Variable>("pa")),
        std::move(evalNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pa"}}, std::move(filterNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {{"index1",
                           IndexDefinition{{{makeIndexPath("a"), CollationOp::Ascending},
                                            {makeIndexPath("b"), CollationOp::Ascending}},
                                           true /*isMultiKey*/,
                                           {DistributionType::Centralized},
                                           {}}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(5, 8, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // We need a Seek to obtain value for "a".
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pa}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'a': pa}, c1]\n"
        "Unique [{rid_0}]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {[Const [1"
        " | minKey], Const [1 | maxKey]]}]\n",
        optimized);
}

TEST(PhysRewriter, FilterIndexing4) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    // Make sure the intervals do not contain Null.
    ABT evalNode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Gt, Constant::int64(1))),
                         make<Variable>("pa")),
        std::move(evalNode));

    ABT filterBNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("b",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Gt, Constant::int64(1)))),
                         make<Variable>("root")),
        std::move(filterANode));

    ABT filterCNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("c",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Gt, Constant::int64(1)))),
                         make<Variable>("root")),
        std::move(filterBNode));

    ABT filterDNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("d",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Gt, Constant::int64(1)))),
                         make<Variable>("root")),
        std::move(filterCNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pa"}}, std::move(filterDNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("b"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("c"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("d"), CollationOp::Ascending}},
                                 false /*isMultiKey*/,
                                 {DistributionType::Centralized},
                                 {}}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);

    // For now leave only GroupBy+Union RIDIntersect.
    phaseManager.getHints()._disableHashJoinRIDIntersect = true;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(20, 35, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Assert the correct CEs for each node in group 1. Group 1 contains residual predicates.
    std::vector<std::pair<std::string, double>> pathAndCEs = {
        {"Memo.1.physicalNodes.1.nodeInfo.node.ce", 143.6810174757394},
        {"Memo.1.physicalNodes.1.nodeInfo.node.child.ce", 189.57056733575502},
        {"Memo.1.physicalNodes.1.nodeInfo.node.child.child.ce", 330.00000000000006},
        {"Memo.1.physicalNodes.1.nodeInfo.node.child.child.child.ce", 330.00000000000006}};
    const BSONObj explain = ExplainGenerator::explainMemoBSONObj(phaseManager.getMemo());
    for (const auto& pathAndCE : pathAndCEs) {
        BSONElement el = dotted_path_support::extractElementAtPath(explain, pathAndCE.first);
        ASSERT_EQ(el.Double(), pathAndCE.second);
    }

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pa}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_14]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_13]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_12]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [1]\n"
        "IndexScan [{'<indexKey> 0': pa, '<indexKey> 1': evalTemp_12, '<indexKey> 2': evalTemp_13"
        ", '<indexKey> 3': evalTemp_14}, scanDefName: c1, indexDefName: index1, interval: {>Const"
        " [1 | maxKey | maxKey | maxKey]}]\n",
        optimized);
}

TEST(PhysRewriter, FilterIndexing5) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalANode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Gt, Constant::int64(0))),
                         make<Variable>("pa")),
        std::move(evalANode));

    ABT evalBNode = make<EvaluationNode>(
        "pb",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("root")),
        std::move(filterANode));

    ABT filterBNode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Gt, Constant::int64(0))),
                         make<Variable>("pb")),
        std::move(evalBNode));

    ABT collationNode = make<CollationNode>(CollationRequirement({{"pb", CollationOp::Ascending}}),
                                            std::move(filterBNode));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pa", "pb"}},
                                  std::move(collationNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("b"), CollationOp::Ascending}},
                                 false /*isMultiKey*/,
                                 {DistributionType::Centralized},
                                 {}}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(10, 25, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // We can cover both fields with the index, and need separate sort on "b".
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pa, pb}]\n"
        "Collation [{pb: Ascending}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pb]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [0]\n"
        "Evaluation [{pb} = Variable [evalTemp_0]]\n"
        "IndexScan [{'<indexKey> 0': pa, '<indexKey> 1': evalTemp_0}, scanDefName: c1, "
        "indexDefName: index1, interval: {>Const [0 | maxKey]}]\n",
        optimized);
}

TEST(PhysRewriter, FilterIndexing6) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalANode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Eq, Constant::int64(0))),
                         make<Variable>("pa")),
        std::move(evalANode));

    ABT evalBNode = make<EvaluationNode>(
        "pb",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("root")),
        std::move(filterANode));

    ABT filterBNode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Gt, Constant::int64(0))),
                         make<Variable>("pb")),
        std::move(evalBNode));

    ABT collationNode = make<CollationNode>(CollationRequirement({{"pb", CollationOp::Ascending}}),
                                            std::move(filterBNode));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pa", "pb"}},
                                  std::move(collationNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("b"), CollationOp::Ascending}},
                                 false /*isMultiKey*/,
                                 {DistributionType::Centralized},
                                 {}}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(5, 15, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // We can cover both fields with the index, and do not need a separate sort on "b".
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pa, pb}]\n"
        "IndexScan [{'<indexKey> 0': pa, '<indexKey> 1': pb}, scanDefName: c1, indexDefName: "
        "index1, interval: {(Const [0 | 0], Const [0 | maxKey]]}]\n",
        optimized);
}

TEST(PhysRewriter, FilterIndexingStress) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT result = make<ScanNode>("root", "c1");

    static constexpr size_t kFilterCount = 15;
    // A query with a large number of filters on different fields.
    for (size_t index = 0; index < kFilterCount; index++) {
        std::ostringstream os;
        os << "field" << index;

        result = make<FilterNode>(
            make<EvalFilter>(
                make<PathGet>(
                    FieldNameType{os.str()},
                    make<PathTraverse>(PathTraverse::kSingleLevel,
                                       make<PathCompare>(Operations::Eq, Constant::int64(0)))),
                make<Variable>("root")),
            std::move(result));
    }

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(result));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{makeNonMultikeyIndexPath("field0"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("field1"), CollationOp::Ascending}},
                                 false /*isMultiKey*/,
                                 {DistributionType::Centralized},
                                 {}}},
                {"index2",
                 IndexDefinition{{{makeNonMultikeyIndexPath("field2"), CollationOp::Ascending}},
                                 false /*isMultiKey*/,
                                 {DistributionType::Centralized},
                                 {}}},
                {"index3",
                 IndexDefinition{{{makeNonMultikeyIndexPath("field3"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("field4"), CollationOp::Ascending}},
                                 false /*isMultiKey*/,
                                 {DistributionType::Centralized},
                                 {}}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);

    // Without the changes to restrict SargableNode split to which this test is tied, we would
    // be exploring 2^kFilterCount plans, one for each created group.
    ASSERT_BETWEEN(60, 80, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    const BSONObj& explainRoot = ExplainGenerator::explainBSONObj(optimized);
    ASSERT_BSON_PATH("\"Filter\"", explainRoot, "child.nodeType");
    ASSERT_BSON_PATH("\"Filter\"", explainRoot, "child.child.nodeType");
    ASSERT_BSON_PATH("\"Filter\"", explainRoot, "child.child.child.nodeType");
    ASSERT_BSON_PATH("\"Filter\"", explainRoot, "child.child.child.child.nodeType");
    ASSERT_BSON_PATH("\"Filter\"", explainRoot, "child.child.child.child.child.nodeType");

    const BSONObj& explainNLJ = dotted_path_support::extractElementAtPath(
                                    explainRoot, "child.child.child.child.child.child")
                                    .Obj();
    ASSERT_BSON_PATH("\"NestedLoopJoin\"", explainNLJ, "nodeType");
    ASSERT_BSON_PATH("\"Filter\"", explainNLJ, "rightChild.nodeType");
    ASSERT_BSON_PATH("\"Filter\"", explainNLJ, "rightChild.child.nodeType");
    ASSERT_BSON_PATH("\"Filter\"", explainNLJ, "rightChild.child.child.nodeType");
    ASSERT_BSON_PATH("\"Filter\"", explainNLJ, "rightChild.child.child.child.nodeType");
    ASSERT_BSON_PATH("\"Filter\"", explainNLJ, "rightChild.child.child.child.child.nodeType");
    ASSERT_BSON_PATH("\"Filter\"", explainNLJ, "rightChild.child.child.child.child.child.nodeType");
    ASSERT_BSON_PATH(
        "\"LimitSkip\"", explainNLJ, "rightChild.child.child.child.child.child.child.nodeType");
    ASSERT_BSON_PATH(
        "\"Seek\"", explainNLJ, "rightChild.child.child.child.child.child.child.child.nodeType");

    ASSERT_BSON_PATH("\"MergeJoin\"", explainNLJ, "leftChild.nodeType");
    ASSERT_BSON_PATH("\"IndexScan\"", explainNLJ, "leftChild.leftChild.nodeType");
    ASSERT_BSON_PATH("\"index1\"", explainNLJ, "leftChild.leftChild.indexDefName");
    ASSERT_BSON_PATH("\"Union\"", explainNLJ, "leftChild.rightChild.nodeType");
    ASSERT_BSON_PATH("\"Evaluation\"", explainNLJ, "leftChild.rightChild.children.0.nodeType");
    ASSERT_BSON_PATH("\"IndexScan\"", explainNLJ, "leftChild.rightChild.children.0.child.nodeType");
    ASSERT_BSON_PATH(
        "\"index3\"", explainNLJ, "leftChild.rightChild.children.0.child.indexDefName");
}

TEST(PhysRewriter, FilterIndexingVariable) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    // In the absence of full implementation of query parameterization, here we pretend we have a
    // function "getQueryParam" which will return a query parameter by index.
    const auto getQueryParamFn = [](const size_t index) {
        return make<FunctionCall>("getQueryParam", makeSeq(Constant::int32(index)));
    };

    ABT scanNode = make<ScanNode>("root", "c1");

    // Encode a condition using two query parameters (expressed as functions):
    // "a" > param_0 AND "a" >= param_1 (observe param_1 comparison is inclusive).
    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>(
                "a",
                make<PathTraverse>(
                    PathTraverse::kSingleLevel,
                    make<PathComposeM>(make<PathCompare>(Operations::Gt, getQueryParamFn(0)),
                                       make<PathCompare>(Operations::Gte, getQueryParamFn(1))))),
            make<Variable>("root")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.getHints()._disableScan = true;
    phaseManager.optimize(optimized);
    ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Observe unioning of two index scans with complex expressions for bounds. This encodes:
    // (max(param_0, param_1), Const [maxKey]] U [param_0 > param_1 ? MaxKey : param_1, max(param_0,
    // param_1)]
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "GroupBy [{rid_0}]\n"
        "|   aggregations: \n"
        "Union [{rid_0}]\n"
        "|   IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {[If ["
        "] BinaryOp [And] BinaryOp [And] BinaryOp [Or] BinaryOp [Or] BinaryOp [And] BinaryOp [Lt]"
        " FunctionCall [getQueryParam] Const [1] FunctionCall [getQueryParam] Const [0] BinaryOp "
        "[Lt] FunctionCall [getQueryParam] Const [0] Const [maxKey] Const [true] BinaryOp [Or] Bi"
        "naryOp [And] BinaryOp [Lt] FunctionCall [getQueryParam] Const [0] FunctionCall [getQuery"
        "Param] Const [1] Const [true] BinaryOp [And] BinaryOp [Lt] FunctionCall [getQueryParam] "
        "Const [0] Const [maxKey] Const [true] BinaryOp [And] BinaryOp [Lt] FunctionCall [getQuer"
        "yParam] Const [0] Const [maxKey] Const [true] BinaryOp [Gt] FunctionCall [getQueryParam]"
        " Const [1] FunctionCall [getQueryParam] Const [0] FunctionCall [getQueryParam] Const [1]"
        " Const [maxKey], FunctionCall [getQueryParam] Const [1]]}]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {>If [] Bi"
        "naryOp [Gte] FunctionCall [getQueryParam] Const [0] FunctionCall [getQueryParam] Const ["
        "1] FunctionCall [getQueryParam] Const [0] FunctionCall [getQueryParam] Const [1]}]\n",
        optimized);
}

TEST(PhysRewriter, FilterIndexingMaxKey) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterNode1 = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>(
                "a",
                make<PathTraverse>(
                    PathTraverse::kSingleLevel,
                    make<PathComposeM>(make<PathCompare>(Operations::Gt, Constant::int64(1)),
                                       make<PathCompare>(Operations::Lte, Constant::maxKey())))),
            make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode2 = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>(
                "b",
                make<PathTraverse>(
                    PathTraverse::kSingleLevel,
                    make<PathComposeM>(make<PathCompare>(Operations::Gt, Constant::int64(2)),
                                       make<PathCompare>(Operations::Lt, Constant::maxKey())))),
            make<Variable>("root")),
        std::move(filterNode1));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode2));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);

    // Observe redundant predicate a <= MaxKey is removed.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_3]\n"
        "|   PathTraverse [1]\n"
        "|   PathComposeM []\n"
        "|   |   PathCompare [Lt]\n"
        "|   |   Const [maxKey]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [2]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_2]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_2, 'b': evalTemp_3}, c1]\n",
        optimized);
}

TEST(PhysRewriter, FilterIndexingRIN) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    // Construct a query which tests "a" > 1 and "c" > 2 and "e" = 3.
    ABT rootNode = NodeBuilder{}
                       .root("root")
                       .filter(_evalf(_get("a", _traverse1(_cmp("Gt", "1"_cint64))), "root"_var))
                       .filter(_evalf(_get("c", _traverse1(_cmp("Gt", "2"_cint64))), "root"_var))
                       .filter(_evalf(_get("e", _traverse1(_cmp("Eq", "3"_cint64))), "root"_var))
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
                 IndexDefinition{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("b"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("c"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("d"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("e"), CollationOp::Ascending}},
                                 false /*isMultiKey*/,
                                 {DistributionType::Centralized},
                                 {}}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);

    // We need to apply those hints in order to coax the RIN plan.
    phaseManager.getHints()._minIndexEqPrefixes = 3;
    phaseManager.getHints()._maxIndexEqPrefixes = 3;
    phaseManager.getHints()._disableScan = true;

    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(8, 12, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate RIN plan which consists of three equality prefixes.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "NestedLoopJoin [joinType: Inner, {evalTemp_57, evalTemp_58}]\n"
        "|   |   Const [true]\n"
        "|   NestedLoopJoin [joinType: Inner, {evalTemp_57, evalTemp_58, evalTemp_59, "
        "evalTemp_60}]\n"
        "|   |   |   Const [true]\n"
        "|   |   IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: "
        "{=Variable [evalTemp_57] | Variable [evalTemp_58] | Variable [evalTemp_59] | Variable "
        "[evalTemp_60] | Const [3]}]\n"
        "|   SpoolProducer [Lazy, id: 1, {evalTemp_59, evalTemp_60}]\n"
        "|   |   |   Const [true]\n"
        "|   Union [{evalTemp_59, evalTemp_60}]\n"
        "|   |   NestedLoopJoin [joinType: Inner, {rinInner_2, rinInner_3}]\n"
        "|   |   |   |   Const [true]\n"
        "|   |   |   LimitSkip [limit: 1, skip: 0]\n"
        "|   |   |   IndexScan [{'<indexKey> 2': evalTemp_59, '<indexKey> 3': evalTemp_60}, "
        "scanDefName: c1, indexDefName: index1, interval: {(Variable [evalTemp_57] | Variable "
        "[evalTemp_58] | Variable [rinInner_2] | Variable [rinInner_3] | Const [maxKey], Variable "
        "[evalTemp_57] | Variable [evalTemp_58] | Const [maxKey] | Const [maxKey] | Const "
        "[maxKey])}]\n"
        "|   |   SpoolConsumer [Stack, id: 1, {rinInner_2, rinInner_3}]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   IndexScan [{'<indexKey> 2': evalTemp_59, '<indexKey> 3': evalTemp_60}, scanDefName: "
        "c1, indexDefName: index1, interval: {(Variable [evalTemp_57] | Variable [evalTemp_58] | "
        "Const [2] | Const [maxKey] | Const [maxKey], Variable [evalTemp_57] | Variable "
        "[evalTemp_58] | Const [maxKey] | Const [maxKey] | Const [maxKey]]}]\n"
        "SpoolProducer [Lazy, id: 2, {evalTemp_57, evalTemp_58}]\n"
        "|   |   Const [true]\n"
        "Union [{evalTemp_57, evalTemp_58}]\n"
        "|   NestedLoopJoin [joinType: Inner, {rinInner_0, rinInner_1}]\n"
        "|   |   |   Const [true]\n"
        "|   |   LimitSkip [limit: 1, skip: 0]\n"
        "|   |   IndexScan [{'<indexKey> 0': evalTemp_57, '<indexKey> 1': evalTemp_58}, "
        "scanDefName: c1, indexDefName: index1, interval: {(Variable [rinInner_0] | Variable "
        "[rinInner_1] | Const [maxKey] | Const [maxKey] | Const [maxKey], Const [maxKey | maxKey | "
        "maxKey | maxKey | maxKey])}]\n"
        "|   SpoolConsumer [Stack, id: 2, {rinInner_0, rinInner_1}]\n"
        "LimitSkip [limit: 1, skip: 0]\n"
        "IndexScan [{'<indexKey> 0': evalTemp_57, '<indexKey> 1': evalTemp_58}, scanDefName: c1, "
        "indexDefName: index1, interval: {>Const [1 | maxKey | maxKey | maxKey | maxKey]}]\n",
        optimized);
}

TEST(PhysRewriter, FilterIndexingRIN1) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    // Construct a query which tests "a" > 1 and "b" > 2, and sorts descending on "a", then
    // ascending on "b".
    ABT rootNode = NodeBuilder{}
                       .root("root", "pa", "pb")
                       .collation({"pa:-1", "pb:1"})
                       .filter(_evalf(_traverse1(_cmp("Gt", "1"_cint64)), "pa"_var))
                       .filter(_evalf(_traverse1(_cmp("Gt", "2"_cint64)), "pb"_var))
                       .eval("pa", _evalp(_get("a", _id()), "root"_var))
                       .eval("pb", _evalp(_get("b", _id()), "root"_var))
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
                 IndexDefinition{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("b"), CollationOp::Ascending}},
                                 false /*isMultiKey*/,
                                 {DistributionType::Centralized},
                                 {}}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);

    // We need to apply those hints in order to coax out the RIN plan.
    phaseManager.getHints()._minIndexEqPrefixes = 2;
    phaseManager.getHints()._maxIndexEqPrefixes = 2;
    phaseManager.getHints()._disableScan = true;

    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(10, 15, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Observe how the index scan for the first equality prefix (on "a") is reversed while the
    // second one (on "a") is not.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pa, pb, root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "NestedLoopJoin [joinType: Inner, {pa}]\n"
        "|   |   Const [true]\n"
        "|   IndexScan [{'<indexKey> 1': pb, '<rid>': rid_0}, scanDefName: c1, indexDefName: "
        "index1, interval: {(Variable [pa] | Const [2], Variable [pa] | Const [maxKey]]}]\n"
        "SpoolProducer [Lazy, id: 2, {pa}]\n"
        "|   |   Const [true]\n"
        "Union [{pa}]\n"
        "|   NestedLoopJoin [joinType: Inner, {rinInner_1}]\n"
        "|   |   |   Const [true]\n"
        "|   |   LimitSkip [limit: 1, skip: 0]\n"
        "|   |   IndexScan [{'<indexKey> 0': pa}, scanDefName: c1, indexDefName: index1, interval: "
        "{(Const [1 | maxKey], Variable [rinInner_1] | Const [minKey])}, reversed]\n"
        "|   SpoolConsumer [Stack, id: 2, {rinInner_1}]\n"
        "LimitSkip [limit: 1, skip: 0]\n"
        "IndexScan [{'<indexKey> 0': pa}, scanDefName: c1, indexDefName: index1, interval: {>Const "
        "[1 | maxKey]}, reversed]\n",
        optimized);
}

TEST(PhysRewriter, FilterIndexingRIN2) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    // Construct a query which tests "a" in [1, 2] U [3, 4] and "b" in [5, 6] U [7, 8].
    ABT rootNode =
        NodeBuilder{}
            .root("root", "pa", "pb")
            .filter(_evalf(_composea(_composem(_cmp("Gte", "1"_cint64), _cmp("Lte", "2"_cint64)),
                                     _composem(_cmp("Gte", "3"_cint64), _cmp("Lte", "4"_cint64))),
                           "pa"_var))
            .filter(_evalf(_composea(_composem(_cmp("Gte", "5"_cint64), _cmp("Lte", "6"_cint64)),
                                     _composem(_cmp("Gte", "7"_cint64), _cmp("Lte", "8"_cint64))),
                           "pb"_var))
            .eval("pa", _evalp(_get("a", _id()), "root"_var))
            .eval("pb", _evalp(_get("b", _id()), "root"_var))
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
                 IndexDefinition{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("b"), CollationOp::Ascending}},
                                 false /*isMultiKey*/,
                                 {DistributionType::Centralized},
                                 {}}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);

    // We need to apply those hints in order to coax out the RIN plan.
    phaseManager.getHints()._minIndexEqPrefixes = 2;
    phaseManager.getHints()._maxIndexEqPrefixes = 2;
    phaseManager.getHints()._disableScan = true;

    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(5, 10, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pa, pb, root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "GroupBy [{rid_0}]\n"
        "|   aggregations: \n"
        "|       [pa]\n"
        "|           FunctionCall [$first]\n"
        "|           Variable [disjunction_0]\n"
        "|       [pb]\n"
        "|           FunctionCall [$first]\n"
        "|           Variable [disjunction_1]\n"
        "Union [{disjunction_0, disjunction_1, rid_0}]\n"
        "|   NestedLoopJoin [joinType: Inner, {disjunction_0}]\n"
        "|   |   |   Const [true]\n"
        "|   |   GroupBy [{rid_0}]\n"
        "|   |   |   aggregations: \n"
        "|   |   |       [disjunction_1]\n"
        "|   |   |           FunctionCall [$first]\n"
        "|   |   |           Variable [disjunction_3]\n"
        "|   |   Union [{disjunction_3, rid_0}]\n"
        "|   |   |   IndexScan [{'<indexKey> 1': disjunction_3, '<rid>': rid_0}, scanDefName: c1, "
        "indexDefName: index1, interval: {[Variable [disjunction_0] | Const [7], Variable "
        "[disjunction_0] | Const [8]]}]\n"
        "|   |   IndexScan [{'<indexKey> 1': disjunction_3, '<rid>': rid_0}, scanDefName: c1, "
        "indexDefName: index1, interval: {[Variable [disjunction_0] | Const [5], Variable "
        "[disjunction_0] | Const [6]]}]\n"
        "|   SpoolProducer [Lazy, id: 2, {disjunction_0}]\n"
        "|   |   |   Const [true]\n"
        "|   Union [{disjunction_0}]\n"
        "|   |   NestedLoopJoin [joinType: Inner, {rinInner_1}]\n"
        "|   |   |   |   Const [true]\n"
        "|   |   |   LimitSkip [limit: 1, skip: 0]\n"
        "|   |   |   IndexScan [{'<indexKey> 0': disjunction_0}, scanDefName: c1, indexDefName: "
        "index1, interval: {(Variable [rinInner_1] | Const [maxKey], Const [4 | maxKey])}]\n"
        "|   |   SpoolConsumer [Stack, id: 2, {rinInner_1}]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   IndexScan [{'<indexKey> 0': disjunction_0}, scanDefName: c1, indexDefName: index1, "
        "interval: {[Const [3 | minKey], Const [4 | maxKey]]}]\n"
        "NestedLoopJoin [joinType: Inner, {disjunction_0}]\n"
        "|   |   Const [true]\n"
        "|   GroupBy [{rid_0}]\n"
        "|   |   aggregations: \n"
        "|   |       [disjunction_1]\n"
        "|   |           FunctionCall [$first]\n"
        "|   |           Variable [disjunction_2]\n"
        "|   Union [{disjunction_2, rid_0}]\n"
        "|   |   IndexScan [{'<indexKey> 1': disjunction_2, '<rid>': rid_0}, scanDefName: c1, "
        "indexDefName: index1, interval: {[Variable [disjunction_0] | Const [7], Variable "
        "[disjunction_0] | Const [8]]}]\n"
        "|   IndexScan [{'<indexKey> 1': disjunction_2, '<rid>': rid_0}, scanDefName: c1, "
        "indexDefName: index1, interval: {[Variable [disjunction_0] | Const [5], Variable "
        "[disjunction_0] | Const [6]]}]\n"
        "SpoolProducer [Lazy, id: 1, {disjunction_0}]\n"
        "|   |   Const [true]\n"
        "Union [{disjunction_0}]\n"
        "|   NestedLoopJoin [joinType: Inner, {rinInner_0}]\n"
        "|   |   |   Const [true]\n"
        "|   |   LimitSkip [limit: 1, skip: 0]\n"
        "|   |   IndexScan [{'<indexKey> 0': disjunction_0}, scanDefName: c1, indexDefName: "
        "index1, interval: {(Variable [rinInner_0] | Const [maxKey], Const [2 | maxKey])}]\n"
        "|   SpoolConsumer [Stack, id: 1, {rinInner_0}]\n"
        "LimitSkip [limit: 1, skip: 0]\n"
        "IndexScan [{'<indexKey> 0': disjunction_0}, scanDefName: c1, indexDefName: index1, "
        "interval: {[Const [1 | minKey], Const [2 | maxKey]]}]\n",
        optimized);
}

TEST(PhysRewriter, SargableProjectionRenames) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode1 = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode1 =
        make<FilterNode>(make<EvalFilter>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                          make<Variable>("pa")),
                         std::move(evalNode1));

    ABT evalNode2 = make<EvaluationNode>(
        "pa1",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(filterNode1));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(evalNode2));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);

    // Demonstrate we can combine the field access to "a" into a single entry and provide two output
    // projections.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Evaluation [{pa1} = Variable [pa]]\n"
        "Sargable [Complete]\n"
        "|   |   requirements: \n"
        "|   |       {{{root, 'PathGet [a] PathIdentity []', pa, {{{=Const [1]}}}}}}\n"
        "|   scanParams: \n"
        "|       {'a': pa}\n"
        "|           residualReqs: \n"
        "|               {{{pa, 'PathIdentity []', {{{=Const [1]}}}, entryIndex: 0}}}\n"
        "Scan [c1, {root}]\n",
        optimized);
}

TEST(PhysRewriter, SargableAcquireProjection) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a", make<PathCompare>(Operations::Eq, Constant::int64(1))),
                         make<Variable>("root")),
        std::move(scanNode));

    ABT evalNode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(filterNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(evalNode));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);

    // Demonstrate that we combine the field access for the filter and eval nodes.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Sargable [Complete]\n"
        "|   |   requirements: \n"
        "|   |       {{{root, 'PathGet [a] PathIdentity []', pa, {{{=Const [1]}}}}}}\n"
        "|   scanParams: \n"
        "|       {'a': pa}\n"
        "|           residualReqs: \n"
        "|               {{{pa, 'PathIdentity []', {{{=Const [1]}}}, entryIndex: 0}}}\n"
        "Scan [c1, {root}]\n",
        optimized);
}

TEST(PhysRewriter, FilterReorder) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT result = make<ScanNode>("root", "c1");

    ce::PartialSchemaSelHints hints;
    static constexpr size_t kFilterCount = 5;
    for (size_t i = 0; i < kFilterCount; i++) {
        ProjectionName projName = prefixId.getNextId("field");
        hints.emplace(PartialSchemaKey{"root",
                                       make<PathGet>(FieldNameType{projName.value()},
                                                     make<PathTraverse>(PathTraverse::kSingleLevel,
                                                                        make<PathIdentity>()))},
                      kDefaultSelectivity * (kFilterCount - i));
        result = make<FilterNode>(
            make<EvalFilter>(
                make<PathGet>(
                    FieldNameType{projName.value()},
                    make<PathTraverse>(PathTraverse::kSingleLevel,
                                       make<PathCompare>(Operations::Eq, Constant::int64(i)))),
                make<Variable>("root")),
            std::move(result));
    }

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(result));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        makeHintedCE(std::move(hints)),
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_EQ(2, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Observe filters are ordered from most selective (lowest sel) to least selective (highest
    // sel).
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_14]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_15]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_16]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [2]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_17]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [3]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_18]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [4]\n"
        "PhysicalScan [{'<root>': root, 'field_0': evalTemp_14, 'field_1': evalTemp_15, 'field_2'"
        ": evalTemp_16, 'field_3': evalTemp_17, 'field_4': evalTemp_18}, c1]\n",
        optimized);
}

TEST(PhysRewriter, CoveredScan) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ce::PartialSchemaSelHints hints;
    hints.emplace(PartialSchemaKey{"root", make<PathGet>("a", make<PathIdentity>())},
                  SelectivityType{0.01});

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode =
        make<FilterNode>(make<EvalFilter>(make<PathCompare>(Operations::Lt, Constant::int32(1)),
                                          make<Variable>("pa")),
                         std::move(evalNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pa"}}, std::move(filterNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}})}}},
        makeHintedCE(std::move(hints)),
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_EQ(5, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Since we do not optimize with fast null handling, we need to split the predicate between the
    // index scan and fetch in order to handle null.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pa}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'a': pa}, c1]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {<Const [1"
        "]}]\n",
        optimized);
}

TEST(PhysRewriter, EvalIndexing) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode =
        make<FilterNode>(make<EvalFilter>(make<PathCompare>(Operations::Gt, Constant::int64(1)),
                                          make<Variable>("pa")),
                         std::move(evalNode));

    ABT collationNode = make<CollationNode>(CollationRequirement({{"pa", CollationOp::Ascending}}),
                                            std::move(filterNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pa"}}, std::move(collationNode));

    {
        auto phaseManager = makePhaseManager(
            {OptPhase::MemoSubstitutionPhase,
             OptPhase::MemoExplorationPhase,
             OptPhase::MemoImplementationPhase},
            prefixId,
            {{{"c1",
               createScanDef(
                   {},
                   {{"index1",
                     makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}})}}},
            boost::none /*costModel*/,
            {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

        ABT optimized = rootNode;
        phaseManager.optimize(optimized);
        ASSERT_BETWEEN(5, 10, phaseManager.getMemo().getStats()._physPlanExplorationCount);

        // Should not need a collation node.
        ASSERT_EXPLAIN_V2_AUTO(
            "Root [{pa}]\n"
            "IndexScan [{'<indexKey> 0': pa}, scanDefName: c1, indexDefName: index1, interval: "
            "{>Const [1]}]\n",
            optimized);
    }

    {
        // Index and collation node have incompatible ops.
        auto phaseManager = makePhaseManager(
            {OptPhase::MemoSubstitutionPhase,
             OptPhase::MemoExplorationPhase,
             OptPhase::MemoImplementationPhase},
            prefixId,
            {{{"c1",
               createScanDef(
                   {},
                   {{"index1",
                     makeIndexDefinition("a", CollationOp::Clustered, false /*isMultiKey*/)}})}}},
            boost::none /*costModel*/,
            {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

        ABT optimized = rootNode;
        phaseManager.optimize(optimized);
        ASSERT_EQ(10, phaseManager.getMemo().getStats()._physPlanExplorationCount);

        // Index does not have the right collation and now we need a collation node.
        ASSERT_EXPLAIN_V2_AUTO(
            "Root [{pa}]\n"
            "Collation [{pa: Ascending}]\n"
            "IndexScan [{'<indexKey> 0': pa}, scanDefName: c1, indexDefName: index1, interval: "
            "{>Const [1]}]\n",
            optimized);
    }
}

TEST(PhysRewriter, EvalIndexing1) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode =
        make<FilterNode>(make<EvalFilter>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                          make<Variable>("pa")),
                         std::move(evalNode));

    ABT collationNode = make<CollationNode>(CollationRequirement({{"pa", CollationOp::Ascending}}),
                                            std::move(filterNode));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}},
                                  std::move(collationNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(8, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Const "
        "[1]}]\n",
        optimized);
}

TEST(PhysRewriter, EvalIndexing2) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode1 = make<EvaluationNode>(
        "pa1",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT evalNode2 = make<EvaluationNode>(
        "pa2",
        make<EvalPath>(make<PathField>("a", make<PathConstant>(make<Variable>("pa1"))),
                       Constant::int32(0)),
        std::move(evalNode1));

    ABT evalNode3 = make<EvaluationNode>(
        "pa3",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("pa2")),
        std::move(evalNode2));

    ABT collationNode = make<CollationNode>(CollationRequirement({{"pa3", CollationOp::Ascending}}),
                                            std::move(evalNode3));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pa2"}},
                                  std::move(collationNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::ConstEvalPre,
         OptPhase::PathFuse,
         OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.getHints()._fastIndexNullHandling = true;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(8, 18, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Verify collation is subsumed into the index scan.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pa2}]\n"
        "Evaluation [{pa3} = Variable [pa1]]\n"
        "Evaluation [{pa2}]\n"
        "|   EvalPath []\n"
        "|   |   Const [0]\n"
        "|   PathField [a]\n"
        "|   PathConstant []\n"
        "|   Variable [pa1]\n"
        "IndexScan [{'<indexKey> 0': pa1}, scanDefName: c1, indexDefName: index1, interval: {<ful"
        "ly open>}]\n",
        optimized);
}

TEST(PhysRewriter, EvalIndexing3) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT rootNode = NodeBuilder{}
                       .root("pa")
                       .eval("pa", _evalp(_get("a", _traverseN(_get("b", _id()))), "root"_var))
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
                 makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.getHints()._fastIndexNullHandling = true;
    phaseManager.optimize(optimized);
    ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate the traverse is removed.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pa}]\n"
        "Evaluation [{pa}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [evalTemp_0]\n"
        "|   PathGet [b]\n"
        "|   PathIdentity []\n"
        "IndexScan [{'<indexKey> 0': evalTemp_0}, scanDefName: c1, indexDefName: index1, interval: "
        "{<fully open>}]\n",
        optimized);
}

TEST(PhysRewriter, MultiKeyIndex) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ce::PartialSchemaSelHints hints;
    hints.emplace(PartialSchemaKey{"root", make<PathGet>("a", make<PathIdentity>())},
                  kDefaultSelectivity);
    hints.emplace(PartialSchemaKey{"root", make<PathGet>("b", make<PathIdentity>())},
                  kDefaultSelectivity);

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalANode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterANode =
        make<FilterNode>(make<EvalFilter>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                          make<Variable>("pa")),
                         std::move(evalANode));

    ABT evalBNode = make<EvaluationNode>(
        "pb",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("root")),
        std::move(filterANode));

    ABT filterBNode =
        make<FilterNode>(make<EvalFilter>(make<PathCompare>(Operations::Gt, Constant::int64(2)),
                                          make<Variable>("pb")),
                         std::move(evalBNode));

    ABT collationNode = make<CollationNode>(
        CollationRequirement({{"pa", CollationOp::Ascending}, {"pb", CollationOp::Ascending}}),
        std::move(filterBNode));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}},
                                  std::move(collationNode));

    // TODO SERVER-71551 Follow up unit tests with overriden Cost Model.
    auto costModel = getTestCostModel();
    costModel.setEvalStartupCost(1e-6);
    costModel.setGroupByIncrementalCost(1e-4);

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)},
                {"index2",
                 makeIndexDefinition("b", CollationOp::Descending, false /*isMultiKey*/)}})}}},
        makeHintedCE(std::move(hints)),
        costModel,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    {
        ABT optimized = rootNode;

        // Test RIDIntersect using only Group+Union.
        phaseManager.getHints()._disableHashJoinRIDIntersect = true;
        phaseManager.optimize(optimized);
        ASSERT_BETWEEN(13, 25, phaseManager.getMemo().getStats()._physPlanExplorationCount);

        // GroupBy+Union cannot propagate collation requirement, and we need a separate
        // CollationNode.

        ASSERT_EXPLAIN_V2_AUTO(
            "Root [{root}]\n"
            "Collation [{pa: Ascending, pb: Ascending}]\n"
            "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
            "|   |   Const [true]\n"
            "|   LimitSkip [limit: 1, skip: 0]\n"
            "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
            "Filter []\n"
            "|   EvalFilter []\n"
            "|   |   FunctionCall [getArraySize]\n"
            "|   |   Variable [sides_0]\n"
            "|   PathCompare [Eq]\n"
            "|   Const [2]\n"
            "GroupBy [{rid_0}]\n"
            "|   aggregations: \n"
            "|       [pa]\n"
            "|           FunctionCall [$max]\n"
            "|           Variable [unionTemp_0]\n"
            "|       [pb]\n"
            "|           FunctionCall [$max]\n"
            "|           Variable [unionTemp_1]\n"
            "|       [sides_0]\n"
            "|           FunctionCall [$addToSet]\n"
            "|           Variable [sideId_0]\n"
            "Union [{rid_0, sideId_0, unionTemp_0, unionTemp_1}]\n"
            "|   Evaluation [{unionTemp_1} = Variable [pb]]\n"
            "|   Evaluation [{unionTemp_0} = Const [Nothing]]\n"
            "|   Evaluation [{sideId_0} = Const [1]]\n"
            "|   IndexScan [{'<indexKey> 0': pb, '<rid>': rid_0}, scanDefName: c1, indexDefName: "
            "index2, interval: {[Const [maxKey], Const [2])}]\n"
            "Evaluation [{unionTemp_1} = Const [Nothing]]\n"
            "Evaluation [{unionTemp_0} = Variable [pa]]\n"
            "Evaluation [{sideId_0} = Const [0]]\n"
            "IndexScan [{'<indexKey> 0': pa, '<rid>': rid_0}, scanDefName: c1, indexDefName: "
            "index1, "
            "interval: {=Const [1]}]\n",
            optimized);
    }

    {
        ABT optimized = rootNode;

        phaseManager.getHints()._disableGroupByAndUnionRIDIntersect = false;
        phaseManager.getHints()._disableHashJoinRIDIntersect = false;
        phaseManager.optimize(optimized);
        ASSERT_BETWEEN(15, 25, phaseManager.getMemo().getStats()._physPlanExplorationCount);

        // Index2 will be used in reverse direction.
        ASSERT_EXPLAIN_V2_AUTO(
            "Root [{root}]\n"
            "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
            "|   |   Const [true]\n"
            "|   LimitSkip [limit: 1, skip: 0]\n"
            "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
            "HashJoin [joinType: Inner]\n"
            "|   |   Condition\n"
            "|   |       rid_0 = rid_2\n"
            "|   Union [{rid_2}]\n"
            "|   Evaluation [{rid_2} = Variable [rid_0]]\n"
            "|   IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index2, interval: "
            "{[Const [maxKey], Const [2])}, reversed]\n"
            "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Const "
            "[1]}]\n",
            optimized);
    }
}

TEST(PhysRewriter, CompoundIndex1) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalANode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Eq, Constant::int64(1))),
                         make<Variable>("pa")),
        std::move(evalANode));

    ABT evalBNode = make<EvaluationNode>(
        "pb",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("root")),
        std::move(filterANode));

    ABT filterBNode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Eq, Constant::int64(2))),
                         make<Variable>("pb")),
        std::move(evalBNode));

    ABT filterCNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("c",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(3)))),
                         make<Variable>("root")),
        std::move(filterBNode));

    ABT filterDNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("d",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(4)))),
                         make<Variable>("root")),
        std::move(filterCNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterDNode));

    // TODO SERVER-71551 Follow up unit tests with overriden Cost Model.
    auto costModel = getTestCostModel();
    costModel.setIndexScanStartupCost(1e-6);

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("c"), CollationOp::Descending}},
                                 false /*isMultiKey*/}},
                {"index2",
                 IndexDefinition{{{makeNonMultikeyIndexPath("b"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("d"), CollationOp::Ascending}},
                                 false /*isMultiKey*/}}})}}},
        std::move(costModel),
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(35, 50, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    const BSONObj& explainRoot = ExplainGenerator::explainBSONObj(optimized);
    ASSERT_BSON_PATH("\"NestedLoopJoin\"", explainRoot, "child.nodeType");
    ASSERT_BSON_PATH("\"Seek\"", explainRoot, "child.rightChild.child.nodeType");
    ASSERT_BSON_PATH("\"MergeJoin\"", explainRoot, "child.leftChild.nodeType");
    ASSERT_BSON_PATH("\"IndexScan\"", explainRoot, "child.leftChild.leftChild.nodeType");
    ASSERT_BSON_PATH("\"index1\"", explainRoot, "child.leftChild.leftChild.indexDefName");
    ASSERT_BSON_PATH(
        "\"IndexScan\"", explainRoot, "child.leftChild.rightChild.children.0.child.nodeType");
    ASSERT_BSON_PATH(
        "\"index2\"", explainRoot, "child.leftChild.rightChild.children.0.child.indexDefName");
}

TEST(PhysRewriter, CompoundIndex2) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalANode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Eq, Constant::int64(1))),
                         make<Variable>("pa")),
        std::move(evalANode));

    ABT evalBNode = make<EvaluationNode>(
        "pb",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("root")),
        std::move(filterANode));

    ABT filterBNode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Eq, Constant::int64(2))),
                         make<Variable>("pb")),
        std::move(evalBNode));

    ABT filterCNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("c",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(3)))),
                         make<Variable>("root")),
        std::move(filterBNode));

    ABT filterDNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("d",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(4)))),
                         make<Variable>("root")),
        std::move(filterCNode));

    ABT collationNode = make<CollationNode>(
        CollationRequirement({{"pa", CollationOp::Ascending}, {"pb", CollationOp::Ascending}}),
        std::move(filterDNode));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}},
                                  std::move(collationNode));

    // TODO SERVER-71551 Follow up unit tests with overriden Cost Model.
    auto costModel = getTestCostModel();
    costModel.setIndexScanStartupCost(1e-6);

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {
                   {"index1",
                    IndexDefinition{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                     {makeNonMultikeyIndexPath("c"), CollationOp::Descending}},
                                    false /*isMultiKey*/}},
                   {"index2",
                    IndexDefinition{{{makeNonMultikeyIndexPath("b"), CollationOp::Ascending},
                                     {makeNonMultikeyIndexPath("d"), CollationOp::Ascending}},
                                    false /*isMultiKey*/}},
               })}}},
        std::move(costModel),
        {true /*debugMode*/, 3 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(50, 70, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    const BSONObj& explainRoot = ExplainGenerator::explainBSONObj(optimized);
    ASSERT_BSON_PATH("\"NestedLoopJoin\"", explainRoot, "child.nodeType");
    ASSERT_BSON_PATH("\"Seek\"", explainRoot, "child.rightChild.child.nodeType");
    ASSERT_BSON_PATH("\"MergeJoin\"", explainRoot, "child.leftChild.nodeType");
    ASSERT_BSON_PATH("\"IndexScan\"", explainRoot, "child.leftChild.leftChild.nodeType");
    ASSERT_BSON_PATH("\"index1\"", explainRoot, "child.leftChild.leftChild.indexDefName");
    ASSERT_BSON_PATH(
        "\"IndexScan\"", explainRoot, "child.leftChild.rightChild.children.0.child.nodeType");
    ASSERT_BSON_PATH(
        "\"index2\"", explainRoot, "child.leftChild.rightChild.children.0.child.indexDefName");
}

TEST(PhysRewriter, CompoundIndex3) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalANode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Eq, Constant::int64(1))),
                         make<Variable>("pa")),
        std::move(evalANode));

    ABT evalBNode = make<EvaluationNode>(
        "pb",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("root")),
        std::move(filterANode));

    ABT filterBNode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Eq, Constant::int64(2))),
                         make<Variable>("pb")),
        std::move(evalBNode));

    ABT filterCNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("c",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(3)))),
                         make<Variable>("root")),
        std::move(filterBNode));

    ABT filterDNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("d",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(4)))),
                         make<Variable>("root")),
        std::move(filterCNode));

    ABT collationNode = make<CollationNode>(
        CollationRequirement({{"pa", CollationOp::Ascending}, {"pb", CollationOp::Ascending}}),
        std::move(filterDNode));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}},
                                  std::move(collationNode));

    // TODO SERVER-71551 Follow up unit tests with overriden Cost Model.
    auto costModel = getTestCostModel();
    costModel.setIndexScanStartupCost(1e-6);

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {{"index1",
                           IndexDefinition{{{makeIndexPath("a"), CollationOp::Ascending},
                                            {makeIndexPath("c"), CollationOp::Descending}},
                                           true /*isMultiKey*/}},
                          {"index2",
                           IndexDefinition{{{makeIndexPath("b"), CollationOp::Ascending},
                                            {makeIndexPath("d"), CollationOp::Ascending}},
                                           true /*isMultiKey*/}}})}}},
        std::move(costModel),
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(50, 70, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate we have a merge join because we have point predicates.
    const BSONObj& explainRoot = ExplainGenerator::explainBSONObj(optimized);
    ASSERT_BSON_PATH("\"Collation\"", explainRoot, "child.nodeType");
    ASSERT_BSON_PATH("\"pa\"", explainRoot, "child.collation.0.projectionName");
    ASSERT_BSON_PATH("\"pb\"", explainRoot, "child.collation.1.projectionName");
    ASSERT_BSON_PATH("\"NestedLoopJoin\"", explainRoot, "child.child.nodeType");
    ASSERT_BSON_PATH("\"Seek\"", explainRoot, "child.child.rightChild.child.nodeType");
    ASSERT_BSON_PATH("\"MergeJoin\"", explainRoot, "child.child.leftChild.nodeType");

    const BSONObj& explainIndex1 =
        dotted_path_support::extractElementAtPath(
            explainRoot, "child.child.leftChild.rightChild.children.0.child")
            .Obj();
    ASSERT_BSON_PATH("\"IndexScan\"", explainIndex1, "nodeType");
    ASSERT_BSON_PATH("2", explainIndex1, "interval.lowBound.bound.0.value");
    ASSERT_BSON_PATH("2", explainIndex1, "interval.highBound.bound.0.value");
    ASSERT_BSON_PATH("4", explainIndex1, "interval.lowBound.bound.1.value");
    ASSERT_BSON_PATH("4", explainIndex1, "interval.highBound.bound.1.value");

    const BSONObj& explainIndex2 =
        dotted_path_support::extractElementAtPath(explainRoot, "child.child.leftChild.leftChild")
            .Obj();
    ASSERT_BSON_PATH("\"IndexScan\"", explainIndex2, "nodeType");
    ASSERT_BSON_PATH("1", explainIndex2, "interval.lowBound.bound.0.value");
    ASSERT_BSON_PATH("1", explainIndex2, "interval.highBound.bound.0.value");
    ASSERT_BSON_PATH("3", explainIndex2, "interval.lowBound.bound.1.value");
    ASSERT_BSON_PATH("3", explainIndex2, "interval.highBound.bound.1.value");
}

TEST(PhysRewriter, CompoundIndex4Negative) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ce::PartialSchemaSelHints hints;
    hints.emplace(PartialSchemaKey{"root", make<PathGet>("a", make<PathIdentity>())},
                  SelectivityType{0.05});
    hints.emplace(PartialSchemaKey{"root", make<PathGet>("b", make<PathIdentity>())},
                  SelectivityType{0.1});

    ABT scanNode = make<ScanNode>("root", "c1");

    // Create the following expression: {$and: [{a: {$eq: 1}}, {b: {$eq: 2}}]}
    ABT evalANode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Eq, Constant::int64(1))),
                         make<Variable>("pa")),
        std::move(evalANode));

    ABT evalBNode = make<EvaluationNode>(
        "pb",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("root")),
        std::move(filterANode));

    ABT filterBNode = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Eq, Constant::int64(2))),
                         make<Variable>("pb")),
        std::move(evalBNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterBNode));

    // Create the following indexes: {a:1, c:1, {name: 'index1'}}, and {b:1, d:1, {name: 'index2'}}
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("c"), CollationOp::Descending}},
                                 false /*isMultiKey*/}},
                {"index2",
                 IndexDefinition{{{makeNonMultikeyIndexPath("b"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("d"), CollationOp::Ascending}},
                                 false /*isMultiKey*/}}})}}},
        makeHintedCE(std::move(hints)),
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(5, 10, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Index intersection via merge join relies on the fact that the RIDs of equal keys are sorted.
    // Demonstrate that we do not get a merge join when the lookup keys on both intersected indexes
    // do not cover all field indexes. In this case there is no guarantee that the RIDs of all
    // matching keys will be sorted, and therefore they cannot be merge-joined.
    // Also demonstrate we pick index1 with the more selective predicate.

    const BSONObj& explainRoot = ExplainGenerator::explainBSONObj(optimized);
    ASSERT_BSON_PATH("\"NestedLoopJoin\"", explainRoot, "child.nodeType");
    ASSERT_BSON_PATH("\"Filter\"", explainRoot, "child.rightChild.nodeType");
    ASSERT_BSON_PATH("2", explainRoot, "child.rightChild.filter.path.value.value");
    ASSERT_BSON_PATH("\"Seek\"", explainRoot, "child.rightChild.child.child.nodeType");

    ASSERT_BSON_PATH("\"IndexScan\"", explainRoot, "child.leftChild.nodeType");
    ASSERT_BSON_PATH("1", explainRoot, "child.leftChild.interval.lowBound.bound.0.value");
    ASSERT_BSON_PATH("1", explainRoot, "child.leftChild.interval.highBound.bound.0.value");
}

TEST(PhysRewriter, CompoundIndex5) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    // Test the following scenario: (a = 0 or a = 1) and (b = 2 or b = 3) over a compound index on
    // {a, b}.
    ABT rootNode =
        NodeBuilder{}
            .root("root")
            .filter(_evalf(_traverse1(_composea(_cmp("Eq", "0"_cint64), _cmp("Eq", "1"_cint64))),
                           "pa"_var))
            .eval("pa", _evalp(_get("a", _id()), "root"_var))
            .filter(_evalf(_traverse1(_composea(_cmp("Eq", "2"_cint64), _cmp("Eq", "3"_cint64))),
                           "pb"_var))
            .eval("pb", _evalp(_get("b", _id()), "root"_var))
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
                 IndexDefinition{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("b"), CollationOp::Ascending}},
                                 false /*isMultiKey*/}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(7, 12, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate that we create four compound {a, b} index bounds: [=0, =2], [=0, =3], [=1, =2]
    // and [=1, =3].
    // TODO: SERVER-70298: we should be seeing merge joins instead of union+groupby.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "GroupBy [{rid_0}]\n"
        "|   aggregations: \n"
        "Union [{rid_0}]\n"
        "|   |   |   IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval"
        ": {=Const [1 | 3]}]\n"
        "|   |   IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {="
        "Const [0 | 3]}]\n"
        "|   IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Cons"
        "t [1 | 2]}]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Const [0"
        " | 2]}]\n",
        optimized);
}

TEST(PhysRewriter, IndexBoundsIntersect) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterNode1 = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("b",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(1)))),
                         make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode2 = make<FilterNode>(
        make<EvalFilter>(
            make<PathComposeM>(
                make<PathGet>(
                    "a",
                    make<PathTraverse>(PathTraverse::kSingleLevel,
                                       make<PathCompare>(Operations::Gt, Constant::int64(70)))),
                make<PathGet>(
                    "a",
                    make<PathTraverse>(PathTraverse::kSingleLevel,
                                       make<PathCompare>(Operations::Lt, Constant::int64(90))))),
            make<Variable>("root")),
        std::move(filterNode1));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode2));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {{"index1",
                           IndexDefinition{{{makeIndexPath("b"), CollationOp::Ascending},
                                            {makeIndexPath("a"), CollationOp::Ascending}},
                                           true /*isMultiKey*/}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(10, 30, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate that the predicates >70 and <90 are NOT combined into the same interval (70, 90)
    // since the paths are multiKey. With the heuristic estimate we may get either interval in the
    // index, and the other one as a residual filter.

    const BSONObj& explainRoot = ExplainGenerator::explainBSONObj(optimized);
    ASSERT_BSON_PATH("\"NestedLoopJoin\"", explainRoot, "child.nodeType");

    ASSERT_BSON_PATH("\"Filter\"", explainRoot, "child.rightChild.nodeType");
    const std::string filterVal = dotted_path_support::extractElementAtPath(
                                      explainRoot, "child.rightChild.filter.path.input.value.value")
                                      .toString(false /*includeFieldName*/);

    ASSERT_BSON_PATH("\"Seek\"", explainRoot, "child.rightChild.child.child.nodeType");
    ASSERT_BSON_PATH("\"Unique\"", explainRoot, "child.leftChild.nodeType");

    const BSONObj& explainIndexScan =
        dotted_path_support::extractElementAtPath(explainRoot, "child.leftChild.child").Obj();
    ASSERT_BSON_PATH("\"IndexScan\"", explainIndexScan, "nodeType");
    ASSERT_BSON_PATH("1", explainIndexScan, "interval.lowBound.bound.0.value");
    ASSERT_BSON_PATH("1", explainIndexScan, "interval.highBound.bound.0.value");

    const std::string lowBound = dotted_path_support::extractElementAtPath(
                                     explainIndexScan, "interval.lowBound.bound.1.value")
                                     .toString(false /*includeFieldName*/);
    const std::string highBound = dotted_path_support::extractElementAtPath(
                                      explainIndexScan, "interval.highBound.bound.1.value")
                                      .toString(false /*includeFieldName*/);
    ASSERT_TRUE((filterVal == "70" && lowBound == "MinKey" && highBound == "90") ||
                (filterVal == "90" && lowBound == "70" && highBound == "MaxKey"));
}

TEST(PhysRewriter, IndexBoundsIntersect1) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathComposeM>(
                make<PathTraverse>(PathTraverse::kSingleLevel,
                                   make<PathCompare>(Operations::Gt, Constant::int64(70))),
                make<PathTraverse>(PathTraverse::kSingleLevel,
                                   make<PathCompare>(Operations::Lt, Constant::int64(90)))),
            make<Variable>("pa")),
        std::move(evalNode));

    ABT collationNode = make<CollationNode>(CollationRequirement({{"pa", CollationOp::Ascending}}),
                                            std::move(filterNode));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}},
                                  std::move(collationNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending}},
                                 false /*isMultiKey*/}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(5, 10, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate we can intersect the intervals since we have non-multikey paths, and the
    // collation requirement is satisfied via the index scan.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {(Const "
        "[70], Const [90])}]\n",
        optimized);
}

TEST(PhysRewriter, IndexBoundsIntersect2) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathTraverse>(
                PathTraverse::kSingleLevel,
                make<PathComposeM>(make<PathCompare>(Operations::Gt, Constant::int64(70)),
                                   make<PathCompare>(Operations::Lt, Constant::int64(90)))),
            make<Variable>("pa")),
        std::move(evalNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode));

    // TODO SERVER-71551 Follow up unit tests with overriden Cost Model.
    auto costModel = getTestCostModel();
    costModel.setSeekStartupCost(1e-6);
    costModel.setIndexScanStartupCost(1e-6);

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {{"index1",
                           IndexDefinition{{{makeIndexPath("a"), CollationOp::Ascending}},
                                           true /*isMultiKey*/}}})}}},
        std::move(costModel),
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate we can intersect the bounds here because composition does not contain
    // traverse.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "Unique [{rid_0}]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {(Const "
        "[70], Const [90])}]\n",
        optimized);
}

TEST(PhysRewriter, IndexBoundsIntersect3) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>(
                "a",
                make<PathTraverse>(
                    PathTraverse::kSingleLevel,
                    make<PathComposeM>(
                        make<PathGet>("b",
                                      make<PathTraverse>(
                                          PathTraverse::kSingleLevel,
                                          make<PathCompare>(Operations::Gt, Constant::int64(70)))),
                        make<PathGet>(
                            "c",
                            make<PathTraverse>(
                                PathTraverse::kSingleLevel,
                                make<PathCompare>(Operations::Lt, Constant::int64(90))))))),
            make<Variable>("root")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{makeIndexPath(FieldPathType{"a", "b"}, true /*isMultiKey*/),
                                   CollationOp::Ascending}},
                                 true /*isMultiKey*/}},
                {"index2",
                 IndexDefinition{{{makeIndexPath(FieldPathType{"a", "c"}, true /*isMultiKey*/),
                                   CollationOp::Ascending}},
                                 true /*isMultiKey*/}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(10, 15, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // We do not intersect the bounds, because the outer composition is over the different fields.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [root]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathComposeM []\n"
        "|   |   PathGet [c]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Lt]\n"
        "|   |   Const [90]\n"
        "|   PathGet [b]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [70]\n"
        "PhysicalScan [{'<root>': root}, c1]\n",
        optimized);
}

TEST(PhysRewriter, IndexBoundsIntersect4) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT rootNode = NodeBuilder{}
                       .root("root")
                       .filter(_evalf(_composem(_get("a", _traverse1(_cmp("Gt", "70"_cint64))),
                                                _get("a", _traverse1(_cmp("Lt", "90"_cint64)))),
                                      "root"_var))
                       .finish(_scan("root", "c1"));

    const auto makePhaseManagerFn = [&prefixId]() {
        // Make filter and scan costs zero. This should make residual predicates very appealing.
        auto costModel = getTestCostModel();
        costModel.setFilterIncrementalCost(0.0);
        costModel.setScanIncrementalCost(0.0);

        return makePhaseManager(
            {OptPhase::MemoSubstitutionPhase,
             OptPhase::MemoExplorationPhase,
             OptPhase::MemoImplementationPhase},
            prefixId,
            {{{"c1",
               createScanDef({},
                             {{"index1",
                               IndexDefinition{{{makeIndexPath("a"), CollationOp::Ascending}},
                                               true /*isMultiKey*/}}})}}},
            std::move(costModel),
            {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});
    };

    ABT optimized = rootNode;
    auto phaseManager = makePhaseManagerFn();
    phaseManager.getHints()._forceIndexScanForPredicates = true;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN_AUTO(  // NOLINT (test auto-update)
        6,
        10,
        phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate that we get an index intersection plan even though from costing perspective a
    // collection scan with residual predicates should have been preferable.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT (test auto-update)
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "HashJoin [joinType: Inner]\n"
        "|   |   Condition\n"
        "|   |       rid_0 = rid_1\n"
        "|   Union [{rid_1}]\n"
        "|   Evaluation [{rid_1} = Variable [rid_0]]\n"
        "|   Unique [{rid_0}]\n"
        "|   IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {>Const "
        "[70]}]\n"
        "Unique [{rid_0}]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {<Const "
        "[90]}]\n",
        optimized);

    ABT optimized1 = rootNode;
    auto phaseManager1 = makePhaseManagerFn();
    phaseManager1.optimize(optimized1);
    ASSERT_BETWEEN_AUTO(  // NOLINT (test auto-update)
        1,
        2,
        phaseManager1.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate that without the hint we get residual predicates.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT (test auto-update)
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_6]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [70]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_6]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Lt]\n"
        "|   Const [90]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_6}, c1]\n",
        optimized1);
}

TEST(PhysRewriter, IndexResidualReq) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalANode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterANode =
        make<FilterNode>(make<EvalFilter>(make<PathCompare>(Operations::Gt, Constant::int64(0)),
                                          make<Variable>("pa")),
                         std::move(evalANode));

    ABT filterBNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>(
                "b", make<PathGet>("c", make<PathCompare>(Operations::Gt, Constant::int64(0)))),
            make<Variable>("root")),
        std::move(filterANode));

    ABT collationNode = make<CollationNode>(CollationRequirement({{"pa", CollationOp::Ascending}}),
                                            std::move(filterBNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pa"}}, std::move(collationNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("b"), CollationOp::Ascending}},
                                 false /*isMultiKey*/}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(5, 15, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Make sure we can use the index to cover "b" while testing "b.c" with a separate filter.
    ASSERT_EXPLAIN_PROPS_V2_AUTO(
        "Properties [cost: 0.176361, localCost: 0, adjustedCE: 189.571]\n"
        "|   |   Logical:\n"
        "|   |       cardinalityEstimate: \n"
        "|   |           ce: 189.571\n"
        "|   |       projections: \n"
        "|   |           pa\n"
        "|   |           root\n"
        "|   |       indexingAvailability: \n"
        "|   |           [groupId: 0, scanProjection: root, scanDefName: c1, hasProperInterval]\n"
        "|   |       collectionAvailability: \n"
        "|   |           c1\n"
        "|   |       distributionAvailability: \n"
        "|   |           distribution: \n"
        "|   |               type: Centralized\n"
        "|   Physical:\n"
        "|       distribution: \n"
        "|           type: Centralized\n"
        "|       indexingRequirement: \n"
        "|           Complete, dedupRID\n"
        "Root [{pa}]\n"
        "Properties [cost: 0.176361, localCost: 0.176361, adjustedCE: 330]\n"
        "|   |   Logical:\n"
        "|   |       cardinalityEstimate: \n"
        "|   |           ce: 189.571\n"
        "|   |           requirementCEs: \n"
        "|   |               refProjection: root, path: 'PathGet [a] PathIdentity []', ce: 330\n"
        "|   |               refProjection: root, path: 'PathGet [b] PathGet [c] PathIdentity []', "
        "ce: 330\n"
        "|   |       projections: \n"
        "|   |           pa\n"
        "|   |           root\n"
        "|   |       indexingAvailability: \n"
        "|   |           [groupId: 0, scanProjection: root, scanDefName: c1, hasProperInterval]\n"
        "|   |       collectionAvailability: \n"
        "|   |           c1\n"
        "|   |       distributionAvailability: \n"
        "|   |           distribution: \n"
        "|   |               type: Centralized\n"
        "|   Physical:\n"
        "|       collation: \n"
        "|           pa: Ascending\n"
        "|       projections: \n"
        "|           pa\n"
        "|       distribution: \n"
        "|           type: Centralized\n"
        "|       indexingRequirement: \n"
        "|           Index, dedupRID\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_2]\n"
        "|   PathGet [c]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [0]\n"
        "IndexScan [{'<indexKey> 0': pa, '<indexKey> 1': evalTemp_2}, scanDefName: c1, indexDefNa"
        "me: index1, interval: {>Const [0 | maxKey]}]\n",
        phaseManager);
}

TEST(PhysRewriter, IndexResidualReq1) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a", make<PathCompare>(Operations::Eq, Constant::int64(0))),
                         make<Variable>("root")),
        std::move(scanNode));

    ABT filterBNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("b", make<PathCompare>(Operations::Eq, Constant::int64(0))),
                         make<Variable>("root")),
        std::move(filterANode));

    ABT filterCNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("c", make<PathCompare>(Operations::Eq, Constant::int64(0))),
                         make<Variable>("root")),
        std::move(filterBNode));

    ABT evalDNode = make<EvaluationNode>(
        "pd",
        make<EvalPath>(make<PathGet>("d", make<PathIdentity>()), make<Variable>("root")),
        std::move(filterCNode));

    ABT collationNode = make<CollationNode>(CollationRequirement({{"pd", CollationOp::Ascending}}),
                                            std::move(evalDNode));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}},
                                  std::move(collationNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 makeCompositeIndexDefinition({{"a", CollationOp::Ascending, false /*isMultiKey*/},
                                               {"b", CollationOp::Ascending, false /*isMultiKey*/},
                                               {"c", CollationOp::Ascending, false /*isMultiKey*/},
                                               {"d", CollationOp::Ascending, false /*isMultiKey*/}},
                                              false /*isMultiKey*/)},
                {"index2",
                 makeCompositeIndexDefinition({{"a", CollationOp::Ascending, false /*isMultiKey*/},
                                               {"b", CollationOp::Ascending, false /*isMultiKey*/},
                                               {"d", CollationOp::Ascending, false /*isMultiKey*/}},
                                              false /*isMultiKey*/)},
                {"index3",
                 makeCompositeIndexDefinition({{"a", CollationOp::Ascending, false /*isMultiKey*/},
                                               {"d", CollationOp::Ascending, false /*isMultiKey*/}},
                                              false /*isMultiKey*/)}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.getHints()._fastIndexNullHandling = true;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(65, 90, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Prefer index1 over index2 and index3 in order to cover all fields.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {[Const [0"
        " | 0 | 0 | minKey], Const [0 | 0 | 0 | maxKey]]}]\n",
        optimized);
}

TEST(PhysRewriter, IndexResidualReq2) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(0)))),
                         make<Variable>("root")),
        std::move(scanNode));

    ABT filterBNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("b",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(0)))),
                         make<Variable>("root")),
        std::move(filterANode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterBNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {{"index1",
                           makeCompositeIndexDefinition(
                               {{"a", CollationOp::Ascending, true /*isMultiKey*/},
                                {"c", CollationOp::Ascending, true /*isMultiKey*/},
                                {"b", CollationOp::Ascending, true /*isMultiKey*/}})}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(7, 10, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // We can cover "b" with the index and filter before we Seek.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "Unique [{rid_0}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_10]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [0]\n"
        "IndexScan [{'<indexKey> 2': evalTemp_10, '<rid>': rid_0}, scanDefName: c1, indexDefName:"
        " index1, interval: {[Const [0 | minKey | minKey], Const [0 | maxKey | maxKey]]}]\n",
        optimized);
}

TEST(PhysRewriter, ElemMatchIndex) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    // This encodes an elemMatch with a conjunction >70 and <90.
    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("a",
                          make<PathComposeM>(
                              make<PathArr>(),
                              make<PathTraverse>(
                                  PathTraverse::kSingleLevel,
                                  make<PathComposeM>(
                                      make<PathCompare>(Operations::Gt, Constant::int64(70)),
                                      make<PathCompare>(Operations::Lt, Constant::int64(90)))))),
            make<Variable>("root")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(6, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [evalTemp_4]\n"
        "|   |   PathArr []\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root, 'a': evalTemp_4}, c1]\n"
        "Unique [{rid_0}]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {(Const [7"
        "0], Const [90])}]\n",
        optimized);
}

TEST(PhysRewriter, ElemMatchIndex1) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterNode1 = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("b",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(1)))),
                         make<Variable>("root")),
        std::move(scanNode));

    // This encodes an elemMatch with a conjunction >70 and <90.
    ABT filterNode2 = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("a",
                          make<PathComposeM>(
                              make<PathArr>(),
                              make<PathTraverse>(
                                  PathTraverse::kSingleLevel,
                                  make<PathComposeM>(
                                      make<PathCompare>(Operations::Gt, Constant::int64(70)),
                                      make<PathCompare>(Operations::Lt, Constant::int64(90)))))),
            make<Variable>("root")),
        std::move(filterNode1));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode2));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {{"index1",
                           makeCompositeIndexDefinition(
                               {{"b", CollationOp::Ascending, true /*isMultiKey*/},
                                {"a", CollationOp::Ascending, true /*isMultiKey*/}})}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(8, 12, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate we can cover both the filter and the extracted elemMatch predicate with the
    // index.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [evalTemp_17]\n"
        "|   |   PathArr []\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root, 'a': evalTemp_17}, c1]\n"
        "Unique [{rid_0}]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {(Const [1"
        " | 70], Const [1 | 90])}]\n",
        optimized);
}

TEST(PhysRewriter, ElemMatchIndexNoArrays) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    // This encodes an elemMatch with a conjunction >70 and <90.
    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("a",
                          make<PathComposeM>(
                              make<PathArr>(),
                              make<PathTraverse>(
                                  PathTraverse::kSingleLevel,
                                  make<PathComposeM>(
                                      make<PathCompare>(Operations::Gt, Constant::int64(70)),
                                      make<PathCompare>(Operations::Lt, Constant::int64(90)))))),
            make<Variable>("root")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 makeIndexDefinition("a", CollationOp::Ascending, false /*multiKey*/)}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(2, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // If we do not have arrays (index is not multikey) we simplify to unsatisfiable query.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Evaluation [{root} = Const [Nothing]]\n"
        "LimitSkip [limit: 0, skip: 0]\n"
        "CoScan []\n",
        optimized);
}

TEST(PhysRewriter, ObjectElemMatchResidual) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT bPred =
        make<PathGet>("b",
                      make<PathTraverse>(PathTraverse::kSingleLevel,
                                         make<PathCompare>(Operations::Eq, Constant::int64(1))));
    ABT cPred =
        make<PathGet>("c",
                      make<PathTraverse>(PathTraverse::kSingleLevel,
                                         make<PathCompare>(Operations::Eq, Constant::int64(1))));
    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>(
                "a",
                make<PathComposeM>(
                    make<PathArr>(),
                    make<PathTraverse>(
                        PathTraverse::kSingleLevel,
                        make<PathComposeM>(make<PathComposeM>(std::move(bPred), std::move(cPred)),
                                           make<PathComposeA>(make<PathObj>(), make<PathArr>()))))),
            make<Variable>("root")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {{"index1",
                           makeCompositeIndexDefinition(
                               {{"b", CollationOp::Ascending, true /*isMultiKey*/},
                                {"a", CollationOp::Ascending, true /*isMultiKey*/}})}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(25, 35, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // We should pick the index, and do at least some filtering before the fetch.
    // We don't have index bounds, both because 'a' is not the first field of the index,
    // and because the predicates are on child fields 'a.b' and 'a.c'.
    // Also, we can't satisfy 'a.b' and 'a.c' on the same scan, because that would force
    // both predicates to match the same array-element of 'a'.

    // TODO SERVER-70780 we could be simplifying the paths even more:
    // ComposeA PathArr PathObj is true when the input is an array or object.
    // But the other 'Get Traverse Compare' here can only be true when the input is an object.
    // So the 'ComposeA PathArr PathObj' is redundant and we could remove it.

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [root]\n"
        "|   PathGet [a] PathTraverse [1] PathComposeM []\n"
        "|   |   PathComposeA []\n"
        "|   |   |   PathArr []\n"
        "|   |   PathObj []\n"
        "|   PathComposeM []\n"
        "|   |   PathGet [c] PathTraverse [1] PathCompare [Eq] Const [1]\n"
        "|   PathGet [b] PathTraverse [1] PathCompare [Eq] Const [1]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [evalTemp_3]\n"
        "|   |   PathArr []\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root, 'a': evalTemp_3}, c1]\n"
        "Unique [{rid_0}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_7]\n"
        "|   PathGet [c] PathTraverse [1] PathCompare [Eq] Const [1]\n"
        "IndexScan [{'<indexKey> 1': evalTemp_7, '<rid>': rid_0}, scanDefName: c1, indexDefName: "
        "index1, interval: {<fully open>}]\n",
        optimized);
}

TEST(PhysRewriter, ObjectElemMatchBounds) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT bPred =
        make<PathGet>("b",
                      make<PathTraverse>(PathTraverse::kSingleLevel,
                                         make<PathCompare>(Operations::Eq, Constant::int64(4))));
    ABT cPred =
        make<PathGet>("c",
                      make<PathTraverse>(PathTraverse::kSingleLevel,
                                         make<PathCompare>(Operations::Eq, Constant::int64(5))));
    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>(
                "a",
                make<PathComposeM>(
                    make<PathArr>(),
                    make<PathTraverse>(
                        PathTraverse::kSingleLevel,
                        make<PathComposeM>(make<PathComposeM>(bPred, cPred),
                                           make<PathComposeA>(make<PathObj>(), make<PathArr>()))))),
            make<Variable>("root")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{makeIndexPath(FieldPathType{"a", "b"}, true /*isMultiKey*/),
                                   CollationOp::Ascending}},
                                 true /*isMultiKey*/}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(15, 20, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // We should pick the index, and generate bounds for the 'b' predicate.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [root]\n"
        "|   PathGet [a] PathTraverse [1] PathComposeM []\n"
        "|   |   PathComposeA []\n"
        "|   |   |   PathArr []\n"
        "|   |   PathObj []\n"
        "|   PathComposeM []\n"
        "|   |   PathGet [c] PathTraverse [1] PathCompare [Eq] Const [5]\n"
        "|   PathGet [b] PathTraverse [1] PathCompare [Eq] Const [4]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [evalTemp_2]\n"
        "|   |   PathArr []\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root, 'a': evalTemp_2}, c1]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Const "
        "[4]}]\n",
        optimized);
}

TEST(PhysRewriter, NestedElemMatch) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "coll1");

    auto elemMatch = [](ABT path) -> ABT {
        return make<PathComposeM>(make<PathArr>(),
                                  make<PathTraverse>(PathTraverse::kSingleLevel, std::move(path)));
    };
    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>(
                "a", elemMatch(elemMatch(make<PathCompare>(Operations::Eq, Constant::int64(2))))),
            make<Variable>("root")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode));

    // TODO SERVER-71551 Follow up unit tests with overriden Cost Model.
    auto costModel = getTestCostModel();
    costModel.setGroupByStartupCost(1e-6);
    costModel.setGroupByIncrementalCost(1e-4);
    costModel.setEvalStartupCost(1e-6);

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"coll1",
           createScanDef(
               {},
               {{"index1",
                 makeIndexDefinition("a", CollationOp::Ascending, true /*isMultiKey*/)}})}}},
        std::move(costModel),
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(10, 20, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // We should not generate tight index bounds [2, 2], because nested elemMatch only matches
    // arrays of arrays, and multikey indexes only unwind one level of arrays.  We can generate
    // PathArr bounds, but that only tells us which documents have arrays-of-arrays; then we can run
    // a residual predicate to check that the inner array contains '2'.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [root]\n"
        "|   PathGet [a] PathTraverse [1] PathComposeM []\n"
        "|   |   PathTraverse [1] PathCompare [Eq] Const [2]\n"
        "|   PathArr []\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [evalTemp_2]\n"
        "|   |   PathArr []\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root, 'a': evalTemp_2}, coll1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   FunctionCall [getArraySize] Variable [sides_0]\n"
        "|   PathCompare [Eq] Const [2]\n"
        "GroupBy [{rid_0}]\n"
        "|   aggregations: \n"
        "|       [sides_0]\n"
        "|           FunctionCall [$addToSet] Variable [sideId_0]\n"
        "Union [{rid_0, sideId_0}]\n"
        "|   Evaluation [{sideId_0} = Const [1]]\n"
        "|   IndexScan [{'<rid>': rid_0}, scanDefName: coll1, indexDefName: index1, interval: "
        "{[Const [[]], Const [BinData(0, )])}]\n"
        "Evaluation [{sideId_0} = Const [0]]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_3]\n"
        "|   PathTraverse [1] PathCompare [Eq] Const [2]\n"
        "IndexScan [{'<indexKey> 0': evalTemp_3, '<rid>': rid_0}, scanDefName: coll1, indexDefNam"
        "e: index1, interval: {<fully open>}]\n",
        optimized);
}

TEST(PhysRewriter, PathObj) {
    using namespace properties;

    ce::PartialSchemaSelHints hints;
    hints.emplace(PartialSchemaKey{"root", make<PathGet>("a", make<PathIdentity>())},
                  kDefaultSelectivity);

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a", make<PathObj>()), make<Variable>("root")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {{"index1",
                           makeCompositeIndexDefinition(
                               {{"a", CollationOp::Ascending, false /*isMultiKey*/},
                                {"b", CollationOp::Ascending, true /*isMultiKey*/}})}})}}},
        makeHintedCE(std::move(hints)),
        boost::none /*costModel*/,
        DebugInfo{true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests},
        {} /*hints*/);

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // We should get index bounds for the PathObj.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "Unique [{rid_0}]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {[Const [{"
        "} | minKey], Const [[] | minKey])}]\n",
        optimized);
}

TEST(PhysRewriter, ArrayConstantIndex) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterNode1 = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("b",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(0)))),
                         make<Variable>("root")),
        std::move(scanNode));

    const auto [tag, val] = sbe::value::makeNewArray();
    sbe::value::Array* arr = sbe::value::getArrayView(val);
    for (int i = 0; i < 3; i++) {
        arr->push_back(sbe::value::TypeTags::NumberInt32, i + 1);
    }
    ABT arrayConst = make<Constant>(tag, val);

    // This encodes a match against an array constant.
    ABT filterNode2 = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("a",
                          make<PathComposeA>(
                              make<PathTraverse>(PathTraverse::kSingleLevel,
                                                 make<PathCompare>(Operations::Eq, arrayConst)),
                              make<PathCompare>(Operations::Eq, arrayConst))),
            make<Variable>("root")),
        std::move(filterNode1));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"root"}},
                                  std::move(filterNode2));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {{"index1",
                           makeCompositeIndexDefinition(
                               {{"b", CollationOp::Ascending, true /*isMultiKey*/},
                                {"a", CollationOp::Ascending, true /*isMultiKey*/}})}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(7, 10, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate we get index bounds to handle the array constant, while we also retain the
    // original filter. We have index bound with the array itself unioned with bound using the first
    // array element.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [root]\n"
        "|   PathGet [a]\n"
        "|   PathComposeA []\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [[1, 2, 3]]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [[1, 2, 3]]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "GroupBy [{rid_0}]\n"
        "|   aggregations: \n"
        "Union [{rid_0}]\n"
        "|   IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Cons"
        "t [0 | [1, 2, 3]]}]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Const [0"
        " | 1]}]\n",
        optimized);
}

TEST(PhysRewriter, ArrayConstantNoIndex) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterNode1 = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("b",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(0)))),
                         make<Variable>("root")),
        std::move(scanNode));

    const auto [tag, val] = sbe::value::makeNewArray();
    sbe::value::Array* arr = sbe::value::getArrayView(val);
    for (int i = 0; i < 3; i++) {
        arr->push_back(sbe::value::TypeTags::NumberInt32, i + 1);
    }
    ABT arrayConst = make<Constant>(tag, val);

    // This encodes a match against an array constant.
    ABT filterNode2 = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("a",
                          make<PathComposeA>(
                              make<PathTraverse>(PathTraverse::kSingleLevel,
                                                 make<PathCompare>(Operations::Eq, arrayConst)),
                              make<PathCompare>(Operations::Eq, arrayConst))),
            make<Variable>("root")),
        std::move(filterNode1));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"root"}},
                                  std::move(filterNode2));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(3, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Without an index, we retain the original array bounds predicate, and do not duplicate the
    // predicates in the sargable node (they are perf only)
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [root]\n"
        "|   PathGet [a]\n"
        "|   PathComposeA []\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [[1, 2, 3]]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [[1, 2, 3]]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_1]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [0]\n"
        "PhysicalScan [{'<root>': root, 'b': evalTemp_1}, c1]\n",
        optimized);
}

TEST(PhysRewriter, CollationLimit) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT collationNode = make<CollationNode>(CollationRequirement({{"pa", CollationOp::Ascending}}),
                                            std::move(evalNode));
    ABT limitSkipNode = make<LimitSkipNode>(LimitSkipRequirement{20, 0}, std::move(collationNode));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}},
                                  std::move(limitSkipNode));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(9, 11, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // We have a collation node with limit-skip physical properties. It will be lowered to a
    // sort node with limit.
    ASSERT_EXPLAIN_PROPS_V2_AUTO(
        "Properties [cost: 4.75042, localCost: 0, adjustedCE: 20]\n"
        "|   |   Logical:\n"
        "|   |       cardinalityEstimate: \n"
        "|   |           ce: 20\n"
        "|   |       projections: \n"
        "|   |           pa\n"
        "|   |           root\n"
        "|   |       collectionAvailability: \n"
        "|   |           c1\n"
        "|   |       distributionAvailability: \n"
        "|   |           distribution: \n"
        "|   |               type: Centralized\n"
        "|   Physical:\n"
        "|       distribution: \n"
        "|           type: Centralized\n"
        "Root [{root}]\n"
        "Properties [cost: 4.75042, localCost: 4.32193, adjustedCE: 20]\n"
        "|   |   Logical:\n"
        "|   |       cardinalityEstimate: \n"
        "|   |           ce: 1000\n"
        "|   |           requirementCEs: \n"
        "|   |               refProjection: root, path: 'PathGet [a] PathIdentity []', ce: 1000\n"
        "|   |       projections: \n"
        "|   |           pa\n"
        "|   |           root\n"
        "|   |       indexingAvailability: \n"
        "|   |           [groupId: 0, scanProjection: root, scanDefName: c1]\n"
        "|   |       collectionAvailability: \n"
        "|   |           c1\n"
        "|   |       distributionAvailability: \n"
        "|   |           distribution: \n"
        "|   |               type: Centralized\n"
        "|   Physical:\n"
        "|       collation: \n"
        "|           pa: Ascending\n"
        "|       limitSkip:\n"
        "|           limit: 20\n"
        "|           skip: 0\n"
        "|       projections: \n"
        "|           root\n"
        "|       distribution: \n"
        "|           type: Centralized\n"
        "|       indexingRequirement: \n"
        "|           Complete, dedupRID\n"
        "Collation [{pa: Ascending}]\n"
        "Properties [cost: 0.428487, localCost: 0.428487, adjustedCE: 1000]\n"
        "|   |   Logical:\n"
        "|   |       cardinalityEstimate: \n"
        "|   |           ce: 1000\n"
        "|   |           requirementCEs: \n"
        "|   |               refProjection: root, path: 'PathGet [a] PathIdentity []', ce: 1000\n"
        "|   |       projections: \n"
        "|   |           pa\n"
        "|   |           root\n"
        "|   |       indexingAvailability: \n"
        "|   |           [groupId: 0, scanProjection: root, scanDefName: c1]\n"
        "|   |       collectionAvailability: \n"
        "|   |           c1\n"
        "|   |       distributionAvailability: \n"
        "|   |           distribution: \n"
        "|   |               type: Centralized\n"
        "|   Physical:\n"
        "|       projections: \n"
        "|           root\n"
        "|           pa\n"
        "|       distribution: \n"
        "|           type: Centralized\n"
        "|       indexingRequirement: \n"
        "|           Complete, dedupRID\n"
        "PhysicalScan [{'<root>': root, 'a': pa}, c1]\n",
        phaseManager);
}

TEST(PhysRewriter, PartialIndex1) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(3)))),
                         make<Variable>("root")),
        std::move(scanNode));
    ABT filterBNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("b",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(2)))),
                         make<Variable>("root")),
        std::move(filterANode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterBNode));

    // TODO: Test cases where partial filter bound is a range which subsumes the query
    // requirement
    // TODO: (e.g. half open interval)
    auto conversionResult = convertExprToPartialSchemaReq(
        make<EvalFilter>(make<PathGet>("b",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(2)))),
                         make<Variable>("root")),
        true /*isFilterContext*/,
        {} /*pathToInterval*/);
    ASSERT_TRUE(conversionResult);
    ASSERT_FALSE(conversionResult->_retainPredicate);

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {{"index1",
                           IndexDefinition{{{makeIndexPath("a"), CollationOp::Ascending}},
                                           true /*isMultiKey*/,
                                           {DistributionType::Centralized},
                                           std::move(conversionResult->_reqMap)}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Partial schema requirement is not on an index field. We get a seek on this field.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [evalTemp_4]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [2]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root, 'b': evalTemp_4}, c1]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Const [3"
        "]}]\n",
        optimized);
}

TEST(PhysRewriter, PartialIndex2) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(3)))),
                         make<Variable>("root")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterANode));

    auto conversionResult = convertExprToPartialSchemaReq(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(3)))),
                         make<Variable>("root")),
        true /*isFilterContext*/,
        {} /*pathToInterval*/);
    ASSERT_TRUE(conversionResult);
    ASSERT_FALSE(conversionResult->_retainPredicate);

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {{"index1",
                           IndexDefinition{{{makeIndexPath("a"), CollationOp::Ascending}},
                                           true /*isMultiKey*/,
                                           {DistributionType::Centralized},
                                           std::move(conversionResult->_reqMap)}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Partial schema requirement on an index field.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Const "
        "[3]}]\n",
        optimized);
}

TEST(PhysRewriter, PartialIndexReject) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(3)))),
                         make<Variable>("root")),
        std::move(scanNode));
    ABT filterBNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("b",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(2)))),
                         make<Variable>("root")),
        std::move(filterANode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterBNode));

    auto conversionResult = convertExprToPartialSchemaReq(
        make<EvalFilter>(make<PathGet>("b",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(4)))),
                         make<Variable>("root")),
        true /*isFilterContext*/,
        {} /*pathToInterval*/);
    ASSERT_TRUE(conversionResult);
    ASSERT_FALSE(conversionResult->_retainPredicate);

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {{"index1",
                           IndexDefinition{{{makeIndexPath("a"), CollationOp::Ascending}},
                                           true /*isMultiKey*/,
                                           {DistributionType::Centralized},
                                           std::move(conversionResult->_reqMap)}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(3, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Incompatible partial filter. Use scan.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_3]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [2]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_2]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [3]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_2, 'b': evalTemp_3}, c1]\n",
        optimized);
}

TEST(PhysRewriter, RequireRID) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(3)))),
                         make<Variable>("root")),
        std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode));

    auto phaseManager = makePhaseManagerRequireRID(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(2, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Make sure the Scan node returns rid, and the Root node refers to it.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{rid_0, root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_0]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [3]\n"
        "PhysicalScan [{'<rid>': rid_0, '<root>': root, 'a': evalTemp_0}, c1]\n",
        optimized);
}

TEST(PhysRewriter, RequireRID1) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("scan_0", "c1");

    // Non-sargable filter node.
    ABT filterNode = make<FilterNode>(Constant::boolean(true), std::move(scanNode));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"scan_0"}},
                                  std::move(filterNode));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManagerRequireRID(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(3, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{rid_0, scan_0}]\n"
        "Filter []\n"
        "|   Const [true]\n"
        "PhysicalScan [{'<rid>': rid_0, '<root>': scan_0}, c1]\n",
        optimized);
}

TEST(PhysRewriter, UnionRewrite) {
    using namespace properties;

    ABT scanNode1 = make<ScanNode>("ptest1", "test1");
    ABT scanNode2 = make<ScanNode>("ptest2", "test2");

    // Each branch produces two projections, pUnion1 and pUnion2.
    ABT evalNode1 = make<EvaluationNode>(
        "pUnion1",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("ptest1")),
        std::move(scanNode1));
    ABT evalNode2 = make<EvaluationNode>(
        "pUnion2",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("ptest1")),
        std::move(evalNode1));

    ABT evalNode3 = make<EvaluationNode>(
        "pUnion1",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("ptest2")),
        std::move(scanNode2));
    ABT evalNode4 = make<EvaluationNode>(
        "pUnion2",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("ptest2")),
        std::move(evalNode3));

    ABT unionNode =
        make<UnionNode>(ProjectionNameVector{"pUnion1", "pUnion2"}, makeSeq(evalNode2, evalNode4));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pUnion1"}},
                                  std::move(unionNode));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"test1", createScanDef({}, {})}, {"test2", createScanDef({}, {})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pUnion1}]\n"
        "Union [{pUnion1}]\n"
        "|   PhysicalScan [{'a': pUnion1}, test2]\n"
        "PhysicalScan [{'a': pUnion1}, test1]\n",
        optimized);
}

TEST(PhysRewriter, JoinRewrite) {
    using namespace properties;

    ABT scanNode1 = make<ScanNode>("ptest1", "test1");
    ABT scanNode2 = make<ScanNode>("ptest2", "test2");

    // Each branch produces two projections, pUnion1 and pUnion2.
    ABT evalNode1 = make<EvaluationNode>(
        "p11",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("ptest1")),
        std::move(scanNode1));
    ABT evalNode2 = make<EvaluationNode>(
        "p12",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("ptest1")),
        std::move(evalNode1));

    ABT evalNode3 = make<EvaluationNode>(
        "p21",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("ptest2")),
        std::move(scanNode2));
    ABT evalNode4 = make<EvaluationNode>(
        "p22",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("ptest2")),
        std::move(evalNode3));

    ABT joinNode = make<BinaryJoinNode>(
        JoinType::Inner,
        ProjectionNameSet{},
        make<BinaryOp>(Operations::Eq, make<Variable>("p12"), make<Variable>("p22")),
        std::move(evalNode2),
        std::move(evalNode4));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"p11", "p21"}},
                                  std::move(joinNode));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"test1", createScanDef({}, {})}, {"test2", createScanDef({}, {})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{p11, p21}]\n"
        "NestedLoopJoin [joinType: Inner, ]\n"
        "|   |   BinaryOp [Eq]\n"
        "|   |   |   Variable [p22]\n"
        "|   |   Variable [p12]\n"
        "|   PhysicalScan [{'a': p21, 'b': p22}, test2]\n"
        "PhysicalScan [{'a': p11, 'b': p12}, test1]\n",
        optimized);
}

TEST(PhysRewriter, JoinRewrite1) {
    using namespace properties;

    ABT scanNode1 = make<ScanNode>("ptest1", "test1");
    ABT scanNode2 = make<ScanNode>("ptest2", "test2");

    ABT evalNode1 = make<EvaluationNode>(
        "p1",
        make<EvalPath>(make<PathGet>("a1", make<PathIdentity>()), make<Variable>("ptest1")),
        std::move(scanNode1));
    ABT evalNode2 = make<EvaluationNode>(
        "p2",
        make<EvalPath>(make<PathGet>("a2", make<PathIdentity>()), make<Variable>("ptest1")),
        std::move(evalNode1));

    ABT filterNode1 = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("b", make<PathCompare>(Operations::Gt, make<Variable>("p1"))),
            make<Variable>("ptest2")),
        std::move(scanNode2));
    ABT filterNode2 = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("b", make<PathCompare>(Operations::Gt, make<Variable>("p2"))),
            make<Variable>("ptest2")),
        std::move(filterNode1));

    ABT joinNode = make<BinaryJoinNode>(JoinType::Inner,
                                        ProjectionNameSet{"p1", "p2"},
                                        Constant::boolean(true),
                                        std::move(evalNode2),
                                        std::move(filterNode2));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"p1", "p2"}},
                                  std::move(joinNode));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"test1", createScanDef({}, {})},
          {"test2",
           createScanDef({},
                         {{"index1",
                           {{{makeNonMultikeyIndexPath("b"), CollationOp::Ascending}},
                            false /*isMultiKey*/}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(rootNode);
    phaseManager.optimize(optimized);
    ASSERT_EQ(6, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate index nested loop join and variable interval intersection.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{p1, p2}]\n"
        "NestedLoopJoin [joinType: Inner, {p1, p2}]\n"
        "|   |   Const [true]\n"
        "|   IndexScan [{}, scanDefName: test2, indexDefName: index1, interval: {>If [] BinaryOp "
        "[Gte] Variable [p1] Variable [p2] Variable [p1] Variable [p2]}]\n"
        "PhysicalScan [{'a1': p1, 'a2': p2}, test1]\n",
        optimized);
}

TEST(PhysRewriter, RootInterval) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("root", "c1");

    // We have a predicate applied directly over the root projection without field extraction.
    ABT filterNode =
        make<FilterNode>(make<EvalFilter>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                          make<Variable>("root")),
                         std::move(scanNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"root"}}, std::move(filterNode));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_EQ(2, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [root]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'<root>': root}, c1]\n",
        optimized);
}

TEST(PhysRewriter, ResidualFilterPathIsBalanced) {
    using namespace properties;

    ABT root =
        NodeBuilder{}
            .root("root")
            .filter(_evalf(_get("a",
                                _traverse1(_composea(
                                    _composea(_cmp("Lte", "0"_cint64), _cmp("Eq", "1"_cint64)),
                                    _composea(_cmp("Gt", "2"_cint64), _cmp("Gte", "3"_cint64))))),
                           "root"_var))
            .finish(_scan("root", "c1"));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = root;
    phaseManager.optimize(optimized);

    // The FilterNode is first converted to a SargableNode, then it's converted back to a FilterNode
    // because it can't be satisfied with an index. The path under the resulting FilterNode should
    // be balanced.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_0]\n"
        "|   PathTraverse [1]\n"
        "|   PathComposeA []\n"
        "|   |   PathCompare [Gt]\n"
        "|   |   Const [2]\n"
        "|   PathComposeA []\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [1]\n"
        "|   PathCompare [Lte]\n"
        "|   Const [0]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_0}, c1]\n",
        optimized);
}

TEST(PhysRewriter, DisjunctiveEqsConsolidatedIntoEqMember) {
    using namespace properties;

    const auto [tag1, val1] = sbe::value::makeNewArray();
    sbe::value::Array* arr1 = sbe::value::getArrayView(val1);
    for (int i = 1; i < 4; i++) {
        arr1->push_back(sbe::value::TypeTags::NumberInt32, i);
    }
    ABT arrayConst1 = make<Constant>(tag1, val1);

    const auto [tag2, val2] = sbe::value::makeNewArray();
    sbe::value::Array* arr2 = sbe::value::getArrayView(val2);
    for (int i = 5; i < 8; i++) {
        arr2->push_back(sbe::value::TypeTags::NumberInt32, i);
    }
    ABT arrayConst2 = make<Constant>(tag2, val2);

    ABT root =
        NodeBuilder{}
            .root("root")
            .filter(_evalf(_get("a",
                                _traverse1(_composea(
                                    _composea(_composea(_cmp("EqMember", ExprHolder{arrayConst1}),
                                                        _cmp("Eq", "8"_cint64)),
                                              _composea(_cmp("EqMember", ExprHolder{arrayConst2}),
                                                        _cmp("Eq", "4"_cint64))),
                                    _composea(_cmp("Lt", "0"_cint64), _cmp("Gt", "20"_cint64))))),
                           "root"_var))
            .finish(_scan("root", "c1"));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", createScanDef({}, {})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = root;
    phaseManager.optimize(optimized);
    ASSERT_EQ(2, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // The FilterNode is first converted to a SargableNode, then it's converted back to a FilterNode
    // because it can't be satisfied with an index. The path under the resulting FilterNode should
    // be combined into one EqMember path that accounts for all original separate Eq and EqMember
    // paths, as well as one Gt path and one Lt path.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_0]\n"
        "|   PathTraverse [1]\n"
        "|   PathComposeA []\n"
        "|   |   PathCompare [Gt]\n"
        "|   |   Const [20]\n"
        "|   PathComposeA []\n"
        "|   |   PathCompare [EqMember]\n"
        "|   |   Const [[1, 2, 3, 4, 5, 6, 7, 8]]\n"
        "|   PathCompare [Lt]\n"
        "|   Const [0]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_0}, c1]\n",
        optimized);
}

TEST(PhysRewriter, EqMemberSargable) {
    using namespace properties;

    const auto [tag, val] = sbe::value::makeNewArray();
    sbe::value::Array* arr = sbe::value::getArrayView(val);
    for (int i = 1; i < 4; i++) {
        arr->push_back(sbe::value::TypeTags::NumberInt32, i);
    }
    ABT arrayConst = make<Constant>(tag, val);

    ABT rootNode =
        NodeBuilder{}
            .root("root")
            .filter(
                _evalf(_get("a", _traverse1(_cmp("EqMember", ExprHolder{arrayConst}))), "root"_var))
            .finish(_scan("root", "c1"));

    {
        auto prefixId = PrefixId::createForTests();
        auto phaseManager = makePhaseManager(
            {OptPhase::MemoSubstitutionPhase},
            prefixId,
            {{{"c1",
               createScanDef(
                   {},
                   {{"index1",
                     makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}})}}},
            boost::none /*costModel*/,
            {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

        ABT optimized = rootNode;
        phaseManager.optimize(optimized);

        ASSERT_EXPLAIN_V2_AUTO(
            "Root [{root}]\n"
            "Sargable [Complete]\n"
            "|   |   |   requirements: \n"
            "|   |   |       {{{root, 'PathGet [a] PathIdentity []', {{{=Const [1]}} U {{=Const "
            "[2]}} "
            "U {{=Const [3]}}}}}}\n"
            "|   |   candidateIndexes: \n"
            "|   |       candidateId: 1, index1, {}, {Compound}, {{{=Const [1]}} U {{=Const [2]}} "
            "U "
            "{{=Const [3]}}}\n"
            "|   scanParams: \n"
            "|       {'a': evalTemp_0}\n"
            "|           residualReqs: \n"
            "|               {{{evalTemp_0, 'PathIdentity []', {{{=Const [1]}} U {{=Const [2]}} U "
            "{{=Const [3]}}}, entryIndex: 0}}}\n"
            "Scan [c1, {root}]\n",
            optimized);
    }

    {
        auto prefixId = PrefixId::createForTests();
        auto phaseManager = makePhaseManager(
            {OptPhase::MemoSubstitutionPhase,
             OptPhase::MemoExplorationPhase,
             OptPhase::MemoImplementationPhase},
            prefixId,
            {{{"c1",
               createScanDef({}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}})}}},
            boost::none /*costModel*/,
            {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

        ABT optimized = rootNode;
        phaseManager.optimize(optimized);
        ASSERT_EQ(4, phaseManager.getMemo().getStats()._physPlanExplorationCount);

        // Test sargable filter is satisfied with an index scan.
        ASSERT_EXPLAIN_PROPS_V2_AUTO(
            "Properties [cost: 0.173038, localCost: 0, adjustedCE: 54.6819]\n"
            "|   |   Logical:\n"
            "|   |       cardinalityEstimate: \n"
            "|   |           ce: 54.6819\n"
            "|   |       projections: \n"
            "|   |           root\n"
            "|   |       indexingAvailability: \n"
            "|   |           [groupId: 0, scanProjection: root, scanDefName: c1, "
            "hasProperInterval]\n"
            "|   |       collectionAvailability: \n"
            "|   |           c1\n"
            "|   |       distributionAvailability: \n"
            "|   |           distribution: \n"
            "|   |               type: Centralized\n"
            "|   Physical:\n"
            "|       distribution: \n"
            "|           type: Centralized\n"
            "|       indexingRequirement: \n"
            "|           Complete, dedupRID\n"
            "Root [{root}]\n"
            "Properties [cost: 0.173038, localCost: 0.0180785, adjustedCE: 54.6819]\n"
            "|   |   Logical:\n"
            "|   |       cardinalityEstimate: \n"
            "|   |           ce: 54.6819\n"
            "|   |           requirementCEs: \n"
            "|   |               refProjection: root, path: 'PathGet [a] PathTraverse [1] "
            "PathIdentity []', ce: 54.6819\n"
            "|   |       projections: \n"
            "|   |           root\n"
            "|   |       indexingAvailability: \n"
            "|   |           [groupId: 0, scanProjection: root, scanDefName: c1, "
            "hasProperInterval]\n"
            "|   |       collectionAvailability: \n"
            "|   |           c1\n"
            "|   |       distributionAvailability: \n"
            "|   |           distribution: \n"
            "|   |               type: Centralized\n"
            "|   Physical:\n"
            "|       projections: \n"
            "|           root\n"
            "|       distribution: \n"
            "|           type: Centralized\n"
            "|       indexingRequirement: \n"
            "|           Complete, dedupRID\n"
            "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
            "|   |   Const [true]\n"
            "|   Properties [cost: 0.0757996, localCost: 0.0757996, adjustedCE: 54.6819]\n"
            "|   |   |   Logical:\n"
            "|   |   |       cardinalityEstimate: \n"
            "|   |   |           ce: 1000\n"
            "|   |   |       projections: \n"
            "|   |   |           root\n"
            "|   |   |       indexingAvailability: \n"
            "|   |   |           [groupId: 0, scanProjection: root, scanDefName: c1, eqPredsOnly]\n"
            "|   |   |       collectionAvailability: \n"
            "|   |   |           c1\n"
            "|   |   |       distributionAvailability: \n"
            "|   |   |           distribution: \n"
            "|   |   |               type: Centralized\n"
            "|   |   Physical:\n"
            "|   |       projections: \n"
            "|   |           root\n"
            "|   |       distribution: \n"
            "|   |           type: Centralized\n"
            "|   |       indexingRequirement: \n"
            "|   |           Seek, dedupRID\n"
            "|   |       repetitionEstimate: 54.6819\n"
            "|   LimitSkip [limit: 1, skip: 0]\n"
            "|   Seek [ridProjection: rid_0, {'<root>': root}, c1]\n"
            "Properties [cost: 0.0791597, localCost: 0.0791597, adjustedCE: 18.2273]\n"
            "|   |   Logical:\n"
            "|   |       cardinalityEstimate: \n"
            "|   |           ce: 54.6819\n"
            "|   |           requirementCEs: \n"
            "|   |               refProjection: root, path: 'PathGet [a] PathTraverse [1] "
            "PathIdentity []', ce: 54.6819\n"
            "|   |       projections: \n"
            "|   |           root\n"
            "|   |       indexingAvailability: \n"
            "|   |           [groupId: 0, scanProjection: root, scanDefName: c1, "
            "hasProperInterval]\n"
            "|   |       collectionAvailability: \n"
            "|   |           c1\n"
            "|   |       distributionAvailability: \n"
            "|   |           distribution: \n"
            "|   |               type: Centralized\n"
            "|   Physical:\n"
            "|       projections: \n"
            "|           rid_0\n"
            "|       distribution: \n"
            "|           type: Centralized\n"
            "|       indexingRequirement: \n"
            "|           Index, dedupRID\n"
            "GroupBy [{rid_0}]\n"
            "|   aggregations: \n"
            "Union [{rid_0}]\n"
            "|   |   IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: "
            "{=Const [3]}]\n"
            "|   IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: "
            "{=Const [2]}]\n"
            "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Const "
            "[1]}]\n",
            phaseManager);
    }
}

TEST(PhysRewriter, IndexSubfieldCovered) {
    using namespace properties;

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode1 = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterNode1 =
        make<FilterNode>(make<EvalFilter>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                          make<Variable>("pa")),
                         std::move(evalNode1));

    ABT evalNode2 = make<EvaluationNode>(
        "pb",
        make<EvalPath>(make<PathGet>("a", make<PathGet>("b", make<PathIdentity>())),
                       make<Variable>("root")),
        std::move(filterNode1));

    ABT filterNode2 =
        make<FilterNode>(make<EvalFilter>(make<PathCompare>(Operations::Eq, Constant::int64(2)),
                                          make<Variable>("pb")),
                         std::move(evalNode2));

    ABT filterNode3 = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>(
                "a",
                make<PathTraverse>(
                    PathTraverse::kSingleLevel,
                    make<PathGet>("c", make<PathCompare>(Operations::Eq, Constant::int64(3))))),
            make<Variable>("root")),
        std::move(filterNode2));

    ABT rootNode = make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pa", "pb"}},
                                  std::move(filterNode3));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(20, 35, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Observe we have a covered plan. The filters for subfields "b" and "c" are expressed as
    // residual predicates. Also observe the traverse for "a.c" is removed due to "a" being
    // non-multikey.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pa, pb}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pb]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [2]\n"
        "Evaluation [{pb}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [pa]\n"
        "|   PathGet [b]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pa]\n"
        "|   PathGet [c]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [3]\n"
        "IndexScan [{'<indexKey> 0': pa}, scanDefName: c1, indexDefName: index1, interval: {=Cons"
        "t [1]}]\n",
        optimized);
}

TEST(PhysRewriter, PerfOnlyPreds1) {
    using namespace properties;

    ce::PartialSchemaSelHints hints;
    hints.emplace(PartialSchemaKey{"root", make<PathGet>("a", make<PathIdentity>())},
                  SelectivityType{0.01});
    hints.emplace(PartialSchemaKey{"root", make<PathGet>("b", make<PathIdentity>())},
                  SelectivityType{0.02});

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT evalNode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));
    ABT filterNode1 = make<FilterNode>(
        make<EvalFilter>(make<PathTraverse>(PathTraverse::kSingleLevel,
                                            make<PathCompare>(Operations::Lt, Constant::int64(1))),
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

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 makeCompositeIndexDefinition({{"b", CollationOp::Ascending, false /*isMultiKey*/},
                                               {"a", CollationOp::Ascending, false /*isMultiKey*/}},
                                              false /*isMultiKey*/)}})}}},
        makeHintedCE(std::move(hints)),
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.getHints()._disableYieldingTolerantPlans = false;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(15, 20, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate predicates are repeated on the Seek side. Also demonstrate null handling, and the
    // fact that we apply the predicates on the Seek side in increasing selectivity order.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{pa}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [evalTemp_3]\n"
        "|   |   PathCompare [Eq] Const [2]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [pa]\n"
        "|   |   PathCompare [Lt] Const [1]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'a': pa, 'b': evalTemp_3}, c1]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {[Const [2"
        " | minKey], Const [2 | 1])}]\n",
        optimized);
}

TEST(PhysRewriter, PerfOnlyPreds2) {
    using namespace properties;

    ce::PartialSchemaSelHints hints;
    hints.emplace(PartialSchemaKey{"root", make<PathGet>("a", make<PathIdentity>())},
                  SelectivityType{0.001});
    hints.emplace(PartialSchemaKey{"root", make<PathGet>("b", make<PathIdentity>())},
                  SelectivityType{0.001});

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

    // TODO SERVER-71551 Follow up unit tests with overriden Cost Model.
    auto costModel = getTestCostModel();
    costModel.setSeekStartupCost(1e-6);
    costModel.setIndexScanStartupCost(1e-6);
    costModel.setMergeJoinStartupCost(1e-6);

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(
               {},
               {{"index1", makeIndexDefinition("a", CollationOp::Ascending, false /*isMultiKey*/)},
                {"index2",
                 makeIndexDefinition("b", CollationOp::Ascending, false /*isMultiKey*/)}})}}},
        makeHintedCE(std::move(hints)),
        std::move(costModel),
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.getHints()._disableYieldingTolerantPlans = false;
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(10, 17, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Demonstrate an intersection plan, with predicates repeated on the Seek side.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{pa}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [evalTemp_2]\n"
        "|   |   PathCompare [Eq] Const [2]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [pa]\n"
        "|   |   PathCompare [Eq] Const [1]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'a': pa, 'b': evalTemp_2}, c1]\n"
        "MergeJoin []\n"
        "|   |   |   Condition\n"
        "|   |   |       rid_0 = rid_5\n"
        "|   |   Collation\n"
        "|   |       Ascending\n"
        "|   Union [{rid_5}]\n"
        "|   Evaluation [{rid_5} = Variable [rid_0]]\n"
        "|   IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index2, interval: {=Const "
        "[2]}]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Const [1"
        "]}]\n",
        optimized);
}

TEST(PhysRewriter, ConjunctionTraverseMultikey1) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT root = NodeBuilder{}
                   .root("root")
                   .filter(_evalf(
                       // Start with conjunction of two traverses, over the same field.
                       // The two traverses don't have to match the same array element,
                       // so it's important not to combine them into one traverse.
                       _composem(_get("a", _traverse1(_get("x", _cmp("Eq", "1"_cint64)))),
                                 _get("a", _traverse1(_get("y", _cmp("Eq", "1"_cint64))))),
                       "root"_var))
                   .finish(_scan("root", "c1"));
    // Hint one predicate to be more selective than the other to ensure we have a predictable
    // outcome instead of a tie.
    ce::PartialSchemaSelHints hints;
    hints.emplace(PartialSchemaKey{"root", _get("a", _traverse1(_get("x", _id())))._n},
                  kDefaultSelectivity);
    hints.emplace(PartialSchemaKey{"root", _get("a", _traverse1(_get("y", _id())))._n},
                  kDefaultSelectivity * 0.1);

    auto phaseManager = makePhaseManager(
        {
            OptPhase::MemoSubstitutionPhase,
            OptPhase::MemoExplorationPhase,
            OptPhase::MemoImplementationPhase,
        },
        prefixId,
        Metadata{{{"c1",
                   createScanDef({},
                                 {{"index1",
                                   makeIndexDefinition(
                                       "a", CollationOp::Ascending, true /*isMultiKey*/)}})}}},
        makeHintedCE(std::move(hints)),
        boost::none /*costModel*/,
        DebugInfo{true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(root);
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(6, 10, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // We end up with a multikey index scan. Each row in the index is an array-element of 'a'.
    // We should not check (a conjunction of) both predicates on the same index scan,
    // because that forces the same array element to match both, which is stricter than
    // the original query.
    // But at the same time, the index should help satisfy one predicate or the other.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [evalTemp_10]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathGet [x]\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [1]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root, 'a': evalTemp_10}, c1]\n"
        "Unique [{rid_0}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_8]\n"
        "|   PathGet [y]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "IndexScan [{'<indexKey> 0': evalTemp_8, '<rid>': rid_0}, scanDefName: c1, indexDefName: "
        "index1, interval: {<fully open>}]\n",
        optimized);
}

TEST(PhysRewriter, ConjunctionTraverseMultikey2) {
    using namespace properties;
    auto prefixId = PrefixId::createForTests();

    ABT root = NodeBuilder{}
                   .root("root")
                   .filter(_evalf(
                       // Start with conjunction of two traverses, over the same field.
                       // The two traverses don't have to match the same array element,
                       // so it's important not to combine them into one traverse.
                       _composem(_get("a", _traverse1(_cmp("Eq", "1"_cint64))),
                                 _get("a", _traverse1(_get("x", _cmp("Eq", "1"_cint64))))),
                       "root"_var))
                   .finish(_scan("root", "c1"));

    auto phaseManager = makePhaseManager(
        {
            OptPhase::MemoSubstitutionPhase,
            OptPhase::MemoExplorationPhase,
            OptPhase::MemoImplementationPhase,
        },
        prefixId,
        Metadata{{{"c1",
                   createScanDef({},
                                 {{"index1",
                                   makeIndexDefinition(
                                       "a", CollationOp::Ascending, true /*isMultiKey*/)}})}}},
        {} /*costModel*/,
        DebugInfo{true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = std::move(root);
    phaseManager.optimize(optimized);
    ASSERT_BETWEEN(6, 10, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // If we use the index to satisfy {a: 1} then we can't also use it to satisfy {'a.x': 1},
    // because that would be forcing the same array element to match both predicates.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [evalTemp_5]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathGet [x]\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [1]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root, 'a': evalTemp_5}, c1]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {=Const [1"
        "]}]\n",
        optimized);
}

TEST(PhysRewriter, ExtractAllPlans) {
    using namespace properties;

    ABT rootNode = NodeBuilder{}
                       .root("root")
                       .filter(_evalf(_composem(_get("a", _traverse1(_cmp("Gt", "70"_cint64))),
                                                _get("a", _traverse1(_cmp("Lt", "90"_cint64)))),
                                      "root"_var))
                       .filter(_evalf(_get("b", _traverse1(_cmp("Eq", "1"_cint64))), "root"_var))
                       .finish(_scan("root", "c1"));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef({},
                         {{"index1",
                           IndexDefinition{{{makeIndexPath("b"), CollationOp::Ascending},
                                            {makeIndexPath("a"), CollationOp::Ascending}},
                                           true /*isMultiKey*/}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.getHints()._disableBranchAndBound = true;
    phaseManager.getHints()._keepRejectedPlans = true;
    auto plans = phaseManager.optimizeNoAssert(std::move(optimized), true /*includeRejected*/);
    ASSERT_EQ(22, plans.size());

    // Sort plans by estimated cost. If costs are equal, sort lexicographically by plan explain.
    // This allows us to break ties if costs are equal.
    std::sort(plans.begin(), plans.end(), [](const PlanAndProps& e1, const PlanAndProps& e2) {
        const auto c1 = e1.getRootAnnotation()._cost;
        const auto c2 = e2.getRootAnnotation()._cost;
        if (c1 < c2) {
            return true;
        }
        if (c2 < c1) {
            return false;
        }

        const auto explain1 = ExplainGenerator::explainV2(e1._node);
        const auto explain2 = ExplainGenerator::explainV2(e2._node);
        return explain1 < explain2;
    });

    const auto getExplainForPlan = [&plans](const size_t planId) -> std::string {
        return str::stream() << "Cost: " << plans.at(planId).getRootAnnotation()._cost.toString()
                             << "\n"
                             << ExplainGenerator::explainV2(plans.at(planId)._node);
    };

    // Display the 3 best plans.
    ASSERT_STR_EQ_AUTO(  // NOLINT
        "Cost: 0.0641986\n"
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [evalTemp_18]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Gt]\n"
        "|   |   Const [70]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root, 'a': evalTemp_18}, c1]\n"
        "Unique [{rid_0}]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {[Const [1 "
        "| minKey], Const [1 | 90])}]\n",
        getExplainForPlan(0));

    ASSERT_STR_EQ_AUTO(  // NOLINT
        "Cost: 0.0641986\n"
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [evalTemp_20]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Lt]\n"
        "|   |   Const [90]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root, 'a': evalTemp_20}, c1]\n"
        "Unique [{rid_0}]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {(Const [1 "
        "| 70], Const [1 | maxKey]]}]\n",
        getExplainForPlan(1));

    ASSERT_STR_EQ_AUTO(  // NOLINT
        "Cost: 0.0973208\n"
        "Root [{root}]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [evalTemp_16]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Gt]\n"
        "|   |   Const [70]\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [evalTemp_16]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Lt]\n"
        "|   |   Const [90]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'<root>': root, 'a': evalTemp_16}, c1]\n"
        "Unique [{rid_0}]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: c1, indexDefName: index1, interval: {[Const [1 "
        "| minKey], Const [1 | maxKey]]}]\n",
        getExplainForPlan(2));
}
}  // namespace
}  // namespace mongo::optimizer
