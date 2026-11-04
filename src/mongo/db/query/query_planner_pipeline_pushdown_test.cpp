// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/checked_cast.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/index_path_projection.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_internal_projection.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/metadata/path_arrayness.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry_mock.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace {
using namespace mongo;

/**
 * Make a minimal IndexEntry from just a key pattern. A dummy name will be added.
 */
IndexEntry buildSimpleIndexEntry(const BSONObj& kp, WildcardProjection* wcProjection = nullptr) {
    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
            IndexConfig::kLatestIndexVersion,
            false,
            {},
            {},
            false,
            false,
            CoreIndexInfo::Identifier("test_foo"),
            {},
            wcProjection};
}

class QueryPlannerPipelinePushdownTest : public QueryPlannerTest {
protected:
    QueryPlannerPipelinePushdownTest() : QueryPlannerTest() {
        secondaryCollMap.emplace(kSecondaryNamespace, CollectionInfo());
    }

    static std::shared_ptr<const PathArrayness> makePathArrayness(
        const BSONObj& keyPattern, const MultikeyPaths& multikeyPaths) {
        auto pathArrayness = std::make_shared<PathArrayness>();
        pathArrayness->addPathsFromIndexKeyPattern(keyPattern, multikeyPaths, true);
        return pathArrayness;
    }

    // Presents the planner with a $** index over 'keyPattern', with multikey paths recorded in
    // 'multikeyPathSet' as $** indexes do, in the keys themselves. Modeled on
    // QueryPlannerWildcardTest::addWildcardIndex.
    void addWildcardIndex(const BSONObj& keyPattern,
                          const std::set<FieldRef>& multikeyPathSet = {}) {
        wildcardProjection = WildcardKeyGenerator::createProjectionExecutor(keyPattern, BSONObj());

        IndexSpec spec;
        spec.version(1).name(kWildcardIndexName).addKeys(keyPattern);
        auto descriptor = IndexDescriptor(IndexNames::WILDCARD, spec.toBSON());

        params.mainCollectionInfo.indexes.push_back(
            {keyPattern,
             IndexType::INDEX_WILDCARD,
             IndexConfig::kLatestIndexVersion,
             !multikeyPathSet.empty(),
             {},  // multikeyPaths
             multikeyPathSet,
             false,  // sparse
             false,  // unique
             IndexEntry::Identifier{kWildcardIndexName},
             BSON("wildcardProjection" << BSONObj()),
             wildcardProjection.get_ptr(),
             std::make_shared<IndexCatalogEntryMock>(nullptr,
                                                     nullptr,
                                                     "" /* ident */,
                                                     std::move(descriptor),
                                                     false /* isFrozen */,
                                                     nullptr,
                                                     nullptr)});
    }

    void assertGroupRoot(const QuerySolution& solution, bool eligibleForStreamingGroup) {
        auto* root = solution.root();
        ASSERT_EQUALS(root->getType(), StageType::STAGE_GROUP) << root->toString();
        ASSERT_EQUALS(static_cast<const GroupNode*>(root)->eligibleForStreamingGroup,
                      eligibleForStreamingGroup)
            << root->toString();
    }

    std::vector<boost::intrusive_ptr<DocumentSource>> makeInnerPipelineStages(
        const Pipeline& pipeline) {
        std::vector<boost::intrusive_ptr<DocumentSource>> stages;
        for (auto&& source : pipeline.getSources()) {
            stages.emplace_back(source);
        }
        return stages;
    }

