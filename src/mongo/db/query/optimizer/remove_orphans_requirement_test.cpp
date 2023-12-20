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

#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/inline_auto_update.h"

namespace mongo::optimizer {
namespace {

using namespace unit_test_abt_literals;

const DatabaseName testDBName =
    DatabaseNameUtil::deserialize(boost::none, "test", SerializationContext::stateDefault());

TEST(PhysRewriter, RemoveOrphansEnforcerMultipleCollections) {
    // Hypothetical MQL which could generate this ABT:
    //   db.c1.aggregate([{$unionWith: {coll: "c2", pipeline: [{$match: {}}]}}])
    ABT rootNode = NodeBuilder{}
                       .root("root")
                       .un(ProjectionNameVector{"root"},
                           {NodeHolder{NodeBuilder{}.finish(_scan("root", "c2"))}})
                       .finish(_scan("root", "c1"));

    auto prefixId = PrefixId::createForTests();

    auto scanDef1 =
        createScanDef(testDBName,
                      UUID::gen(),
                      ScanDefOptions{},
                      IndexDefinitions{},
                      MultikeynessTrie{},
                      ConstEval::constFold,
                      // Sharded on {a: 1}
                      DistributionAndPaths{DistributionType::Centralized},
                      true /*exists*/,
                      boost::none /*ce*/,
                      ShardingMetadata({{_get("a", _id())._n, CollationOp::Ascending}}, true));

    auto scanDef2 = createScanDef(
        DatabaseNameUtil::deserialize(boost::none, "test2", SerializationContext::stateDefault()),
        UUID::gen(),
        ScanDefOptions{},
        IndexDefinitions{},
        MultikeynessTrie{},
        ConstEval::constFold,
        // Sharded on {b: 1}
        DistributionAndPaths{DistributionType::Centralized},
        true /*exists*/,
        boost::none /*ce*/,
        ShardingMetadata({{_get("b", _id())._n, CollationOp::Ascending}}, true));

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", scanDef1}, {"c2", scanDef2}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);

    // Note the evaluation node to project the shard key and filter node to perform shard filtering.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Union [{root}]\n"
        "|   Filter []\n"
        "|   |   FunctionCall [shardFilter]\n"
        "|   |   Variable [shardKey_3]\n"
        "|   PhysicalScan [{'<root>': root, 'b': shardKey_3}, c2]\n"
        "Filter []\n"
        "|   FunctionCall [shardFilter]\n"
        "|   Variable [shardKey_1]\n"
        "PhysicalScan [{'<root>': root, 'a': shardKey_1}, c1]\n",
        optimized);
}

// Common setup function to construct optimizer metadata with no indexes and invoke optimization
// given a physical plan and sharding metadata. Returns the optimized plan.
static ABT optimizeABTWithShardingMetadataNoIndexes(ABT& rootNode,
                                                    ShardingMetadata shardingMetadata) {
    auto prefixId = PrefixId::createForTests();

    // Shard keys guarentee non-multikeyness of all their components. In some cases, there might not
    // be an index backing the shard key. So to make use of the multikeyness data of the shard key,
    // we populate the multikeyness trie.
    MultikeynessTrie trie;
    for (auto&& comp : shardingMetadata.shardKey()) {
        trie.add(comp._path);
    }

    auto scanDef = createScanDef(testDBName,
                                 UUID::gen(),
                                 ScanDefOptions{},
                                 IndexDefinitions{},
                                 std::move(trie),
                                 ConstEval::constFold,
                                 DistributionAndPaths{DistributionType::Centralized},
                                 true /*exists*/,
                                 boost::none /*ce*/,
                                 shardingMetadata);

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1", scanDef}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);
    return optimized;
};

TEST(PhysRewriter, ScanNodeRemoveOrphansImplementerBasic) {
    ABT rootNode = NodeBuilder{}.root("root").finish(_scan("root", "c1"));

    ShardingMetadata sm({{_get("a", _id())._n, CollationOp::Ascending},
                         {_get("b", _id())._n, CollationOp::Ascending}},
                        true);
    const ABT optimized = optimizeABTWithShardingMetadataNoIndexes(rootNode, sm);
    // The fields of the shard key are extracted in the physical scan.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   FunctionCall [shardFilter]\n"
        "|   |   Variable [shardKey_3]\n"
        "|   Variable [shardKey_2]\n"
        "PhysicalScan [{'<root>': root, 'a': shardKey_2, 'b': shardKey_3}, c1]\n",
        optimized);
}

