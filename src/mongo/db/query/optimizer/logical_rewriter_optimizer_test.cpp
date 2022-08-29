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

#include "mongo/db/query/optimizer/cascades/ce_heuristic.h"
#include "mongo/db/query/optimizer/cascades/logical_props_derivation.h"
#include "mongo/db/query/optimizer/cascades/rewriter_rules.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo::optimizer {
namespace {

TEST(LogicalRewriter, RootNodeMerge) {
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("a", "test");
    ABT limitSkipNode1 =
        make<LimitSkipNode>(properties::LimitSkipRequirement(-1, 10), std::move(scanNode));
    ABT limitSkipNode2 =
        make<LimitSkipNode>(properties::LimitSkipRequirement(5, 0), std::move(limitSkipNode1));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"a"}},
                                  std::move(limitSkipNode2));

    ASSERT_EXPLAIN(
        "Root []\n"
        "  projections: \n"
        "    a\n"
        "  RefBlock: \n"
        "    Variable [a]\n"
        "  LimitSkip []\n"
        "    limitSkip:\n"
        "      limit: 5\n"
        "      skip: 0\n"
        "    LimitSkip []\n"
        "      limitSkip:\n"
        "        limit: (none)\n"
        "        skip: 10\n"
        "      Scan [test]\n"
        "        BindBlock:\n"
        "          [a]\n"
        "            Source []\n",
        rootNode);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase},
                                 prefixId,
                                 {{{"test", {{}, {}}}}},
                                 DebugInfo::kDefaultForTests);
    ABT rewritten = std::move(rootNode);
    ASSERT_TRUE(phaseManager.optimize(rewritten));

    ASSERT_EXPLAIN(
        "Root []\n"
        "  projections: \n"
        "    a\n"
        "  RefBlock: \n"
        "    Variable [a]\n"
        "  LimitSkip []\n"
        "    limitSkip:\n"
        "      limit: 5\n"
        "      skip: 10\n"
        "    Scan [test]\n"
        "      BindBlock:\n"
        "        [a]\n"
        "          Source []\n",
        rewritten);
}

TEST(LogicalRewriter, Memo) {
    using namespace cascades;
    using namespace properties;

    Metadata metadata{{{"test", {}}}};
    Memo memo(DebugInfo::kDefaultForTests,
              metadata,
              std::make_unique<DefaultLogicalPropsDerivation>(),
              std::make_unique<HeuristicCE>());

    ABT scanNode = make<ScanNode>("ptest", "test");
    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathConstant>(make<UnaryOp>(Operations::Neg, Constant::int64(1))),
                         make<Variable>("ptest")),
        std::move(scanNode));
    ABT evalNode = make<EvaluationNode>(
        "P1",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("ptest")),
        std::move(filterNode));

    NodeIdSet insertedNodeIds;
    const GroupIdType rootGroupId = memo.integrate(evalNode, {}, insertedNodeIds);
    ASSERT_EQ(2, rootGroupId);
    ASSERT_EQ(3, memo.getGroupCount());

    NodeIdSet expectedInsertedNodeIds = {{0, 0}, {1, 0}, {2, 0}};
    ASSERT_TRUE(insertedNodeIds == expectedInsertedNodeIds);

    ASSERT_EXPLAIN_MEMO(
        "Memo: \n"
        "    groupId: 0\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 1000\n"
        "    |   |       projections: \n"
        "    |   |           ptest\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest, scanDefName: test, eqPredsOnly]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Scan [test]\n"
        "    |               BindBlock:\n"
        "    |                   [ptest]\n"
        "    |                       Source []\n"
        "    physicalNodes: \n"
        "    groupId: 1\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 100\n"
        "    |   |       projections: \n"
        "    |   |           ptest\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest, scanDefName: test]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Filter []\n"
        "    |           |   EvalFilter []\n"
        "    |           |   |   Variable [ptest]\n"
        "    |           |   PathConstant []\n"
        "    |           |   UnaryOp [Neg]\n"
        "    |           |   Const [1]\n"
        "    |           MemoLogicalDelegator [groupId: 0]\n"
        "    physicalNodes: \n"
        "    groupId: 2\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 100\n"
        "    |   |       projections: \n"
        "    |   |           P1\n"
        "    |   |           ptest\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest, scanDefName: test]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Evaluation []\n"
        "    |           |   BindBlock:\n"
        "    |           |       [P1]\n"
        "    |           |           EvalPath []\n"
        "    |           |           |   Variable [ptest]\n"
        "    |           |           PathConstant []\n"
        "    |           |           Const [2]\n"
        "    |           MemoLogicalDelegator [groupId: 1]\n"
        "    physicalNodes: \n",
        memo);

    {
        // Try to insert into the memo again.
        NodeIdSet insertedNodeIds;
        const GroupIdType group = memo.integrate(evalNode, {}, insertedNodeIds);
        ASSERT_EQ(2, group);
        ASSERT_EQ(3, memo.getGroupCount());

        // Nothing was inserted.
        ASSERT_EQ(1, memo.getGroup(0)._logicalNodes.size());
        ASSERT_EQ(1, memo.getGroup(1)._logicalNodes.size());
        ASSERT_EQ(1, memo.getGroup(2)._logicalNodes.size());
    }

    // Insert a different tree, this time only scan and project.
    ABT scanNode1 = make<ScanNode>("ptest", "test");
    ABT evalNode1 = make<EvaluationNode>(
        "P1",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("ptest")),
        std::move(scanNode1));

    {
        NodeIdSet insertedNodeIds1;
        const GroupIdType rootGroupId1 = memo.integrate(evalNode1, {}, insertedNodeIds1);
        ASSERT_EQ(3, rootGroupId1);
        ASSERT_EQ(4, memo.getGroupCount());

        // Nothing was inserted in first 3 groups.
        ASSERT_EQ(1, memo.getGroup(0)._logicalNodes.size());
        ASSERT_EQ(1, memo.getGroup(1)._logicalNodes.size());
        ASSERT_EQ(1, memo.getGroup(2)._logicalNodes.size());
    }

    {
        const Group& group = memo.getGroup(3);
        ASSERT_EQ(1, group._logicalNodes.size());

        ASSERT_EXPLAIN(
            "Evaluation []\n"
            "  BindBlock:\n"
            "    [P1]\n"
            "      EvalPath []\n"
            "        PathConstant []\n"
            "          Const [2]\n"
            "        Variable [ptest]\n"
            "  MemoLogicalDelegator [groupId: 0]\n",
            group._logicalNodes.at(0));
    }
}

