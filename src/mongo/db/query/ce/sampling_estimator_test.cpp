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
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/db/exec/sbe/abt/sbe_abt_test_util.h"
#include "mongo/db/pipeline/abt/document_source_visitor.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/query/ce/sampling_estimator.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/inline_auto_update.h"


namespace mongo::optimizer {
namespace {

using namespace unit_test_abt_literals;

OptPhaseManager makeTestOptPhaseManager(PrefixId& prefixId,
                                        Metadata metadata,
                                        OptPhaseManager phaseManagerForSampling,
                                        std::unique_ptr<ce::SamplingExecutor> executor,
                                        QueryParameterMap qp,
                                        OptimizerCounterInfo& optCounterInfo) {
    return OptPhaseManager{
        {{OptPhase::MemoSubstitutionPhase,
          OptPhase::MemoExplorationPhase,
          OptPhase::MemoImplementationPhase},
         kDefaultExplorationSet,
         kDefaultSubstitutionSet},
        prefixId,
        false /*requireRID*/,
        std::move(metadata),
        std::make_unique<ce::SamplingEstimator>(std::move(phaseManagerForSampling),
                                                1000 /*collectionSize*/,
                                                DebugInfo::kDefaultForTests,
                                                prefixId,
                                                makeHeuristicCE(),
                                                std::move(executor)),
        makeHeuristicCE(),
        makeCostEstimator(getTestCostModel()),
        defaultConvertPathToInterval,
        ConstEval::constFold,
        DebugInfo::kDefaultForTests,
        {} /*queryHints*/,
        std::move(qp),
        optCounterInfo};
}

TEST(SamplingEstimatorTest, SampleIndexedFields) {
    auto prefixId = PrefixId::createForTests();

    // Constructs a query which tests 'a.b' = 1 and 'a.c' = 1 where 'a.c' is indexed.
    ABT rootNode =
        NodeBuilder{}
            .root("root")
            .filter(_evalf(_get("a", _traverse1(_get("b", _traverse1(_cmp("Eq", "1"_cint64))))),
                           "root"_var))
            .filter(_evalf(_get("a", _traverse1(_get("c", _traverse1(_cmp("Eq", "1"_cint64))))),
                           "root"_var))
            .finish(_scan("root", "c1"));

    Metadata metadata{
        {{"c1",
          createScanDef(
              {},
              {{"index1",
                IndexDefinition{{{makeIndexPath(FieldPathType{"a", "c"}, true /*isMultiKey*/),
                                  CollationOp::Ascending}},
                                true /*isMultiKey*/}}})}}};

    QueryParameterMap qp;
    OptimizerCounterInfo optCounterInfo;
    // We are not lowering the paths.
    OptPhaseManager phaseManagerForSampling{{{OptPhase::MemoSubstitutionPhase,
                                              OptPhase::MemoExplorationPhase,
                                              OptPhase::MemoImplementationPhase},
                                             kDefaultExplorationSet,
                                             kDefaultSubstitutionSet},
                                            prefixId,
                                            false /*requireRID*/,
                                            metadata,
                                            makeHeuristicCE(),
                                            makeHeuristicCE(),
                                            makeCostEstimator(getTestCostModel()),
                                            defaultConvertPathToInterval,
                                            defaultConvertPathToInterval,
                                            DebugInfo::kDefaultForProd,
                                            {._sqrtSampleSizeEnabled = false},
                                            qp,
                                            optCounterInfo};

    // Used to record the sampling plans.
    ABTVector nodes;

    // Not optimizing fully.
    OptPhaseManager phaseManager = makeTestOptPhaseManager(prefixId,
                                                           metadata,
                                                           std::move(phaseManagerForSampling),
                                                           std::make_unique<ABTRecorder>(nodes),
                                                           qp,
                                                           optCounterInfo);

    PlanAndProps planAndProps = phaseManager.optimizeAndReturnProps(std::move(rootNode));

    ASSERT_EQ(1, nodes.size());

    // We have a single plan to sample the predicate with indexed field 'a.c'.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{sum}]\n"
        "GroupBy []\n"
        "|   aggregations: \n"
        "|       [sum]\n"
        "|           FunctionCall [$sum]\n"
        "|           Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_3]\n"
        "|   PathTraverse [1]\n"
        "|   PathGet [c]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 100, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'a': evalTemp_3}, c1]\n"
        "LimitSkip [limit: 10, skip: 0]\n"
        "PhysicalScan [{'<rid>': rid_0}, c1]\n",
        nodes.front());
}

TEST(SamplingEstimatorTest, DoNotSampleUnindexedFields) {
    auto prefixId = PrefixId::createForTests();

    // Constructs a query where none of the fields (both 'a.b' and 'a.c') is indexed.
    ABT rootNode =
        NodeBuilder{}
            .root("root")
            .filter(_evalf(_get("a", _traverse1(_get("b", _traverse1(_cmp("Eq", "1"_cint64))))),
                           "root"_var))
            .filter(_evalf(_get("a", _traverse1(_get("c", _traverse1(_cmp("Eq", "1"_cint64))))),
                           "root"_var))
            .finish(_scan("root", "c1"));

    Metadata metadata{{{"c1",
                        createScanDef({},
                                      {{"index1",
                                        makeIndexDefinition(
                                            "c", CollationOp::Ascending, true /*isMultiKey*/)}})}}};

    QueryParameterMap qp;
    OptimizerCounterInfo optCounterInfo;
    // We are not lowering the paths.
    OptPhaseManager phaseManagerForSampling{{{OptPhase::MemoSubstitutionPhase,
                                              OptPhase::MemoExplorationPhase,
                                              OptPhase::MemoImplementationPhase},
                                             kDefaultExplorationSet,
                                             kDefaultSubstitutionSet},
                                            prefixId,
                                            false /*requireRID*/,
                                            metadata,
                                            makeHeuristicCE(),
                                            makeHeuristicCE(),
                                            makeCostEstimator(getTestCostModel()),
                                            defaultConvertPathToInterval,
                                            defaultConvertPathToInterval,
                                            DebugInfo::kDefaultForProd,
                                            {._sqrtSampleSizeEnabled = false},
                                            qp,
                                            optCounterInfo};

    // Used to record the sampling plans.
    ABTVector nodes;

    // Not optimizing fully.
    OptPhaseManager phaseManager = makeTestOptPhaseManager(prefixId,
                                                           metadata,
                                                           std::move(phaseManagerForSampling),
                                                           std::make_unique<ABTRecorder>(nodes),
                                                           qp,
                                                           optCounterInfo);

    PlanAndProps planAndProps = phaseManager.optimizeAndReturnProps(std::move(rootNode));

    // There is no generated sampling plans as there is no indexed field in this query.
    ASSERT_EQ(0, nodes.size());
}

TEST_F(NodeSBE, SampleTwoPredicatesAtOnceTest) {
    auto prefixId = PrefixId::createForTests();
    const std::string scanDefName = "test";
    Metadata metadata{{{scanDefName,
                        createScanDef({},
                                      {{"index1",
                                        makeCompositeIndexDefinition(
                                            {{"a", CollationOp::Ascending, false},
                                             {"b", CollationOp::Ascending, false}})}})}}};
    auto opCtx = makeOperationContext();
    auto pipeline = parsePipeline("[{$match: {a: {$gte: 1}, b: {$gte: 1}}}]",
                                  NamespaceString::createNamespaceString_forTest("test"),
                                  opCtx.get());
    const ProjectionName scanProjName = prefixId.getNextId("scan");

    QueryParameterMap qp;
    OptimizerCounterInfo optCounterInfo;
    ABT tree = translatePipelineToABT(metadata,
                                      *pipeline.get(),
                                      scanProjName,
                                      make<ScanNode>(scanProjName, scanDefName),
                                      prefixId,
                                      qp);

    // We are not lowering the paths.
    OptPhaseManager phaseManagerForSampling{{{OptPhase::MemoSubstitutionPhase,
                                              OptPhase::MemoExplorationPhase,
                                              OptPhase::MemoImplementationPhase},
                                             kDefaultExplorationSet,
                                             kDefaultSubstitutionSet},
                                            prefixId,
                                            false /*requireRID*/,
                                            metadata,
                                            makeHeuristicCE(),
                                            makeHeuristicCE(),
                                            makeCostEstimator(getTestCostModel()),
                                            defaultConvertPathToInterval,
                                            defaultConvertPathToInterval,
                                            DebugInfo::kDefaultForProd,
                                            {._sqrtSampleSizeEnabled = false},
                                            qp,
                                            optCounterInfo};

    // Used to record the sampling plans.
    ABTVector nodes;

    // Not optimizing fully.
    OptPhaseManager phaseManager = makeTestOptPhaseManager(prefixId,
                                                           metadata,
                                                           std::move(phaseManagerForSampling),
                                                           std::make_unique<ABTRecorder>(nodes),
                                                           qp,
                                                           optCounterInfo);

    PlanAndProps planAndProps = phaseManager.optimizeAndReturnProps(std::move(tree));
    ASSERT_EQ(3, nodes.size());

    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{sum}]\n"
        "GroupBy []\n"
        "|   aggregations: \n"
        "|       [sum]\n"
        "|           FunctionCall [$sum]\n"
        "|           Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_15]\n"
        "|   PathComposeM []\n"
        "|   |   PathCompare [Lt]\n"
        "|   |   Const [\"\"]\n"
        "|   PathCompare [Gte]\n"
        "|   Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_14]\n"
        "|   PathComposeM []\n"
        "|   |   PathCompare [Lt]\n"
        "|   |   Const [\"\"]\n"
        "|   PathCompare [Gte]\n"
        "|   Const [1]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 100, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'a': evalTemp_15, 'b': evalTemp_14}, test]\n"
        "LimitSkip [limit: 10, skip: 0]\n"
        "PhysicalScan [{'<rid>': rid_0}, test]\n",
        nodes.front());

    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{sum}]\n"
        "GroupBy []\n"
        "|   aggregations: \n"
        "|       [sum]\n"
        "|           FunctionCall [$sum]\n"
        "|           Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_15]\n"
        "|   PathComposeM []\n"
        "|   |   PathCompare [Lt]\n"
        "|   |   Const [\"\"]\n"
        "|   PathCompare [Gte]\n"
        "|   Const [1]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 100, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'a': evalTemp_15}, test]\n"
        "LimitSkip [limit: 10, skip: 0]\n"
        "PhysicalScan [{'<rid>': rid_0}, test]\n",
        nodes.at(1));

    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{sum}]\n"
        "GroupBy []\n"
        "|   aggregations: \n"
        "|       [sum]\n"
        "|           FunctionCall [$sum]\n"
        "|           Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_14]\n"
        "|   PathComposeM []\n"
        "|   |   PathCompare [Lt]\n"
        "|   |   Const [\"\"]\n"
        "|   PathCompare [Gte]\n"
        "|   Const [1]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 100, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {'b': evalTemp_14}, test]\n"
        "LimitSkip [limit: 10, skip: 0]\n"
        "PhysicalScan [{'<rid>': rid_0}, test]\n",
        nodes.at(2));
}

