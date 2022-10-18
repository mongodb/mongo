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
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/utils/unit_test_pipeline_utils.h"
#include "mongo/unittest/golden_test.h"


namespace mongo::optimizer {

unittest::GoldenTestConfig goldenTestConfigABTOptimization{"src/mongo/db/test_output/pipeline/abt"};

TEST(ABTTranslate, OptimizePipelineTests) {
    unittest::GoldenTestContext gctx(&goldenTestConfigABTOptimization);

    // TODO SERVER-70028: the $or and $in explains below should be equal after 70028 is complete.
    testABTTranslationAndOptimization(
        gctx,
        "optimized $match with $or: pipeline is able to use a SargableNode with a disjunction of "
        "point intervals.",
        "[{$match: {$or: [{a: 1}, {a: 2}, {a: 3}]}}]",
        "collection",
        {OptPhase::MemoSubstitutionPhase},
        {{{"collection",
           createScanDef({}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}})}}});

    testABTTranslationAndOptimization(
        gctx,
        "optimized $match with $in and a list of equalities becomes a comparison to an EqMember "
        "list.",
        "[{$match: {a: {$in: [1, 2, 3]}}}]",
        "collection",
        {OptPhase::MemoSubstitutionPhase},
        {{{"collection",
           createScanDef({}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}})}}});

    testABTTranslationAndOptimization(
        gctx,
        "optimized $project inclusion then $match: observe the Filter can be reordered "
        "against the Eval node",
        "[{$project: {a: 1, b: 1}}, {$match: {a: 2}}]",
        "collection",
        {OptPhase::ConstEvalPre,
         OptPhase::PathFuse,
         OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase});

    testABTTranslationAndOptimization(gctx,
                                      "optimized $match basic",
                                      "[{$match: {a: 1, b: 2}}]",
                                      "collection",
                                      {OptPhase::MemoSubstitutionPhase,
                                       OptPhase::MemoExplorationPhase,
                                       OptPhase::MemoImplementationPhase});

    testABTTranslationAndOptimization(
        gctx,
        "optimized $expr filter: make sure we have a single array constant for (1, 2, 'str', ...)",
        "[{$project: {a: {$filter: {input: [1, 2, 'str', {a: 2.0, b:'s'}, 3, 4], as: 'num', cond: "
        "{$and: [{$gte: ['$$num', 2]}, {$lte: ['$$num', 3]}]}}}}}]",
        "collection",
        {OptPhase::ConstEvalPre});

    testABTTranslationAndOptimization(
        gctx,
        "optimized $group local global",
        "[{$group: {_id: '$a', c: {$sum: '$b'}}}]",
        "collection",
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        {{{"collection",
           createScanDef({}, {}, ConstEval::constFold, {DistributionType::UnknownPartitioning})}},
         5 /*numberOfPartitions*/});

    testABTTranslationAndOptimization(gctx,
                                      "optimized $unwind then $sort",
                                      "[{$unwind: '$x'}, {$sort: {'x': 1}}]",
                                      "collection",
                                      OptPhaseManager::getAllRewritesSet());

    testABTTranslationAndOptimization(
        gctx,
        "optimized $match with index",
        "[{$match: {'a': 10}}]",
        "collection",
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        {{{"collection",
           createScanDef({}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}})}}});

    testABTTranslationAndOptimization(
        gctx,
        "optimized $match index covered",
        "[{$project: {_id: 0, a: 1}}, {$match: {'a': 10}}]",
        "collection",
        {OptPhase::ConstEvalPre,
         OptPhase::PathFuse,
         OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        {{{"collection",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending}}},
                                 false /*multiKey*/}}})}}});

    testABTTranslationAndOptimization(
        gctx,
        "optimized $match index covered, match then project",
        "[{$match: {'a': 10}}, {$project: {_id: 0, a: 1}}]",
        "collection",
        {OptPhase::ConstEvalPre,
         OptPhase::PathFuse,
         OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        {{{"collection",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending}}},
                                 false /*multiKey*/}}})}}});

    testABTTranslationAndOptimization(
        gctx,
        "optimized $match index covered, match on two indexed keys then project",
        "[{$match: {'a': 10, 'b': 20}}, {$project: {_id: 0, a: 1}}]",
        "collection",
        {OptPhase::ConstEvalPre,
         OptPhase::PathFuse,
         OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        {{{"collection",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                   {makeNonMultikeyIndexPath("b"), CollationOp::Ascending}}},
                                 false /*multiKey*/}}})}}});

    testABTTranslationAndOptimization(
        gctx,
        "optimized $match index covered, match on three indexed keys then project",
        "[{$match: {'a': 10, 'b': 20, 'c': 30}}, {$project: {_id: 0, a: 1, b: 1, c: 1}}]",
        "collection",
        {OptPhase::ConstEvalPre,
         OptPhase::PathFuse,
         OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        {{{"collection",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                   {makeNonMultikeyIndexPath("b"), CollationOp::Ascending},
                                   {makeNonMultikeyIndexPath("c"), CollationOp::Ascending}}},
                                 false /*multiKey*/}}})}}});

    testABTTranslationAndOptimization(
        gctx,
        "optimized $match index covered, inclusion project then match on three indexed keys",
        "[{$project: {_id: 0, a: 1, b: 1, c: 1}}, {$match: {'a': 10, 'b': 20, 'c': 30}}]",
        "collection",
        {OptPhase::ConstEvalPre,
         OptPhase::PathFuse,
         OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        {{{"collection",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                   {makeNonMultikeyIndexPath("b"), CollationOp::Ascending},
                                   {makeNonMultikeyIndexPath("c"), CollationOp::Ascending}}},
                                 false /*multiKey*/}}})}}});

    testABTTranslationAndOptimization(
        gctx,
        "optimized $match sort index",
        "[{$match: {'a': 10}}, {$sort: {'a': 1}}]",
        "collection",
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        {{{"collection",
           createScanDef({}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}})}}});

    testABTTranslationAndOptimization(
        gctx,
        "optimized range index",
        "[{$match: {'a': {$gt: 70, $lt: 90}}}]",
        "collection",
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        {{{"collection",
           createScanDef({}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}})}}},
        {},
        true);

    testABTTranslationAndOptimization(
        gctx,
        "optimized index on two keys",
        "[{$match: {'a': 2, 'b': 2}}]",
        "collection",
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        {{{"collection",
           createScanDef({},
                         {{"index1",
                           IndexDefinition{{{makeIndexPath("a"), CollationOp::Ascending},
                                            {makeIndexPath("b"), CollationOp::Ascending}},
                                           true /*multiKey*/}}})}}});

    testABTTranslationAndOptimization(
        gctx,
        "optimized index on one key",
        "[{$match: {'a': 2, 'b': 2}}]",
        "collection",
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase,
         OptPhase::ConstEvalPost},
        {{{"collection",
           createScanDef({}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}})}}});

    testABTTranslationAndOptimization(
        gctx,
        "optimized $group eval no inline: verify that \"b\" is not inlined in the group "
        "expression, but is coming from the physical scan",
        "[{$group: {_id: null, a: {$first: '$b'}}}]",
        "collection",
        OptPhaseManager::getAllRewritesSet());

    std::string scanDefA = "collA";
    std::string scanDefB = "collB";
    Metadata metadata{{{scanDefA, {}}, {scanDefB, {}}}};
    testABTTranslationAndOptimization(gctx,
                                      "optimized union",
                                      "[{$unionWith: 'collB'}, {$match: {_id: 1}}]",
                                      scanDefA,
                                      {OptPhase::MemoSubstitutionPhase,
                                       OptPhase::MemoExplorationPhase,
                                       OptPhase::MemoImplementationPhase},
                                      metadata,
                                      {},
                                      false,
                                      {{NamespaceString("a." + scanDefB), {}}});

    testABTTranslationAndOptimization(
        gctx,
        "optimized common expression elimination",
        "[{$project: {foo: {$add: ['$b', 1]}, bar: {$add: ['$b', 1]}}}]",
        "test",
        {OptPhase::ConstEvalPre},
        {{{"test", createScanDef({}, {})}}});

    testABTTranslationAndOptimization(
        gctx,
        "optimized group by dependency: demonstrate that \"c\" is set to the array size "
        "(not the array itself coming from the group)",
        "[{$group: {_id: {}, b: {$addToSet: '$a'}}}, {$project: "
        "{_id: 0, b: {$size: '$b'}}}, {$project: {_id: 0, c: '$b'}}]",
        "test",
        {OptPhase::ConstEvalPre,
         OptPhase::PathFuse,
         OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        {{{"test", createScanDef({}, {})}}});

    testABTTranslationAndOptimization(
        gctx,
        "optimized double $elemMatch",
        "[{$match: {a: {$elemMatch: {$gte: 5, $lte: 6}}, b: {$elemMatch: {$gte: 1, $lte: 3}}}}]",
        "test",
        {OptPhase::MemoSubstitutionPhase},
        {{{"test", createScanDef({}, {})}}},
        defaultConvertPathToInterval);
}