TEST(PhysRewriter, ScanNodeRemoveOrphansImplementerDottedBasic) {
    ABT rootNode = NodeBuilder{}.root("root").finish(_scan("root", "c1"));
    ShardingMetadata sm({{_get("a", _get("b", _id()))._n, CollationOp::Ascending},
                         {_get("c", _get("d", _id()))._n, CollationOp::Ascending}},
                        true);
    const ABT optimized = optimizeABTWithShardingMetadataNoIndexes(rootNode, sm);
    // The top-level of each field's path is pushed down into the physical scan, and the rest of
    // the path is obtained with an evaluation node.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   FunctionCall [shardFilter]\n"
        "|   |   Variable [shardKey_5]\n"
        "|   Variable [shardKey_4]\n"
        "Evaluation [{shardKey_5}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [shardKey_3]\n"
        "|   PathGet [d]\n"
        "|   PathIdentity []\n"
        "Evaluation [{shardKey_4}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [shardKey_2]\n"
        "|   PathGet [b]\n"
        "|   PathIdentity []\n"
        "PhysicalScan [{'<root>': root, 'a': shardKey_2, 'c': shardKey_3}, c1]\n",
        optimized);
}

TEST(PhysRewriter, ScanNodeRemoveOrphansImplementerDottedSharedPrefix) {
    ABT rootNode = NodeBuilder{}.root("root").finish(_scan("root", "c1"));
    ShardingMetadata sm({{_get("a", _get("b", _id()))._n, CollationOp::Ascending},
                         {_get("a", _get("c", _id()))._n, CollationOp::Ascending}},
                        true);
    const ABT optimized = optimizeABTWithShardingMetadataNoIndexes(rootNode, sm);
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   FunctionCall [shardFilter]\n"
        "|   |   Variable [shardKey_4]\n"
        "|   Variable [shardKey_3]\n"
        "Evaluation [{shardKey_4}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [shardKey_2]\n"
        "|   PathGet [c]\n"
        "|   PathIdentity []\n"
        "Evaluation [{shardKey_3}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [shardKey_2]\n"
        "|   PathGet [b]\n"
        "|   PathIdentity []\n"
        "PhysicalScan [{'<root>': root, 'a': shardKey_2}, c1]\n",
        optimized);
}

TEST(PhysRewriter, ScanNodeRemoveOrphansImplementerDottedDoubleSharedPrefix) {
    ABT rootNode = NodeBuilder{}.root("root").finish(_scan("root", "c1"));
    // Sharded on {a.b.c: 1, a.b.d:1}
    ShardingMetadata sm({{_get("a", _get("b", _get("c", _id())))._n, CollationOp::Ascending},
                         {_get("a", _get("b", _get("d", _id())))._n, CollationOp::Ascending}},
                        true);
    const ABT optimized = optimizeABTWithShardingMetadataNoIndexes(rootNode, sm);
    // Only the top level of shared paths is currently pushed down into the physical scan.
    // TODO SERVER-79435: Factor out a shared path to the greatest extent possible (e.g. 'a.b'
    // rather than just 'a').
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   FunctionCall [shardFilter]\n"
        "|   |   Variable [shardKey_4]\n"
        "|   Variable [shardKey_3]\n"
        "Evaluation [{shardKey_4}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [shardKey_2]\n"
        "|   PathGet [b]\n"
        "|   PathGet [d]\n"
        "|   PathIdentity []\n"
        "Evaluation [{shardKey_3}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [shardKey_2]\n"
        "|   PathGet [b]\n"
        "|   PathGet [c]\n"
        "|   PathIdentity []\n"
        "PhysicalScan [{'<root>': root, 'a': shardKey_2}, c1]\n",
        optimized);
}

TEST(PhysRewriter, ScanNodeRemoveOrphansImplementerSeekTargetBasic) {
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

    ShardingMetadata sm({{_get("b", _id())._n, CollationOp::Ascending}}, true);

    auto scanDef = createScanDef(testDBName,
                                 UUID::gen(),
                                 {},
                                 {{"index1", makeIndexDefinition("a", CollationOp::Ascending)}},
                                 MultikeynessTrie{},
                                 ConstEval::constFold,
                                 DistributionAndPaths{DistributionType::Centralized},
                                 true,
                                 boost::none,
                                 sm);
    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase,
         OptPhase::ProjNormalize},
        prefixId,
        {{{"c1", scanDef}}},
        /*costModel*/ boost::none,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});
    ABT optimized = rootNode;
    phaseManager.optimize(optimized);

    // Assert plan structure contains NLJ with in index scan on left and shard filter + seek on the
    // right.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{renamed_1}]\n"
        "NestedLoopJoin [joinType: Inner, {renamed_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   FunctionCall [shardFilter]\n"
        "|   |   Variable [renamed_2]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: renamed_0, {'<root>': renamed_1, 'b': renamed_2}, c1]\n"
        "IndexScan [{'<rid>': renamed_0}, scanDefName: c1, indexDefName: index1, interval: "
        "{=Const [1]}]\n",
        optimized);
}