TEST(LogicalRewriter, FilterProjectRewrite) {
    using namespace properties;
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("ptest", "test");
    ABT collationNode = make<CollationNode>(
        CollationRequirement({{"ptest", CollationOp::Ascending}}), std::move(scanNode));
    ABT evalNode =
        make<EvaluationNode>("P1",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")),
                             std::move(collationNode));
    ABT filterNode = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("P1")),
                                      std::move(evalNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{{}}, std::move(filterNode));

    ASSERT_EXPLAIN(
        "Root []\n"
        "  projections: \n"
        "  RefBlock: \n"
        "  Filter []\n"
        "    EvalFilter []\n"
        "      PathIdentity []\n"
        "      Variable [P1]\n"
        "    Evaluation []\n"
        "      BindBlock:\n"
        "        [P1]\n"
        "          EvalPath []\n"
        "            PathIdentity []\n"
        "            Variable [ptest]\n"
        "      Collation []\n"
        "        collation: \n"
        "          ptest: Ascending\n"
        "        RefBlock: \n"
        "          Variable [ptest]\n"
        "        Scan [test]\n"
        "          BindBlock:\n"
        "            [ptest]\n"
        "              Source []\n",
        rootNode);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase},
                                 prefixId,
                                 {{{"test", {{}, {}}}}},
                                 DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    ASSERT_TRUE(phaseManager.optimize(latest));

    ASSERT_EXPLAIN(
        "Root []\n"
        "  projections: \n"
        "  RefBlock: \n"
        "  Collation []\n"
        "    collation: \n"
        "      ptest: Ascending\n"
        "    RefBlock: \n"
        "      Variable [ptest]\n"
        "    Filter []\n"
        "      EvalFilter []\n"
        "        PathIdentity []\n"
        "        Variable [P1]\n"
        "      Evaluation []\n"
        "        BindBlock:\n"
        "          [P1]\n"
        "            EvalPath []\n"
        "              PathIdentity []\n"
        "              Variable [ptest]\n"
        "        Scan [test]\n"
        "          BindBlock:\n"
        "            [ptest]\n"
        "              Source []\n",
        latest);
}