    /*
     * Builds a pipeline from raw input.
     */
    std::unique_ptr<Pipeline> buildTestPipeline(const std::vector<BSONObj>& rawPipeline) {
        expCtx->addResolvedNamespaces({kSecondaryNamespace});
        return pipeline_factory::makePipeline(
            rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    }

    const NamespaceString kSecondaryNamespace =
        NamespaceString::createNamespaceString_forTest("test.other");
    std::map<NamespaceString, CollectionInfo> secondaryCollMap;
    const std::string kWildcardIndexName = "test_wildcard";
    boost::optional<WildcardProjection> wildcardProjection;
};

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfASingleGroup) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$group: {_id: '$_id', count: {$sum: '$x'}}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    // 'runQueryWithPipeline()' only does planning of the find subsystem.
    ASSERT_EQUALS(getNumSolutions(), 1U);
    ASSERT(QueryPlannerTestLib::solutionMatches("{cscan: {dir:1, filter: {x:1}}}", solns[0]->root())
               .isOK())
        << solns[0]->root()->toString();

    // Check the plan after lowering $group into the find subsystem.
    ASSERT(!cq->cqPipeline().empty());
    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), /*secondaryCollInfos=*/{});
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$_id'}, accs: [{count: {$sum: '$x'}}], node: "
        "{cscan: {dir:1, filter: {x:1}}}"
        "}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfGroupCarriesNonArrayIdFields) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$group: {_id: {x: '$a', y: '$b'}, count: {$sum: '$c'}}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);
    checked_cast<DocumentSourceGroup&>(*pipeline->getSources().front())
        .setNonArrayIdFields({"_id.x"});

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));
    ASSERT_EQUALS(getNumSolutions(), 1U);

    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), /*secondaryCollInfos=*/{});
    ASSERT_EQ(checked_cast<const GroupNode&>(*solution->root()).nonArrayIdFields,
              OrderedPathSet({"_id.x"}));
}

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfTwoGroups) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$group: {_id: '$_id', count: {$sum: '$x'}}}"),
        fromjson("{$group: {_id: '$_id', count: {$min: '$count'}}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    // 'runQueryWithPipeline()' only does planning of the find subsystem.
    ASSERT_EQUALS(getNumSolutions(), 1U);
    ASSERT(QueryPlannerTestLib::solutionMatches("{cscan: {dir:1, filter: {x:1}}}", solns[0]->root())
               .isOK())
        << solns[0]->root()->toString();

    // Check the plan after lowering $group into the find subsystem.
    ASSERT(!cq->cqPipeline().empty());
    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), /*secondaryCollInfos=*/{});
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$_id'}, accs: [{count: {$min: '$count'}}], node: "
        "{group: {key: {_id: '$_id'}, accs: [{count: {$sum: '$x'}}], node: "
        "{cscan: {dir:1, filter: {x:1}}}"
        "}}"
        "}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfOneGroupWithMultipleAccumulators) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$group: {_id: '$_id', count: {$sum: '$x'}, m: {$min: '$y'}}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    // 'runQueryWithPipeline()' only does planning of the find subsystem.
    ASSERT_EQUALS(getNumSolutions(), 1U);
    ASSERT(QueryPlannerTestLib::solutionMatches("{cscan: {dir:1, filter: {x:1}}}", solns[0]->root())
               .isOK())
        << solns[0]->root()->toString();

    // Check the plan after lowering $group into the find subsystem.
    ASSERT(!cq->cqPipeline().empty());
    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), /*secondaryCollInfos=*/{});
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$_id'}, accs: [{count: {$sum: '$x'}}, {m: {$min: '$y'}}], node: "
        "{cscan: {dir:1, filter: {x:1}}}"
        "}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfASingleLookup) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$lookup: {from: '" + std::string{kSecondaryNamespace.coll()} +
                 "', localField: 'x', foreignField: 'y', as: 'out'}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    // 'runQueryWithPipeline()' only does planning of the find subsystem.
    ASSERT_EQUALS(getNumSolutions(), 1U);
    ASSERT(QueryPlannerTestLib::solutionMatches("{cscan: {dir:1, filter: {x:1}}}", solns[0]->root())
               .isOK())
        << solns[0]->root()->toString();

    // Check the plan after lowering $lookup into the find subsystem.
    ASSERT(!cq->cqPipeline().empty());
    auto solution = QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), secondaryCollMap);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{eq_lookup: {foreignCollection: '" + kSecondaryNamespace.toString_forTest() +
            "', joinFieldLocal: 'x', joinFieldForeign: 'y', joinField: 'out', "
            "strategy: 'NestedLoopJoin', node: "
            "{cscan: {dir:1, filter: {x:1}}}}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfTwoLookups) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$lookup: {from: '" + std::string{kSecondaryNamespace.coll()} +
                 "', localField: 'x', foreignField: 'y', as: 'out'}}"),
        fromjson("{$lookup: {from: '" + std::string{kSecondaryNamespace.coll()} +
                 "', localField: 'a', foreignField: 'b', as: 'c'}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    // 'runQueryWithPipeline()' only does planning of the find subsystem.
    ASSERT_EQUALS(getNumSolutions(), 1U);
    ASSERT(QueryPlannerTestLib::solutionMatches("{cscan: {dir:1, filter: {x:1}}}", solns[0]->root())
               .isOK())
        << solns[0]->root()->toString();

    // Check the plan after lowering both $lookups into the find subsystem.
    ASSERT(!cq->cqPipeline().empty());
    auto solution = QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), secondaryCollMap);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{eq_lookup: {foreignCollection: '" + kSecondaryNamespace.toString_forTest() +
            "', joinFieldLocal: 'a', joinFieldForeign: 'b', joinField: 'c', "
            "strategy: 'NestedLoopJoin', node: "
            "{eq_lookup: {foreignCollection: '" +
            kSecondaryNamespace.toString_forTest() +
            "', joinFieldLocal: 'x', joinFieldForeign: 'y', joinField: 'out',"
            "strategy: 'NestedLoopJoin', node: {cscan: {dir:1, filter: {x:1}}}}}}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfASingleLookupUsingWildcardIndex) {
    // A single-path wildcard index covering the foreign field routes $lookup to SBE's dynamic
    // indexed loop join. The resulting index scan's key pattern must have the '$_path' component
    // inserted ahead of the foreign field, since that's the real on-disk key shape of a wildcard
    // index (see extendWithAggPipeline()).
    auto wcProjection = WildcardKeyGenerator::createProjectionExecutor(BSON("$**" << 1), BSONObj());
    secondaryCollMap[kSecondaryNamespace].indexes.push_back(
        buildSimpleIndexEntry(BSON("$**" << 1), &wcProjection));

    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$lookup: {from: '" + std::string{kSecondaryNamespace.coll()} +
                 "', localField: 'x', foreignField: 'y', as: 'out'}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    ASSERT_EQUALS(getNumSolutions(), 1U);
    ASSERT(!cq->cqPipeline().empty());
    auto solution = QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), secondaryCollMap);

    // The eq_lookup matcher's 'node' field only checks the local-side plan (children[0]); it has
    // no support for inspecting the foreign-side index scan, so we check that part directly.
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{eq_lookup: {foreignCollection: '" + kSecondaryNamespace.toString_forTest() +
            "', joinFieldLocal: 'x', joinFieldForeign: 'y', joinField: 'out', "
            "strategy: 'DynamicIndexedLoopJoin', node: "
            "{cscan: {dir:1, filter: {x:1}}}}}",
        solution->root()))
        << solution->root()->toString();

    auto* eqLookupNode = static_cast<EqLookupNode*>(solution->root());
    ASSERT_EQUALS(eqLookupNode->children[1]->getType(), STAGE_FETCH);
    auto* fetchNode = static_cast<FetchNode*>(eqLookupNode->children[1].get());
    ASSERT_EQUALS(fetchNode->children[0]->getType(), STAGE_IXSCAN);
    auto* ixScanNode = static_cast<IndexScanNode*>(fetchNode->children[0].get());

    // The real on-disk shape of a wildcard index key is {$_path: 1, <field>: 1}, not just
    // {<field>: 1}, so '$_path' must be inserted ahead of the foreign field.
    ASSERT_BSONOBJ_EQ(ixScanNode->index.keyPattern, fromjson("{'$_path': 1, y: 1}"));
    ASSERT_EQUALS(ixScanNode->index.multikeyPaths.size(), 2U);
    ASSERT_TRUE(ixScanNode->index.multikey);
    ASSERT_TRUE(ixScanNode->shouldDedup);
}

