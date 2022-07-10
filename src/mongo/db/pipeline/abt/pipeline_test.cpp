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

#include <boost/intrusive_ptr.hpp>

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using namespace optimizer;

TEST(ABTTranslate, MatchWithInEmptyList) {
    // A $match with $in and an empty equalities list should not match any documents.
    ABT emptyListIn = translatePipeline("[{$match: {a: {$in: []}}}]");
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathConstant []\n"
        "|   Const [false]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        emptyListIn);
}

TEST(ABTTranslate, MatchWithInSingletonList) {
    // A $match with $in and singleton equalities list should simplify to single equality.
    ABT singletonListIn = translatePipeline("[{$match: {a: {$in: [1]}}}]");
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        singletonListIn);
}

TEST(ABTTranslate, MatchWithInList) {
    // A $match with $in and a list of equalities becomes a series of nested comparisons.
    ABT listIn = translatePipeline("[{$match: {a: {$in: [1, 2, 3]}}}]");
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathComposeA []\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [3]\n"
        "|   PathComposeA []\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [2]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        listIn);
}

TEST(ABTTranslate, MatchWithInDuplicateElementsRemoved) {
    // A $match with $in and a list of equalities has the duplicates removed from the list.
    ABT listIn = translatePipeline("[{$match: {a: {$in: ['abc', 'def', 'ghi', 'def']}}}]");
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathComposeA []\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [\"ghi\"]\n"
        "|   PathComposeA []\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [\"def\"]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [\"abc\"]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        listIn);
}

TEST(ABTTranslate, EmptyElemMatch) {
    ABT emptyElemMatch = translatePipeline("[{$match: {'a': {$elemMatch: {}}}}]");
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathComposeM []\n"
        "|   |   PathArr []\n"
        "|   PathTraverse [1]\n"
        "|   PathComposeM []\n"
        "|   |   PathComposeA []\n"
        "|   |   |   PathArr []\n"
        "|   |   PathObj []\n"
        "|   PathConstant []\n"
        "|   Const [true]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        emptyElemMatch);
}

TEST(ABTTranslate, MatchWithElemMatchAndIn) {
    ABT elemMatchIn = translatePipeline("[{$match: {'a.b': {$elemMatch: {$in: [1, 2, 3]}}}}]");

    // The PathGet and PathTraverse operators interact correctly when $in is under $elemMatch.
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathGet [b]\n"
        "|   PathComposeM []\n"
        "|   |   PathArr []\n"
        "|   PathTraverse [1]\n"
        "|   PathComposeA []\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [3]\n"
        "|   PathComposeA []\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [2]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        elemMatchIn);
}

TEST(ABTTranslate, MatchWithOrConvertedToIn) {
    ABT orTranslated = translatePipeline("[{$match: {$or: [{a: 1}, {a: 2}, {a: 3}]}}]");
    ABT inTranslated = translatePipeline("[{$match: {a: {$in: [1, 2, 3]}}}]");

    PrefixId prefixId;
    std::string scanDefName = "collection";
    Metadata metadata = {
        {{scanDefName,
          ScanDefinition{{}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}}}}}};
    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase},
                                 prefixId,
                                 metadata,
                                 DebugInfo::kDefaultForTests);

    ASSERT_TRUE(phaseManager.optimize(orTranslated));
    ASSERT_TRUE(phaseManager.optimize(inTranslated));

    // Both pipelines are able to use a SargableNode with a disjunction of point intervals.
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Sargable [Complete]\n"
        "|   |   |   |   requirementsMap: \n"
        "|   |   |   |       refProjection: scan_0, path: 'PathGet [a] PathTraverse [1] "
        "PathIdentity []', intervals: {{{[Const [1], Const [1]]}} U {{[Const [2], Const [2]]}} U "
        "{{[Const [3], Const [3]]}}}\n"
        "|   |   |   candidateIndexes: \n"
        "|   |   |       candidateId: 1, index1, {}, {0}, {{{[Const [1], Const [1]]}} U {{[Const "
        "[2], Const [2]]}} U {{[Const [3], Const [3]]}}}\n"
        "|   |   BindBlock:\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        orTranslated);
    ASSERT(orTranslated == inTranslated);
}

