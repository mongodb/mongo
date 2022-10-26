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

#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"


namespace mongo::optimizer {
namespace {

TEST(ReferenceTrackerTest, GetDefinitionsForScan) {
    // ABT is just a scan.
    ABT scanNode = make<ScanNode>("scanProjection", "testColl");

    auto env = VariableEnvironment::build(scanNode);
    ASSERT(!env.hasFreeVariables());

    // Check that the ScanNode originates a projection which is visible to ancestors in the ABT.
    auto scanProjs = env.getProjections(scanNode.ref());
    ProjectionNameSet expectedScanSet = {"scanProjection"};
    ASSERT(expectedScanSet == scanProjs);
    auto scanDefs = env.getDefinitions(scanNode.ref());
    ASSERT_EQ(scanDefs.size(), 1);
    ASSERT(scanDefs.find("scanProjection") != scanDefs.end());
    ASSERT(scanDefs.find("scanProjection")->second.definedBy == scanNode.ref());
}

TEST(ReferenceTrackerTest, GetDefinitionsForEval) {
    // ABT is a scan followed by an eval which creates a new projection.
    ABT scanNode = make<ScanNode>("scanProjection", "testColl");
    auto scanNodeRef = scanNode.ref();
    ABT evalNode = make<EvaluationNode>(
        "evalProjection",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("scanProjection")),
        std::move(scanNode));

    // Check that the eval node can reference "scanProjection".
    auto env = VariableEnvironment::build(evalNode);
    ASSERT(!env.hasFreeVariables());

    // Check that the EvalNode propagates up its child's projections and its own projection.
    auto evalProjs = env.getProjections(evalNode.ref());
    ProjectionNameSet expectedEvalSet = {"evalProjection", "scanProjection"};
    ASSERT(expectedEvalSet == evalProjs);
    auto evalDefs = env.getDefinitions(evalNode.ref());
    ASSERT_EQ(evalDefs.size(), 2);
    ASSERT(evalDefs.find("evalProjection") != evalDefs.end());
    ASSERT(evalDefs.find("evalProjection")->second.definedBy == evalNode.ref());
    ASSERT(evalDefs.find("scanProjection") != evalDefs.end());
    ASSERT(evalDefs.find("scanProjection")->second.definedBy == scanNodeRef);
}

DEATH_TEST_REGEX(ReferenceTrackerTest, BuildBadEnvForEval, "Tripwire assertion.*6624030") {
    // ABT is a scan followed by an eval which creates a new projection that overwrites the scan.
    ABT evalNode = make<EvaluationNode>(
        "scanProjection",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("scanProjection")),
        make<ScanNode>("scanProjection", "testColl"));

    // Check that the evaluation node cannot overwrite a projection from its child.
    ASSERT_THROWS_CODE(VariableEnvironment::build(evalNode), DBException, 6624030);
}

TEST(ReferenceTrackerTest, GetDefinitionsForGroup) {
    // ABT is a scan followed by two simple evals and a group which groups on one of the added
    // projections and aggregates on the other.
    ABT scanNode = make<ScanNode>("scanProjection", "testColl");
    ABT evalNode1 = make<EvaluationNode>("evalProj1", Constant::int64(5), std::move(scanNode));
    ABT evalNode2 = make<EvaluationNode>("evalProj2", Constant::int64(5), std::move(evalNode1));
    ABT groupByNode = make<GroupByNode>(ProjectionNameVector{"evalProj1"},
                                        ProjectionNameVector{"groupAggProj"},
                                        makeSeq(make<Variable>("evalProj2")),
                                        std::move(evalNode2));

    // The group does in fact resolve the free variables in its agg expressions.
    auto env = VariableEnvironment::build(groupByNode);
    ASSERT(!env.hasFreeVariables());

    // Check that the GroupNode only propagates its own projections (i.e., no evalProj2).
    auto groupProjs = env.getProjections(groupByNode.ref());
    ProjectionNameSet expectedGroupSet = {"evalProj1", "groupAggProj"};
    ASSERT(expectedGroupSet == groupProjs);
    auto groupDefs = env.getDefinitions(groupByNode.ref());
    ASSERT_EQ(groupDefs.size(), 2);
    ASSERT(groupDefs.find("evalProj1") != groupDefs.end());
    ASSERT(groupDefs.find("evalProj1")->second.definedBy == groupByNode.ref());
    ASSERT(groupDefs.find("groupAggProj") != groupDefs.end());
    ASSERT(groupDefs.find("groupAggProj")->second.definedBy == groupByNode.ref());
}