TEST(PhysRewriter, ScanNodeRemoveOrphansImplementerSeekTargetDottedSharedPrefix) {
    ABT rootNode = NodeBuilder{}
                       .root("root")
                       .filter(_evalf(_get("e", _traverse1(_cmp("Eq", "3"_cint64))), "root"_var))
                       .finish(_scan("root", "c1"));
    // Sharded on {a.b.c: 1, a.b.d:1}
    ShardingMetadata sm({{_get("a", _get("b", _get("c", _id())))._n, CollationOp::Ascending},
                         {_get("a", _get("b", _get("d", _id())))._n, CollationOp::Ascending}},
                        true);
    auto shardScanDef =
        createScanDef(testDBName,
                      UUID::gen(),
                      ScanDefOptions{},
                      {{"index1", makeIndexDefinition("e", CollationOp::Ascending)}},
                      MultikeynessTrie{},
                      ConstEval::constFold,
                      DistributionAndPaths{DistributionType::Centralized},
                      true /*exists*/,
                      boost::none /*ce*/,
                      sm);

    auto prefixId = PrefixId::createForTests();

    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase,
         OptPhase::ProjNormalize},
        prefixId,
        {{{"c1", shardScanDef}}},
        /*costModel*/ boost::none,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});
    ABT optimized = rootNode;
    phaseManager.optimize(optimized);

    // Assert top level field of shard key is pushed down into the SeekNode.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{renamed_1}]\n"
        "NestedLoopJoin [joinType: Inner, {renamed_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   FunctionCall [shardFilter]\n"
        "|   |   |   Variable [renamed_4]\n"
        "|   |   Variable [renamed_3]\n"
        "|   Evaluation [{renamed_4}]\n"
        "|   |   EvalPath []\n"
        "|   |   |   Variable [renamed_2]\n"
        "|   |   PathGet [b]\n"
        "|   |   PathGet [d]\n"
        "|   |   PathIdentity []\n"
        "|   Evaluation [{renamed_3}]\n"
        "|   |   EvalPath []\n"
        "|   |   |   Variable [renamed_2]\n"
        "|   |   PathGet [b]\n"
        "|   |   PathGet [c]\n"
        "|   |   PathIdentity []\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: renamed_0, {'<root>': renamed_1, 'a': renamed_2}, c1]\n"
        "IndexScan [{'<rid>': renamed_0}, scanDefName: c1, indexDefName: index1, interval: "
        "{=Const [3]}]\n",
        optimized);
}

TEST(PhysRewriter, RemoveOrphansSargableNodeComplete) {
    // Hypothetical MQL which could generate this ABT: {$match: {a: 1}}
    ABT root = NodeBuilder{}
                   .root("root")
                   .filter(_evalf(_get("a", _traverse1(_cmp("Eq", "1"_cint64))), "root"_var))
                   .finish(_scan("root", "c1"));
    // Shard key {a: 1, b: 1};
    ShardingMetadata sm({{_get("a", _id())._n, CollationOp::Ascending},
                         {_get("b", _id())._n, CollationOp::Ascending}},
                        true);
    const ABT optimized = optimizeABTWithShardingMetadataNoIndexes(root, sm);

    // Projections on 'a' and 'b' pushed down into PhysicalScan and used as args to 'shardFilter()'.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   FunctionCall [shardFilter]\n"
        "|   |   Variable [shardKey_2]\n"
        "|   Variable [evalTemp_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_0]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_0, 'b': shardKey_2}, c1]\n",
        optimized);
}

TEST(PhysRewriter, RemoveOrphansSargableNodeCompleteDottedShardKey) {
    // {$match: {"a.b": {$gt: 1}}}
    ABT root =
        NodeBuilder{}
            .root("root")
            .filter(_evalf(_get("a", _traverse1(_get("b", _traverse1(_cmp("Gt", "1"_cint64))))),
                           "root"_var))
            .finish(_scan("root", "c1"));
    // Shard key {'a.b': 1}
    ShardingMetadata sm({{_get("a", _get("b", _id()))._n, CollationOp::Ascending}}, true);
    const ABT optimized = optimizeABTWithShardingMetadataNoIndexes(root, sm);

    // Push down projection on 'a' into PhysicalScan and use that stream to project 'b' to use as
    // input to 'shardFilter()'. This avoids explicitly projecting 'a.b' from the root projection.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   FunctionCall [shardFilter]\n"
        "|   Variable [shardKey_1]\n"
        "Evaluation [{shardKey_1}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [evalTemp_0]\n"
        "|   PathGet [b]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_0]\n"
        "|   PathGet [b]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_0}, c1]\n",
        optimized);
}

