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
#include "mongo/db/query/optimizer/cascades/cost_derivation.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"


namespace mongo::optimizer {
namespace {

// Default selectivity of predicates used by HintedCE to force certain plans.
constexpr double kDefaultSelectivity = 0.1;

DEATH_TEST_REGEX(Optimizer, HitIterationLimitInrunStructuralPhases, "Tripwire assertion.*6808700") {
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("scanProjection", "testColl");
    ABT evalNode = make<EvaluationNode>("evalProj1", Constant::int64(5), std::move(scanNode));


    OptPhaseManager phaseManager(
        {OptPhase::PathFuse, OptPhase::ConstEvalPre},
        prefixId,
        {{{"test1", createScanDef({}, {})}, {"test2", createScanDef({}, {})}}},
        DebugInfo(true, DebugInfo::kDefaultDebugLevelForTests, 0));

    ASSERT_THROWS_CODE(phaseManager.optimize(evalNode), DBException, 6808700);
}

DEATH_TEST_REGEX(Optimizer,
                 LogicalWriterFailedToRewriteFixPointMemSubPhase,
                 "Tripwire assertion.*6808702") {
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


    OptPhaseManager phaseManager({OptPhase::MemoSubstitutionPhase},
                                 prefixId,
                                 {{{"test", createScanDef({}, {})}}},
                                 DebugInfo(true, DebugInfo::kDefaultDebugLevelForTests, 0));

    ASSERT_THROWS_CODE(phaseManager.optimize(rootNode), DBException, 6808702);
}

DEATH_TEST_REGEX(Optimizer,
                 LogicalWriterFailedToRewriteFixPointMemExpPhase,
                 "Tripwire assertion.*6808702") {
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


    OptPhaseManager phaseManager({OptPhase::MemoExplorationPhase},
                                 prefixId,
                                 {{{"test", createScanDef({}, {})}}},
                                 DebugInfo(true, DebugInfo::kDefaultDebugLevelForTests, 0));

    ASSERT_THROWS_CODE(phaseManager.optimize(rootNode), DBException, 6808702);
}

DEATH_TEST_REGEX(Optimizer, BadGroupID, "Tripwire assertion.*6808704") {
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


    OptPhaseManager phaseManager({OptPhase::MemoImplementationPhase},
                                 prefixId,
                                 {{{"test", createScanDef({}, {})}}},
                                 DebugInfo(true, DebugInfo::kDefaultDebugLevelForTests, 0));

    ASSERT_THROWS_CODE(phaseManager.optimize(rootNode), DBException, 6808704);
}

DEATH_TEST_REGEX(Optimizer, EnvHasFreeVariables, "Tripwire assertion.*6808711") {
    using namespace properties;
    PrefixId prefixId;

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
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"p3"}}, std::move(filter2Node));

    OptPhaseManager phaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"test", createScanDef({}, {})}}},
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ASSERT_THROWS_CODE(phaseManager.optimize(rootNode), DBException, 6808711);
}

DEATH_TEST_REGEX(Optimizer, FailedToRetrieveRID, "Tripwire assertion.*6808705") {
    using namespace properties;
    PrefixId prefixId;

    ABT scanNode = make<ScanNode>("root", "c1");

    ABT projectionANode = make<EvaluationNode>(
        "pa",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    ABT filterANode =
        make<FilterNode>(make<EvalFilter>(make<PathCompare>(Operations::Gt, Constant::int64(0)),
                                          make<Variable>("pa")),
                         std::move(projectionANode));

    ABT projectionBNode = make<EvaluationNode>(
        "pb",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("root")),
        std::move(filterANode));

    ABT filterBNode =
        make<FilterNode>(make<EvalFilter>(make<PathCompare>(Operations::Gt, Constant::int64(1)),
                                          make<Variable>("pb")),
                         std::move(projectionBNode));

    ABT groupByNode = make<GroupByNode>(ProjectionNameVector{"pa"},
                                        ProjectionNameVector{"pc"},
                                        makeSeq(make<Variable>("pb")),
                                        std::move(filterBNode));

    ABT rootNode =
        make<RootNode>(ProjectionRequirement{ProjectionNameVector{"pc"}}, std::move(groupByNode));

    OptPhaseManager phaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        true /*requireRID*/,
        {{{"c1",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{
                     {{makeNonMultikeyIndexPath("a"), CollationOp::Ascending}},
                     false /*isMultiKey*/,
                     {DistributionType::HashPartitioning, makeSeq(makeNonMultikeyIndexPath("a"))},
                     {}}}},
               ConstEval::constFold,
               {DistributionType::HashPartitioning, makeSeq(makeNonMultikeyIndexPath("b"))})}},
         5 /*numberOfPartitions*/},
        std::make_unique<HeuristicCE>(),
        std::make_unique<DefaultCosting>(),
        {} /*pathToInterval*/,
        ConstEval::constFold,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ASSERT_THROWS_CODE(phaseManager.optimize(rootNode), DBException, 6808705);
}

}  // namespace
}  // namespace mongo::optimizer