DEATH_TEST_REGEX(ReferenceTrackerTest, BuildBadEnvForGroup, "Tripwire assertion.*6624033") {
    // The same group as above but now it groups on a non-existent projection.
    ABT scanNode = make<ScanNode>("scanProjection", "testColl");
    ABT evalNode1 = make<EvaluationNode>("evalProj1", Constant::int64(5), std::move(scanNode));
    ABT evalNode2 = make<EvaluationNode>("evalProj2", Constant::int64(5), std::move(evalNode1));
    ABT groupByNode = make<GroupByNode>(ProjectionNameVector{"evalProjDoesNotExist"},
                                        ProjectionNameVector{"groupAggProj"},
                                        makeSeq(make<Variable>("evalProj2")),
                                        std::move(evalNode2));

    ASSERT_THROWS_CODE(VariableEnvironment::build(groupByNode), DBException, 6624033);
}

TEST(ReferenceTrackerTest, GetDefinitionsForFilter) {
    // ABT is a scan followed by an eval and then a filter.
    ABT scanNode = make<ScanNode>("scanProjection", "testColl");
    auto scanNodeRef = scanNode.ref();
    ABT evalNode = make<EvaluationNode>("evalProj", Constant::int64(5), std::move(scanNode));
    auto evalNodeRef = evalNode.ref();
    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathIdentity>(), make<Variable>("evalProj")), std::move(evalNode));

    // The filter resolves any variables it refers to.
    auto env = VariableEnvironment::build(filterNode);
    ASSERT(!env.hasFreeVariables());

    // Check that the FilterNode propagates up its children's projections without modification.
    auto filterProjs = env.getProjections(filterNode.ref());
    ProjectionNameSet expectedFilterSet = {"evalProj", "scanProjection"};
    ASSERT(expectedFilterSet == filterProjs);
    auto filterDefs = env.getDefinitions(filterNode.ref());
    ASSERT_EQ(filterDefs.size(), 2);
    ASSERT(filterDefs.find("evalProj") != filterDefs.end());
    ASSERT(filterDefs.find("evalProj")->second.definedBy == evalNodeRef);
    ASSERT(filterDefs.find("scanProjection") != filterDefs.end());
    ASSERT(filterDefs.find("scanProjection")->second.definedBy == scanNodeRef);
}

TEST(ReferenceTrackerTest, GetDefinitionsForLet) {
    // ABT is a scan followed by an eval node which contains a let expression.
    auto letNode =
        make<Let>("x",
                  Constant::int64(100),
                  make<BinaryOp>(Operations::Add, make<Variable>("x"), make<Variable>("scanProj")));
    auto letRef = letNode.ref();
    ABT evalNode = make<EvaluationNode>(
        "evalProj", std::move(letNode), make<ScanNode>("scanProj", "testColl"));

    // The let resolves references to its local variable and other variables with visible
    // definitions.
    auto env = VariableEnvironment::build(evalNode);
    ASSERT(!env.hasFreeVariables());

    // The let does not pass up its local definitions to ancestor nodes.
    ProjectionNameSet expectedProjSet = {"evalProj", "scanProj"};
    ASSERT(expectedProjSet == env.topLevelProjections());

    // But, the environment keeps the info about the definitions for all variables in the ABT. Check
    // that the local variable is defined by the Let.
    auto variables = VariableEnvironment::getVariables(evalNode);
    auto xVar = std::find_if(variables._variables.begin(),
                             variables._variables.end(),
                             [&](const Variable& var) { return var.name() == "x"; });
    ASSERT(env.isLastRef(*xVar));
    ASSERT(env.getDefinition(*xVar).definedBy == letRef);
}