TEST(ABTTranslate, PartialIndex) {
    unittest::GoldenTestContext gctx(&goldenTestConfigABTOptimization);

    PrefixId prefixId;
    std::string scanDefName = "collection";
    ProjectionName scanProjName = prefixId.getNextId("scan");

    // By default the constant is translated as "int32".
    auto conversionResult = convertExprToPartialSchemaReq(
        make<EvalFilter>(
            make<PathGet>("b",
                          make<PathTraverse>(make<PathCompare>(Operations::Eq, Constant::int32(2)),
                                             PathTraverse::kSingleLevel)),
            make<Variable>(scanProjName)),
        true /*isFilterContext*/,
        {} /*pathToInterval*/);
    ASSERT_TRUE(conversionResult.has_value());
    ASSERT_FALSE(conversionResult->_retainPredicate);
    Metadata metadata = {
        {{scanDefName,
          createScanDef({},
                        {{"index1",
                          IndexDefinition{{{makeIndexPath("a"), CollationOp::Ascending}},
                                          true /*multiKey*/,
                                          {DistributionType::Centralized},
                                          std::move(conversionResult->_reqMap)}}})}}};

    testABTTranslationAndOptimization(
        gctx,
        "optimized partial index: the expression matches the pipeline",
        "[{$match: {'a': 3, 'b': 2}}]",
        scanDefName,
        OptPhaseManager::getAllRewritesSet(),
        metadata);

    testABTTranslationAndOptimization(
        gctx,
        "optimized partial index negative: the expression does not match the pipeline",
        "[{$match: {'a': 3, 'b': 3}}]",
        scanDefName,
        OptPhaseManager::getAllRewritesSet(),
        metadata);
}
}  // namespace mongo::optimizer