TEST(SamplingEstimatorTest, SampleWithoutChunks) {
    auto prefixId = PrefixId::createForTests();

    // Constructs a query which tests 'a.b' = 1 and 'a.c' = 1 where 'a.c' is indexed.
    ABT rootNode =
        NodeBuilder{}
            .root("root")
            .filter(_evalf(_get("a", _traverse1(_get("b", _traverse1(_cmp("Eq", "1"_cint64))))),
                           "root"_var))
            .filter(_evalf(_get("a", _traverse1(_get("c", _traverse1(_cmp("Eq", "1"_cint64))))),
                           "root"_var))
            .finish(_scan("root", "c1"));

    Metadata metadata{
        {{"c1",
          createScanDef(
              {},
              {{"index1",
                IndexDefinition{{{makeIndexPath(FieldPathType{"a", "c"}, true /*isMultiKey*/),
                                  CollationOp::Ascending}},
                                true /*isMultiKey*/}}})}}};


    QueryParameterMap qp;
    OptimizerCounterInfo optCounterInfo;
    // We are not lowering the paths.
    OptPhaseManager phaseManagerForSampling{
        {{OptPhase::MemoSubstitutionPhase,
          OptPhase::MemoExplorationPhase,
          OptPhase::MemoImplementationPhase},
         kDefaultExplorationSet,
         kDefaultSubstitutionSet},
        prefixId,
        false /*requireRID*/,
        metadata,
        makeHeuristicCE(),
        makeHeuristicCE(),
        makeCostEstimator(getTestCostModel()),
        defaultConvertPathToInterval,
        defaultConvertPathToInterval,
        DebugInfo::kDefaultForProd,
        {._numSamplingChunks = 0, ._sqrtSampleSizeEnabled = false},
        qp,
        optCounterInfo};

    // Used to record the sampling plans.
    ABTVector nodes;

    // Not optimizing fully.
    OptPhaseManager phaseManager = makeTestOptPhaseManager(prefixId,
                                                           metadata,
                                                           std::move(phaseManagerForSampling),
                                                           std::make_unique<ABTRecorder>(nodes),
                                                           qp,
                                                           optCounterInfo);

    PlanAndProps planAndProps = phaseManager.optimizeAndReturnProps(std::move(rootNode));

    ASSERT_EQ(1, nodes.size());

    // We have a single plan to sample the predicate with indexed field 'a.c' without chunks. We
    // expect no rid projection at the physical scan node.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{sum}]\n"
        "GroupBy []\n"
        "|   aggregations: \n"
        "|       [sum]\n"
        "|           FunctionCall [$sum]\n"
        "|           Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_3]\n"
        "|   PathTraverse [1]\n"
        "|   PathGet [c]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "LimitSkip [limit: 1000, skip: 0]\n"
        "PhysicalScan [{'a': evalTemp_3}, c1]\n",
        nodes.front());
}