TEST_F(QueryPlannerPipelinePushdownTest, PushdownOfTwoLookupsAndTwoGroups) {
    const std::vector<BSONObj> rawPipeline = {
        fromjson("{$lookup: {from: '" + std::string{kSecondaryNamespace.coll()} +
                 "', localField: 'x', foreignField: 'y', as: 'out'}}"),
        fromjson("{$group: {_id: '$out', count: {$sum: '$x'}}}"),
        fromjson("{$lookup: {from: '" + std::string{kSecondaryNamespace.coll()} +
                 "', localField: 'a', foreignField: 'b', as: 'c'}}"),
        fromjson("{$group: {_id: '$c', count: {$min: '$count'}}}"),
    };
    auto pipeline = buildTestPipeline(rawPipeline);

    runQueryWithPipeline(fromjson("{x: 1}"), makeInnerPipelineStages(*pipeline.get()));

    // 'runQueryWithPipeline()' only does planning of the find subsystem.
    ASSERT_EQUALS(getNumSolutions(), 1U);
    ASSERT(QueryPlannerTestLib::solutionMatches("{cscan: {dir:1, filter: {x:1}}}", solns[0]->root())
               .isOK())
        << solns[0]->root()->toString();

    // Check the plan after lowering the $groups and $lookups into the find subsystem.
    ASSERT(!cq->cqPipeline().empty());
    auto solution = QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), secondaryCollMap);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$c'}, accs: [{count: {$min: '$count'}}], node: "
        "{eq_lookup: {foreignCollection: '" +
            kSecondaryNamespace.toString_forTest() +
            "', joinFieldLocal: 'a', joinFieldForeign: 'b', joinField: 'c', "
            "strategy: 'NestedLoopJoin', node: "
            "{group: {key: {_id: '$out'}, accs: [{count: {$sum: '$x'}}], node: "
            "{eq_lookup: {foreignCollection: '" +
            kSecondaryNamespace.toString_forTest() +
            "', joinFieldLocal: 'x', joinFieldForeign: 'y', joinField: 'out',"
            "strategy: 'NestedLoopJoin', node: "
            "{cscan: {dir:1, filter: {x:1}}}}}}}}}}}",
        solution->root()))
        << solution->root()->toString();
}

// Tests for removeInclusionProjectionBelowGroup: the optimizer eliminates a $project that sits
// directly below a $group when the group doesn't actually read the projected fields.
//
// These tests construct a DocumentSourceInternalProjection directly (the stage type that
// extendWithAggPipeline recognises as a pushdown-able $project) so they can exercise the
// QSN-level optimisation without requiring trySbeEngine to be set.