TEST(ABTTranslate, SortLimitSkip) {
    ABT translated = translatePipeline(
        "[{$limit: 5}, "
        "{$skip: 3}, "
        "{$sort: {a: 1, b: -1}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Collation []\n"
        "|   |   collation: \n"
        "|   |       sort_0: Ascending\n"
        "|   |       sort_1: Descending\n"
        "|   RefBlock: \n"
        "|       Variable [sort_0]\n"
        "|       Variable [sort_1]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [sort_1]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [b]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [sort_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [a]\n"
        "|           PathIdentity []\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: (none)\n"
        "|       skip: 3\n"
        "LimitSkip []\n"
        "|   limitSkip:\n"
        "|       limit: 5\n"
        "|       skip: 0\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, ProjectRetain) {
    PrefixId prefixId;
    std::string scanDefName = "collection";
    Metadata metadata = {{{scanDefName, ScanDefinition{{}, {}}}}};
    ABT translated = translatePipeline(
        metadata, "[{$project: {a: 1, b: 1}}, {$match: {a: 2}}]", scanDefName, prefixId);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::ConstEvalPre,
                                  OptPhaseManager::OptPhase::PathFuse,
                                  OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase,
                                  OptPhaseManager::OptPhase::MemoImplementationPhase},
                                 prefixId,
                                 metadata,
                                 DebugInfo::kDefaultForTests);

    ABT optimized = translated;
    ASSERT_TRUE(phaseManager.optimize(optimized));

    // Observe the Filter can be reordered against the Eval node.
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Const [{}]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [b]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [fieldProj_2]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [a]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [fieldProj_1]\n"
        "|           PathField [_id]\n"
        "|           PathConstant []\n"
        "|           Variable [fieldProj_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [fieldProj_1]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [2]\n"
        "PhysicalScan [{'_id': fieldProj_0, 'a': fieldProj_1, 'b': fieldProj_2}, collection]\n"
        "    BindBlock:\n"
        "        [fieldProj_0]\n"
        "            Source []\n"
        "        [fieldProj_1]\n"
        "            Source []\n"
        "        [fieldProj_2]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, ProjectRetain1) {
    ABT translated = translatePipeline("[{$project: {a1: 1, a2: 1, a3: 1, a4: 1, a5: 1, a6: 1}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathKeep [_id, a1, a2, a3, a4, a5, a6]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, AddFields) {
    // Since '$z' is a single element, it will be considered a renamed path.
    ABT translated = translatePipeline("[{$addFields: {a: '$z'}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathComposeM []\n"
        "|           |   PathDefault []\n"
        "|           |   Const [{}]\n"
        "|           PathField [a]\n"
        "|           PathConstant []\n"
        "|           Variable [projRenamedPath_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [projRenamedPath_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [z]\n"
        "|           PathIdentity []\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, ProjectRenames) {
    // Since '$c' is a single element, it will be considered a renamed path.
    ABT translated = translatePipeline("[{$project: {'a.b': '$c'}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [a]\n"
        "|           |   PathTraverse [inf]\n"
        "|           |   PathComposeM []\n"
        "|           |   |   PathDefault []\n"
        "|           |   |   Const [{}]\n"
        "|           |   PathComposeM []\n"
        "|           |   |   PathField [b]\n"
        "|           |   |   PathConstant []\n"
        "|           |   |   Variable [projRenamedPath_0]\n"
        "|           |   PathKeep [b]\n"
        "|           PathKeep [_id, a]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [projRenamedPath_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [c]\n"
        "|           PathIdentity []\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, ProjectPaths) {
    ABT translated = translatePipeline("[{$project: {'a.b.c': '$x.y.z'}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [a]\n"
        "|           |   PathTraverse [inf]\n"
        "|           |   PathComposeM []\n"
        "|           |   |   PathField [b]\n"
        "|           |   |   PathTraverse [inf]\n"
        "|           |   |   PathComposeM []\n"
        "|           |   |   |   PathDefault []\n"
        "|           |   |   |   Const [{}]\n"
        "|           |   |   PathComposeM []\n"
        "|           |   |   |   PathField [c]\n"
        "|           |   |   |   PathConstant []\n"
        "|           |   |   |   Variable [projGetPath_0]\n"
        "|           |   |   PathKeep [c]\n"
        "|           |   PathKeep [b]\n"
        "|           PathKeep [_id, a]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [projGetPath_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [x]\n"
        "|           PathTraverse [inf]\n"
        "|           PathGet [y]\n"
        "|           PathTraverse [inf]\n"
        "|           PathGet [z]\n"
        "|           PathIdentity []\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, ProjectPaths1) {
    ABT translated = translatePipeline("[{$project: {'a.b':1, 'a.c':1, 'b':1}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [a]\n"
        "|           |   PathTraverse [inf]\n"
        "|           |   PathComposeM []\n"
        "|           |   |   PathKeep [b, c]\n"
        "|           |   PathObj []\n"
        "|           PathKeep [_id, a, b]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, ProjectInclusion) {
    ABT translated = translatePipeline("[{$project: {a: {$add: ['$c.d', 2]}, b: 1}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathComposeM []\n"
        "|           |   PathDefault []\n"
        "|           |   Const [{}]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [a]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [projGetPath_0]\n"
        "|           PathKeep [_id, a, b]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [projGetPath_0]\n"
        "|           BinaryOp [Add]\n"
        "|           |   Const [2]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [c]\n"
        "|           PathTraverse [inf]\n"
        "|           PathGet [d]\n"
        "|           PathIdentity []\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, ProjectExclusion) {
    ABT translated = translatePipeline("[{$project: {a: 0, b: 0}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathDrop [a, b]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, ProjectReplaceRoot) {
    ABT translated = translatePipeline("[{$replaceRoot: {newRoot: '$a'}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       newRoot_0\n"
        "|   RefBlock: \n"
        "|       Variable [newRoot_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [newRoot_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [a]\n"
        "|           PathIdentity []\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, MatchBasic) {
    PrefixId prefixId;
    std::string scanDefName = "collection";

    ABT translated = translatePipeline("[{$match: {a: 1, b: 2}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [b]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [2]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase,
                                  OptPhaseManager::OptPhase::MemoImplementationPhase},
                                 prefixId,
                                 {{{scanDefName, ScanDefinition{{}, {}}}}},
                                 DebugInfo::kDefaultForTests);

    ABT optimized = translated;
    ASSERT_TRUE(phaseManager.optimize(optimized));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_1]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [2]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_0]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'<root>': scan_0, 'a': evalTemp_0, 'b': evalTemp_1}, collection]\n"
        "    BindBlock:\n"
        "        [evalTemp_0]\n"
        "            Source []\n"
        "        [evalTemp_1]\n"
        "            Source []\n"
        "        [scan_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, MatchPath1) {
    ABT translated = translatePipeline("[{$match: {$expr: {$eq: ['$a', 1]}}}]");

    // Demonstrate simple path is converted to EvalFilter.
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, MatchPath2) {
    ABT translated = translatePipeline("[{$match: {$expr: {$eq: ['$a.b', 1]}}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathConstant []\n"
        "|   BinaryOp [Eq]\n"
        "|   |   Const [1]\n"
        "|   EvalPath []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [inf]\n"
        "|   PathGet [b]\n"
        "|   PathIdentity []\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, ElemMatchPath) {
    ABT translated = translatePipeline(
        "[{$project: {a: {$literal: [1, 2, 3, 4]}}}, {$match: {a: {$elemMatch: {$gte: 2, $lte: "
        "3}}}}]");

    // Observe type bracketing in the filter.
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [combinedProjection_0]\n"
        "|   PathGet [a]\n"
        "|   PathComposeM []\n"
        "|   |   PathArr []\n"
        "|   PathTraverse [1]\n"
        "|   PathComposeM []\n"
        "|   |   PathComposeM []\n"
        "|   |   |   PathCompare [Lt]\n"
        "|   |   |   Const [\"\"]\n"
        "|   |   PathCompare [Gte]\n"
        "|   |   Const [2]\n"
        "|   PathComposeM []\n"
        "|   |   PathCompare [Gte]\n"
        "|   |   Const [nan]\n"
        "|   PathCompare [Lte]\n"
        "|   Const [3]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathComposeM []\n"
        "|           |   PathDefault []\n"
        "|           |   Const [{}]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [a]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [projGetPath_0]\n"
        "|           PathKeep [_id, a]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [projGetPath_0]\n"
        "|           Const [[1, 2, 3, 4]]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, MatchProject) {
    ABT translated = translatePipeline(
        "[{$project: {s: {$add: ['$a', '$b']}, c: 1}}, "
        "{$match: {$or: [{c: 2}, {s: {$gte: 10}}]}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [combinedProjection_0]\n"
        "|   PathComposeA []\n"
        "|   |   PathGet [s]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathComposeM []\n"
        "|   |   |   PathCompare [Lt]\n"
        "|   |   |   Const [\"\"]\n"
        "|   |   PathCompare [Gte]\n"
        "|   |   Const [10]\n"
        "|   PathGet [c]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [2]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathComposeM []\n"
        "|           |   PathDefault []\n"
        "|           |   Const [{}]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [s]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [projGetPath_0]\n"
        "|           PathKeep [_id, c, s]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [projGetPath_0]\n"
        "|           BinaryOp [Add]\n"
        "|           |   EvalPath []\n"
        "|           |   |   Variable [scan_0]\n"
        "|           |   PathGet [b]\n"
        "|           |   PathIdentity []\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [a]\n"
        "|           PathIdentity []\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, ProjectComplex) {
    ABT translated = translatePipeline("[{$project: {'a1.b.c':1, 'a.b.c.d.e':'str'}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [a1]\n"
        "|           |   PathTraverse [inf]\n"
        "|           |   PathComposeM []\n"
        "|           |   |   PathField [b]\n"
        "|           |   |   PathTraverse [inf]\n"
        "|           |   |   PathComposeM []\n"
        "|           |   |   |   PathKeep [c]\n"
        "|           |   |   PathObj []\n"
        "|           |   PathComposeM []\n"
        "|           |   |   PathKeep [b]\n"
        "|           |   PathObj []\n"
        "|           PathComposeM []\n"
        "|           |   PathField [a]\n"
        "|           |   PathTraverse [inf]\n"
        "|           |   PathComposeM []\n"
        "|           |   |   PathField [b]\n"
        "|           |   |   PathTraverse [inf]\n"
        "|           |   |   PathComposeM []\n"
        "|           |   |   |   PathField [c]\n"
        "|           |   |   |   PathTraverse [inf]\n"
        "|           |   |   |   PathComposeM []\n"
        "|           |   |   |   |   PathField [d]\n"
        "|           |   |   |   |   PathTraverse [inf]\n"
        "|           |   |   |   |   PathComposeM []\n"
        "|           |   |   |   |   |   PathDefault []\n"
        "|           |   |   |   |   |   Const [{}]\n"
        "|           |   |   |   |   PathComposeM []\n"
        "|           |   |   |   |   |   PathField [e]\n"
        "|           |   |   |   |   |   PathConstant []\n"
        "|           |   |   |   |   |   Variable [projGetPath_0]\n"
        "|           |   |   |   |   PathKeep [e]\n"
        "|           |   |   |   PathKeep [d]\n"
        "|           |   |   PathKeep [c]\n"
        "|           |   PathKeep [b]\n"
        "|           PathKeep [_id, a, a1]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [projGetPath_0]\n"
        "|           Const [\"str\"]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, ExprFilter) {
    ABT translated = translatePipeline(
        "[{$project: {a: {$filter: {input: [1, 2, 'str', {a: 2.0, b:'s'}, 3, 4], as: 'num', cond: "
        "{$and: [{$gte: ['$$num', 2]}, {$lte: ['$$num', 3]}]}}}}}]");

    PrefixId prefixId;
    std::string scanDefName = "collection";
    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::ConstEvalPre},
                                 prefixId,
                                 {{{scanDefName, ScanDefinition{{}, {}}}}},
                                 DebugInfo::kDefaultForTests);

    ABT optimized = translated;
    ASSERT_TRUE(phaseManager.optimize(optimized));

    // Make sure we have a single array constant for (1, 2, 'str', ...).
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathComposeM []\n"
        "|           |   PathDefault []\n"
        "|           |   Const [{}]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [a]\n"
        "|           |   PathConstant []\n"
        "|           |   EvalPath []\n"
        "|           |   |   Const [[1, 2, \"str\", {\"a\" : 2, \"b\" : \"s\"}, 3, 4]]\n"
        "|           |   PathTraverse [inf]\n"
        "|           |   PathLambda []\n"
        "|           |   LambdaAbstraction [projGetPath_0_var_1]\n"
        "|           |   If []\n"
        "|           |   |   |   Const [Nothing]\n"
        "|           |   |   Variable [projGetPath_0_var_1]\n"
        "|           |   BinaryOp [And]\n"
        "|           |   |   BinaryOp [Gte]\n"
        "|           |   |   |   Const [2]\n"
        "|           |   |   Variable [projGetPath_0_var_1]\n"
        "|           |   BinaryOp [Lte]\n"
        "|           |   |   Const [3]\n"
        "|           |   Variable [projGetPath_0_var_1]\n"
        "|           PathKeep [_id, a]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, GroupBasic) {
    ABT translated =
        translatePipeline("[{$group: {_id: '$a.b', s: {$sum: {$multiply: ['$b', '$c']}}}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       agg_project_0\n"
        "|   RefBlock: \n"
        "|       Variable [agg_project_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [agg_project_0]\n"
        "|           EvalPath []\n"
        "|           |   Const [{}]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [s]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [s_agg_0]\n"
        "|           PathField [_id]\n"
        "|           PathConstant []\n"
        "|           Variable [groupByProj_0]\n"
        "GroupBy []\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [groupByProj_0]\n"
        "|   aggregations: \n"
        "|       [s_agg_0]\n"
        "|           FunctionCall [$sum]\n"
        "|           Variable [groupByInputProj_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [groupByInputProj_0]\n"
        "|           BinaryOp [Mult]\n"
        "|           |   EvalPath []\n"
        "|           |   |   Variable [scan_0]\n"
        "|           |   PathGet [c]\n"
        "|           |   PathIdentity []\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [b]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [groupByProj_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [a]\n"
        "|           PathTraverse [inf]\n"
        "|           PathGet [b]\n"
        "|           PathIdentity []\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, GroupLocalGlobal) {
    ABT translated = translatePipeline("[{$group: {_id: '$a', c: {$sum: '$b'}}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       agg_project_0\n"
        "|   RefBlock: \n"
        "|       Variable [agg_project_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [agg_project_0]\n"
        "|           EvalPath []\n"
        "|           |   Const [{}]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [c]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [c_agg_0]\n"
        "|           PathField [_id]\n"
        "|           PathConstant []\n"
        "|           Variable [groupByProj_0]\n"
        "GroupBy []\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [groupByProj_0]\n"
        "|   aggregations: \n"
        "|       [c_agg_0]\n"
        "|           FunctionCall [$sum]\n"
        "|           Variable [groupByInputProj_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [groupByInputProj_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [b]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [groupByProj_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [a]\n"
        "|           PathIdentity []\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);

    PrefixId prefixId;
    std::string scanDefName = "collection";
    OptPhaseManager phaseManager(
        {OptPhaseManager::OptPhase::MemoSubstitutionPhase,
         OptPhaseManager::OptPhase::MemoExplorationPhase,
         OptPhaseManager::OptPhase::MemoImplementationPhase},
        prefixId,
        {{{scanDefName, ScanDefinition{{}, {}, {DistributionType::UnknownPartitioning}}}},
         5 /*numberOfPartitions*/},
        DebugInfo::kDefaultForTests);

    ABT optimized = std::move(translated);
    ASSERT_TRUE(phaseManager.optimize(optimized));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       agg_project_0\n"
        "|   RefBlock: \n"
        "|       Variable [agg_project_0]\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: Centralized\n"
        "|   RefBlock: \n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [agg_project_0]\n"
        "|           EvalPath []\n"
        "|           |   Const [{}]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [c]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [c_agg_0]\n"
        "|           PathField [_id]\n"
        "|           PathConstant []\n"
        "|           Variable [groupByProj_0]\n"
        "GroupBy [Global]\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [groupByProj_0]\n"
        "|   aggregations: \n"
        "|       [c_agg_0]\n"
        "|           FunctionCall [$sum]\n"
        "|           Variable [preagg_0]\n"
        "Exchange []\n"
        "|   |   distribution: \n"
        "|   |       type: HashPartitioning\n"
        "|   |           projections: \n"
        "|   |               groupByProj_0\n"
        "|   RefBlock: \n"
        "|       Variable [groupByProj_0]\n"
        "GroupBy [Local]\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [groupByProj_0]\n"
        "|   aggregations: \n"
        "|       [preagg_0]\n"
        "|           FunctionCall [$sum]\n"
        "|           Variable [groupByInputProj_0]\n"
        "PhysicalScan [{'a': groupByProj_0, 'b': groupByInputProj_0}, collection, parallel]\n"
        "    BindBlock:\n"
        "        [groupByInputProj_0]\n"
        "            Source []\n"
        "        [groupByProj_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, UnwindBasic) {
    ABT translated = translatePipeline("[{$unwind: {path: '$a.b.c'}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       embedProj_0\n"
        "|   RefBlock: \n"
        "|       Variable [embedProj_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [embedProj_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathField [a]\n"
        "|           PathTraverse [inf]\n"
        "|           PathField [b]\n"
        "|           PathTraverse [inf]\n"
        "|           PathField [c]\n"
        "|           PathConstant []\n"
        "|           Variable [unwoundProj_0]\n"
        "Unwind []\n"
        "|   BindBlock:\n"
        "|       [unwoundPid_0]\n"
        "|           Source []\n"
        "|       [unwoundProj_0]\n"
        "|           Source []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [unwoundProj_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [a]\n"
        "|           PathGet [b]\n"
        "|           PathGet [c]\n"
        "|           PathIdentity []\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, UnwindComplex) {
    ABT translated = translatePipeline(
        "[{$unwind: {path: '$a.b.c', includeArrayIndex: 'p1.pid', preserveNullAndEmptyArrays: "
        "true}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       embedPidProj_0\n"
        "|   RefBlock: \n"
        "|       Variable [embedPidProj_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [embedPidProj_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [embedProj_0]\n"
        "|           PathField [p1]\n"
        "|           PathField [pid]\n"
        "|           PathConstant []\n"
        "|           If []\n"
        "|           |   |   Const [null]\n"
        "|           |   Variable [unwoundPid_0]\n"
        "|           BinaryOp [Gte]\n"
        "|           |   Const [0]\n"
        "|           Variable [unwoundPid_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [embedProj_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathField [a]\n"
        "|           PathTraverse [inf]\n"
        "|           PathField [b]\n"
        "|           PathTraverse [inf]\n"
        "|           PathField [c]\n"
        "|           PathLambda []\n"
        "|           LambdaAbstraction [unwoundLambdaVarName_0]\n"
        "|           If []\n"
        "|           |   |   Variable [unwoundLambdaVarName_0]\n"
        "|           |   Variable [unwoundProj_0]\n"
        "|           BinaryOp [Gte]\n"
        "|           |   Const [0]\n"
        "|           Variable [unwoundPid_0]\n"
        "Unwind [retainNonArrays]\n"
        "|   BindBlock:\n"
        "|       [unwoundPid_0]\n"
        "|           Source []\n"
        "|       [unwoundProj_0]\n"
        "|           Source []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [unwoundProj_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [a]\n"
        "|           PathGet [b]\n"
        "|           PathGet [c]\n"
        "|           PathIdentity []\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, UnwindAndGroup) {
    ABT translated = translatePipeline(
        "[{$unwind:{path: '$a.b', preserveNullAndEmptyArrays: true}}, "
        "{$group:{_id: '$a.b'}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       agg_project_0\n"
        "|   RefBlock: \n"
        "|       Variable [agg_project_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [agg_project_0]\n"
        "|           EvalPath []\n"
        "|           |   Const [{}]\n"
        "|           PathField [_id]\n"
        "|           PathConstant []\n"
        "|           Variable [groupByProj_0]\n"
        "GroupBy []\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [groupByProj_0]\n"
        "|   aggregations: \n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [groupByProj_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [embedProj_0]\n"
        "|           PathGet [a]\n"
        "|           PathTraverse [inf]\n"
        "|           PathGet [b]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [embedProj_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathField [a]\n"
        "|           PathTraverse [inf]\n"
        "|           PathField [b]\n"
        "|           PathLambda []\n"
        "|           LambdaAbstraction [unwoundLambdaVarName_0]\n"
        "|           If []\n"
        "|           |   |   Variable [unwoundLambdaVarName_0]\n"
        "|           |   Variable [unwoundProj_0]\n"
        "|           BinaryOp [Gte]\n"
        "|           |   Const [0]\n"
        "|           Variable [unwoundPid_0]\n"
        "Unwind [retainNonArrays]\n"
        "|   BindBlock:\n"
        "|       [unwoundPid_0]\n"
        "|           Source []\n"
        "|       [unwoundProj_0]\n"
        "|           Source []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [unwoundProj_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [a]\n"
        "|           PathGet [b]\n"
        "|           PathIdentity []\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, UnwindSort) {
    ABT translated = translatePipeline("[{$unwind: '$x'}, {$sort: {'x': 1}}]");

    PrefixId prefixId;
    std::string scanDefName = "collection";
    OptPhaseManager phaseManager(OptPhaseManager::getAllRewritesSet(),
                                 prefixId,
                                 {{{scanDefName, ScanDefinition{{}, {}}}}},
                                 DebugInfo::kDefaultForTests);

    ABT optimized = translated;
    ASSERT_TRUE(phaseManager.optimize(optimized));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       embedProj_0\n"
        "|   RefBlock: \n"
        "|       Variable [embedProj_0]\n"
        "Collation []\n"
        "|   |   collation: \n"
        "|   |       sort_0: Ascending\n"
        "|   RefBlock: \n"
        "|       Variable [sort_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [sort_0]\n"
        "|           Variable [unwoundProj_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [embedProj_0]\n"
        "|           If []\n"
        "|           |   |   Variable [scan_0]\n"
        "|           |   FunctionCall [setField]\n"
        "|           |   |   |   Variable [unwoundProj_0]\n"
        "|           |   |   Const [\"x\"]\n"
        "|           |   Variable [scan_0]\n"
        "|           BinaryOp [Or]\n"
        "|           |   FunctionCall [isObject]\n"
        "|           |   Variable [scan_0]\n"
        "|           FunctionCall [exists]\n"
        "|           Variable [unwoundProj_0]\n"
        "Unwind []\n"
        "|   BindBlock:\n"
        "|       [unwoundPid_0]\n"
        "|           Source []\n"
        "|       [unwoundProj_0]\n"
        "|           Source []\n"
        "PhysicalScan [{'<root>': scan_0, 'x': unwoundProj_0}, collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n"
        "        [unwoundProj_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, MatchIndex) {
    PrefixId prefixId;
    std::string scanDefName = "collection";

    Metadata metadata = {
        {{scanDefName,
          ScanDefinition{{}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}}}}}};
    ABT translated = translatePipeline(metadata, "[{$match: {'a': 10}}]", scanDefName, prefixId);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase,
                                  OptPhaseManager::OptPhase::MemoImplementationPhase},
                                 prefixId,
                                 metadata,
                                 DebugInfo::kDefaultForTests);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [10]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);

    ABT optimized = std::move(translated);
    ASSERT_TRUE(phaseManager.optimize(optimized));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "BinaryJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip []\n"
        "|   |   limitSkip:\n"
        "|   |       limit: 1\n"
        "|   |       skip: 0\n"
        "|   Seek [ridProjection: rid_0, {'<root>': scan_0}, collection]\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           Source []\n"
        "|   RefBlock: \n"
        "|       Variable [rid_0]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: collection, indexDefName: index1, interval: "
        "{[Const [10], Const [10]]}]\n"
        "    BindBlock:\n"
        "        [rid_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, MatchIndexCovered) {
    PrefixId prefixId;
    std::string scanDefName = "collection";

    Metadata metadata = {
        {{scanDefName,
          ScanDefinition{
              {},
              {{"index1",
                IndexDefinition{{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending}}},
                                false /*multiKey*/}}}}}}};
    ABT translated = translatePipeline(
        metadata, "[{$project: {_id: 0, a: 1}}, {$match: {'a': 10}}]", scanDefName, prefixId);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::ConstEvalPre,
                                  OptPhaseManager::OptPhase::PathFuse,
                                  OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase,
                                  OptPhaseManager::OptPhase::MemoImplementationPhase},
                                 prefixId,
                                 metadata,
                                 DebugInfo::kDefaultForTests);

    ABT optimized = std::move(translated);
    ASSERT_TRUE(phaseManager.optimize(optimized));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Const [{}]\n"
        "|           PathField [a]\n"
        "|           PathConstant []\n"
        "|           Variable [fieldProj_0]\n"
        "IndexScan [{'<indexKey> 0': fieldProj_0}, scanDefName: collection, indexDefName: index1, "
        "interval: {[Const [10], Const [10]]}]\n"
        "    BindBlock:\n"
        "        [fieldProj_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, MatchIndexCovered1) {
    PrefixId prefixId;
    std::string scanDefName = "collection";

    Metadata metadata = {
        {{scanDefName,
          ScanDefinition{
              {},
              {{"index1",
                IndexDefinition{{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending}}},
                                false /*multiKey*/}}}}}}};
    ABT translated = translatePipeline(
        metadata, "[{$match: {'a': 10}}, {$project: {_id: 0, a: 1}}]", scanDefName, prefixId);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::ConstEvalPre,
                                  OptPhaseManager::OptPhase::PathFuse,
                                  OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase,
                                  OptPhaseManager::OptPhase::MemoImplementationPhase},
                                 prefixId,
                                 metadata,
                                 DebugInfo::kDefaultForTests);

    ABT optimized = std::move(translated);
    ASSERT_TRUE(phaseManager.optimize(optimized));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Const [{}]\n"
        "|           PathField [a]\n"
        "|           PathConstant []\n"
        "|           Variable [fieldProj_0]\n"
        "IndexScan [{'<indexKey> 0': fieldProj_0}, scanDefName: collection, indexDefName: index1, "
        "interval: {[Const [10], Const [10]]}]\n"
        "    BindBlock:\n"
        "        [fieldProj_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, MatchIndexCovered2) {
    PrefixId prefixId;
    std::string scanDefName = "collection";

    Metadata metadata = {
        {{scanDefName,
          ScanDefinition{
              {},
              {{"index1",
                IndexDefinition{{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("b"), CollationOp::Ascending}}},
                                false /*multiKey*/}}}}}}};
    ABT translated = translatePipeline(metadata,
                                       "[{$match: {'a': 10, 'b': 20}}, {$project: {_id: 0, a: 1}}]",
                                       scanDefName,
                                       prefixId);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::ConstEvalPre,
                                  OptPhaseManager::OptPhase::PathFuse,
                                  OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase,
                                  OptPhaseManager::OptPhase::MemoImplementationPhase},
                                 prefixId,
                                 metadata,
                                 DebugInfo::kDefaultForTests);

    ABT optimized = std::move(translated);
    ASSERT_TRUE(phaseManager.optimize(optimized));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Const [{}]\n"
        "|           PathField [a]\n"
        "|           PathConstant []\n"
        "|           Variable [fieldProj_0]\n"
        "IndexScan [{'<indexKey> 0': fieldProj_0}, scanDefName: collection, indexDefName: index1, "
        "interval: {[Const [10], Const [10]], [Const [20], Const [20]]}]\n"
        "    BindBlock:\n"
        "        [fieldProj_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, MatchIndexCovered3) {
    PrefixId prefixId;
    std::string scanDefName = "collection";

    Metadata metadata = {
        {{scanDefName,
          ScanDefinition{
              {},
              {{"index1",
                IndexDefinition{{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("b"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("c"), CollationOp::Ascending}}},
                                false /*multiKey*/}}}}}}};
    ABT translated = translatePipeline(
        metadata,
        "[{$match: {'a': 10, 'b': 20, 'c': 30}}, {$project: {_id: 0, a: 1, b: 1, c: 1}}]",
        scanDefName,
        prefixId);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::ConstEvalPre,
                                  OptPhaseManager::OptPhase::PathFuse,
                                  OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase,
                                  OptPhaseManager::OptPhase::MemoImplementationPhase},
                                 prefixId,
                                 metadata,
                                 DebugInfo::kDefaultForTests);

    ABT optimized = std::move(translated);
    ASSERT_TRUE(phaseManager.optimize(optimized));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Const [{}]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [c]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [fieldProj_2]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [b]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [fieldProj_1]\n"
        "|           PathField [a]\n"
        "|           PathConstant []\n"
        "|           Variable [fieldProj_0]\n"
        "IndexScan [{'<indexKey> 0': fieldProj_0, '<indexKey> 1': fieldProj_1, '<indexKey> 2': "
        "fieldProj_2}, scanDefName: collection, indexDefName: index1, interval: {[Const [10], "
        "Const [10]], [Const [20], Const [20]], [Const [30], Const [30]]}]\n"
        "    BindBlock:\n"
        "        [fieldProj_0]\n"
        "            Source []\n"
        "        [fieldProj_1]\n"
        "            Source []\n"
        "        [fieldProj_2]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, MatchIndexCovered4) {
    PrefixId prefixId;
    std::string scanDefName = "collection";

    Metadata metadata = {
        {{scanDefName,
          ScanDefinition{
              {},
              {{"index1",
                IndexDefinition{{{{makeNonMultikeyIndexPath("a"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("b"), CollationOp::Ascending},
                                  {makeNonMultikeyIndexPath("c"), CollationOp::Ascending}}},
                                false /*multiKey*/}}}}}}};
    ABT translated = translatePipeline(
        metadata,
        "[{$project: {_id: 0, a: 1, b: 1, c: 1}}, {$match: {'a': 10, 'b': 20, 'c': 30}}]",
        scanDefName,
        prefixId);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::ConstEvalPre,
                                  OptPhaseManager::OptPhase::PathFuse,
                                  OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase,
                                  OptPhaseManager::OptPhase::MemoImplementationPhase},
                                 prefixId,
                                 metadata,
                                 DebugInfo::kDefaultForTests);

    ABT optimized = std::move(translated);
    ASSERT_TRUE(phaseManager.optimize(optimized));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Const [{}]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [c]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [fieldProj_2]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [b]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [fieldProj_1]\n"
        "|           PathField [a]\n"
        "|           PathConstant []\n"
        "|           Variable [fieldProj_0]\n"
        "IndexScan [{'<indexKey> 0': fieldProj_0, '<indexKey> 1': fieldProj_1, '<indexKey> 2': "
        "fieldProj_2}, scanDefName: collection, indexDefName: index1, interval: {[Const [10], "
        "Const [10]], [Const [20], Const [20]], [Const [30], Const [30]]}]\n"
        "    BindBlock:\n"
        "        [fieldProj_0]\n"
        "            Source []\n"
        "        [fieldProj_1]\n"
        "            Source []\n"
        "        [fieldProj_2]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, MatchSortIndex) {
    PrefixId prefixId;
    std::string scanDefName = "collection";

    Metadata metadata = {
        {{scanDefName,
          ScanDefinition{{}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}}}}}};
    ABT translated = translatePipeline(
        metadata, "[{$match: {'a': 10}}, {$sort: {'a': 1}}]", scanDefName, prefixId);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase,
                                  OptPhaseManager::OptPhase::MemoImplementationPhase},
                                 prefixId,
                                 metadata,
                                 DebugInfo::kDefaultForTests);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Collation []\n"
        "|   |   collation: \n"
        "|   |       sort_0: Ascending\n"
        "|   RefBlock: \n"
        "|       Variable [sort_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [sort_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [a]\n"
        "|           PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [10]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);

    ABT optimized = std::move(translated);
    ASSERT_TRUE(phaseManager.optimize(optimized));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Collation []\n"
        "|   |   collation: \n"
        "|   |       sort_0: Ascending\n"
        "|   RefBlock: \n"
        "|       Variable [sort_0]\n"
        "BinaryJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip []\n"
        "|   |   limitSkip:\n"
        "|   |       limit: 1\n"
        "|   |       skip: 0\n"
        "|   Seek [ridProjection: rid_0, {'<root>': scan_0, 'a': sort_0}, collection]\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           Source []\n"
        "|   |       [sort_0]\n"
        "|   |           Source []\n"
        "|   RefBlock: \n"
        "|       Variable [rid_0]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: collection, indexDefName: index1, interval: "
        "{[Const [10], Const [10]]}]\n"
        "    BindBlock:\n"
        "        [rid_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, RangeIndex) {
    PrefixId prefixId;
    std::string scanDefName = "collection";
    Metadata metadata = {
        {{scanDefName,
          ScanDefinition{{}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}}}}}};
    ABT translated =
        translatePipeline(metadata, "[{$match: {'a': {$gt: 70, $lt: 90}}}]", scanDefName, prefixId);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathComposeM []\n"
        "|   |   PathCompare [Gte]\n"
        "|   |   Const [nan]\n"
        "|   PathCompare [Lt]\n"
        "|   Const [90]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathComposeM []\n"
        "|   |   PathCompare [Lt]\n"
        "|   |   Const [\"\"]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [70]\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase,
                                  OptPhaseManager::OptPhase::MemoImplementationPhase},
                                 prefixId,
                                 metadata,
                                 DebugInfo::kDefaultForTests);

    // Demonstrate we can get an intersection plan, even though it might not be the best one under
    // the heuristic CE.
    phaseManager.getHints()._disableScan = true;

    ABT optimized = std::move(translated);
    ASSERT_TRUE(phaseManager.optimize(optimized));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "BinaryJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip []\n"
        "|   |   limitSkip:\n"
        "|   |       limit: 1\n"
        "|   |       skip: 0\n"
        "|   Seek [ridProjection: rid_0, {'<root>': scan_0}, collection]\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           Source []\n"
        "|   RefBlock: \n"
        "|       Variable [rid_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   FunctionCall [getArraySize]\n"
        "|   |   Variable [sides_0]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [2]\n"
        "GroupBy []\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [rid_0]\n"
        "|   aggregations: \n"
        "|       [sides_0]\n"
        "|           FunctionCall [$addToSet]\n"
        "|           Variable [sideId_0]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [rid_0]\n"
        "|   |           Source []\n"
        "|   |       [sideId_0]\n"
        "|   |           Source []\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [sideId_0]\n"
        "|   |           Const [1]\n"
        "|   IndexScan [{'<rid>': rid_0}, scanDefName: collection, indexDefName: index1, interval: "
        "{[Const [nan], Const [90])}]\n"
        "|       BindBlock:\n"
        "|           [rid_0]\n"
        "|               Source []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [sideId_0]\n"
        "|           Const [0]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: collection, indexDefName: index1, interval: "
        "{(Const [70], Const [\"\"])}]\n"
        "    BindBlock:\n"
        "        [rid_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, Index1) {
    {
        PrefixId prefixId;
        std::string scanDefName = "collection";
        Metadata metadata = {
            {{scanDefName,
              ScanDefinition{{},
                             {{"index1",
                               IndexDefinition{{{makeIndexPath("a"), CollationOp::Ascending},
                                                {makeIndexPath("b"), CollationOp::Ascending}},
                                               true /*multiKey*/}}}}}}};

        ABT translated =
            translatePipeline(metadata, "[{$match: {'a': 2, 'b': 2}}]", scanDefName, prefixId);

        ASSERT_EXPLAIN_V2(
            "Root []\n"
            "|   |   projections: \n"
            "|   |       scan_0\n"
            "|   RefBlock: \n"
            "|       Variable [scan_0]\n"
            "Filter []\n"
            "|   EvalFilter []\n"
            "|   |   Variable [scan_0]\n"
            "|   PathGet [b]\n"
            "|   PathTraverse [1]\n"
            "|   PathCompare [Eq]\n"
            "|   Const [2]\n"
            "Filter []\n"
            "|   EvalFilter []\n"
            "|   |   Variable [scan_0]\n"
            "|   PathGet [a]\n"
            "|   PathTraverse [1]\n"
            "|   PathCompare [Eq]\n"
            "|   Const [2]\n"
            "Scan [collection]\n"
            "    BindBlock:\n"
            "        [scan_0]\n"
            "            Source []\n",
            translated);

        OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                      OptPhaseManager::OptPhase::MemoExplorationPhase,
                                      OptPhaseManager::OptPhase::MemoImplementationPhase},
                                     prefixId,
                                     metadata,
                                     DebugInfo::kDefaultForTests);

        ABT optimized = translated;
        ASSERT_TRUE(phaseManager.optimize(optimized));

        ASSERT_EXPLAIN_V2(
            "Root []\n"
            "|   |   projections: \n"
            "|   |       scan_0\n"
            "|   RefBlock: \n"
            "|       Variable [scan_0]\n"
            "BinaryJoin [joinType: Inner, {rid_0}]\n"
            "|   |   Const [true]\n"
            "|   LimitSkip []\n"
            "|   |   limitSkip:\n"
            "|   |       limit: 1\n"
            "|   |       skip: 0\n"
            "|   Seek [ridProjection: rid_0, {'<root>': scan_0}, collection]\n"
            "|   |   BindBlock:\n"
            "|   |       [scan_0]\n"
            "|   |           Source []\n"
            "|   RefBlock: \n"
            "|       Variable [rid_0]\n"
            "IndexScan [{'<rid>': rid_0}, scanDefName: collection, indexDefName: index1, interval: "
            "{[Const [2], Const [2]], [Const [2], Const [2]]}]\n"
            "    BindBlock:\n"
            "        [rid_0]\n"
            "            Source []\n",
            optimized);
    }

    {
        PrefixId prefixId;
        std::string scanDefName = "collection";
        Metadata metadata = {
            {{scanDefName,
              ScanDefinition{{}, {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}}}}}};

        ABT translated =
            translatePipeline(metadata, "[{$match: {'a': 2, 'b': 2}}]", scanDefName, prefixId);

        ASSERT_EXPLAIN_V2(
            "Root []\n"
            "|   |   projections: \n"
            "|   |       scan_0\n"
            "|   RefBlock: \n"
            "|       Variable [scan_0]\n"
            "Filter []\n"
            "|   EvalFilter []\n"
            "|   |   Variable [scan_0]\n"
            "|   PathGet [b]\n"
            "|   PathTraverse [1]\n"
            "|   PathCompare [Eq]\n"
            "|   Const [2]\n"
            "Filter []\n"
            "|   EvalFilter []\n"
            "|   |   Variable [scan_0]\n"
            "|   PathGet [a]\n"
            "|   PathTraverse [1]\n"
            "|   PathCompare [Eq]\n"
            "|   Const [2]\n"
            "Scan [collection]\n"
            "    BindBlock:\n"
            "        [scan_0]\n"
            "            Source []\n",
            translated);

        // Demonstrate we can use an index over only one field.
        OptPhaseManager phaseManager({OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                      OptPhaseManager::OptPhase::MemoExplorationPhase,
                                      OptPhaseManager::OptPhase::MemoImplementationPhase,
                                      OptPhaseManager::OptPhase::ConstEvalPost},
                                     prefixId,
                                     metadata,
                                     DebugInfo::kDefaultForTests);

        ABT optimized = translated;
        ASSERT_TRUE(phaseManager.optimize(optimized));

        ASSERT_EXPLAIN_V2(
            "Root []\n"
            "|   |   projections: \n"
            "|   |       scan_0\n"
            "|   RefBlock: \n"
            "|       Variable [scan_0]\n"
            "BinaryJoin [joinType: Inner, {rid_0}]\n"
            "|   |   Const [true]\n"
            "|   Filter []\n"
            "|   |   EvalFilter []\n"
            "|   |   |   Variable [evalTemp_2]\n"
            "|   |   PathTraverse [1]\n"
            "|   |   PathCompare [Eq]\n"
            "|   |   Const [2]\n"
            "|   LimitSkip []\n"
            "|   |   limitSkip:\n"
            "|   |       limit: 1\n"
            "|   |       skip: 0\n"
            "|   Seek [ridProjection: rid_0, {'<root>': scan_0, 'b': evalTemp_2}, collection]\n"
            "|   |   BindBlock:\n"
            "|   |       [evalTemp_2]\n"
            "|   |           Source []\n"
            "|   |       [scan_0]\n"
            "|   |           Source []\n"
            "|   RefBlock: \n"
            "|       Variable [rid_0]\n"
            "IndexScan [{'<rid>': rid_0}, scanDefName: collection, indexDefName: index1, interval: "
            "{[Const [2], Const [2]]}]\n"
            "    BindBlock:\n"
            "        [rid_0]\n"
            "            Source []\n",
            optimized);
    }
}