TEST(ReferenceTrackerTest, BuildLetWithFreeVariable) {
    // Again, ABT is a scan followed by an eval node which contains a let expression. This time, the
    // let expression also refers to the projection being defined by the eval.
    auto letNode =
        make<Let>("x",
                  Constant::int64(100),
                  make<BinaryOp>(Operations::Add, make<Variable>("x"), make<Variable>("evalProj")));
    ABT evalNode = make<EvaluationNode>(
        "evalProj", std::move(letNode), make<ScanNode>("scanProj", "testColl"));

    // The "evalProj" referenced by the let correctly cannot be resolved.
    auto env = VariableEnvironment::build(evalNode);
    ASSERT(env.hasFreeVariables());
}

TEST(ReferenceTrackerTest, GetDefinitionsForUnion) {
    // ABT is a union of two eval nodes which have two projection names in common.
    ABT eval1 = make<EvaluationNode>(
        "pUnion2",
        make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest1")),
        make<EvaluationNode>("pUnion1",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest1")),
                             make<ScanNode>("ptest1", "test1")));
    ABT eval2 = make<EvaluationNode>(
        "pUnion2",
        make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest2")),
        make<EvaluationNode>("pUnion1",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("ptest2")),
                             make<ScanNode>("ptest2", "test2")));

    // We only union on one of the projections.
    ABT unionNode = make<UnionNode>(ProjectionNameVector{"pUnion1"}, makeSeq(eval1, eval2));

    auto env = VariableEnvironment::build(unionNode);
    ASSERT(!env.hasFreeVariables());

    // The union only propagates the specified projections.
    auto unionProjs = env.getProjections(unionNode.ref());
    ProjectionNameSet expectedSet = {"pUnion1"};
    ASSERT(expectedSet == unionProjs);
    auto unionDefs = env.getDefinitions(unionNode.ref());
    ASSERT_EQ(unionDefs.size(), 1);
    ASSERT(unionDefs.find("pUnion1") != unionDefs.end());
    ASSERT(unionDefs.find("pUnion1")->second.definedBy == unionNode.ref());
}

/**
 * The following tests cover more basic variable resolution behavior, specifically around free
 * variables. FilterNode is used as a running example of a node which exhibits on the default
 * behavior of passing up child projections and definitions unmodified.
 */

TEST(ReferenceTrackerTest, NoFreeVariables) {
    // There are no free variables when filtering on a variable defined previously.
    ABT filterNode = make<FilterNode>(make<Variable>("evalProj"),
                                      make<EvaluationNode>("evalProj",
                                                           Constant::int64(1),
                                                           make<ScanNode>("scanProj", "testColl")));
    auto env = VariableEnvironment::build(filterNode);
    ASSERT(!env.hasFreeVariables());
}

TEST(ReferenceTrackerTest, FreeVariablesNoMatchingDef) {
    // There are free variables when filtering on a variable not defined previously.
    ABT filterNode = make<FilterNode>(make<Variable>("otherProj"),
                                      make<EvaluationNode>("evalProj",
                                                           Constant::int64(1),
                                                           make<ScanNode>("scanProj", "testColl")));
    auto env = VariableEnvironment::build(filterNode);
    ASSERT(env.hasFreeVariables());
    ASSERT_EQ(env.freeOccurences("otherProj"), 1);
}