TEST_F(QueryPlannerPipelinePushdownTest, ComputedProjectionBeforeZeroDependencyGroupIsEliminated) {
    // Pipeline: [$_internalProjection({k0: {$add: ['$a', '$b']}}), $group({_id: null, total: {$sum:
    // 1}})] The $group has no field dependencies (_id is constant null, accumulator is constant 1).
    // Even though the $project computes an expression, its output is entirely unused by the $group,
    // so the optimizer should eliminate the intermediate projection node.
    auto projStage = make_intrusive<DocumentSourceInternalProjection>(
        expCtx, fromjson("{k0: {$add: ['$a', '$b']}}"), InternalProjectionPolicyEnum::kAggregate);

    const std::vector<BSONObj> rawGroupPipeline = {
        fromjson("{$group: {_id: null, total: {$sum: 1}}}"),
    };
    auto groupPipeline = buildTestPipeline(rawGroupPipeline);
    auto stages = makeInnerPipelineStages(*groupPipeline);
    stages.insert(stages.begin(), projStage);

    runQueryWithPipeline(BSONObj(), std::move(stages));
    ASSERT_EQUALS(getNumSolutions(), 1U);

    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), /*secondaryCollInfos=*/{});
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: {$const: null}}, accs: [{total: {$sum: {$const: 1}}}], "
        "node: {cscan: {dir: 1}}}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest,
       ComputedProjectionBeforeGroupWithFieldDepsIsNotEliminated) {
    // Pipeline: [$_internalProjection({k0: {$add: ['$a', '$b']}}), $group({_id: null, total: {$sum:
    // '$k0'}})] The $group accumulates over field 'k0' (computed by the $project), so
    // requiredFields is non-empty.  The projection must be preserved.
    auto projStage = make_intrusive<DocumentSourceInternalProjection>(
        expCtx, fromjson("{k0: {$add: ['$a', '$b']}}"), InternalProjectionPolicyEnum::kAggregate);

    const std::vector<BSONObj> rawGroupPipeline = {
        fromjson("{$group: {_id: null, total: {$sum: '$k0'}}}"),
    };
    auto groupPipeline = buildTestPipeline(rawGroupPipeline);
    auto stages = makeInnerPipelineStages(*groupPipeline);
    stages.insert(stages.begin(), projStage);

    runQueryWithPipeline(BSONObj(), std::move(stages));
    ASSERT_EQUALS(getNumSolutions(), 1U);

    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), /*secondaryCollInfos=*/{});
    // The direct-to-cscan pattern must NOT match — a proj node must still be present between the
    // group and the cscan.
    ASSERT_NOT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: {$const: null}}, accs: [{total: {$sum: '$k0'}}], "
        "node: {cscan: {dir: 1}}}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest,
       MixedProjectionBeforeGroupReadingOnlyPassthroughFieldIsEliminated) {
    // Pipeline: [$_internalProjection({k0: {$add: ['$a', '$b']}, b: 1}), $group({_id: '$b',
    // cnt: {$sum: 1}})] The projection is mixed: 'k0' is computed, 'b' is a plain pass-through.
    // The $group only reads 'b', which isFieldRetainedExactly returns true for, so the
    // projection should be eliminated.
    auto projStage = make_intrusive<DocumentSourceInternalProjection>(
        expCtx,
        fromjson("{k0: {$add: ['$a', '$b']}, b: 1}"),
        InternalProjectionPolicyEnum::kAggregate);

    const std::vector<BSONObj> rawGroupPipeline = {
        fromjson("{$group: {_id: '$b', cnt: {$sum: 1}}}"),
    };
    auto groupPipeline = buildTestPipeline(rawGroupPipeline);
    auto stages = makeInnerPipelineStages(*groupPipeline);
    stages.insert(stages.begin(), projStage);

    runQueryWithPipeline(BSONObj(), std::move(stages));
    ASSERT_EQUALS(getNumSolutions(), 1U);

    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), /*secondaryCollInfos=*/{});
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$b'}, accs: [{cnt: {$sum: {$const: 1}}}], "
        "node: {cscan: {dir: 1}}}}",
        solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest,
       MixedProjectionBeforeGroupReadingComputedFieldIsNotEliminated) {
    // Pipeline: [$_internalProjection({k0: {$add: ['$a', '$b']}, b: 1}), $group({_id: '$k0'})]
    // The $group reads 'k0', which is a computed field in the projection.
    // isFieldRetainedExactly('k0') is false, so the projection must be preserved.
    auto projStage = make_intrusive<DocumentSourceInternalProjection>(
        expCtx,
        fromjson("{k0: {$add: ['$a', '$b']}, b: 1}"),
        InternalProjectionPolicyEnum::kAggregate);

    const std::vector<BSONObj> rawGroupPipeline = {
        fromjson("{$group: {_id: '$k0'}}"),
    };
    auto groupPipeline = buildTestPipeline(rawGroupPipeline);
    auto stages = makeInnerPipelineStages(*groupPipeline);
    stages.insert(stages.begin(), projStage);

    runQueryWithPipeline(BSONObj(), std::move(stages));
    ASSERT_EQUALS(getNumSolutions(), 1U);

    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), /*secondaryCollInfos=*/{});
    // The direct-to-cscan pattern must NOT match — a proj node must still be present between the
    // group and the cscan.
    ASSERT_NOT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$k0'}, accs: [], node: {cscan: {dir: 1}}}}", solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerPipelinePushdownTest,
       ExclusionProjectionNotExcludingRequiredFieldsIsEliminatedBeforeGroup) {
    // Pipeline: [$_internalProjection({c: 0}), $group({_id: '$a', total: {$sum: '$b'}})]
    // {c: 0} retains 'a' and 'b' exactly (isFieldRetainedExactly returns true for fields not
    // mentioned in an exclusion), so the optimizer should eliminate the projection.
    auto projStage = make_intrusive<DocumentSourceInternalProjection>(
        expCtx, fromjson("{c: 0}"), InternalProjectionPolicyEnum::kAggregate);

    const std::vector<BSONObj> rawGroupPipeline = {
        fromjson("{$group: {_id: '$a', total: {$sum: '$b'}}}"),
    };
    auto groupPipeline = buildTestPipeline(rawGroupPipeline);
    auto stages = makeInnerPipelineStages(*groupPipeline);
    stages.insert(stages.begin(), projStage);

    runQueryWithPipeline(BSONObj(), std::move(stages));
    ASSERT_EQUALS(getNumSolutions(), 1U);

    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), /*secondaryCollInfos=*/{});
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(
        "{group: {key: {_id: '$a'}, accs: [{total: {$sum: '$b'}}], "
        "node: {cscan: {dir: 1}}}}",
        solution->root()))
        << solution->root()->toString();
}