TEST(ABTTranslate, GroupMultiKey) {
    ABT translated = translatePipeline(
        "[{$group: {_id: {'isin': '$isin', 'year': '$year'}, 'count': {$sum: 1}, 'open': {$first: "
        "'$$ROOT'}}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       agg_project_0\n"
        "|   RefBlock: \n"
        "|       Variable [agg_project_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [agg_project_0]\n"
        "|           EvalPath []\n"
        "|           |   Const [{}]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [open]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [open_agg_0]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [count]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [count_agg_0]\n"
        "|           PathField [_id]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [year]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [groupByProj_1]\n"
        "|           PathField [isin]\n"
        "|           PathConstant []\n"
        "|           Variable [groupByProj_0]\n"
        "GroupBy []\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [groupByProj_0]\n"
        "|   |           Variable [groupByProj_1]\n"
        "|   aggregations: \n"
        "|       [count_agg_0]\n"
        "|           FunctionCall [$sum]\n"
        "|           Const [1]\n"
        "|       [open_agg_0]\n"
        "|           FunctionCall [$first]\n"
        "|           Variable [scan_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [groupByProj_1]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [year]\n"
        "|           PathIdentity []\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [groupByProj_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [isin]\n"
        "|           PathIdentity []\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, GroupEvalNoInline) {
    ABT translated = translatePipeline("[{$group: {_id: null, a: {$first: '$b'}}}]");

    PrefixId prefixId;
    std::string scanDefName = "collection";
    OptPhaseManager phaseManager(OptPhaseManager::getAllRewritesSet(),
                                 prefixId,
                                 {{{scanDefName, ScanDefinition{{}, {}}}}},
                                 DebugInfo::kDefaultForTests);

    ABT optimized = translated;
    ASSERT_TRUE(phaseManager.optimize(optimized));

    // Verify that "b" is not inlined in the group expression, but is coming from the physical scan.
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       agg_project_0\n"
        "|   RefBlock: \n"
        "|       Variable [agg_project_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [agg_project_0]\n"
        "|           Let [inputField_1]\n"
        "|           |   If []\n"
        "|           |   |   |   Variable [inputField_1]\n"
        "|           |   |   FunctionCall [setField]\n"
        "|           |   |   |   |   Variable [a_agg_0]\n"
        "|           |   |   |   Const [\"a\"]\n"
        "|           |   |   Variable [inputField_1]\n"
        "|           |   BinaryOp [Or]\n"
        "|           |   |   FunctionCall [isObject]\n"
        "|           |   |   Variable [inputField_1]\n"
        "|           |   FunctionCall [exists]\n"
        "|           |   Variable [a_agg_0]\n"
        "|           If []\n"
        "|           |   |   Const [{}]\n"
        "|           |   FunctionCall [setField]\n"
        "|           |   |   |   Variable [groupByProj_0]\n"
        "|           |   |   Const [\"_id\"]\n"
        "|           |   Const [{}]\n"
        "|           BinaryOp [Or]\n"
        "|           |   FunctionCall [isObject]\n"
        "|           |   Const [{}]\n"
        "|           FunctionCall [exists]\n"
        "|           Variable [groupByProj_0]\n"
        "GroupBy []\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [groupByProj_0]\n"
        "|   aggregations: \n"
        "|       [a_agg_0]\n"
        "|           FunctionCall [$first]\n"
        "|           Variable [groupByInputProj_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [groupByProj_0]\n"
        "|           Const [null]\n"
        "PhysicalScan [{'b': groupByInputProj_0}, collection]\n"
        "    BindBlock:\n"
        "        [groupByInputProj_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, ArrayExpr) {
    ABT translated = translatePipeline("[{$project: {a: ['$b', '$c']}}]");

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathComposeM []\n"
        "|           |   PathDefault []\n"
        "|           |   Const [{}]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [a]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [projGetPath_0]\n"
        "|           PathKeep [_id, a]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [projGetPath_0]\n"
        "|           FunctionCall [newArray]\n"
        "|           |   EvalPath []\n"
        "|           |   |   Variable [scan_0]\n"
        "|           |   PathGet [c]\n"
        "|           |   PathIdentity []\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [b]\n"
        "|           PathIdentity []\n"
        "Scan [collection]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

TEST(ABTTranslate, Union) {
    PrefixId prefixId;
    std::string scanDefA = "collA";
    std::string scanDefB = "collB";

    Metadata metadata{{{scanDefA, {}}, {scanDefB, {}}}};
    ABT translated = translatePipeline(metadata,
                                       "[{$unionWith: 'collB'}, {$match: {_id: 1}}]",
                                       prefixId.getNextId("scan"),
                                       scanDefA,
                                       prefixId,
                                       {{NamespaceString("a." + scanDefB), {}}});

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [_id]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           Source []\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           EvalPath []\n"
        "|   |           |   Variable [scan_1]\n"
        "|   |           PathIdentity []\n"
        "|   Scan [collB]\n"
        "|       BindBlock:\n"
        "|           [scan_1]\n"
        "|               Source []\n"
        "Scan [collA]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);

    OptPhaseManager phaseManager(
        {OptPhaseManager::OptPhase::MemoSubstitutionPhase,
         OptPhaseManager::OptPhase::MemoExplorationPhase,
         OptPhaseManager::OptPhase::MemoImplementationPhase},
        prefixId,
        {{{scanDefA, ScanDefinition{{}, {}}}, {scanDefB, ScanDefinition{{}, {}}}}},
        DebugInfo::kDefaultForTests);

    ABT optimized = translated;
    ASSERT_TRUE(phaseManager.optimize(optimized));

    // Note that the optimized ABT will show the filter push-down.
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Union []\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           Source []\n"
        "|   Filter []\n"
        "|   |   EvalFilter []\n"
        "|   |   |   Variable [scan_0]\n"
        "|   |   PathGet [_id]\n"
        "|   |   PathTraverse [1]\n"
        "|   |   PathCompare [Eq]\n"
        "|   |   Const [1]\n"
        "|   Evaluation []\n"
        "|   |   BindBlock:\n"
        "|   |       [scan_0]\n"
        "|   |           EvalPath []\n"
        "|   |           |   Variable [scan_1]\n"
        "|   |           PathIdentity []\n"
        "|   PhysicalScan [{'<root>': scan_1}, collB]\n"
        "|       BindBlock:\n"
        "|           [scan_1]\n"
        "|               Source []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_0]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'<root>': scan_0, '_id': evalTemp_0}, collA]\n"
        "    BindBlock:\n"
        "        [evalTemp_0]\n"
        "            Source []\n"
        "        [scan_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, PartialIndex) {
    PrefixId prefixId;
    std::string scanDefName = "collection";
    ProjectionName scanProjName = prefixId.getNextId("scan");

    // The expression matches the pipeline.
    // By default the constant is translated as "int32".
    auto conversionResult = convertExprToPartialSchemaReq(
        make<EvalFilter>(
            make<PathGet>("b",
                          make<PathTraverse>(make<PathCompare>(Operations::Eq, Constant::int32(2)),
                                             PathTraverse::kSingleLevel)),
            make<Variable>(scanProjName)),
        true /*isFilterContext*/);
    ASSERT_TRUE(conversionResult.has_value());
    ASSERT_FALSE(conversionResult->_hasEmptyInterval);
    ASSERT_FALSE(conversionResult->_retainPredicate);

    Metadata metadata = {
        {{scanDefName,
          ScanDefinition{{},
                         {{"index1",
                           IndexDefinition{{{makeIndexPath("a"), CollationOp::Ascending}},
                                           true /*multiKey*/,
                                           {DistributionType::Centralized},
                                           std::move(conversionResult->_reqMap)}}}}}}};

    ABT translated = translatePipeline(
        metadata, "[{$match: {'a': 3, 'b': 2}}]", scanProjName, scanDefName, prefixId);

    OptPhaseManager phaseManager(
        OptPhaseManager::getAllRewritesSet(), prefixId, metadata, DebugInfo::kDefaultForTests);

    ABT optimized = translated;
    ASSERT_TRUE(phaseManager.optimize(optimized));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "BinaryJoin [joinType: Inner, {rid_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   FunctionCall [traverseF]\n"
        "|   |   |   |   Const [false]\n"
        "|   |   |   LambdaAbstraction [valCmp_0]\n"
        "|   |   |   BinaryOp [Eq]\n"
        "|   |   |   |   Const [2]\n"
        "|   |   |   Variable [valCmp_0]\n"
        "|   |   Variable [evalTemp_2]\n"
        "|   LimitSkip []\n"
        "|   |   limitSkip:\n"
        "|   |       limit: 1\n"
        "|   |       skip: 0\n"
        "|   Seek [ridProjection: rid_0, {'<root>': scan_0, 'b': evalTemp_2}, collection]\n"
        "|   |   BindBlock:\n"
        "|   |       [evalTemp_2]\n"
        "|   |           Source []\n"
        "|   |       [scan_0]\n"
        "|   |           Source []\n"
        "|   RefBlock: \n"
        "|       Variable [rid_0]\n"
        "IndexScan [{'<rid>': rid_0}, scanDefName: collection, indexDefName: index1, interval: "
        "{[Const [3], Const [3]]}]\n"
        "    BindBlock:\n"
        "        [rid_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, PartialIndexNegative) {
    PrefixId prefixId;
    std::string scanDefName = "collection";
    ProjectionName scanProjName = prefixId.getNextId("scan");

    // The expression does not match the pipeline.
    auto conversionResult = convertExprToPartialSchemaReq(
        make<EvalFilter>(
            make<PathGet>("b",
                          make<PathTraverse>(make<PathCompare>(Operations::Eq, Constant::int32(2)),
                                             PathTraverse::kSingleLevel)),
            make<Variable>(scanProjName)),
        true /*isFilterContext*/);
    ASSERT_TRUE(conversionResult.has_value());
    ASSERT_FALSE(conversionResult->_hasEmptyInterval);
    ASSERT_FALSE(conversionResult->_retainPredicate);

    Metadata metadata = {
        {{scanDefName,
          ScanDefinition{{},
                         {{"index1",
                           IndexDefinition{{{makeIndexPath("a"), CollationOp::Ascending}},
                                           true /*multiKey*/,
                                           {DistributionType::Centralized},
                                           std::move(conversionResult->_reqMap)}}}}}}};

    ABT translated = translatePipeline(
        metadata, "[{$match: {'a': 3, 'b': 3}}]", scanProjName, scanDefName, prefixId);

    OptPhaseManager phaseManager(
        OptPhaseManager::getAllRewritesSet(), prefixId, metadata, DebugInfo::kDefaultForTests);

    ABT optimized = translated;
    ASSERT_TRUE(phaseManager.optimize(optimized));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   FunctionCall [traverseF]\n"
        "|   |   |   Const [false]\n"
        "|   |   LambdaAbstraction [valCmp_1]\n"
        "|   |   BinaryOp [Eq]\n"
        "|   |   |   Const [3]\n"
        "|   |   Variable [valCmp_1]\n"
        "|   Variable [evalTemp_1]\n"
        "Filter []\n"
        "|   FunctionCall [traverseF]\n"
        "|   |   |   Const [false]\n"
        "|   |   LambdaAbstraction [valCmp_0]\n"
        "|   |   BinaryOp [Eq]\n"
        "|   |   |   Const [3]\n"
        "|   |   Variable [valCmp_0]\n"
        "|   Variable [evalTemp_0]\n"
        "PhysicalScan [{'<root>': scan_0, 'a': evalTemp_0, 'b': evalTemp_1}, collection]\n"
        "    BindBlock:\n"
        "        [evalTemp_0]\n"
        "            Source []\n"
        "        [evalTemp_1]\n"
        "            Source []\n"
        "        [scan_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, CommonExpressionElimination) {
    PrefixId prefixId;
    Metadata metadata = {{{"test", {{}, {}}}}};

    auto rootNode =
        translatePipeline(metadata,
                          "[{$project: {foo: {$add: ['$b', 1]}, bar: {$add: ['$b', 1]}}}]",
                          "test",
                          prefixId);

    OptPhaseManager phaseManager(
        {OptPhaseManager::OptPhase::ConstEvalPre}, prefixId, metadata, DebugInfo::kDefaultForTests);

    ASSERT_TRUE(phaseManager.optimize(rootNode));

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_0\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_0]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathComposeM []\n"
        "|           |   PathDefault []\n"
        "|           |   Const [{}]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [foo]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [projGetPath_0]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [bar]\n"
        "|           |   PathConstant []\n"
        "|           |   Variable [projGetPath_0]\n"
        "|           PathKeep [_id, bar, foo]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [projGetPath_0]\n"
        "|           BinaryOp [Add]\n"
        "|           |   Const [1]\n"
        "|           EvalPath []\n"
        "|           |   Variable [scan_0]\n"
        "|           PathGet [b]\n"
        "|           PathIdentity []\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        rootNode);
}

TEST(ABTTranslate, GroupByDependency) {
    PrefixId prefixId;
    Metadata metadata = {{{"test", {{}, {}}}}};

    ABT translated =
        translatePipeline(metadata,
                          "[{$group: {_id: {}, b: {$addToSet: '$a'}}}, {$project: "
                          "{_id: 0, b: {$size: '$b'}}}, {$project: {_id: 0, c: '$b'}}]",
                          "test",
                          prefixId);

    OptPhaseManager phaseManager({OptPhaseManager::OptPhase::ConstEvalPre,
                                  OptPhaseManager::OptPhase::PathFuse,
                                  OptPhaseManager::OptPhase::MemoSubstitutionPhase,
                                  OptPhaseManager::OptPhase::MemoExplorationPhase,
                                  OptPhaseManager::OptPhase::MemoImplementationPhase},
                                 prefixId,
                                 metadata,
                                 DebugInfo::kDefaultForTests);

    ABT optimized = translated;
    ASSERT_TRUE(phaseManager.optimize(optimized));

    // Demonstrate that "c" is set to the array size (not the array itself coming from the group).
    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       combinedProjection_1\n"
        "|   RefBlock: \n"
        "|       Variable [combinedProjection_1]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [combinedProjection_1]\n"
        "|           EvalPath []\n"
        "|           |   Const [{}]\n"
        "|           PathComposeM []\n"
        "|           |   PathField [c]\n"
        "|           |   PathConstant []\n"
        "|           |   FunctionCall [getArraySize]\n"
        "|           |   Variable [b_agg_0]\n"
        "|           PathKeep []\n"
        "GroupBy []\n"
        "|   |   groupings: \n"
        "|   |       RefBlock: \n"
        "|   |           Variable [groupByProj_0]\n"
        "|   aggregations: \n"
        "|       [b_agg_0]\n"
        "|           FunctionCall [$addToSet]\n"
        "|           Variable [groupByInputProj_0]\n"
        "Evaluation []\n"
        "|   BindBlock:\n"
        "|       [groupByProj_0]\n"
        "|           Const [{}]\n"
        "PhysicalScan [{'a': groupByInputProj_0}, test]\n"
        "    BindBlock:\n"
        "        [groupByInputProj_0]\n"
        "            Source []\n",
        optimized);
}

TEST(ABTTranslate, NotEquals) {
    PrefixId prefixId;
    Metadata metadata = {{{"test", {{}, {}}}}};

    ABT translated = translatePipeline(metadata, "[{$match: {'a': {$ne: 2}}}]", "test", prefixId);

    ASSERT_EXPLAIN_V2(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       scan_0\n"
        "|   RefBlock: \n"
        "|       Variable [scan_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathConstant []\n"
        "|   UnaryOp [Not]\n"
        "|   EvalFilter []\n"
        "|   |   Variable [scan_0]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [2]\n"
        "Scan [test]\n"
        "    BindBlock:\n"
        "        [scan_0]\n"
        "            Source []\n",
        translated);
}

}  // namespace
}  // namespace mongo