TEST(PhysRewriter, RemoveOrphansSargableNodeIndex) {
    ABT root = NodeBuilder{}
                   .root("root")
                   .filter(_evalf(_get("a", _cmp("Gt", "1"_cint64)), "root"_var))
                   .finish(_scan("root", "c1"));
    ShardingMetadata sm({{_get("a", _id())._n, CollationOp::Ascending}}, true);

    // Make predicates on PathGet[a] very selective to prefer IndexScan plan over collection scan.
    ce::PartialSchemaSelHints ceHints;
    ceHints.emplace(PartialSchemaKey{"root", _get("a", _id())._n}, SelectivityType{0.01});

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase,
         OptPhase::ProjNormalize},
        prefixId,
        {{{"c1",
           createScanDef(testDBName,
                         UUID::gen(),
                         {},
                         {{"index1", makeIndexDefinition("a", CollationOp::Ascending, false)}},
                         MultikeynessTrie{},
                         ConstEval::constFold,
                         DistributionAndPaths{DistributionType::Centralized},
                         true /*exists*/,
                         boost::none /*ce*/,
                         sm)}}},
        makeHintedCE(std::move(ceHints)),
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = root;
    phaseManager.optimize(optimized);

    ASSERT_BETWEEN(10, 16, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // The shard filter is performed on the index side of the NLJ and pushed the projection into the
    // index scan.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{renamed_2}]\n"
        "NestedLoopJoin [joinType: Inner, {renamed_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: renamed_0, {'<root>': renamed_2}, c1]\n"
        "Filter []\n"
        "|   FunctionCall [shardFilter]\n"
        "|   Variable [renamed_1]\n"
        "IndexScan [{'<indexKey> 0': renamed_1, '<rid>': renamed_0}, scanDefName: c1, "
        "indexDefName: index1, interval: {>Const [1]}]\n",
        optimized);
}

TEST(PhysRewriter, RemoveOrphansCovered) {
    ABT root = NodeBuilder{}
                   .root("pa")
                   .eval("pa", _evalp(_get("a", _id()), "root"_var))
                   .filter(_evalf(_get("a", _cmp("Gt", "1"_cint64)), "root"_var))
                   .finish(_scan("root", "c1"));
    ShardingMetadata sm({{_get("a", _id())._n, CollationOp::Ascending}}, true);

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(testDBName,
                         UUID::gen(),
                         {},
                         {{"index1", makeIndexDefinition("a", CollationOp::Ascending, false)}},
                         MultikeynessTrie::fromIndexPath(_get("a", _id())._n),
                         ConstEval::constFold,
                         DistributionAndPaths{DistributionType::Centralized},
                         true /*exists*/,
                         boost::none /*ce*/,
                         sm)}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = root;
    phaseManager.optimize(optimized);

    ASSERT_BETWEEN_AUTO(  // NOLINT
        5,
        15,
        phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // No seek required.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{pa}]\n"
        "Filter []\n"
        "|   FunctionCall [shardFilter]\n"
        "|   Variable [pa]\n"
        "IndexScan [{'<indexKey> 0': pa}, scanDefName: c1, indexDefName: index1, interval: "
        "{>Const [1]}]\n",
        optimized);
}

TEST(PhysRewriter, RemoveOrphansIndexDoesntCoverShardKey) {
    ABT root = NodeBuilder{}
                   .root("root")
                   .filter(_evalf(_get("a", _cmp("Gt", "1"_cint64)), "root"_var))
                   .finish(_scan("root", "c1"));
    ShardingMetadata sm({{_get("a", _id())._n, CollationOp::Ascending},
                         {_get("b", _id())._n, CollationOp::Ascending}},
                        true);

    // Make predicates on PathGet[a] very selective to prefer IndexScan plan over collection scan.
    ce::PartialSchemaSelHints ceHints;
    ceHints.emplace(PartialSchemaKey{"root", _get("a", _id())._n}, SelectivityType{0.01});

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase,
         OptPhase::ProjNormalize},
        prefixId,
        {{{"c1",
           createScanDef(testDBName,
                         UUID::gen(),
                         {},
                         {{"index1", makeIndexDefinition("a", CollationOp::Ascending, false)}},
                         MultikeynessTrie{},
                         ConstEval::constFold,
                         DistributionAndPaths{DistributionType::Centralized},
                         true /*exists*/,
                         boost::none /*ce*/,
                         sm)}}},
        makeHintedCE(std::move(ceHints)),
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = root;
    phaseManager.optimize(optimized);

    ASSERT_BETWEEN(8, 14, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Shard key {a: 1, b: 1} and index on {a: 1} means that shard filtering must occur on the seek
    // side.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{renamed_1}]\n"
        "NestedLoopJoin [joinType: Inner, {renamed_0}]\n"
        "|   |   Const [true]\n"
        "|   Filter []\n"
        "|   |   FunctionCall [shardFilter]\n"
        "|   |   |   Variable [renamed_3]\n"
        "|   |   Variable [renamed_2]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: renamed_0, {'<root>': renamed_1, 'a': renamed_2, 'b': "
        "renamed_3}, c1]\n"
        "IndexScan [{'<rid>': renamed_0}, scanDefName: c1, indexDefName: index1, interval: "
        "{>Const [1]}]\n",
        optimized);
}