class StreamingGroupPlanningTest : public QueryPlannerPipelinePushdownTest {
protected:
    void setUp() override {
        QueryPlannerPipelinePushdownTest::setUp();
        params.mainCollectionInfo.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    }

    std::unique_ptr<QuerySolution> planAggregate(
        const std::vector<BSONObj>& rawPipeline,
        std::shared_ptr<const PathArrayness> pathArrayness,
        std::unique_ptr<CollatorInterface> collator = nullptr) {
        planCandidates(rawPipeline, std::move(pathArrayness), std::move(collator));
        ASSERT_EQUALS(getNumSolutions(), 1U);
        return QueryPlanner::extendWithAggPipeline(
            *cq, std::move(solns[0]), {} /* secondaryCollInfos */);
    }

    void planCandidates(const std::vector<BSONObj>& rawPipeline,
                        std::shared_ptr<const PathArrayness> pathArrayness,
                        std::unique_ptr<CollatorInterface> collator = nullptr) {
        clearState();
        expCtx = ExpressionContextBuilder{}
                     .opCtx(opCtx.get())
                     .ns(nss)
                     .collator(std::move(collator))
                     .build();
        expCtx->setPathArraynessForNss(nss, std::move(pathArrayness));
        auto pipeline =
            pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
        pipeline_optimization::optimizePipeline(*pipeline);

        cq = uassertStatusOK(createCanonicalQuery(expCtx, nss, *pipeline));
        cq->setSbeCompatible(markQueriesSbeCompatible);
        attachPipelineStages(
            MultipleCollectionAccessor{},
            pipeline.get(),
            /*needsMerge=*/false,
            cq.get(),
            std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForTest{}));

        auto statusWithSolns = QueryPlanner::plan(*cq, params);
        ASSERT_OK(statusWithSolns.getStatus());
        solns = std::move(statusWithSolns.getValue());
    }

    static std::shared_ptr<const PathArrayness> nonArray(const BSONObj& keyPattern) {
        return makePathArrayness(keyPattern, MultikeyPaths(keyPattern.nFields()));
    }

    static const OrderedPathSet& rootNonArrayIdFields(const QuerySolution& solution) {
        return checked_cast<const GroupNode&>(*solution.root()).nonArrayIdFields;
    }
};

class StreamingGroupEligibilityTest : public StreamingGroupPlanningTest {
protected:
    unittest::ServerParameterGuard eligibilityCounterEnabled{
        "featureFlagStreamingGroupEligibilityCounter", true};
};

TEST_F(StreamingGroupEligibilityTest, EligibleForCoveredIndexSort) {
    addIndex(fromjson("{a: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: '$a', total: {$sum: '$a'}}}")},
                                  nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, true);
    ASSERT_EQ(rootNonArrayIdFields(*solution), OrderedPathSet({"_id"}));
}

TEST_F(StreamingGroupPlanningTest, IneligibleWithoutFeatureFlag) {
    unittest::ServerParameterGuard eligibilityCounterDisabled{
        "featureFlagStreamingGroupEligibilityCounter", false};
    addIndex(fromjson("{a: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: '$a', total: {$sum: '$a'}}}")},
                                  nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, false);
    ASSERT_EQ(rootNonArrayIdFields(*solution), OrderedPathSet{});
}

