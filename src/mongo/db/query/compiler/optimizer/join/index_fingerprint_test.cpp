// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/index_fingerprint.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mongo::join_ordering {
namespace {

const auto kNss = NamespaceString::createNamespaceString_forTest("test", "fingerprint");
// A second collection, for the multi-collection makeNodeFingerprints tests.
const auto kOtherNss = NamespaceString::createNamespaceString_forTest("test", "fingerprintOther");

// The shape of a node's access path query. Aggregate-initialized by the tests so that each call
// site names the parts it cares about and omits the rest. At namespace scope rather than nested in
// the fixture, so that its default member initializers are usable in a default argument below.
struct QueryArgs {
    BSONObj filter = BSONObj::kEmptyObject;
    BSONObj projection = BSONObj::kEmptyObject;
    BSONObj sort = BSONObj::kEmptyObject;
    // Only the makeNodeFingerprints tests need more than one collection.
    NamespaceString nss = kNss;
};

class IndexFingerprintTest : public JoinOrderingTestFixture {
public:
    void setUp() override {
        JoinOrderingTestFixture::setUp();
        createCollection(kNss);
        createCollection(kOtherNss);
    }

    // Builds an index on 'nss'. Deliberately not the fixture's createIndex(), which routes through
    // IndexBuildsCoordinator and requires the caller to already hold the collection X lock; this
    // takes its own lock. Only valid because these tests never insert documents.
    void addIndexOn(const NamespaceString& nss,
                    BSONObj keyPattern,
                    std::string name,
                    BSONObj extraOptions = BSONObj::kEmptyObject) {
        BSONObjBuilder spec;
        spec.append("v", int(IndexConfig::kLatestIndexVersion));
        spec.append("name", name);
        spec.append("key", keyPattern);
        spec.appendElements(extraOptions);
        ASSERT_OK(storageInterface()->createIndexesOnEmptyCollection(
            operationContext(), nss, {spec.obj()}));
    }

    void addIndex(BSONObj keyPattern,
                  std::string name,
                  BSONObj extraOptions = BSONObj::kEmptyObject) {
        addIndexOn(kNss, std::move(keyPattern), std::move(name), std::move(extraOptions));
    }

    // Drops the index named 'name' through the index catalog, mirroring a real dropIndex DDL.
    void dropIndexOn(const NamespaceString& nss, std::string_view name) {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite),
            MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer(operationContext(), &coll);
        auto* writable = writer.getWritableCollection(operationContext());
        size_t dropped = 0;
        writable->getIndexCatalog()->dropIndexes(
            operationContext(),
            writable,
            [&](const IndexCatalogEntry* entry) {
                return entry->descriptor()->indexName() == name;
            },
            [&](const IndexCatalogEntry*) { ++dropped; });
        ASSERT_EQ(1, dropped);
        wuow.commit();
    }

    void dropIndex(std::string_view name) {
        dropIndexOn(kNss, name);
    }