TEST(PhysRewriter, RemoveOrphansDottedPathIndex) {
    ABT root = NodeBuilder{}
                   .root("root")
                   .filter(_evalf(_get("a", _get("b", _cmp("Gt", "1"_cint64))), "root"_var))
                   .finish(_scan("root", "c1"));
    ShardingMetadata sm({{_get("a", _get("b", _id()))._n, CollationOp::Ascending}}, true);

    // Make predicates on PathGet[a] PathGet [b] very selective to prefer IndexScan plan over
    // collection scan.
    ce::PartialSchemaSelHints ceHints;
    ceHints.emplace(PartialSchemaKey{"root", _get("a", _get("b", _id()))._n},
                    SelectivityType{0.01});

    auto prefixId = PrefixId::createForTests();
    IndexCollationSpec indexSpec{
        IndexCollationEntry(_get("a", _get("b", _id()))._n, CollationOp::Ascending),
        IndexCollationEntry(_get("a", _get("c", _id()))._n, CollationOp::Ascending)};
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase,
         OptPhase::ProjNormalize},
        prefixId,
        {{{"c1",
           createScanDef(testDBName,
                         UUID::gen(),
                         {},
                         {{"index1", {indexSpec, false}}},
                         MultikeynessTrie{},
                         ConstEval::constFold,
                         DistributionAndPaths{DistributionType::Centralized},
                         true /*exists*/,
                         boost::none /*ce*/,
                         sm)}}},
        makeHintedCE(std::move(ceHints)),
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = root;
    phaseManager.optimize(optimized);

    ASSERT_BETWEEN(10, 16, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Shard key {"a.b": 1} and index on {"a.b": 1, "a.c": 1}
    // The index scan produces the projections for "a.b" to perform shard filtering.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{renamed_2}]\n"
        "NestedLoopJoin [joinType: Inner, {renamed_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: renamed_0, {'<root>': renamed_2}, c1]\n"
        "Filter []\n"
        "|   FunctionCall [shardFilter]\n"
        "|   Variable [renamed_1]\n"
        "IndexScan [{'<indexKey> 0': renamed_1, '<rid>': renamed_0}, scanDefName: c1, "
        "indexDefName: index1, interval: {>Const [1 | maxKey]}]\n",
        optimized);
}

TEST(PhysRewriter, RemoveOrphanedMultikeyIndex) {
    // Shard key: {a: 1}
    // Index: {a: 1, b: 1} -> multikey on b
    // Query: {$match: {a: {$gt: 2}, b: {$gt: 3}}}
    ABT root = NodeBuilder{}
                   .root("root")
                   .filter(_evalf(_get("a", _cmp("Gt", "2"_cint64)), "root"_var))
                   .filter(_evalf(_get("b", _cmp("Gt", "3"_cint64)), "root"_var))
                   .finish(_scan("root", "c1"));
    ShardingMetadata sm({{_get("a", _id())._n, CollationOp::Ascending}}, true);

    ce::PartialSchemaSelHints ceHints;
    ceHints.emplace(PartialSchemaKey{"root", _get("a", _id())._n}, SelectivityType{0.01});
    ceHints.emplace(PartialSchemaKey{"root", _get("b", _id())._n}, SelectivityType{0.01});

    auto prefixId = PrefixId::createForTests();
    ABT indexPath0 = _get("a", _id())._n;
    ABT indexPath1 = _get("b", _id())._n;
    IndexCollationSpec indexSpec{IndexCollationEntry(indexPath0, CollationOp::Ascending),
                                 IndexCollationEntry(indexPath1, CollationOp::Ascending)};
    auto multikeyTrie = MultikeynessTrie::fromIndexPath(indexPath0);
    multikeyTrie.add(indexPath1);
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase,
         OptPhase::ProjNormalize},
        prefixId,
        {{{"c1",
           createScanDef(testDBName,
                         UUID::gen(),
                         {},
                         {{"index1", {indexSpec, false}}},
                         std::move(multikeyTrie),
                         ConstEval::constFold,
                         DistributionAndPaths{DistributionType::Centralized},
                         true /*exists*/,
                         boost::none /*ce*/,
                         sm)}}},
        makeHintedCE(std::move(ceHints)),
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = root;
    phaseManager.optimize(optimized);

    ASSERT_BETWEEN(24, 30, phaseManager.getMemo().getStats()._physPlanExplorationCount);

    // Ensure that we perform the shard filter using a projection from the index scan.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{renamed_3}]\n"
        "NestedLoopJoin [joinType: Inner, {renamed_0}]\n"
        "|   |   Const [true]\n"
        "|   LimitSkip [limit: 1, skip: 0]\n"
        "|   Seek [ridProjection: renamed_0, {'<root>': renamed_3}, c1]\n"
        "Filter []\n"
        "|   FunctionCall [shardFilter]\n"
        "|   Variable [renamed_1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [renamed_2]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [3]\n"
        "IndexScan [{'<indexKey> 0': renamed_1, '<indexKey> 1': renamed_2, '<rid>': renamed_0}, "
        "scanDefName: c1, indexDefName: index1, interval: {>Const [2 | maxKey]}]\n",
        optimized);
}