TEST_F(StreamingGroupEligibilityTest, EligibleAfterEqualityPrefix) {
    addIndex(fromjson("{a: 1, b: 1}"));

    auto solution = planAggregate(
        {fromjson("{$match: {a: 5}}"), fromjson("{$group: {_id: '$b', total: {$sum: '$b'}}}")},
        nonArray(fromjson("{a: 1, b: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForDescendingSort) {
    addIndex(fromjson("{a: -1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: '$a', total: {$sum: '$a'}}}")},
                                  nonArray(fromjson("{a: -1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForLeadingSort) {
    addIndex(fromjson("{a: 1}"));

    auto solution = planAggregate(
        {fromjson("{$sort: {a: 1}}"), fromjson("{$group: {_id: '$a', total: {$sum: '$a'}}}")},
        nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForLeadingProjection) {
    addIndex(fromjson("{a: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$project: {_id: 0, a: 1}}"),
                                   fromjson("{$group: {_id: '$a', total: {$sum: '$a'}}}")},
                                  nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleWhenKeyNotCoveredBySort) {
    addIndex(fromjson("{a: 1, b: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: '$b', total: {$sum: '$b'}}}")},
                                  nonArray(fromjson("{a: 1, b: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, EligibilityDependsOnCandidatePlan) {
    addIndex(fromjson("{a: 1}"));
    addIndex(fromjson("{b: 1}"));

    planCandidates({fromjson("{$match: {a: {$gt: 0}, b: {$gt: 0}}}"),
                    fromjson("{$group: {_id: '$a', total: {$sum: '$b'}}}")},
                   nonArray(fromjson("{a: 1, b: 1}")));
    ASSERT_EQUALS(getNumSolutions(), 2U);
    const auto& group = checked_cast<const DocumentSourceGroup&>(*cq->cqPipeline().front());
    ASSERT_EQ(group.getNonArrayIdFields(), OrderedPathSet({"_id"}));

    StringMap<bool> eligibleByIndex;
    for (auto&& candidate : solns) {
        auto scansA = QueryPlannerTestLib::solutionMatches(
                          "{proj: {spec: {a: 1, b: 1, _id: 0}, node: {fetch: {node: {ixscan: "
                          "{pattern: {a: 1}}}}}}}",
                          candidate->root())
                          .isOK();
        auto solution = QueryPlanner::extendWithAggPipeline(
            *cq, std::move(candidate), {} /* secondaryCollInfos */);
        eligibleByIndex[scansA ? "a_1" : "b_1"] =
            checked_cast<const GroupNode&>(*solution->root()).eligibleForStreamingGroup;
    }
    ASSERT_EQ(eligibleByIndex.size(), 2U);
    ASSERT_TRUE(eligibleByIndex.at("a_1"));
    ASSERT_FALSE(eligibleByIndex.at("b_1"));
}

TEST_F(StreamingGroupEligibilityTest, IneligibleForEqualityOnKey) {
    addIndex(fromjson("{a: 1}"));

    auto solution = planAggregate(
        {fromjson("{$match: {a: 5}}"), fromjson("{$group: {_id: '$a', total: {$sum: '$a'}}}")},
        nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleWhenProjectionOverwritesEqualityKey) {
    addIndex(fromjson("{a: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: 5}}"),
                                   fromjson("{$project: {_id: 0, a: '$c'}}"),
                                   fromjson("{$group: {_id: '$a', n: {$sum: 1}}}")},
                                  nonArray(fromjson("{a: 1, c: 1}")));
    assertGroupRoot(*solution, false);
    ASSERT_EQ(rootNonArrayIdFields(*solution), OrderedPathSet({"_id"}));
}

TEST_F(StreamingGroupEligibilityTest, IneligibleWhenProjectionRenamesArrayIntoKey) {
    addIndex(fromjson("{a: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$project: {_id: 0, a: '$c'}}"),
                                   fromjson("{$group: {_id: '$a', n: {$sum: 1}}}")},
                                  nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, false);
    ASSERT_EQ(rootNonArrayIdFields(*solution), OrderedPathSet{});
}

TEST_F(StreamingGroupEligibilityTest, IneligibleForHashedIndex) {
    addIndex(fromjson("{a: 'hashed'}"));

    auto solution = planAggregate(
        {fromjson("{$match: {a: 5}}"), fromjson("{$group: {_id: '$a', total: {$sum: '$a'}}}")},
        nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, false);
    ASSERT_EQ(rootNonArrayIdFields(*solution), OrderedPathSet({"_id"}));
}

TEST_F(StreamingGroupEligibilityTest, EligibleForNonCoveredPlan) {
    addIndex(fromjson("{a: 1, b: 1}"));

    auto solution = planAggregate(
        {fromjson("{$match: {a: 5}}"), fromjson("{$group: {_id: '$b', total: {$sum: '$c'}}}")},
        nonArray(fromjson("{a: 1, b: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForPartialIndex) {
    const auto partialFilterObj = fromjson("{a: {$gt: 0}}");
    auto partialFilter = parseMatchExpression(partialFilterObj);
    addIndex(fromjson("{a: 1}"), partialFilter.get());

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: '$a', total: {$sum: '$a'}}}")},
                                  nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForSparseIndex) {
    addIndex(fromjson("{a: 1}"), false /* multikey */, true /* sparse */);

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: '$a', total: {$sum: '$a'}}}")},
                                  nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForWildcardIndex) {
    addWildcardIndex(fromjson("{'$**': 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: '$a', total: {$sum: '$a'}}}")},
                                  nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleForWildcardMultikeyPath) {
    addWildcardIndex(fromjson("{'$**': 1}"), {FieldRef("a")});

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: '$a', total: {$sum: '$a'}}}")},
                                  nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, InclusionProjectionBelowEligibleGroupIsElided) {
    addIndex(fromjson("{a: 1, b: 1}"));

    auto solution = planAggregate(
        {fromjson("{$match: {a: 5}}"), fromjson("{$group: {_id: '$b', total: {$sum: '$c'}}}")},
        nonArray(fromjson("{a: 1, b: 1}")));
    assertGroupRoot(*solution, true);
    ASSERT_EQUALS(solution->root()->children[0]->getType(), StageType::STAGE_FETCH);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForAnyAccumulator) {
    addIndex(fromjson("{a: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: '$a', arr: {$push: '$a'}}}")},
                                  nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForSingleMemberObjectId) {
    addIndex(fromjson("{a: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: {x: '$a'}, total: {$sum: '$a'}}}")},
                                  nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForCompoundKey) {
    addIndex(fromjson("{a: 1, b: 1, c: 1}"));

    auto solution =
        planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                       fromjson("{$group: {_id: {x: '$a', y: '$b'}, total: {$sum: '$c'}}}")},
                      nonArray(fromjson("{a: 1, b: 1, c: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForCompoundKeyCoveringWholeSort) {
    addIndex(fromjson("{a: 1, b: 1, c: 1}"));

    auto solution =
        planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                       fromjson("{$group: {_id: {x: '$a', y: '$b', z: '$c'}, n: {$sum: 1}}}")},
                      nonArray(fromjson("{a: 1, b: 1, c: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForCompoundKeyInAnyOrder) {
    addIndex(fromjson("{a: 1, b: 1, c: 1}"));

    auto solution =
        planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                       fromjson("{$group: {_id: {y: '$b', x: '$a'}, total: {$sum: '$c'}}}")},
                      nonArray(fromjson("{a: 1, b: 1, c: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForMixedDirections) {
    addIndex(fromjson("{a: 1, b: -1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: {x: '$a', y: '$b'}, n: {$sum: 1}}}")},
                                  nonArray(fromjson("{a: 1, b: -1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForDottedCompoundKey) {
    addIndex(fromjson("{'a.b': 1, c: 1}"));

    auto solution = planAggregate({fromjson("{$match: {'a.b': {$gt: 0}}}"),
                                   fromjson("{$group: {_id: {x: '$a.b', y: '$c'}, n: {$sum: 1}}}")},
                                  nonArray(fromjson("{'a.b': 1, c: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForRepeatedPath) {
    addIndex(fromjson("{a: 1, b: 1}"));

    auto solution =
        planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                       fromjson("{$group: {_id: {x: '$a', y: '$a'}, total: {$sum: '$b'}}}")},
                      nonArray(fromjson("{a: 1, b: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForRepeatedPathInCompoundKey) {
    addIndex(fromjson("{a: 1, b: 1, c: 1}"));

    auto solution = planAggregate(
        {fromjson("{$match: {a: {$gt: 0}}}"),
         fromjson("{$group: {_id: {x: '$a', y: '$b', z: '$a'}, total: {$sum: '$c'}}}")},
        nonArray(fromjson("{a: 1, b: 1, c: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, EligibleForCompoundKeyAfterEqualityPrefix) {
    addIndex(fromjson("{a: 1, b: 1, c: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: 5}}"),
                                   fromjson("{$group: {_id: {x: '$b', y: '$c'}, n: {$sum: 1}}}")},
                                  nonArray(fromjson("{a: 1, b: 1, c: 1}")));
    assertGroupRoot(*solution, true);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleWhenCompoundKeyIsNotSortPrefix) {
    addIndex(fromjson("{a: 1, b: 1, c: 1}"));

    auto solution =
        planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                       fromjson("{$group: {_id: {x: '$a', y: '$c'}, total: {$sum: '$b'}}}")},
                      nonArray(fromjson("{a: 1, b: 1, c: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleWhenCompoundKeyExceedsSort) {
    addIndex(fromjson("{a: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: {x: '$a', y: '$b'}, n: {$sum: 1}}}")},
                                  nonArray(fromjson("{a: 1, b: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleForEqualityBoundIdField) {
    addIndex(fromjson("{a: 1, b: 1, c: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: 5}}"),
                                   fromjson("{$group: {_id: {x: '$a', y: '$b'}, n: {$sum: 1}}}")},
                                  nonArray(fromjson("{a: 1, b: 1, c: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleWhenProjectionRenamesConstantIntoCompoundKey) {
    addIndex(fromjson("{a: 1, b: 1, c: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: 5}}"),
                                   fromjson("{$project: {_id: 0, b: '$a', c: 1}}"),
                                   fromjson("{$group: {_id: {x: '$b', y: '$c'}, n: {$sum: 1}}}")},
                                  nonArray(fromjson("{a: 1, b: 1, c: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest,
       IneligibleWhenProjectionOverwritesEqualityFieldInCompoundKey) {
    addIndex(fromjson("{a: 1, b: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: 5}}"),
                                   fromjson("{$project: {_id: 0, a: '$c', b: 1}}"),
                                   fromjson("{$group: {_id: {x: '$a', y: '$b'}, n: {$sum: 1}}}")},
                                  nonArray(fromjson("{a: 1, b: 1, c: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleWhenCompoundKeyMemberMayBeArray) {
    addIndex(fromjson("{a: 1, b: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: {x: '$a', y: '$c'}, n: {$sum: 1}}}")},
                                  nonArray(fromjson("{a: 1, b: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleForComputedIdMember) {
    addIndex(fromjson("{a: 1, b: 1}"));

    auto solution =
        planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                       fromjson("{$group: {_id: {x: '$a', y: {$add: ['$b', 1]}}, n: {$sum: 1}}}")},
                      nonArray(fromjson("{a: 1, b: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleForNestedIdMember) {
    addIndex(fromjson("{a: 1, b: 1}"));

    auto solution =
        planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                       fromjson("{$group: {_id: {x: '$a', y: {z: '$b'}}, n: {$sum: 1}}}")},
                      nonArray(fromjson("{a: 1, b: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleForWholeDocumentIdMember) {
    addIndex(fromjson("{a: 1}"));

    auto solution =
        planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                       fromjson("{$group: {_id: {x: '$a', y: '$$ROOT'}, n: {$sum: 1}}}")},
                      nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleForArrayId) {
    addIndex(fromjson("{a: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: ['$a'], total: {$sum: '$a'}}}")},
                                  nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleForComputedId) {
    addIndex(fromjson("{a: 1}"));

    auto solution =
        planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                       fromjson("{$group: {_id: {$add: ['$a', 1]}, total: {$sum: '$a'}}}")},
                      nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleWhenKeyMayBeArray) {
    addIndex(fromjson("{a: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$group: {_id: '$a', total: {$sum: '$a'}}}")},
                                  makePathArrayness(fromjson("{a: 1}"), MultikeyPaths{{0}}));
    assertGroupRoot(*solution, false);
    ASSERT_EQ(rootNonArrayIdFields(*solution), OrderedPathSet{});
}

TEST_F(StreamingGroupEligibilityTest, IneligibleForNonSimpleCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(fromjson("{a: 1}"), &collator);

    auto solution = planAggregate(
        {fromjson("{$match: {a: {$gt: 'foo'}}}"), fromjson("{$group: {_id: '$a'}}")},
        nonArray(fromjson("{a: 1}")),
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleWhenWillBeMerged) {
    addIndex(fromjson("{a: 1}"));

    auto solution =
        planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                       fromjson("{$group: {_id: '$a', total: {$sum: '$a'}, $willBeMerged: true}}")},
                      nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleForWholeDocumentId) {
    addIndex(fromjson("{a: 1}"));

    auto solution =
        planAggregate({fromjson("{$match: {a: {$gt: 0}}}"), fromjson("{$group: {_id: '$$ROOT'}}")},
                      nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, false);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleWhenNotFirstPushedDownStage) {
    unittest::ServerParameterGuard nonLeadingMatch{"featureFlagSbeNonLeadingMatch", true};
    addIndex(fromjson("{a: 1}"));

    auto solution = planAggregate({fromjson("{$match: {a: {$gt: 0}}}"),
                                   fromjson("{$limit: 10}"),
                                   fromjson("{$match: {b: 1}}"),
                                   fromjson("{$group: {_id: '$a', total: {$sum: '$a'}}}")},
                                  nonArray(fromjson("{a: 1}")));
    assertGroupRoot(*solution, false);
    ASSERT_EQ(rootNonArrayIdFields(*solution), OrderedPathSet({"_id"}));
    ASSERT_EQUALS(solution->root()->children[0]->getType(), StageType::STAGE_MATCH);
}

TEST_F(StreamingGroupEligibilityTest, IneligibleForTimeSeries) {
    addIndex(fromjson("{a: 1}"));

    auto unpackStage = make_intrusive<DocumentSourceInternalUnpackBucket>(
        expCtx,
        timeseries::BucketUnpacker{timeseries::BucketSpec(
            "time", boost::none, {"a"}, timeseries::BucketSpec::Behavior::kInclude)},
        3600 /* bucketMaxSpanSeconds */);
    auto pipeline = buildTestPipeline({fromjson("{$group: {_id: '$a', total: {$sum: '$a'}}}")});
    checked_cast<DocumentSourceGroup&>(*pipeline->getSources().front())
        .setNonArrayIdFields({"_id"});
    auto stages = makeInnerPipelineStages(*pipeline);
    stages.insert(stages.begin(), unpackStage);

    runQueryWithPipeline(fromjson("{a: {$gt: 0}}"), std::move(stages));
    ASSERT_EQUALS(getNumSolutions(), 1U);

    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), {} /* secondaryCollInfos */);
    assertGroupRoot(*solution, false);
}

}  //  namespace