TEST(SamplingEstimatorTest, FilterNode) {
    auto prefixId = PrefixId::createForTests();

    // Constructs a query whose filter node cannot be converted to a sargable node.
    ABT rootNode = NodeBuilder{}
                       .root("root")
                       .filter(_binary("Gt", "5"_cint64, "0"_cint64))
                       .finish(_scan("root", "c1"));

    Metadata metadata{{{"c1", {}}}};

    QueryParameterMap qp;
    OptimizerCounterInfo optCounterInfo;
    // We are not lowering the paths.
    OptPhaseManager phaseManagerForSampling{{{OptPhase::MemoSubstitutionPhase,
                                              OptPhase::MemoExplorationPhase,
                                              OptPhase::MemoImplementationPhase},
                                             kDefaultExplorationSet,
                                             kDefaultSubstitutionSet},
                                            prefixId,
                                            false /*requireRID*/,
                                            metadata,
                                            makeHeuristicCE(),
                                            makeHeuristicCE(),
                                            makeCostEstimator(getTestCostModel()),
                                            defaultConvertPathToInterval,
                                            defaultConvertPathToInterval,
                                            DebugInfo::kDefaultForProd,
                                            {._sqrtSampleSizeEnabled = false},
                                            qp,
                                            optCounterInfo};
    // Enables sampling for filter node.
    phaseManagerForSampling.getHints()._forceSamplingCEFallBackForFilterNode = false;

    // Used to record the sampling plans.
    ABTVector nodes;

    // Not optimizing fully.
    OptPhaseManager phaseManager = makeTestOptPhaseManager(prefixId,
                                                           metadata,
                                                           std::move(phaseManagerForSampling),
                                                           std::make_unique<ABTRecorder>(nodes),
                                                           qp,
                                                           optCounterInfo);

    PlanAndProps planAndProps = phaseManager.optimizeAndReturnProps(std::move(rootNode));

    ASSERT_EQ(1, nodes.size());

    // We have a single plan to sample the filter node when sampling for filter node is enabled.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{sum}]\n"
        "GroupBy []\n"
        "|   aggregations: \n"
        "|       [sum]\n"
        "|           FunctionCall [$sum]\n"
        "|           Const [1]\n"
        "Filter []\n"
        "|   BinaryOp [Gt]\n"
        "|   |   Const [0]\n"
        "|   Const [5]\n"
        "NestedLoopJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 100, skip: 0]\n"
        "|   Seek [ridProjection: rid_0, {}, c1]\n"
        "LimitSkip [limit: 10, skip: 0]\n"
        "PhysicalScan [{'<rid>': rid_0}, c1]\n",
        nodes.front());
}

}  // namespace
}  // namespace mongo::optimizer