    // Hides or unhides an existing index, which is what a collMod does.
    void setIndexHiddenOn(const NamespaceString& nss, std::string_view name, bool hidden) {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite),
            MODE_X);
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer(operationContext(), &coll);
        auto* writable = writer.getWritableCollection(operationContext());
        writable->updateHiddenSetting(operationContext(), name, hidden);
        // 'updateHiddenSetting' only writes the durable catalog metadata; the in-memory
        // IndexCatalogEntry keeps its old descriptor until rebuilt, so 'desc->hidden()' would still
        // report the previous value. A real collMod pairs the two calls the same way -- see
        // '_processCollModIndexRequestHidden' and the 'refreshEntry' at the end of
        // 'processCollModIndexRequest'.
        auto* entry = writable->getIndexCatalog()->findIndexByName(operationContext(), name);
        ASSERT(entry);
        writable->getIndexCatalog()->refreshEntry(
            operationContext(), writable, entry, CreateIndexEntryFlags::kIsReady);
        wuow.commit();
    }

    // All ready index entries on 'nss'. In production this list is the INLJ-eligible subset
    // produced by 'extractINLJEligibleIndexes'; computeRelevantIndexFingerprint itself
    // does no eligibility filtering, so these tests hand it the catalog contents directly and
    // exercise only the relevance filtering and hashing that it does own.
    std::vector<std::shared_ptr<const IndexCatalogEntry>> readyIndexes(
        const MultipleCollectionAccessor& mca, const NamespaceString& nss) {
        return mca.lookupCollection(nss)->getIndexCatalog()->getEntriesShared(
            IndexCatalog::InclusionPolicy::kReady);
    }

    // Hashes the indexes of 'kNss' that are relevant to 'relevantFields'.
    std::vector<IndexFingerprint> relevantHashes(
        std::initializer_list<std::string> relevantFields) {
        auto mca = multipleCollectionAccessor(operationContext(), {kNss});
        return computeRelevantIndexHashes(readyIndexes(mca, kNss), StringSet(relevantFields));
    }

    // Fingerprints the indexes of 'kNss' named in 'usedIndexNames'.
    IndexFingerprint usedFingerprint(std::initializer_list<std::string> usedIndexNames) {
        auto mca = multipleCollectionAccessor(operationContext(), {kNss});
        return computeUsedIndexFingerprint(readyIndexes(mca, kNss), StringSet(usedIndexNames));
    }

    // Fingerprints a node against the current catalog of 'kNss': one referencing 'relevantFields',
    // whose plan reads from the indexes in 'usedIndexNames'. Used by the canReuseNodeFingerprint
    // tests below, which compare one taken before a DDL against one taken after.
    NodeFingerprint currentFingerprint(std::initializer_list<std::string> usedIndexNames,
                                       std::initializer_list<std::string> relevantFields = {"a"}) {
        return NodeFingerprint{.usedFingerprint = usedFingerprint(usedIndexNames),
                               .relevantIndexHashes = relevantHashes(relevantFields)};
    }

    // Deliberately not the fixture's makeCanonicalQuery(), which cannot express a sort and which
    // installs a pre-parsed filter. Going through ParsedFindCommandParams parses filter, projection
    // and sort the way a real find command would.
    std::unique_ptr<CanonicalQuery> makeQuery(const QueryArgs& args) {
        auto expCtx = ExpressionContextBuilder{}.opCtx(operationContext()).build();
        auto findCmd = std::make_unique<FindCommandRequest>(args.nss);
        findCmd->setFilter(args.filter.getOwned());
        findCmd->setProjection(args.projection.getOwned());
        findCmd->setSort(args.sort.getOwned());
        return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = expCtx, .parsedFind = ParsedFindCommandParams{std::move(findCmd)}});
    }

    void assertRelevantFields(const CanonicalQuery& cq, const StringSet& expected) {
        ASSERT_EQ(expected, relevantFieldsForQuery(cq));
    }

    // Adds a node to the fixture's 'graph' whose access path is a query of the given shape.
    NodeId addGraphNode(const QueryArgs& args = {}) {
        auto nodeId = graph.addNode(args.nss, makeQuery(args), boost::none);
        ASSERT_TRUE(nodeId.has_value());
        return *nodeId;
    }

    // Registers 'fieldPath' as a resolved path owned by 'nodeId' and returns its id, for use as one
    // side of a join predicate.
    PathId addJoinPath(NodeId nodeId, std::string fieldPath) {
        auto pathId = static_cast<PathId>(resolvedPaths.size());
        resolvedPaths.emplace_back(ResolvedPath{nodeId, FieldPath(std::move(fieldPath))});
        return pathId;
    }

    // Freezes 'graph' so that it can be queried. Deliberately not the fixture's makeContext(),
    // which also builds the cardinality and cost scaffolding these tests have no use for.
    JoinGraph freeze() {
        return JoinGraph(std::move(graph));
    }

    void assertNodeFields(const JoinGraph& joinGraph, NodeId nodeId, const StringSet& expected) {
        ASSERT_EQ(expected, relevantFieldsPerNode(joinGraph, resolvedPaths)[nodeId]);
    }

    // A cached INLJ node on 'nodeId' probing the index named 'indexName'.
    static std::unique_ptr<CachedJoinPlan> inljPlanNode(NodeId nodeId, std::string indexName) {
        return std::make_unique<CachedJoinPlan>(
            CachedInljNode{.nodeId = nodeId, .inljForeignIndexName = std::move(indexName)});
    }

    struct CachedIndex {
        std::string name;
        BSONObj keyPattern;
    };

    // A cached access path on 'nodeId' reading from 'indexes': the first is the root of the index
    // tree, any others are its children.
    static std::unique_ptr<CachedJoinPlan> accessPathPlanNode(
        NodeId nodeId, const std::vector<CachedIndex>& indexes) {
        auto cacheData = std::make_unique<SolutionCacheData>();
        for (const auto& index : indexes) {
            auto node = std::make_unique<PlanCacheIndexTree>();
            node->entry = std::make_unique<IndexEntry>(index.keyPattern,
                                                       IndexType::INDEX_BTREE,
                                                       IndexConfig::kLatestIndexVersion,
                                                       false /* multikey */,
                                                       MultikeyPaths{},
                                                       std::set<FieldRef>{},
                                                       false /* sparse */,
                                                       false /* unique */,
                                                       IndexEntry::Identifier{index.name},
                                                       BSONObj() /* infoObj */,
                                                       nullptr /* wildcardProjection */);
            if (cacheData->tree) {
                cacheData->tree->children.push_back(std::move(node));
            } else {
                cacheData->tree = std::move(node);
            }
        }
        return std::make_unique<CachedJoinPlan>(
            CachedAccessPath{.nodeId = nodeId, .solnCacheData = std::move(cacheData)});
    }

    // A cached plan over 'left' and 'right'.
    static std::unique_ptr<CachedJoinPlan> joinPlanNode(std::unique_ptr<CachedJoinPlan> left,
                                                        std::unique_ptr<CachedJoinPlan> right) {
        return std::make_unique<CachedJoinPlan>(
            CachedJoinNode{.left = std::move(left), .right = std::move(right)});
    }

    // A two-node cached plan that reads from no index at all, for the tests that only care about
    // the relevant index set.
    static std::unique_ptr<CachedJoinPlan> planUsingNoIndexes() {
        return joinPlanNode(accessPathPlanNode(NodeId{0}, {}), accessPathPlanNode(NodeId{1}, {}));
    }

    // Fingerprints every node of 'joinGraph' against the live index catalogs of 'nssList'. The
    // accessor is held for the whole call so that the catalog entries it hands out stay valid.
    std::vector<NodeFingerprint> fingerprintGraph(const JoinGraph& joinGraph,
                                                  std::vector<NamespaceString> nssList,
                                                  const CachedJoinPlan* plan = nullptr) {
        auto mca = multipleCollectionAccessor(operationContext(), nssList);
        AvailableIndexes perCollIdxs;
        for (const auto& nss : nssList) {
            perCollIdxs.emplace(nss, readyIndexes(mca, nss));
        }
        auto defaultPlan = plan ? nullptr : planUsingNoIndexes();
        auto fingerprints = makeNodeFingerprints(
            joinGraph, resolvedPaths, perCollIdxs, plan ? *plan : *defaultPlan);
        // Checked here rather than per test: every caller below indexes the result by NodeId, which
        // would be an out-of-range read rather than a clean failure if this ever regressed.
        ASSERT_EQ(joinGraph.numNodes(), fingerprints.size());
        return fingerprints;
    }
};

//
// Tests for relevantFieldsForQuery: the query's filter.
//
// The function intentionally produces a superset of what the plan enumerator will build bounds on
// today, so these tests are mostly about the places where a narrower walk would lose a field.
//