TEST(PhysRewriter, RemoveOrphanEqualityOnSimpleShardKey) {
    // Query: {$match: {a: 1, b: 1}}
    ABT root = NodeBuilder{}
                   .root("root")
                   .filter(_evalf(_get("a", _traverse1(_cmp("Eq", "1"_cint64))), "root"_var))
                   .filter(_evalf(_get("b", _traverse1(_cmp("Eq", "1"_cint64))), "root"_var))
                   .finish(_scan("root", "c1"));
    // Shard key {a: 1}
    ShardingMetadata sm({{_get("a", _id())._n, CollationOp::Ascending}}, true);
    const ABT optimized = optimizeABTWithShardingMetadataNoIndexes(root, sm);

    // No shard filter in the plan.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_3]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_2]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_2, 'b': evalTemp_3}, c1]\n",
        optimized);
}

TEST(PhysRewriter, RemoveOrphanEqualityWithComplexPSR) {
    // Query: {$match: {a: 1, b: 1}}
    ABT root = NodeBuilder{}
                   .root("root")
                   .filter(_evalf(_composem(_get("a", _traverse1(_cmp("Eq", "1"_cint64))),
                                            _get("b", _traverse1(_cmp("Eq", "1"_cint64)))),
                                  "root"_var))
                   .finish(_scan("root", "c1"));
    // Shard key {a: 1}
    ShardingMetadata sm({{_get("a", _id())._n, CollationOp::Ascending}}, true);
    const ABT optimized = optimizeABTWithShardingMetadataNoIndexes(root, sm);

    // No shard filter in the plan.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_3]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_2]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_2, 'b': evalTemp_3}, c1]\n",
        optimized);
}

TEST(PhysRewriter, RemoveOrphanEqualityOnCompoundShardKey) {
    // Query: {$match: {a: 1, b: 1}}
    ABT root = NodeBuilder{}
                   .root("root")
                   .filter(_evalf(_get("a", _traverse1(_cmp("Eq", "1"_cint64))), "root"_var))
                   .filter(_evalf(_get("b", _traverse1(_cmp("Eq", "1"_cint64))), "root"_var))
                   .finish(_scan("root", "c1"));
    // Shard key {a: 1, b: 1}
    ShardingMetadata sm({{_get("a", _id())._n, CollationOp::Ascending},
                         {_get("b", _id())._n, CollationOp::Ascending}},
                        true);
    const ABT optimized = optimizeABTWithShardingMetadataNoIndexes(root, sm);

    // No shard filter in the plan.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_3]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_2]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_2, 'b': evalTemp_3}, c1]\n",
        optimized);
}

TEST(PhysRewriter, RemoveOrphanNoEqualityOnCompoundShardKey) {
    // Query: {$match: {a: 1}}
    ABT root = NodeBuilder{}
                   .root("root")
                   .filter(_evalf(_get("a", _traverse1(_cmp("Eq", "1"_cint64))), "root"_var))
                   .finish(_scan("root", "c1"));
    // Shard key {a: 1, b: 1}
    ShardingMetadata sm({{_get("a", _id())._n, CollationOp::Ascending},
                         {_get("b", _id())._n, CollationOp::Ascending}},
                        true);
    const ABT optimized = optimizeABTWithShardingMetadataNoIndexes(root, sm);

    // These is a shard filter in the plan.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{root}]\n"
        "Filter []\n"
        "|   FunctionCall [shardFilter]\n"
        "|   |   Variable [shardKey_2]\n"
        "|   Variable [evalTemp_0]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_0]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_0, 'b': shardKey_2}, c1]\n",
        optimized);
    ;
}