TEST(LogicalRewriter, FilterProjectComplexRewrite) {
    using namespace properties;
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT projection2Node = make<EvaluationNode>(
        "p2", make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")), std::move(scanNode));

    ABT projection3Node =
        make<EvaluationNode>("p3",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")),
                             std::move(projection2Node));

    ABT collationNode = make<CollationNode>(
        CollationRequirement({{"ptest", CollationOp::Ascending}}), std::move(projection3Node));

    ABT projection1Node =
        make<EvaluationNode>("p1",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")),
                             std::move(collationNode));

    ABT filter1Node = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("p1")),
                                       std::move(projection1Node));

    ABT filterScanNode = make<FilterNode>(
        make<EvalFilter>(make<PathIdentity>(), make<Variable>("ptest")), std::move(filter1Node));

    ABT filter2Node = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("p2")),
                                       std::move(filterScanNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{{}}, std::move(filter2Node));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   RefBlock: \n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [p2]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [ptest]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [p1]\n"
        "|   PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [p1]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Collation []\n"
        "|   |   collation: \n"
        "|   |       ptest: Ascending\n"
        "|   RefBlock: \n"
        "|       Variable [ptest]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [p3]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [p2]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        rootNode);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase},
                                 prefixId,
                                 {{{"test", {{}, {}}}}},
                                 DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    ASSERT_TRUE(phaseManager.optimize(latest));

    // Note: this assert depends on the order on which we consider rewrites.
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   RefBlock: \n"
        "Collation []\n"
        "|   |   collation: \n"
        "|   |       ptest: Ascending\n"
        "|   RefBlock: \n"
        "|       Variable [ptest]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [p2]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [ptest]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [p1]\n"
        "|   PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [p1]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [p3]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [p2]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, FilterProjectGroupRewrite) {
    using namespace properties;
    PrefixId prefixId;

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

    ABT filterANode = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("a")),
                                       std::move(groupByNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"c"}},
                                  std::move(filterANode));

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase},
                                 prefixId,
                                 {{{"test", {{}, {}}}}},
                                 DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    ASSERT_TRUE(phaseManager.optimize(latest));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       c\n"
        "|   RefBlock: \n"
        "|       Variable [c]\n"
        "GroupBy []\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [a]\n"
        "|   aggregations: \n"
        "|       [c]\n"
        "|           Variable [b]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [b]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [a]\n"
        "|   PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [a]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, FilterProjectUnwindRewrite) {
    using namespace properties;
    PrefixId prefixId;

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

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"a", "b"}},
                                  std::move(filterBNode));

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase},
                                 prefixId,
                                 {{{"test", {{}, {}}}}},
                                 DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    ASSERT_TRUE(phaseManager.optimize(latest));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       a\n"
        "|   |       b\n"
        "|   RefBlock: \n"
        "|       Variable [a]\n"
        "|       Variable [b]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [b]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [a]\n"
        "|   PathIdentity []\n"
        "Unwind []\n"
        "|   BindBlock:\n"
        "|       [a]\n"
        "|           Source []\n"
        "|       [a_pid]\n"
        "|           Source []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [b]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [a]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, FilterProjectExchangeRewrite) {
    using namespace properties;
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT projectionANode = make<EvaluationNode>(
        "a", make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")), std::move(scanNode));
    ABT projectionBNode =
        make<EvaluationNode>("b",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")),
                             std::move(projectionANode));

    ABT exchangeNode = make<ExchangeNode>(
        properties::DistributionRequirement({DistributionType::HashPartitioning, {"a"}}),
        std::move(projectionBNode));

    ABT filterANode = make<FilterNode>(make<EvalFilter>(make<PathIdentity>(), make<Variable>("a")),
                                       std::move(exchangeNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"a", "b"}},
                                  std::move(filterANode));

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase},
                                 prefixId,
                                 {{{"test", {{}, {}}}}},
                                 DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    ASSERT_TRUE(phaseManager.optimize(latest));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       a\n"
        "|   |       b\n"
        "|   RefBlock: \n"
        "|       Variable [a]\n"
        "|       Variable [b]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [b]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: HashPartitioning\n"
        "|   |           projections: \n"
        "|   |               a\n"
        "|   RefBlock: \n"
        "|       Variable [a]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [a]\n"
        "|   PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [a]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, UnwindCollationRewrite) {
    using namespace properties;
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT projectionANode = make<EvaluationNode>(
        "a", make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")), std::move(scanNode));
    ABT projectionBNode =
        make<EvaluationNode>("b",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest")),
                             std::move(projectionANode));

    // This collation node should stay below the unwind.
    ABT collationANode = make<CollationNode>(CollationRequirement({{"a", CollationOp::Ascending}}),
                                             std::move(projectionBNode));

    // This collation node should go above the unwind.
    ABT collationBNode = make<CollationNode>(CollationRequirement({{"b", CollationOp::Ascending}}),
                                             std::move(collationANode));

    ABT unwindNode =
        make<UnwindNode>("a", "a_pid", false /*retainNonArrays*/, std::move(collationBNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"a", "b"}},
                                  std::move(unwindNode));

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase},
                                 prefixId,
                                 {{{"test", {{}, {}}}}},
                                 DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    ASSERT_TRUE(phaseManager.optimize(latest));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       a\n"
        "|   |       b\n"
        "|   RefBlock: \n"
        "|       Variable [a]\n"
        "|       Variable [b]\n"
        "Collation []\n"
        "|   |   collation: \n"
        "|   |       b: Ascending\n"
        "|   RefBlock: \n"
        "|       Variable [b]\n"
        "Unwind []\n"
        "|   BindBlock:\n"
        "|       [a]\n"
        "|           Source []\n"
        "|       [a_pid]\n"
        "|           Source []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [b]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [a]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest]\n"
        "|           PathIdentity []\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, FilterUnionReorderSingleProjection) {
    PrefixId prefixId;
    ABT scanNode1 = make<ScanNode>("ptest1", "test1");
    ABT scanNode2 = make<ScanNode>("ptest2", "test2");
    // Create two eval nodes such that the two branches of the union share a projection.
    ABT evalNode1 =
        make<EvaluationNode>("pUnion",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest1")),
                             std::move(scanNode1));
    ABT evalNode2 =
        make<EvaluationNode>("pUnion",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest2")),
                             std::move(scanNode2));

    ABT unionNode = make<UnionNode>(ProjectionNameVector{"pUnion"}, makeSeq(evalNode1, evalNode2));

    ABT filter = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("a",
                          make<PathTraverse>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                             PathTraverse::kSingleLevel)),
            make<Variable>("pUnion")),
        std::move(unionNode));
    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"pUnion"}},
                                  std::move(filter));

    ABT latest = std::move(rootNode);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       pUnion\n"
        "|   RefBlock: \n"
        "|       Variable [pUnion]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pUnion]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion]\n"
        "|   |           Source []\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion]\n"
        "|   |           EvalPath []\n"
        "|   |           |   Variable [ptest2]\n"
        "|   |           PathIdentity []\n"
        "|   Scan [test2]\n"
        "|       BindBlock:\n"
        "|           [ptest2]\n"
        "|               Source []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [pUnion]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest1]\n"
        "|           PathIdentity []\n"
        "Scan [test1]\n"
        "    BindBlock:\n"
        "        [ptest1]\n"
        "            Source []\n",
        latest);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase},
                                 prefixId,
                                 {{{"test1", {{}, {}}}, {"test2", {{}, {}}}}},
                                 DebugInfo::kDefaultForTests);
    ASSERT_TRUE(phaseManager.optimize(latest));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       pUnion\n"
        "|   RefBlock: \n"
        "|       Variable [pUnion]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion]\n"
        "|   |           Source []\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [pUnion]\n"
        "|   |   PathGet [a]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [1]\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion]\n"
        "|   |           EvalPath []\n"
        "|   |           |   Variable [ptest2]\n"
        "|   |           PathIdentity []\n"
        "|   Scan [test2]\n"
        "|       BindBlock:\n"
        "|           [ptest2]\n"
        "|               Source []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pUnion]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [pUnion]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest1]\n"
        "|           PathIdentity []\n"
        "Scan [test1]\n"
        "    BindBlock:\n"
        "        [ptest1]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, MultipleFilterUnionReorder) {
    PrefixId prefixId;
    ABT scanNode1 = make<ScanNode>("ptest1", "test1");
    ABT scanNode2 = make<ScanNode>("ptest2", "test2");

    // Create multiple shared projections for each child.
    ABT pUnion11 =
        make<EvaluationNode>("pUnion1",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest1")),
                             std::move(scanNode1));
    ABT pUnion12 =
        make<EvaluationNode>("pUnion2",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest1")),
                             std::move(pUnion11));

    ABT pUnion21 =
        make<EvaluationNode>("pUnion1",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest2")),
                             std::move(scanNode2));
    ABT pUnion22 =
        make<EvaluationNode>("pUnion2",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest2")),
                             std::move(pUnion21));

    ABT unionNode =
        make<UnionNode>(ProjectionNameVector{"pUnion1", "pUnion2"}, makeSeq(pUnion12, pUnion22));

    // Create two filters, one for each of the two common projections.
    ABT filterUnion1 = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("a",
                          make<PathTraverse>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                             PathTraverse::kSingleLevel)),
            make<Variable>("pUnion1")),
        std::move(unionNode));
    ABT filterUnion2 = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("a",
                          make<PathTraverse>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                             PathTraverse::kSingleLevel)),
            make<Variable>("pUnion2")),
        std::move(filterUnion1));
    ABT rootNode = make<RootNode>(
        properties::ProjectionRequirement{ProjectionNameVector{"pUnion1", "pUnion2"}},
        std::move(filterUnion2));

    ABT latest = std::move(rootNode);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       pUnion1\n"
        "|   |       pUnion2\n"
        "|   RefBlock: \n"
        "|       Variable [pUnion1]\n"
        "|       Variable [pUnion2]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pUnion2]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pUnion1]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion1]\n"
        "|   |           Source []\n"
        "|   |       [pUnion2]\n"
        "|   |           Source []\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion2]\n"
        "|   |           EvalPath []\n"
        "|   |           |   Variable [ptest2]\n"
        "|   |           PathIdentity []\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion1]\n"
        "|   |           EvalPath []\n"
        "|   |           |   Variable [ptest2]\n"
        "|   |           PathIdentity []\n"
        "|   Scan [test2]\n"
        "|       BindBlock:\n"
        "|           [ptest2]\n"
        "|               Source []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [pUnion2]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest1]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [pUnion1]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest1]\n"
        "|           PathIdentity []\n"
        "Scan [test1]\n"
        "    BindBlock:\n"
        "        [ptest1]\n"
        "            Source []\n",
        latest);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase},
                                 prefixId,
                                 {{{"test1", {{}, {}}}, {"test2", {{}, {}}}}},
                                 DebugInfo::kDefaultForTests);
    ASSERT_TRUE(phaseManager.optimize(latest));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       pUnion1\n"
        "|   |       pUnion2\n"
        "|   RefBlock: \n"
        "|       Variable [pUnion1]\n"
        "|       Variable [pUnion2]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion1]\n"
        "|   |           Source []\n"
        "|   |       [pUnion2]\n"
        "|   |           Source []\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [pUnion2]\n"
        "|   |   PathGet [a]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [1]\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion2]\n"
        "|   |           EvalPath []\n"
        "|   |           |   Variable [ptest2]\n"
        "|   |           PathIdentity []\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [pUnion1]\n"
        "|   |   PathGet [a]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [1]\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [pUnion1]\n"
        "|   |           EvalPath []\n"
        "|   |           |   Variable [ptest2]\n"
        "|   |           PathIdentity []\n"
        "|   Scan [test2]\n"
        "|       BindBlock:\n"
        "|           [ptest2]\n"
        "|               Source []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pUnion2]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [pUnion2]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest1]\n"
        "|           PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [pUnion1]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [pUnion1]\n"
        "|           EvalPath []\n"
        "|           |   Variable [ptest1]\n"
        "|           PathIdentity []\n"
        "Scan [test1]\n"
        "    BindBlock:\n"
        "        [ptest1]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, FilterUnionUnionPushdown) {
    PrefixId prefixId;
    ABT scanNode1 = make<ScanNode>("ptest", "test1");
    ABT scanNode2 = make<ScanNode>("ptest", "test2");
    ABT unionNode = make<UnionNode>(ProjectionNameVector{"ptest"}, makeSeq(scanNode1, scanNode2));

    ABT scanNode3 = make<ScanNode>("ptest", "test3");
    ABT parentUnionNode =
        make<UnionNode>(ProjectionNameVector{"ptest"}, makeSeq(unionNode, scanNode3));

    ABT filter = make<FilterNode>(
        make<EvalFilter>(
            make<PathGet>("a",
                          make<PathTraverse>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                             PathTraverse::kSingleLevel)),
            make<Variable>("ptest")),
        std::move(parentUnionNode));
    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"ptest"}},
                                  std::move(filter));

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase},
                                 prefixId,
                                 {{{"test1", {{}, {}}}, {"test2", {{}, {}}}, {"test3", {{}, {}}}}},
                                 DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       ptest\n"
        "|   RefBlock: \n"
        "|       Variable [ptest]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [ptest]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [ptest]\n"
        "|   |           Source []\n"
        "|   Scan [test3]\n"
        "|       BindBlock:\n"
        "|           [ptest]\n"
        "|               Source []\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [ptest]\n"
        "|   |           Source []\n"
        "|   Scan [test2]\n"
        "|       BindBlock:\n"
        "|           [ptest]\n"
        "|               Source []\n"
        "Scan [test1]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);

    ASSERT_TRUE(phaseManager.optimize(latest));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       ptest\n"
        "|   RefBlock: \n"
        "|       Variable [ptest]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [ptest]\n"
        "|   |           Source []\n"
        "|   Sargable [Complete]\n"
        "|   |   |   |   |   |   requirementsMap: \n"
        "|   |   |   |   |   |       refProjection: ptest, path: 'PathGet [a] PathTraverse [1] "
        "PathIdentity []', intervals: {{{[Const [1], Const [1]]}}}\n"
        "|   |   |   |   |   candidateIndexes: \n"
        "|   |   |   |   scanParams: \n"
        "|   |   |   |       {'a': evalTemp_0}\n"
        "|   |   |   |           residualReqs: \n"
        "|   |   |   |               refProjection: evalTemp_0, path: 'PathTraverse [1] "
        "PathIdentity []', intervals: {{{[Const [1], Const [1]]}}}, entryIndex: 0\n"
        "|   |   |   BindBlock:\n"
        "|   |   RefBlock: \n"
        "|   |       Variable [ptest]\n"
        "|   Scan [test3]\n"
        "|       BindBlock:\n"
        "|           [ptest]\n"
        "|               Source []\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [ptest]\n"
        "|   |           Source []\n"
        "|   Sargable [Complete]\n"
        "|   |   |   |   |   |   requirementsMap: \n"
        "|   |   |   |   |   |       refProjection: ptest, path: 'PathGet [a] PathTraverse [1] "
        "PathIdentity []', intervals: {{{[Const [1], Const [1]]}}}\n"
        "|   |   |   |   |   candidateIndexes: \n"
        "|   |   |   |   scanParams: \n"
        "|   |   |   |       {'a': evalTemp_2}\n"
        "|   |   |   |           residualReqs: \n"
        "|   |   |   |               refProjection: evalTemp_2, path: 'PathTraverse [1] "
        "PathIdentity []', intervals: {{{[Const [1], Const [1]]}}}, entryIndex: 0\n"
        "|   |   |   BindBlock:\n"
        "|   |   RefBlock: \n"
        "|   |       Variable [ptest]\n"
        "|   Scan [test2]\n"
        "|       BindBlock:\n"
        "|           [ptest]\n"
        "|               Source []\n"
        "Sargable [Complete]\n"
        "|   |   |   |   |   requirementsMap: \n"
        "|   |   |   |   |       refProjection: ptest, path: 'PathGet [a] PathTraverse [1] "
        "PathIdentity []', intervals: {{{[Const [1], Const [1]]}}}\n"
        "|   |   |   |   candidateIndexes: \n"
        "|   |   |   scanParams: \n"
        "|   |   |       {'a': evalTemp_1}\n"
        "|   |   |           residualReqs: \n"
        "|   |   |               refProjection: evalTemp_1, path: 'PathTraverse [1] PathIdentity "
        "[]', intervals: {{{[Const [1], Const [1]]}}}, entryIndex: 0\n"
        "|   |   BindBlock:\n"
        "|   RefBlock: \n"
        "|       Variable [ptest]\n"
        "Scan [test1]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);
}