TEST(ReferenceTrackerTest, MultiFreeVariablesRecorded) {
    // There are free variables when filtering 2x on variables not defined previously, and both
    // free variables are recorded.
    ABT filterNode = make<FilterNode>(
        make<Variable>("otherProj2"),
        make<FilterNode>(make<Variable>("otherProj"),
                         make<EvaluationNode>("evalProj",
                                              Constant::int64(1),
                                              make<ScanNode>("scanProj", "testColl"))));
    auto env = VariableEnvironment::build(filterNode);
    ASSERT(env.hasFreeVariables());
    ASSERT_EQ(env.freeOccurences("otherProj"), 1);
    ASSERT_EQ(env.freeOccurences("otherProj2"), 1);
}

TEST(ReferenceTrackerTest, FreeVariablesOutOfOrderDef) {
    // There are free variables when filtering on a variable that is defined higher in the tree.
    ABT evalNode = make<EvaluationNode>(
        "evalProj",
        Constant::int64(1),
        make<FilterNode>(make<Variable>("evalProj"), make<ScanNode>("scanProj", "testColl")));
    auto env = VariableEnvironment::build(evalNode);
    ASSERT(env.hasFreeVariables());
    ASSERT_EQ(env.freeOccurences("evalProj"), 1);

    // And there are still free variables when an additional filter is included and resolved.
    ABT filterNode = make<FilterNode>(
        make<Variable>("evalProj"),
        make<EvaluationNode>(
            "evalProj",
            Constant::int64(1),
            make<FilterNode>(make<Variable>("evalProj"), make<ScanNode>("scanProj", "testColl"))));
    auto filterEnv = VariableEnvironment::build(filterNode);
    ASSERT(filterEnv.hasFreeVariables());
    ASSERT_EQ(filterEnv.freeOccurences("evalProj"), 1);

    // And there are more free variables when an additional filter is included and not resolved.
    ABT filterNode2 = make<FilterNode>(
        make<Variable>("evalProj2"),
        make<EvaluationNode>(
            "evalProj",
            Constant::int64(1),
            make<FilterNode>(make<Variable>("evalProj"), make<ScanNode>("scanProj", "testColl"))));
    auto env2 = VariableEnvironment::build(filterNode2);
    ASSERT(env2.hasFreeVariables());
    ASSERT_EQ(env2.freeOccurences("evalProj"), 1);
    ASSERT_EQ(env2.freeOccurences("evalProj2"), 1);
}

TEST(ReferenceTrackerTest, FreeVariablesReferenceGroupProjectedVar) {
    // There are no free variables when referencing a variable projected by a group.
    ABT filterNode = make<FilterNode>(
        make<Variable>("evalProj"),
        make<GroupByNode>(ProjectionNameVector{"evalProj"},
                          ProjectionNameVector{"groupAggProj"},
                          makeSeq(Constant::int64(10)),
                          make<EvaluationNode>("evalProj",
                                               Constant::int64(1),
                                               make<ScanNode>("scanProj", "testColl"))));
    auto env = VariableEnvironment::build(filterNode);
    ASSERT(!env.hasFreeVariables());
}

TEST(ReferenceTrackerTest, FreeVariablesReferenceGroupMaskedVar) {
    // There are free variables when referencing a variable masked by a group.
    ABT filterNode = make<FilterNode>(
        make<Variable>("evalProj2"),
        make<GroupByNode>(
            ProjectionNameVector{"evalProj1"},
            ProjectionNameVector{"groupAggProj"},
            makeSeq(Constant::int64(10)),
            make<EvaluationNode>("evalProj2",
                                 Constant::int64(1),
                                 make<EvaluationNode>("evalProj1",
                                                      Constant::int64(1),
                                                      make<ScanNode>("scanProj", "testColl")))));
    auto env = VariableEnvironment::build(filterNode);
    ASSERT(env.hasFreeVariables());
    ASSERT_EQ(env.freeOccurences("evalProj2"), 1);
}