TEST_F(IndexFingerprintTest, RelevantFieldsCollectsFilterFields) {
    assertRelevantFields(*makeQuery({.filter = fromjson("{a: 1, 'b.c': {$gt: 1}}")}), {"a", "b.c"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsTraversesNorSubtrees) {
    // QueryPlannerIXSelect::getFields stops at a $nor and would report no fields here, but the
    // planner does consider indexes underneath a negation. A multi-operand $nor is used because a
    // single-operand one is normalized to $not before the query is ever planned.
    assertRelevantFields(*makeQuery({.filter = fromjson("{$nor: [{a: 1}, {b: 1}]}")}), {"a", "b"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsTraversesOrAndNotSubtrees) {
    assertRelevantFields(*makeQuery({.filter = fromjson("{$or: [{a: 1}, {b: {$not: {$gt: 1}}}]}")}),
                         {"a", "b"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsPrefixesElemMatchObjectFields) {
    // '{a: {$elemMatch: {b: 1}}}' is really a predicate on 'a.b', which is how an index over it
    // would be declared. 'a' itself is kept too, in case an index is built on the array field.
    assertRelevantFields(*makeQuery({.filter = fromjson("{a: {$elemMatch: {b: 1, 'c.d': 2}}}")}),
                         {"a", "a.b", "a.c.d"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsHandlesElemMatchValue) {
    // The value form has no child path to prefix.
    assertRelevantFields(*makeQuery({.filter = fromjson("{a: {$elemMatch: {$gt: 1}}}")}), {"a"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsCollectsInAndNinFields) {
    assertRelevantFields(*makeQuery({.filter = fromjson("{a: {$in: [1, 2]}}")}), {"a"});
    assertRelevantFields(*makeQuery({.filter = fromjson("{b: {$nin: [1, 2]}}")}), {"b"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsTraversesDeeplyNestedLogicalOperators) {
    assertRelevantFields(
        *makeQuery({.filter = fromjson("{$and: [{$or: [{a: 1}, {'b.c': 2}]},"
                                       "        {d: {$not: {$gt: 3}}},"
                                       "        {$nor: [{e: 4}, {$and: [{f: 5}, {g: 6}]}]}]}")}),
        {"a", "b.c", "d", "e", "f", "g"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsCollectsExprComparisonAgainstAConstant) {
    assertRelevantFields(*makeQuery({.filter = fromjson("{$expr: {$eq: ['$a', 1]}}")}), {"a"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsCollectsInternalExprEqField) {
    assertRelevantFields(*makeQuery({.filter = fromjson("{a: {$_internalExprEq: 1}}")}), {"a"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsCollectsExprComparisonBetweenTwoFields) {
    assertRelevantFields(*makeQuery({.filter = fromjson("{$expr: {$eq: ['$a', '$b']}}")}),
                         {"a", "b"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsCollectsNestedExprFields) {
    assertRelevantFields(
        *makeQuery({.filter = fromjson("{$expr: {$gt: [{$add: ['$a', '$b.c']}, '$d']}}")}),
        {"a", "b.c", "d"});
}

//
// Tests for relevantFieldsForQuery: the projection and the sort.
//
// Neither of these appears in any predicate, but we may still build index bounds over them, e.g.
// covered projection or index provided sort.
//

TEST_F(IndexFingerprintTest, RelevantFieldsCollectsInclusionProjectionFields) {
    // '_id' is implicitly included, and an index over it is just as capable of covering the
    // projection as one over 'a' or 'b'.
    assertRelevantFields(*makeQuery({.projection = fromjson("{a: 1, 'b.c': 1}")}),
                         {"_id", "a", "b.c"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsCollectsExclusionProjectionFields) {
    // For an exclusion projection these are the excluded paths rather than the required ones. We
    // collect them anyway rather than reason about whether a covered plan is possible: the field
    // set only has to be a superset.
    assertRelevantFields(*makeQuery({.projection = fromjson("{a: 0}")}), {"a"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsCollectsSortFields) {
    assertRelevantFields(*makeQuery({.sort = fromjson("{a: 1, 'b.c': -1}")}), {"a", "b.c"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsIgnoresMetaSort) {
    // A '$meta' sort part carries no field path and no index can provide it.
    assertRelevantFields(*makeQuery({.sort = fromjson("{a: {$meta: 'randVal'}}")}), {});
}

TEST_F(IndexFingerprintTest, RelevantFieldsUnionsFilterProjectionAndSort) {
    assertRelevantFields(*makeQuery({.filter = fromjson("{a: 1}"),
                                     .projection = fromjson("{b: 1}"),
                                     .sort = fromjson("{c: 1}")}),
                         {"_id", "a", "b", "c"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsOfEmptyQueryIsEmpty) {
    assertRelevantFields(*makeQuery({}), {});
}

//
// Tests for relevantFieldsPerNode: each node's own query unioned with the join predicates it owns.
//
// The join term is what these cover: which edges contribute, and which side of each predicate the
// node owns.
//

TEST_F(IndexFingerprintTest, NodeWithNoLocalPredicatesStillGetsItsJoinFields) {
    // The join field is where an INLJ probe index would be built, so it is relevant even though
    // nothing in the node's own query mentions it. Each node gets only its own side of the join.
    auto left = addGraphNode();
    auto right = addGraphNode();
    ASSERT_TRUE(
        graph.addSimpleEqualityEdge(left, right, addJoinPath(left, "x"), addJoinPath(right, "y"))
            .has_value());
    auto joinGraph = freeze();

    assertNodeFields(joinGraph, left, {"x"});
    assertNodeFields(joinGraph, right, {"y"});
}

TEST_F(IndexFingerprintTest, NodeFieldsAreUnaffectedByEdgeSideNormalization) {
    // 'MutableJoinGraph::makeEdge' normalizes every edge to 'left < right' and swaps the predicate
    // sides with it, so this edge is stored with its arguments reversed. Attribution comes from
    // ResolvedPath::nodeId rather than from the edge's endpoints, so that must not matter.
    auto first = addGraphNode();
    auto second = addGraphNode();
    ASSERT_TRUE(
        graph
            .addSimpleEqualityEdge(second, first, addJoinPath(second, "y"), addJoinPath(first, "x"))
            .has_value());
    auto joinGraph = freeze();

    assertNodeFields(joinGraph, first, {"x"});
    assertNodeFields(joinGraph, second, {"y"});
}

TEST_F(IndexFingerprintTest, JoinFieldsUseTheUnderlyingNameNotTheRenamedOne) {
    // 'ResolvedPath::fieldPathAfterRenames' is what a field is called once the node's own query has
    // been applied. Index key patterns are declared against the base collection, so the fingerprint
    // has to key off 'underlyingFieldPath' -- matching on the renamed name would find no index.
    auto left = addGraphNode();
    auto right = addGraphNode();
    const auto renamedPath = static_cast<PathId>(resolvedPaths.size());
    resolvedPaths.emplace_back(ResolvedPath{left, FieldPath("underlying"), FieldPath("renamed")});
    ASSERT_TRUE(
        graph.addSimpleEqualityEdge(left, right, renamedPath, addJoinPath(right, "y")).has_value());
    auto joinGraph = freeze();

    assertNodeFields(joinGraph, left, {"underlying"});
}

TEST_F(IndexFingerprintTest, SelfJoinKeepsTheTwoNodesFieldsApart) {
    // A collection joined to itself is two nodes over one namespace -- self edges are rejected by
    // 'MutableJoinGraph::addEdge'. The fields must still be attributed per node, since the two
    // instances can be probed by different indexes.
    auto left = addGraphNode({.filter = fromjson("{a: 1}")});
    // 'addGraphNode' puts every node on 'kNss', so these two are already the same collection.
    auto right = addGraphNode({.filter = fromjson("{b: 1}")});
    ASSERT_TRUE(
        graph.addSimpleEqualityEdge(left, right, addJoinPath(left, "x"), addJoinPath(right, "y"))
            .has_value());
    auto joinGraph = freeze();

    assertNodeFields(joinGraph, left, {"a", "x"});
    assertNodeFields(joinGraph, right, {"b", "y"});
}

TEST_F(IndexFingerprintTest, NodeNotParticipatingInAJoinGetsNoJoinFields) {
    auto isolated = addGraphNode();
    auto left = addGraphNode();
    auto right = addGraphNode();
    ASSERT_TRUE(
        graph.addSimpleEqualityEdge(left, right, addJoinPath(left, "x"), addJoinPath(right, "y"))
            .has_value());
    auto joinGraph = freeze();

    // An index on 'x' or 'y' cannot change the plan chosen for a node that does not participate in
    // that join. Neither path is owned by 'isolated', so neither reaches its field set.
    assertNodeFields(joinGraph, isolated, {});
}

TEST_F(IndexFingerprintTest, FieldsFromAllOfANodesEdgesAreUnioned) {
    auto center = addGraphNode();
    auto first = addGraphNode();
    auto second = addGraphNode();
    ASSERT_TRUE(graph
                    .addSimpleEqualityEdge(
                        center, first, addJoinPath(center, "x"), addJoinPath(first, "otherSide1"))
                    .has_value());
    ASSERT_TRUE(graph
                    .addSimpleEqualityEdge(
                        center, second, addJoinPath(center, "z"), addJoinPath(second, "otherSide2"))
                    .has_value());
    auto joinGraph = freeze();

    assertNodeFields(joinGraph, center, {"x", "z"});
}

TEST_F(IndexFingerprintTest, MultiplePredicatesOnOneEdgeAllContribute) {
    auto left = addGraphNode();
    auto right = addGraphNode();
    auto edgeId =
        graph.addSimpleEqualityEdge(left, right, addJoinPath(left, "x"), addJoinPath(right, "y"));
    ASSERT_TRUE(edgeId.has_value());
    // A compound join condition adds its predicates to the existing edge rather than a new one, so
    // this also covers the inner loop over 'JoinEdge::predicates'.
    auto sameEdgeId =
        graph.addSimpleEqualityEdge(left, right, addJoinPath(left, "x2"), addJoinPath(right, "y2"));
    ASSERT_TRUE(sameEdgeId.has_value());
    ASSERT_EQ(*edgeId, *sameEdgeId);
    auto joinGraph = freeze();

    assertNodeFields(joinGraph, left, {"x", "x2"});
    assertNodeFields(joinGraph, right, {"y", "y2"});
}

TEST_F(IndexFingerprintTest, NodeFieldsUnionTheLocalQueryAndTheJoinFields) {
    // The node's filter, projection and sort all reach the field set through
    // relevantFieldsForQuery, and the join field is added on top.
    auto left = addGraphNode({.filter = fromjson("{a: 1}"),
                              .projection = fromjson("{b: 1}"),
                              .sort = fromjson("{c: 1}")});
    auto right = addGraphNode();
    ASSERT_TRUE(
        graph.addSimpleEqualityEdge(left, right, addJoinPath(left, "x"), addJoinPath(right, "y"))
            .has_value());
    auto joinGraph = freeze();

    assertNodeFields(joinGraph, left, {"_id", "a", "b", "c", "x"});
}

TEST_F(IndexFingerprintTest, RelevantFieldsIncludeNodePredicateAndJoinFields) {
    // The same two terms, but reached through a graph built by initGraph() and a real
    // JoinReorderingContext rather than the hand-assembled graphs above.
    initGraph(2);
    const auto leftJoinPath = static_cast<PathId>(resolvedPaths.size());
    resolvedPaths.emplace_back(ResolvedPath{NodeId{0}, FieldPath("joinLeft")});
    const auto rightJoinPath = static_cast<PathId>(resolvedPaths.size());
    resolvedPaths.emplace_back(ResolvedPath{NodeId{1}, FieldPath("joinRight")});
    ASSERT_TRUE(
        graph.addSimpleEqualityEdge(NodeId{0}, NodeId{1}, leftJoinPath, rightJoinPath).has_value());

    auto ctx = makeContext();

    // initGraph gives each node a predicate on its own 'a<i>' field, so the join fields added above
    // are distinguishable from the predicate fields.
    const auto perNodeFields = relevantFieldsPerNode(ctx.joinGraph, ctx.resolvedPaths);

    const auto& leftFields = perNodeFields[NodeId{0}];
    ASSERT_TRUE(leftFields.contains("a0"));        // local predicate field
    ASSERT_TRUE(leftFields.contains("joinLeft"));  // join predicate field, an INLJ probe candidate
    ASSERT_FALSE(leftFields.contains("joinRight"));

    const auto& rightFields = perNodeFields[NodeId{1}];
    ASSERT_TRUE(rightFields.contains("a1"));
    ASSERT_TRUE(rightFields.contains("joinRight"));
    ASSERT_FALSE(rightFields.contains("joinLeft"));
}

//
// computeRelevantIndexHashes: which indexes are relevant.
//
// Only indexes reachable from the node's relevant field set may affect its fingerprint.
//

TEST_F(IndexFingerprintTest, IrrelevantIndexCreationDoesNotChangeFingerprint) {
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto before = relevantHashes({"a"});

    // 'b' is not a relevant field, so no plan over 'a' can be affected by this index.
    addIndex(fromjson("{b: 1}"), "b_1");
    ASSERT_EQ(before, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, IrrelevantIndexDropDoesNotChangeFingerprint) {
    addIndex(fromjson("{a: 1}"), "a_1");
    addIndex(fromjson("{b: 1}"), "b_1");
    const auto before = relevantHashes({"a"});

    dropIndex("b_1");
    ASSERT_EQ(before, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, RelevantIndexCreationChangesFingerprint) {
    const auto before = relevantHashes({"a"});
    addIndex(fromjson("{a: 1}"), "a_1");
    // A newly created relevant index could yield a better plan, so the entry must be replanned.
    ASSERT_NE(before, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, RelevantIndexDropChangesFingerprint) {
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto before = relevantHashes({"a"});

    dropIndex("a_1");
    ASSERT_NE(before, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, CompoundIndexIsRelevantViaItsLeadingField) {
    const auto before = relevantHashes({"a"});
    addIndex(fromjson("{a: 1, b: 1}"), "a_1_b_1");
    ASSERT_NE(before, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, CompoundIndexIsRelevantViaANonLeadingField) {
    // Tests that a predicated field that is not the leading field in an index is still considered
    // relevant.
    const auto before = relevantHashes({"b"});
    addIndex(fromjson("{a: 1, b: 1}"), "a_1_b_1");
    ASSERT_NE(before, relevantHashes({"b"}));
}

TEST_F(IndexFingerprintTest, IndexSharingNoFieldWithTheQueryIsIrrelevant) {
    // The overlap still has to be real: this is what keeps the fingerprint finer-grained than the
    // collection version check it backs up.
    const auto before = relevantHashes({"z"});
    addIndex(fromjson("{a: 1, b: 1}"), "a_1_b_1");
    ASSERT_EQ(before, relevantHashes({"z"}));
}

TEST_F(IndexFingerprintTest, IndexOnAPrefixOfARelevantFieldIsRelevant) {
    // An index on 'a' can generate bounds for a predicate on 'a.b', since satisfying that predicate
    // requires 'a' to be an object.
    const auto before = relevantHashes({"a.b"});
    addIndex(fromjson("{a: 1}"), "a_1");
    ASSERT_NE(before, relevantHashes({"a.b"}));
}

TEST_F(IndexFingerprintTest, IndexOnAnExtensionOfARelevantFieldIsRelevant) {
    // The other direction. We do not try to predict which prefix relations the optimizer can
    // exploit, so both count.
    const auto before = relevantHashes({"a"});
    addIndex(fromjson("{'a.b': 1}"), "a.b_1");
    ASSERT_NE(before, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, PrefixMatchingIsPerPathComponent) {
    // 'a' is a prefix of 'a.b' but not of 'ab' -- overlap is compared component-wise, so a merely
    // textual prefix does not make an unrelated index relevant.
    const auto before = relevantHashes({"a"});
    addIndex(fromjson("{ab: 1}"), "ab_1");
    ASSERT_EQ(before, relevantHashes({"a"}));
}

//
// computeRelevantIndexHashes: what makes two relevant index sets equivalent.
//
// The fingerprint must be a function of the semantics of the index set and nothing else: sensitive
// to every part of a definition the optimizer can see, and insensitive to catalog churn and
// enumeration order.
//

TEST_F(IndexFingerprintTest, FingerprintIsStableAcrossRepeatedComputation) {
    addIndex(fromjson("{a: 1}"), "a_1");
    ASSERT_EQ(relevantHashes({"a"}), relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, RelevantIndexHashesHoldsOneHashPerRelevantIndex) {
    // The hashes are kept per index rather than folded together so that a later caller can tell an
    // index that appeared from one that merely went away, which means the count has to track the
    // relevant index set exactly.
    addIndex(fromjson("{a: 1}"), "a_1");
    addIndex(fromjson("{a: 1, b: 1}"), "a_1_b_1");
    addIndex(fromjson("{z: 1}"), "z_1");

    auto hashes = relevantHashes({"a"});
    ASSERT_EQ(2, hashes.size());
    // Two different indexes, so two different hashes, in ascending order.
    ASSERT_LT(hashes[0], hashes[1]);

    // And the irrelevant one is counted for the field it does cover.
    ASSERT_EQ(1, relevantHashes({"z"}).size());
}

TEST_F(IndexFingerprintTest, RecreatingIdenticalRelevantIndexDoesNotChangeFingerprint) {
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto before = relevantHashes({"a"});

    // A drop and identical recreate churns the catalog and gets a fresh durable ident, but is
    // semantically a no-op: the cached plan's index tags and INLJ index name still resolve to an
    // equivalent index, so the entry must survive rather than be replanned for nothing. This also
    // pins the fingerprint as semantic rather than identity-based, so folding in something like the
    // ident or a creation timestamp would fail here.
    dropIndex("a_1");
    addIndex(fromjson("{a: 1}"), "a_1");
    ASSERT_EQ(before, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, RecreatingRelevantIndexWithDifferentKeyPatternChangesFingerprint) {
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto before = relevantHashes({"a"});

    // Same index name, different definition. IndexEntry::operator== compares names only, so this
    // is precisely the case the fingerprint must not miss.
    dropIndex("a_1");
    addIndex(fromjson("{a: 1, b: 1}"), "a_1");
    ASSERT_NE(before, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, RedefiningRelevantIndexWithDifferentAttributesChangesFingerprint) {
    const std::vector<BSONObj> redefinitions{fromjson("{unique: true}"),
                                             fromjson("{sparse: true}"),
                                             fromjson("{hidden: true}"),
                                             fromjson("{partialFilterExpression: {a: {$gt: 0}}}")};

    addIndex(fromjson("{a: 1}"), "a_1");
    const auto plain = relevantHashes({"a"});

    for (const auto& redefinition : redefinitions) {
        dropIndex("a_1");
        // Apply new property when creating index with same name and key pattern.
        addIndex(fromjson("{a: 1}"), "a_1", redefinition);
        ASSERT_NE(plain, relevantHashes({"a"})) << " for redefinition " << redefinition.toString();

        // And back, so each case is measured against the same baseline, not the previous one.
        dropIndex("a_1");
        addIndex(fromjson("{a: 1}"), "a_1");
        ASSERT_EQ(plain, relevantHashes({"a"})) << " after reverting " << redefinition.toString();
    }
}

TEST_F(IndexFingerprintTest, RedefiningRelevantIndexWithADifferentPartialFilterChangesFingerprint) {
    // Both definitions are partial, so 'isPartial()' is unchanged and only the partial filter
    // itself differs.
    addIndex(fromjson("{a: 1}"), "a_1", fromjson("{partialFilterExpression: {a: {$gt: 0}}}"));
    const auto before = relevantHashes({"a"});

    dropIndex("a_1");
    addIndex(fromjson("{a: 1}"), "a_1", fromjson("{partialFilterExpression: {a: {$gt: 5}}}"));
    ASSERT_NE(before, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, RedefiningRelevantIndexWithADifferentCollationChangesFingerprint) {
    // Both definitions are collated, so only the collation differs.
    addIndex(fromjson("{a: 1}"), "a_1", fromjson("{collation: {locale: 'en_US'}}"));
    const auto before = relevantHashes({"a"});

    dropIndex("a_1");
    addIndex(fromjson("{a: 1}"), "a_1", fromjson("{collation: {locale: 'fr'}}"));
    ASSERT_NE(before, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, HidingARelevantIndexChangesFingerprint) {
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto visible = relevantHashes({"a"});

    setIndexHiddenOn(kNss, "a_1", true);
    ASSERT_NE(visible, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, UnhidingARelevantIndexRestoresTheFingerprint) {
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto visible = relevantHashes({"a"});

    setIndexHiddenOn(kNss, "a_1", true);
    setIndexHiddenOn(kNss, "a_1", false);
    // Hidden state is the only thing that changed, so the fingerprint must land back where it was
    // rather than merely differing from the hidden one.
    ASSERT_EQ(visible, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, HidingAnIrrelevantIndexDoesNotChangeFingerprint) {
    addIndex(fromjson("{a: 1}"), "a_1");
    addIndex(fromjson("{b: 1}"), "b_1");
    const auto before = relevantHashes({"a"});

    // A collMod that hides an index no node references must not invalidate the entry, just as
    // creating or dropping one does not.
    setIndexHiddenOn(kNss, "b_1", true);
    ASSERT_EQ(before, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, ReversingRelevantIndexDirectionChangesFingerprint) {
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto before = relevantHashes({"a"});

    dropIndex("a_1");
    addIndex(fromjson("{a: -1}"), "a_1");
    ASSERT_NE(before, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, RenamingRelevantIndexChangesFingerprint) {
    // TODO (SERVER-132446): Revisit when using index ident instead of name.
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto before = relevantHashes({"a"});

    // Cached INLJ nodes resolve their probe index by name, so the name is part of the fingerprint.
    dropIndex("a_1");
    addIndex(fromjson("{a: 1}"), "a_renamed");
    ASSERT_NE(before, relevantHashes({"a"}));
}

TEST_F(IndexFingerprintTest, FingerprintIsIndependentOfIndexCreationOrder) {
    addIndex(fromjson("{a: 1}"), "a_1");
    addIndex(fromjson("{a: 1, b: 1}"), "a_1_b_1");
    const auto forwardOrder = relevantHashes({"a"});

    dropIndex("a_1");
    dropIndex("a_1_b_1");
    addIndex(fromjson("{a: 1, b: 1}"), "a_1_b_1");
    addIndex(fromjson("{a: 1}"), "a_1");

    ASSERT_EQ(forwardOrder, relevantHashes({"a"}));
}

//
// usedIndexNamesPerNode: which indexes a cached plan reads from.
//

TEST_F(IndexFingerprintTest, UsedIndexNamesFromAnAccessPath) {
    auto plan = accessPathPlanNode(NodeId{0}, {{"a_1", BSON("a" << 1)}, {"b_1", BSON("b" << 1)}});
    auto names = usedIndexNamesPerNode(*plan, 1);
    ASSERT_EQ(StringSet({"a_1", "b_1"}), names[0]);
}

TEST_F(IndexFingerprintTest, UsedIndexNamesFromAnUntaggedAccessPathAreEmpty) {
    // A collection scan tags no index, so the node depends on no index at all.
    auto plan = accessPathPlanNode(NodeId{0}, {});
    ASSERT_TRUE(usedIndexNamesPerNode(*plan, 1)[0].empty());
}

TEST_F(IndexFingerprintTest, UsedIndexNamesFromAnInljProbeIndex) {
    auto plan = inljPlanNode(NodeId{0}, "a_1");
    ASSERT_EQ(StringSet({"a_1"}), usedIndexNamesPerNode(*plan, 1)[0]);
}

TEST_F(IndexFingerprintTest, UsedIndexNamesAreAttributedPerNodeAcrossNestedJoins) {
    auto plan = joinPlanNode(
        accessPathPlanNode(NodeId{0}, {{"a_1", BSON("a" << 1)}}),
        joinPlanNode(
            inljPlanNode(NodeId{1}, "b_1"),
            accessPathPlanNode(NodeId{2}, {{"c_1", BSON("c" << 1)}, {"d_1", BSON("d" << 1)}})));

    auto names = usedIndexNamesPerNode(*plan, 3);
    ASSERT_EQ(StringSet({"a_1"}), names[0]);
    ASSERT_EQ(StringSet({"b_1"}), names[1]);
    ASSERT_EQ(StringSet({"c_1", "d_1"}), names[2]);
}

TEST_F(IndexFingerprintTest, UsedIndexNamesCoverEveryNodeEvenWhenThePlanMentionsNone) {
    auto plan = accessPathPlanNode(NodeId{0}, {{"a_1", BSON("a" << 1)}});
    auto names = usedIndexNamesPerNode(*plan, 2);
    ASSERT_EQ(2, names.size());
    ASSERT_TRUE(names[1].empty());
}

//
// computeUsedIndexFingerprint: detecting a change to an index the plan reads from.
//

TEST_F(IndexFingerprintTest, UsedIndexFingerprintIsStableAcrossRepeatedComputation) {
    addIndex(fromjson("{a: 1}"), "a_1");
    ASSERT_EQ(usedFingerprint({"a_1"}), usedFingerprint({"a_1"}));
}

TEST_F(IndexFingerprintTest, UsedIndexFingerprintOfANodeReadingNoIndexIsNotZero) {
    addIndex(fromjson("{a: 1}"), "a_1");
    ASSERT_NE(IndexFingerprint{}, usedFingerprint({}));
}

TEST_F(IndexFingerprintTest, UsedIndexFingerprintIgnoresIndexesThePlanDoesNotRead) {
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto before = usedFingerprint({"a_1"});

    addIndex(fromjson("{a: 1, b: 1}"), "a_1_b_1");
    dropIndex("a_1_b_1");
    ASSERT_EQ(before, usedFingerprint({"a_1"}));
}

TEST_F(IndexFingerprintTest, DroppingAUsedIndexChangesTheUsedIndexFingerprint) {
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto before = usedFingerprint({"a_1"});

    dropIndex("a_1");
    ASSERT_NE(before, usedFingerprint({"a_1"}));
}

TEST_F(IndexFingerprintTest, RedefiningAUsedIndexChangesTheUsedIndexFingerprint) {
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto before = usedFingerprint({"a_1"});

    dropIndex("a_1");
    addIndex(fromjson("{a: 1, b: 1}"), "a_1");
    ASSERT_NE(before, usedFingerprint({"a_1"}));
}

TEST_F(IndexFingerprintTest, UsedIndexFingerprintIsIndependentOfIndexCreationOrder) {
    addIndex(fromjson("{a: 1}"), "a_1");
    addIndex(fromjson("{b: 1}"), "b_1");
    const auto forwardOrder = usedFingerprint({"a_1", "b_1"});

    dropIndex("a_1");
    dropIndex("b_1");
    addIndex(fromjson("{b: 1}"), "b_1");
    addIndex(fromjson("{a: 1}"), "a_1");

    ASSERT_EQ(forwardOrder, usedFingerprint({"a_1", "b_1"}));
}

//
// canReuseNodeFingerprint: the invalidation decision itself.
//
// 'a' is the relevant field throughout, 'a_1' the index the plan reads from and 'a_1_b_1' a second
// relevant index it merely considered.
//

TEST_F(IndexFingerprintTest, ReusesWhenNothingChanged) {
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto cached = currentFingerprint({"a_1"});
    ASSERT_TRUE(canReuseNodeFingerprint(cached, currentFingerprint({"a_1"})));
}

TEST_F(IndexFingerprintTest, ReusesWhenARelevantButUnusedIndexIsDropped) {
    addIndex(fromjson("{a: 1}"), "a_1");
    addIndex(fromjson("{a: 1, b: 1}"), "a_1_b_1");
    const auto cached = currentFingerprint({"a_1"});

    dropIndex("a_1_b_1");
    ASSERT_TRUE(canReuseNodeFingerprint(cached, currentFingerprint({"a_1"})));
}

TEST_F(IndexFingerprintTest, ReusesWhenEveryRelevantIndexButTheUsedOneIsDropped) {
    addIndex(fromjson("{a: 1}"), "a_1");
    addIndex(fromjson("{a: 1, b: 1}"), "a_1_b_1");
    addIndex(fromjson("{a: -1}"), "a_-1");
    const auto cached = currentFingerprint({"a_1"});

    dropIndex("a_1_b_1");
    dropIndex("a_-1");
    ASSERT_TRUE(canReuseNodeFingerprint(cached, currentFingerprint({"a_1"})));
}

TEST_F(IndexFingerprintTest, ReusesWhenAnIrrelevantIndexIsDropped) {
    addIndex(fromjson("{a: 1}"), "a_1");
    addIndex(fromjson("{z: 1}"), "z_1");
    const auto cached = currentFingerprint({"a_1"});

    dropIndex("z_1");
    ASSERT_TRUE(canReuseNodeFingerprint(cached, currentFingerprint({"a_1"})));
}

TEST_F(IndexFingerprintTest, ReplansWhenTheUsedIndexIsDropped) {
    addIndex(fromjson("{a: 1}"), "a_1");
    addIndex(fromjson("{a: 1, b: 1}"), "a_1_b_1");
    const auto cached = currentFingerprint({"a_1"});

    dropIndex("a_1");
    ASSERT_FALSE(canReuseNodeFingerprint(cached, currentFingerprint({"a_1"})));
}

TEST_F(IndexFingerprintTest, ReplansWhenTheUsedIndexIsRedefinedUnderTheSameName) {
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto cached = currentFingerprint({"a_1"});

    dropIndex("a_1");
    addIndex(fromjson("{a: 1}"), "a_1", fromjson("{sparse: true}"));
    ASSERT_FALSE(canReuseNodeFingerprint(cached, currentFingerprint({"a_1"})));
}

TEST_F(IndexFingerprintTest, ReplansWhenARelevantIndexIsAdded) {
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto cached = currentFingerprint({"a_1"});

    addIndex(fromjson("{a: 1, b: 1}"), "a_1_b_1");
    ASSERT_FALSE(canReuseNodeFingerprint(cached, currentFingerprint({"a_1"})));
}

TEST_F(IndexFingerprintTest, ReplansWhenAnUnusedRelevantIndexIsDroppedAlongsideAnAddition) {
    addIndex(fromjson("{a: 1}"), "a_1");
    addIndex(fromjson("{a: 1, b: 1}"), "a_1_b_1");
    addIndex(fromjson("{a: 1, c: 1}"), "a_1_c_1");
    const auto cached = currentFingerprint({"a_1"});

    dropIndex("a_1_b_1");
    dropIndex("a_1_c_1");
    addIndex(fromjson("{a: 1, d: 1}"), "a_1_d_1");
    ASSERT_FALSE(canReuseNodeFingerprint(cached, currentFingerprint({"a_1"})));
}

TEST_F(IndexFingerprintTest, ReplansWhenAnUnusedRelevantIndexIsRecreatedWithADifferentKeyPattern) {
    addIndex(fromjson("{a: 1}"), "a_1");
    addIndex(fromjson("{a: 1, b: 1}"), "a_1_b_1");
    const auto cached = currentFingerprint({"a_1"});

    dropIndex("a_1_b_1");
    // Create index with same name but different key pattern. The optimizer sees this as a new
    // relevant index it may use and thus invalidates the cache entry.
    addIndex(fromjson("{a: 1, b: -1}"), "a_1_b_1");
    ASSERT_FALSE(canReuseNodeFingerprint(cached, currentFingerprint({"a_1"})));
}

TEST_F(IndexFingerprintTest, ReusesWhenThePlanReadsFromNoIndexAndOneIsDropped) {
    addIndex(fromjson("{a: 1}"), "a_1");
    const auto cached = currentFingerprint({});

    dropIndex("a_1");
    ASSERT_TRUE(canReuseNodeFingerprint(cached, currentFingerprint({})));
}

//
// makeNodeFingerprints: driving the above over a whole graph.
//
// End to end for the library: a real join graph plus real index catalogs in, one fingerprint per
// node out. These are the properties the plan cache actually depends on -- that an index DDL
// changes the fingerprint of exactly those nodes it could affect, and no others.
//

TEST_F(IndexFingerprintTest, MakeNodeFingerprintsIsStableWhenTheCatalogDoesNotChange) {
    auto left = addGraphNode({.filter = fromjson("{a: 1}"), .nss = kNss});
    auto right = addGraphNode({.filter = fromjson("{b: 1}"), .nss = kOtherNss});
    ASSERT_TRUE(
        graph.addSimpleEqualityEdge(left, right, addJoinPath(left, "x"), addJoinPath(right, "y"))
            .has_value());
    addIndexOn(kNss, fromjson("{a: 1}"), "a_1");
    addIndexOn(kOtherNss, fromjson("{b: 1}"), "b_1");
    auto joinGraph = freeze();

    ASSERT_EQ(fingerprintGraph(joinGraph, {kNss, kOtherNss}),
              fingerprintGraph(joinGraph, {kNss, kOtherNss}));
}

TEST_F(IndexFingerprintTest, MakeNodeFingerprintsIsPerNodeNotPerCollection) {
    // Two nodes over the same collection with disjoint relevant fields. Only 'left' cares about
    // 'a', so only its fingerprint may move when an index on 'a' appears.
    auto left = addGraphNode({.filter = fromjson("{a: 1}"), .nss = kNss});
    auto right = addGraphNode({.filter = fromjson("{b: 1}"), .nss = kNss});
    ASSERT_TRUE(
        graph.addSimpleEqualityEdge(left, right, addJoinPath(left, "x"), addJoinPath(right, "y"))
            .has_value());
    auto joinGraph = freeze();

    const auto before = fingerprintGraph(joinGraph, {kNss});

    addIndexOn(kNss, fromjson("{a: 1}"), "a_1");
    const auto after = fingerprintGraph(joinGraph, {kNss});

    ASSERT_NE(before[left], after[left]);
    ASSERT_EQ(before[right], after[right]);
}

TEST_F(IndexFingerprintTest, MakeNodeFingerprintsMovesBothNodesForAnIndexRelevantToBoth) {
    // The complement of MakeNodeFingerprintsIsPerNodeNotPerCollection: two nodes over one
    // collection that both reference 'a'. One index is relevant to both, so dropping it must move
    // fingerprints -- a shared index must not be attributed to just the first node that claims it.
    auto first = addGraphNode({.filter = fromjson("{a: 1}"), .nss = kNss});
    auto second = addGraphNode({.filter = fromjson("{a: {$gt: 5}}"), .nss = kNss});
    ASSERT_TRUE(
        graph
            .addSimpleEqualityEdge(first, second, addJoinPath(first, "x"), addJoinPath(second, "y"))
            .has_value());
    addIndexOn(kNss, fromjson("{a: 1}"), "a_1");
    auto joinGraph = freeze();

    const auto before = fingerprintGraph(joinGraph, {kNss});
    // Their relevant field sets differ ('a,x' vs 'a,y') but select the same index, and the
    // fingerprint is a function of the selected index set rather than of the fields.
    ASSERT_EQ(before[first], before[second]);

    dropIndexOn(kNss, "a_1");
    const auto after = fingerprintGraph(joinGraph, {kNss});

    ASSERT_NE(before[first], after[first]);
    ASSERT_NE(before[second], after[second]);
}

TEST_F(IndexFingerprintTest, MakeNodeFingerprintsIsolatesDdlToTheAffectedCollection) {
    auto left = addGraphNode({.filter = fromjson("{a: 1}"), .nss = kNss});
    auto right = addGraphNode({.filter = fromjson("{a: 1}"), .nss = kOtherNss});
    ASSERT_TRUE(
        graph.addSimpleEqualityEdge(left, right, addJoinPath(left, "x"), addJoinPath(right, "y"))
            .has_value());
    addIndexOn(kNss, fromjson("{a: 1}"), "a_1");
    addIndexOn(kOtherNss, fromjson("{a: 1}"), "a_1");
    auto joinGraph = freeze();

    const auto before = fingerprintGraph(joinGraph, {kNss, kOtherNss});

    // Both nodes have the same relevant fields and identically defined indexes, so only the node
    // whose own collection lost the index may change.
    dropIndexOn(kOtherNss, "a_1");
    const auto after = fingerprintGraph(joinGraph, {kNss, kOtherNss});

    ASSERT_EQ(before[left], after[left]);
    ASSERT_NE(before[right], after[right]);
}

TEST_F(IndexFingerprintTest, MakeNodeFingerprintsIgnoresDdlOnIrrelevantFields) {
    // The whole point of the fingerprint: this DDL bumps the collection version and would
    // invalidate the cache entry outright, but no node's plan can depend on it.
    auto left = addGraphNode({.filter = fromjson("{a: 1}"), .nss = kNss});
    auto right = addGraphNode({.filter = fromjson("{b: 1}"), .nss = kOtherNss});
    ASSERT_TRUE(
        graph.addSimpleEqualityEdge(left, right, addJoinPath(left, "x"), addJoinPath(right, "y"))
            .has_value());
    auto joinGraph = freeze();

    const auto before = fingerprintGraph(joinGraph, {kNss, kOtherNss});

    addIndexOn(kNss, fromjson("{unrelated: 1}"), "unrelated_1");
    addIndexOn(kOtherNss, fromjson("{alsoUnrelated: 1}"), "alsoUnrelated_1");

    ASSERT_EQ(before, fingerprintGraph(joinGraph, {kNss, kOtherNss}));
}

TEST_F(IndexFingerprintTest, MakeNodeFingerprintsReactsToAnIndexOnAJoinFieldOnly) {
    // 'x' appears nowhere in the node's own query -- it is reachable only through the join edge, so
    // this covers the join term of relevantFieldsPerNode all the way through to the fingerprint.
    auto left = addGraphNode({.nss = kNss});
    auto right = addGraphNode({.nss = kOtherNss});
    ASSERT_TRUE(
        graph.addSimpleEqualityEdge(left, right, addJoinPath(left, "x"), addJoinPath(right, "y"))
            .has_value());
    auto joinGraph = freeze();

    const auto before = fingerprintGraph(joinGraph, {kNss, kOtherNss});

    addIndexOn(kNss, fromjson("{x: 1}"), "x_1");
    const auto after = fingerprintGraph(joinGraph, {kNss, kOtherNss});

    ASSERT_NE(before[left], after[left]);
    ASSERT_EQ(before[right], after[right]);
}

TEST_F(IndexFingerprintTest, MakeNodeFingerprintsReactsToAnIndexOnASortFieldOnly) {
    // Likewise for a field that only the sort mentions. Under a leading-predicate-field rule this
    // index would be invisible, yet QueryPlanner::extendCandidatePlans can pick it to provide the
    // sort, and a cached plan doing so must not survive its removal.
    auto left = addGraphNode({.sort = fromjson("{sortField: 1}")});
    auto right = addGraphNode({.nss = kOtherNss});
    ASSERT_TRUE(
        graph.addSimpleEqualityEdge(left, right, addJoinPath(left, "x"), addJoinPath(right, "y"))
            .has_value());
    addIndexOn(kNss, fromjson("{sortField: 1}"), "sortField_1");
    auto joinGraph = freeze();

    const auto before = fingerprintGraph(joinGraph, {kNss, kOtherNss});

    dropIndexOn(kNss, "sortField_1");
    const auto after = fingerprintGraph(joinGraph, {kNss, kOtherNss});

    ASSERT_NE(before[left], after[left]);
    ASSERT_EQ(before[right], after[right]);
}

TEST_F(IndexFingerprintTest, MakeNodeFingerprintsFailsWhenACollectionHasNoIndexList) {
    // 'perCollIdxs' must cover every namespace in the graph. Silently fingerprinting such a node as
    // having no relevant indexes would make every later index DDL on it look like a no-op.
    auto left = addGraphNode({.nss = kNss});
    auto right = addGraphNode({.nss = kOtherNss});
    ASSERT_TRUE(
        graph.addSimpleEqualityEdge(left, right, addJoinPath(left, "x"), addJoinPath(right, "y"))
            .has_value());
    auto joinGraph = freeze();

    ASSERT_THROWS_WITH_CHECK(
        fingerprintGraph(joinGraph, {kNss}), DBException, [](const DBException& ex) {
            EXPECT_EQ(ex.code(), 13259302);
            // A tassert latches a tripwire that aborts the whole suite at shutdown. Deliberately
            // tripping one in a test means accounting for it here.
            assertionCount.tripwire.subtractAndFetch(1);
        });
}

TEST_F(IndexFingerprintTest, MakeNodeFingerprintsProducesOneFingerprintPerNode) {
    initGraph(2);
    auto ctx = makeContext();

    // initGraph only populates the graph; the namespaces it references must actually exist for
    // the fingerprint to read their index catalogs.
    std::vector<NamespaceString> nssList{ctx.joinGraph.getNode(NodeId{0}).collectionName,
                                         ctx.joinGraph.getNode(NodeId{1}).collectionName};
    for (const auto& nss : nssList) {
        createCollection(nss);
    }

    auto mca = multipleCollectionAccessor(operationContext(), nssList);
    AvailableIndexes perCollIdxs;
    for (const auto& nss : nssList) {
        perCollIdxs.emplace(nss, readyIndexes(mca, nss));
    }

    auto plan = planUsingNoIndexes();
    auto fingerprints = makeNodeFingerprints(ctx.joinGraph, ctx.resolvedPaths, perCollIdxs, *plan);
    ASSERT_EQ(2, fingerprints.size());
}

TEST_F(IndexFingerprintTest, MakeNodeFingerprintsKeepsAGraphReusableWhenAnUnusedIndexIsDropped) {
    // End to end over a graph: the left node's plan reads from 'a_1' and merely considered
    // 'a_1_b_1', while the right node's plan reads from nothing.
    auto left = addGraphNode({.filter = fromjson("{a: 1}"), .nss = kNss});
    auto right = addGraphNode({.filter = fromjson("{a: 1}"), .nss = kOtherNss});
    ASSERT_TRUE(
        graph.addSimpleEqualityEdge(left, right, addJoinPath(left, "x"), addJoinPath(right, "y"))
            .has_value());
    addIndexOn(kNss, fromjson("{a: 1}"), "a_1");
    addIndexOn(kNss, fromjson("{a: 1, b: 1}"), "a_1_b_1");
    auto joinGraph = freeze();

    auto plan = joinPlanNode(accessPathPlanNode(left, {{"a_1", BSON("a" << 1)}}),
                             accessPathPlanNode(right, {}));
    const auto before = fingerprintGraph(joinGraph, {kNss, kOtherNss}, plan.get());

    dropIndexOn(kNss, "a_1_b_1");
    const auto after = fingerprintGraph(joinGraph, {kNss, kOtherNss}, plan.get());

    ASSERT_NE(before, after);
    ASSERT_TRUE(canReuseNodeFingerprint(before[left], after[left]));
    ASSERT_TRUE(canReuseNodeFingerprint(before[right], after[right]));

    // Whereas dropping the index the left node reads from does force a replan.
    dropIndexOn(kNss, "a_1");
    const auto afterUsedDrop = fingerprintGraph(joinGraph, {kNss, kOtherNss}, plan.get());
    ASSERT_FALSE(canReuseNodeFingerprint(before[left], afterUsedDrop[left]));
    ASSERT_TRUE(canReuseNodeFingerprint(before[right], afterUsedDrop[right]));
}

}  // namespace
}  // namespace mongo::join_ordering