TEST(LogicalRewriter, UnionPreservesCommonLogicalProps) {
    ABT scanNode1 = make<ScanNode>("ptest1", "test1");
    ABT scanNode2 = make<ScanNode>("ptest2", "test2");
    ABT evalNode1 = make<EvaluationNode>(
        "a",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("ptest1")),
        std::move(scanNode1));

    ABT evalNode2 = make<EvaluationNode>(
        "a",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("ptest2")),
        std::move(scanNode2));
    ABT unionNode = make<UnionNode>(ProjectionNameVector{"a"}, makeSeq(evalNode1, evalNode2));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"a"}},
                                  std::move(unionNode));

    Metadata metadata{{{"test1",
                        ScanDefinition{{},
                                       {},
                                       {DistributionType::HashPartitioning,
                                        makeSeq(make<PathGet>("a", make<PathIdentity>()))}}},
                       {"test2",
                        ScanDefinition{{},
                                       {},
                                       {DistributionType::HashPartitioning,
                                        makeSeq(make<PathGet>("a", make<PathIdentity>()))}}}},
                      2};

    // Run the reordering rewrite such that the scan produces a hash partition.
    PrefixId prefixId;
    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase},
                                 prefixId,
                                 metadata,
                                 DebugInfo::kDefaultForTests);

    ABT optimized = rootNode;
    ASSERT_TRUE(phaseManager.optimize(optimized));

    ASSERT_EXPLAIN_MEMO(
        "Memo: \n"
        "    groupId: 0\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 1000\n"
        "    |   |       projections: \n"
        "    |   |           ptest1\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest1, scanDefName: test1, "
        "eqPredsOnly]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test1\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: UnknownPartitioning\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Scan [test1]\n"
        "    |               BindBlock:\n"
        "    |                   [ptest1]\n"
        "    |                       Source []\n"
        "    physicalNodes: \n"
        "    groupId: 1\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 1000\n"
        "    |   |           requirementCEs: \n"
        "    |   |               refProjection: ptest1, path: 'PathGet [a] PathIdentity []', ce: "
        "1000\n"
        "    |   |       projections: \n"
        "    |   |           a\n"
        "    |   |           ptest1\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest1, scanDefName: test1]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test1\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   |           distribution: \n"
        "    |   |               type: RoundRobin\n"
        "    |   |           distribution: \n"
        "    |   |               type: HashPartitioning\n"
        "    |   |                   projections: \n"
        "    |   |                       a\n"
        "    |   |           distribution: \n"
        "    |   |               type: UnknownPartitioning\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Sargable [Complete]\n"
        "    |           |   |   |   |   |   requirementsMap: \n"
        "    |           |   |   |   |   |       refProjection: ptest1, path: 'PathGet [a] "
        "PathIdentity []', boundProjection: a, intervals: {{{[Const [minKey], Const [maxKey]]}}}\n"
        "    |           |   |   |   |   candidateIndexes: \n"
        "    |           |   |   |   scanParams: \n"
        "    |           |   |   |       {'a': a}\n"
        "    |           |   |   BindBlock:\n"
        "    |           |   |       [a]\n"
        "    |           |   |           Source []\n"
        "    |           |   RefBlock: \n"
        "    |           |       Variable [ptest1]\n"
        "    |           MemoLogicalDelegator [groupId: 0]\n"
        "    physicalNodes: \n"
        "    groupId: 2\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 1000\n"
        "    |   |       projections: \n"
        "    |   |           ptest2\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 2, scanProjection: ptest2, scanDefName: test2, "
        "eqPredsOnly]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test2\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: UnknownPartitioning\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Scan [test2]\n"
        "    |               BindBlock:\n"
        "    |                   [ptest2]\n"
        "    |                       Source []\n"
        "    physicalNodes: \n"
        "    groupId: 3\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 1000\n"
        "    |   |           requirementCEs: \n"
        "    |   |               refProjection: ptest2, path: 'PathGet [a] PathIdentity []', ce: "
        "1000\n"
        "    |   |       projections: \n"
        "    |   |           a\n"
        "    |   |           ptest2\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 2, scanProjection: ptest2, scanDefName: test2]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test2\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   |           distribution: \n"
        "    |   |               type: RoundRobin\n"
        "    |   |           distribution: \n"
        "    |   |               type: HashPartitioning\n"
        "    |   |                   projections: \n"
        "    |   |                       a\n"
        "    |   |           distribution: \n"
        "    |   |               type: UnknownPartitioning\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Sargable [Complete]\n"
        "    |           |   |   |   |   |   requirementsMap: \n"
        "    |           |   |   |   |   |       refProjection: ptest2, path: 'PathGet [a] "
        "PathIdentity []', boundProjection: a, intervals: {{{[Const [minKey], Const [maxKey]]}}}\n"
        "    |           |   |   |   |   candidateIndexes: \n"
        "    |           |   |   |   scanParams: \n"
        "    |           |   |   |       {'a': a}\n"
        "    |           |   |   BindBlock:\n"
        "    |           |   |       [a]\n"
        "    |           |   |           Source []\n"
        "    |           |   RefBlock: \n"
        "    |           |       Variable [ptest2]\n"
        "    |           MemoLogicalDelegator [groupId: 2]\n"
        "    physicalNodes: \n"
        "    groupId: 4\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 2000\n"
        "    |   |       projections: \n"
        "    |   |           a\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test1\n"
        "    |   |           test2\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   |           distribution: \n"
        "    |   |               type: RoundRobin\n"
        "    |   |           distribution: \n"
        "    |   |               type: HashPartitioning\n"
        "    |   |                   projections: \n"
        "    |   |                       a\n"
        "    |   |           distribution: \n"
        "    |   |               type: UnknownPartitioning\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Union []\n"
        "    |           |   |   BindBlock:\n"
        "    |           |   |       [a]\n"
        "    |           |   |           Source []\n"
        "    |           |   MemoLogicalDelegator [groupId: 3]\n"
        "    |           MemoLogicalDelegator [groupId: 1]\n"
        "    physicalNodes: \n"
        "    groupId: 5\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 2000\n"
        "    |   |       projections: \n"
        "    |   |           a\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test1\n"
        "    |   |           test2\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   |           distribution: \n"
        "    |   |               type: RoundRobin\n"
        "    |   |           distribution: \n"
        "    |   |               type: HashPartitioning\n"
        "    |   |                   projections: \n"
        "    |   |                       a\n"
        "    |   |           distribution: \n"
        "    |   |               type: UnknownPartitioning\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Root []\n"
        "    |           |   |   projections: \n"
        "    |           |   |       a\n"
        "    |           |   RefBlock: \n"
        "    |           |       Variable [a]\n"
        "    |           MemoLogicalDelegator [groupId: 4]\n"
        "    physicalNodes: \n",
        phaseManager.getMemo());
}