TEST(ReferenceTrackerTest, FreeVariablesMultiResolvedAtOnce) {
    // There are no free variables when multiple variables can be resolved by a single definition.
    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("scanProj")),
        make<FilterNode>(
            make<EvalFilter>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("scanProj")),
            make<ScanNode>("scanProj", "testColl")));
    auto env = VariableEnvironment::build(filterNode);
    ASSERT(!env.hasFreeVariables());
}

TEST(ReferenceTrackerTest, FreeVariablesBinaryJoin) {
    // ABT is a binary join where each side creates some new projections and the join applies a
    // filter to the right side. Both the filter and the right side contain correlated variables.
    ABT scanNodeLeft = make<ScanNode>("scanProj1", "coll");
    ABT scanNodeRight = make<ScanNode>("scanProj2", "coll");
    ABT evalNodeLeft = make<EvaluationNode>(
        "evalProjA",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("scanProj1")),
        make<EvaluationNode>(
            "evalProjB",
            make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("scanProj1")),
            std::move(scanNodeLeft)));
    ABT evalNodeLeft1 = make<EvaluationNode>(
        "evalProjA1",
        make<EvalPath>(make<PathGet>("a1", make<PathIdentity>()), make<Variable>("scanProj1")),
        std::move(evalNodeLeft));

    // "evalProjA" needs to come from the left child and IS set to be correlated in the binary join
    // below.
    ABT evalNodeRight = make<EvaluationNode>(
        "evalProjC",
        make<EvalPath>(make<PathGet>("c", make<PathIdentity>()), make<Variable>("scanProj2")),
        make<EvaluationNode>("evalProjD",
                             make<EvalPath>(make<PathIdentity>(), make<Variable>("evalProjA")),
                             std::move(scanNodeRight)));

    // "evalProjA1" needs to come from the left child and IS NOT set to be correlated in the binary
    // join below.
    ABT evalNodeRight1 = make<EvaluationNode>(
        "evalProjC1",
        make<EvalPath>(make<PathGet>("c1", make<PathIdentity>()), make<Variable>("evalProjA1")),
        std::move(evalNodeRight));


    ABT joinNode = make<BinaryJoinNode>(
        JoinType::Inner,
        ProjectionNameSet{"evalProjA"},
        make<BinaryOp>(Operations::Eq, make<Variable>("evalProjA"), make<Variable>("evalProjC")),
        std::move(evalNodeLeft1),
        std::move(evalNodeRight1));

    // Check that the binary join resolves "evalProjA" but not "evalProjA1" in the right child and
    // the filter.
    auto env = VariableEnvironment::build(joinNode);
    ASSERT_EQ(env.freeOccurences("evalProjA1"), 1);

    // Check that the binary join node propagates up left and right projections.
    auto binaryProjs = env.getProjections(joinNode.ref());
    ProjectionNameSet expectedBinaryProjSet{"evalProjA",
                                            "evalProjA1",
                                            "evalProjB",
                                            "scanProj1",
                                            "evalProjC",
                                            "evalProjC1",
                                            "evalProjD",
                                            "scanProj2"};
    ASSERT(expectedBinaryProjSet == binaryProjs);
}

TEST(ReferenceTrackerTest, HashJoin) {
    ABT scanNodeLeft = make<ScanNode>("scanProj1", "coll");
    ABT scanNodeRight = make<ScanNode>("scanProj2", "coll");

    ABT evalNodeLeft = make<EvaluationNode>(
        "evalProjA",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("scanProj1")),
        std::move(scanNodeLeft));

    ABT evalNodeRight = make<EvaluationNode>(
        "evalProjB",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("scanProj1")),
        std::move(scanNodeRight));

    ABT joinNode = make<HashJoinNode>(JoinType::Inner,
                                      ProjectionNameVector{"evalProjA"},
                                      ProjectionNameVector{"evalProjB"},
                                      std::move(evalNodeLeft),
                                      std::move(evalNodeRight));

    auto env = VariableEnvironment::build(joinNode);
    ASSERT_EQ(env.freeOccurences("scanProj1"), 1);

    // Check that we propagate left and right projections.
    auto joinProjs = env.getProjections(joinNode.ref());
    ProjectionNameSet expectedProjSet{"evalProjA", "evalProjB", "scanProj1", "scanProj2"};
    ASSERT(expectedProjSet == joinProjs);
}