TEST(PhysRewriter, RemoveOrphanEqualityDottedPathInShardKey) {
    // Query: {$match: {"a.b": 1, "a.c": 1, "a.d": {$gt: 1}}}
    ABT root =
        NodeBuilder{}
            .root("root")
            .filter(_evalf(_get("a", _traverse1(_get("b", _traverse1(_cmp("Eq", "1"_cint64))))),
                           "root"_var))
            .filter(_evalf(_get("a", _traverse1(_get("c", _traverse1(_cmp("Eq", "1"_cint64))))),
                           "root"_var))
            .filter(_evalf(_get("a", _traverse1(_get("d", _traverse1(_cmp("Gt", "1"_cint64))))),
                           "root"_var))
            .finish(_scan("root", "c1"));
    // Shard key {"a.b": 1, "a.c": 1}
    ShardingMetadata sm({{_get("a", _get("b", _id()))._n, CollationOp::Ascending},
                         {_get("a", _get("c", _id()))._n, CollationOp::Ascending}},
                        true);
    const ABT optimized = optimizeABTWithShardingMetadataNoIndexes(root, sm);

    // No shard filter in the plan.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_4]\n"
        "|   PathGet [d]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_4]\n"
        "|   PathGet [c]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_4]\n"
        "|   PathGet [b]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_4}, c1]\n",
        optimized);
}

TEST(PhysRewriter, RemoveOrphanNoEqualityDottedPathInShardKey) {
    // Query: {$match: {"a.b": 1, "a.c": {$gt: 1}, "a.d": 1}}
    ABT root =
        NodeBuilder{}
            .root("root")
            .filter(_evalf(_get("a", _traverse1(_get("b", _traverse1(_cmp("Eq", "1"_cint64))))),
                           "root"_var))
            .filter(_evalf(_get("a", _traverse1(_get("c", _traverse1(_cmp("Gt", "1"_cint64))))),
                           "root"_var))
            .filter(_evalf(_get("a", _traverse1(_get("d", _traverse1(_cmp("Eq", "1"_cint64))))),
                           "root"_var))
            .finish(_scan("root", "c1"));
    // Shard key {"a.b": 1, "a.c": 1}
    ShardingMetadata sm({{_get("a", _get("b", _id()))._n, CollationOp::Ascending},
                         {_get("a", _get("c", _id()))._n, CollationOp::Ascending}},
                        true);
    const ABT optimized = optimizeABTWithShardingMetadataNoIndexes(root, sm);

    // There is shard filter in the plan.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{root}]\n"
        "Filter []\n"
        "|   FunctionCall [shardFilter]\n"
        "|   |   Variable [shardKey_3]\n"
        "|   Variable [shardKey_2]\n"
        "Evaluation [{shardKey_3}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [evalTemp_4]\n"
        "|   PathGet [c]\n"
        "|   PathIdentity []\n"
        "Evaluation [{shardKey_2}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [evalTemp_4]\n"
        "|   PathGet [b]\n"
        "|   PathIdentity []\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_4]\n"
        "|   PathGet [c]\n"
        "|   PathCompare [Gt]\n"
        "|   Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_4]\n"
        "|   PathGet [d]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_4]\n"
        "|   PathGet [b]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_4}, c1]\n",
        optimized);
}

TEST(PhysRewriter, RemoveOrphanEqualityHashedShardKey) {
    // Query: {$match: {a: 1, b: 1}}
    ABT root = NodeBuilder{}
                   .root("root")
                   .filter(_evalf(_get("a", _traverse1(_cmp("Eq", "1"_cint64))), "root"_var))
                   .filter(_evalf(_get("b", _traverse1(_cmp("Eq", "1"_cint64))), "root"_var))
                   .finish(_scan("root", "c1"));
    // Shard key {a: 'hashed'}
    ShardingMetadata sm({{_get("a", _id())._n, CollationOp::Clustered}}, true);
    const ABT optimized = optimizeABTWithShardingMetadataNoIndexes(root, sm);

    // No shard filter in the plan.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Root [{root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_3]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [evalTemp_2]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "PhysicalScan [{'<root>': root, 'a': evalTemp_2, 'b': evalTemp_3}, c1]\n",
        optimized);
}