ABT sargableCETestSetup() {
    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a", make<PathCompare>(Operations::Eq, Constant::int64(1))),
                         make<Variable>("ptest")),
        std::move(scanNode));
    ABT filterBNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("b", make<PathCompare>(Operations::Eq, Constant::int64(2))),
                         make<Variable>("ptest")),
        std::move(filterANode));

    return make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"ptest"}},
                          std::move(filterBNode));
}

TEST(LogicalRewriter, SargableCE) {
    using namespace properties;

    PrefixId prefixId;
    ABT rootNode = sargableCETestSetup();
    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase},
                                 prefixId,
                                 {{{"test", {{}, {}}}}},
                                 DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    ASSERT_TRUE(phaseManager.optimize(latest));

    // Displays SargableNode-specific per-key estimates.
    ASSERT_EXPLAIN_MEMO(
        "Memo: \n"
        "    groupId: 0\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 1000\n"
        "    |   |       projections: \n"
        "    |   |           ptest\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest, scanDefName: test, eqPredsOnly]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Scan [test]\n"
        "    |               BindBlock:\n"
        "    |                   [ptest]\n"
        "    |                       Source []\n"
        "    physicalNodes: \n"
        "    groupId: 1\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 5.62341\n"
        "    |   |           requirementCEs: \n"
        "    |   |               refProjection: ptest, path: 'PathGet [a] PathIdentity []', ce: "
        "31.6228\n"
        "    |   |               refProjection: ptest, path: 'PathGet [b] PathIdentity []', ce: "
        "31.6228\n"
        "    |   |       projections: \n"
        "    |   |           ptest\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest, scanDefName: test, eqPredsOnly]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Sargable [Complete]\n"
        "    |           |   |   |   |   |   requirementsMap: \n"
        "    |           |   |   |   |   |       refProjection: ptest, path: 'PathGet [a] "
        "PathIdentity []', intervals: {{{[Const [1], Const [1]]}}}\n"
        "    |           |   |   |   |   |       refProjection: ptest, path: 'PathGet [b] "
        "PathIdentity []', intervals: {{{[Const [2], Const [2]]}}}\n"
        "    |           |   |   |   |   candidateIndexes: \n"
        "    |           |   |   |   scanParams: \n"
        "    |           |   |   |       {'a': evalTemp_2, 'b': evalTemp_3}\n"
        "    |           |   |   |           residualReqs: \n"
        "    |           |   |   |               refProjection: evalTemp_2, path: 'PathIdentity "
        "[]', intervals: {{{[Const [1], Const [1]]}}}, entryIndex: 0\n"
        "    |           |   |   |               refProjection: evalTemp_3, path: 'PathIdentity "
        "[]', intervals: {{{[Const [2], Const [2]]}}}, entryIndex: 1\n"
        "    |           |   |   BindBlock:\n"
        "    |           |   RefBlock: \n"
        "    |           |       Variable [ptest]\n"
        "    |           MemoLogicalDelegator [groupId: 0]\n"
        "    physicalNodes: \n"
        "    groupId: 2\n"
        "    |   |   Logical properties:\n"
        "    |   |       cardinalityEstimate: \n"
        "    |   |           ce: 5.62341\n"
        "    |   |       projections: \n"
        "    |   |           ptest\n"
        "    |   |       indexingAvailability: \n"
        "    |   |           [groupId: 0, scanProjection: ptest, scanDefName: test, eqPredsOnly]\n"
        "    |   |       collectionAvailability: \n"
        "    |   |           test\n"
        "    |   |       distributionAvailability: \n"
        "    |   |           distribution: \n"
        "    |   |               type: Centralized\n"
        "    |   logicalNodes: \n"
        "    |       logicalNodeId: 0, rule: Root\n"
        "    |           Root []\n"
        "    |           |   |   projections: \n"
        "    |           |   |       ptest\n"
        "    |           |   RefBlock: \n"
        "    |           |       Variable [ptest]\n"
        "    |           MemoLogicalDelegator [groupId: 1]\n"
        "    physicalNodes: \n",
        phaseManager.getMemo());
}

TEST(LogicalRewriter, RemoveNoopFilter) {
    using namespace properties;
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT filterANode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a", make<PathCompare>(Operations::Gte, Constant::minKey())),
                         make<Variable>("ptest")),
        std::move(scanNode));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"ptest"}},
                                  std::move(filterANode));

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase},
                                 prefixId,
                                 {{{"test", {{}, {}}}}},
                                 DebugInfo::kDefaultForTests);
    ABT latest = std::move(rootNode);
    ASSERT_TRUE(phaseManager.optimize(latest));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       ptest\n"
        "|   RefBlock: \n"
        "|       Variable [ptest]\n"
        "Filter []\n"
        "|   Const [true]\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [ptest]\n"
        "            Source []\n",
        latest);
}

}  // namespace
}  // namespace mongo::optimizer