TEST(ReferenceTrackerTest, MergeJoin) {
    ABT scanNodeLeft = make<ScanNode>("scanProj1", "coll");
    ABT scanNodeRight = make<ScanNode>("scanProj2", "coll");

    ABT evalNodeLeft = make<EvaluationNode>(
        "evalProjA",
        make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("scanProj1")),
        std::move(scanNodeLeft));

    ABT evalNodeRight = make<EvaluationNode>(
        "evalProjB",
        make<EvalPath>(make<PathGet>("b", make<PathIdentity>()), make<Variable>("scanProj1")),
        std::move(scanNodeRight));

    ABT joinNode = make<MergeJoinNode>(ProjectionNameVector{"evalProjA"},
                                       ProjectionNameVector{"evalProjB"},
                                       std::vector<CollationOp>{CollationOp::Ascending},
                                       std::move(evalNodeLeft),
                                       std::move(evalNodeRight));

    auto env = VariableEnvironment::build(joinNode);
    ASSERT_EQ(env.freeOccurences("scanProj1"), 1);

    // Check that we propagate left and right projections.
    auto joinProjs = env.getProjections(joinNode.ref());
    ProjectionNameSet expectedProjSet{"evalProjA", "evalProjB", "scanProj1", "scanProj2"};
    ASSERT(expectedProjSet == joinProjs);
}

TEST(ReferenceTrackerTest, SingleVarNotLastRef) {
    // There are no last refs in an ABT that doesn't "finalize" any last refs.
    ABT justVar = make<Variable>("var");
    auto justVarEnv = VariableEnvironment::build(justVar);
    ASSERT_FALSE(justVarEnv.isLastRef(*justVar.cast<Variable>()));
}

TEST(ReferenceTrackerTest, LambdaMarksVarLastRef) {
    // There is a last ref in a basic lambda expression.
    ABT basicLambdaVar = make<Variable>("x");
    auto basicLambdaVarRef = basicLambdaVar.ref();
    auto basicLambda = make<LambdaAbstraction>(
        "x", make<BinaryOp>(Operations::Add, std::move(basicLambdaVar), Constant::int64(10)));
    auto basicLambdaEnv = VariableEnvironment::build(basicLambda);
    ASSERT(basicLambdaEnv.isLastRef(*basicLambdaVarRef.cast<Variable>()));
}

TEST(ReferenceTrackerTest, InMarksOnlyLocalVarLastRef) {
    // An 'in' result with multiple variables only marks the local variable as a last ref.
    ABT localVar = make<Variable>("localVar");
    auto localVarRef = localVar.ref();
    ABT otherVar = make<Variable>("otherProj");
    auto otherVarRef = otherVar.ref();
    ABT letNode =
        make<Let>("localVar",
                  Constant::int64(100),
                  make<BinaryOp>(Operations::Add, std::move(localVar), std::move(otherVar)));
    ABT evalNode = make<EvaluationNode>("letProj",
                                        std::move(letNode),
                                        make<EvaluationNode>("otherProj",
                                                             Constant::int64(100),
                                                             make<ScanNode>("scanProj1", "coll")));
    auto inEnv = VariableEnvironment::build(evalNode);
    ASSERT(!inEnv.hasFreeVariables());
    ASSERT(inEnv.isLastRef(*localVarRef.cast<Variable>()));
    ASSERT_FALSE(inEnv.isLastRef(*otherVarRef.cast<Variable>()));
}
}  // namespace
}  // namespace mongo::optimizer