TEST(PhysRewriter, RIDIntersectRemoveOrphansImplementer) {
    using namespace properties;

    // Query: {a: {$gt: 1}}
    ABT rootNode = NodeBuilder{}
                       .root("root")
                       .filter(_evalf(_get("a", _traverse1(_cmp("Gt", "1"_cint64))), "root"_var))
                       .finish(_scan("root", "c1"));

    auto prefixId = PrefixId::createForTests();
    // Shard key: {a: 1}
    ShardingMetadata sm({{_get("a", _id())._n, CollationOp::Ascending}}, true);
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase,
         OptPhase::MemoExplorationPhase,
         OptPhase::MemoImplementationPhase},
        prefixId,
        {{{"c1",
           createScanDef(DatabaseNameUtil::deserialize(
                             boost::none, "test", SerializationContext::stateDefault()),
                         UUID::gen(),
                         {},
                         {{"index1", makeIndexDefinition("a", CollationOp::Ascending, false)}},
                         MultikeynessTrie{},
                         ConstEval::constFold,
                         DistributionAndPaths{DistributionType::Centralized},
                         true /*exists*/,
                         boost::none /*ce*/,
                         sm)}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 3 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    // Fully explore search space to enumerate all alternatives to verify the RIDIntersect rewrite
    // enumerated the rewrites we expected.
    phaseManager.getHints()._disableBranchAndBound = true;
    phaseManager.getHints()._keepRejectedPlans = true;

    ABT optimized = rootNode;
    phaseManager.optimize(optimized);

    // Examine the memo to verify that there are alternatives in which RemoveOrphansRequirement is
    // pushed down into the left and right child groups.
    const auto& memo = phaseManager.getMemo();

    // Get the ID of the group which performs the RIDIntersect.
    auto ridGroupId = [&memo]() -> boost::optional<size_t> {
        for (size_t i = 0; i < memo.getGroupCount(); ++i) {
            for (auto&& node : memo.getLogicalNodes(i)) {
                if (node.is<RIDIntersectNode>()) {
                    return {i};
                }
            }
        }
        return boost::none;
    }();
    ASSERT_TRUE(ridGroupId.has_value());

    // Get the result of optimization of this RIDIntersect group when optimized with
    // RemoveOrphansRequirement{true}.
    PhysOptimizationResult* ridIntersectWithRemoveOrphans;
    for (auto&& physOptResult : memo.getPhysicalNodes(*ridGroupId)) {
        auto physProps = physOptResult->_physProps;
        if (hasProperty<RemoveOrphansRequirement>(physProps) &&
            getPropertyConst<RemoveOrphansRequirement>(physProps).mustRemove()) {
            ridIntersectWithRemoveOrphans = physOptResult.get();
        }
    }
    ASSERT_NE(ridIntersectWithRemoveOrphans, nullptr);

    // Keep track whether we've seen alternatives that push the RemoveOrphansRequirement into the
    // left and right child respectively.
    bool hasAlternativeWithRorAsLeftChild = false;
    bool hasAlternativeWithRorAsRightChild = false;

    // Put all alternatives in the same vector to iterate over them.
    auto allAlternatives = ridIntersectWithRemoveOrphans->_rejectedNodeInfo;
    if (ridIntersectWithRemoveOrphans->_nodeInfo.has_value()) {
        allAlternatives.push_back(*ridIntersectWithRemoveOrphans->_nodeInfo);
    }
    for (auto&& alternative : allAlternatives) {
        // We don't care about alternatives that don't use the index.
        if (!alternative._node.is<NestedLoopJoinNode>()) {
            continue;
        }
        auto nlj = alternative._node.cast<NestedLoopJoinNode>();
        // Get physical node id of left and right children.
        auto leftNodeId = nlj->getLeftChild().cast<MemoPhysicalDelegatorNode>()->getNodeId();
        auto rightNodeId = nlj->getRightChild().cast<MemoPhysicalDelegatorNode>()->getNodeId();
        // Examine whether the left and right children are optimized with RemoveOrphansRequirement.
        auto leftRor = getPropertyConst<RemoveOrphansRequirement>(
            memo.getPhysicalNodes(leftNodeId._groupId).at(leftNodeId._index)->_physProps);
        auto rightRor = getPropertyConst<RemoveOrphansRequirement>(
            memo.getPhysicalNodes(rightNodeId._groupId).at(rightNodeId._index)->_physProps);
        // RemoveOrphansRequirement should only be pushed down to one child.
        ASSERT_NE(leftRor, rightRor);
        if (leftRor.mustRemove()) {
            hasAlternativeWithRorAsLeftChild = true;
        } else if (rightRor.mustRemove()) {
            hasAlternativeWithRorAsRightChild = true;
        }
    }
    // Assert that both alternatives exist in the memo.
    ASSERT_TRUE(hasAlternativeWithRorAsLeftChild);
    ASSERT_TRUE(hasAlternativeWithRorAsRightChild);
}

TEST(PhysRewriter, HashedShardKey) {
    ABT rootNode = NodeBuilder{}.root("root").finish(_scan("root", "c1"));
    // Sharded on {a: "hashed", b: 1}
    ShardingMetadata sm({{_get("a", _id())._n, CollationOp::Clustered},
                         {_get("b", _id())._n, CollationOp::Ascending}},
                        true);
    ABT optimized = optimizeABTWithShardingMetadataNoIndexes(rootNode, sm);
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "Filter []\n"
        "|   FunctionCall [shardFilter]\n"
        "|   |   Variable [shardKey_3]\n"
        "|   FunctionCall [shardHash]\n"
        "|   Variable [shardKey_2]\n"
        "PhysicalScan [{'<root>': root, 'a': shardKey_2, 'b': shardKey_3}, c1]\n",
        optimized);
}

}  // namespace
}  // namespace mongo::optimizer
