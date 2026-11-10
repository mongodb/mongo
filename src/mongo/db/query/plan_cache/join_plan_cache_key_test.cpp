// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_cache/join_plan_cache_key.h"

#include "mongo/bson/json.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/index_builds_manager.h"
#include "mongo/db/query/compiler/optimizer/join/agg_join_model.h"
#include "mongo/db/query/compiler/optimizer/join/agg_join_model_fixture.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/query_knobs/query_knob_configuration_test_util.h"
#include "mongo/db/query/query_settings/query_knob_overrides.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using namespace join_ordering;

class JoinPlanCacheKeyTest : public JoinOrderingTestFixture {
public:
    void setUp() override {
        JoinOrderingTestFixture::setUp();
        createCollection(kNss0);
        createCollection(kNss1);
        createCollection(kNssC);
        createCollection(kNssOther);
    }

protected:
    struct GraphAndResolvedPaths {
        JoinGraph graph;
        std::vector<ResolvedPath> resolvedPaths;
    };

    GraphAndResolvedPaths makeTwoNodeGraph(NamespaceString nss0,
                                           BSONObj filter0,
                                           NamespaceString nss1,
                                           BSONObj filter1,
                                           std::string leftField,
                                           std::string rightField,
                                           JoinPredicate::Operator op = JoinPredicate::Eq) {
        std::vector<ResolvedPath> rps;
        MutableJoinGraph mg;
        auto n0 = *mg.addNode(nss0, makeCanonicalQuery(nss0, filter0), boost::none);
        auto n1 = *mg.addNode(nss1, makeCanonicalQuery(nss1, filter1), boost::none);

        PathId leftPathId = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n0, FieldPath(leftField)});
        PathId rightPathId = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n1, FieldPath(rightField)});

        mg.addEdge(n0, n1, {{op, leftPathId, rightPathId}});
        return GraphAndResolvedPaths{JoinGraph(std::move(mg)), std::move(rps)};
    }

    // Builds a two-node graph with two predicates on the single edge.
    GraphAndResolvedPaths makeTwoNodeGraphTwoPredicates(NamespaceString nss0,
                                                        NamespaceString nss1,
                                                        std::string left1,
                                                        std::string right1,
                                                        std::string left2,
                                                        std::string right2) {
        std::vector<ResolvedPath> rps;
        MutableJoinGraph mg;
        auto n0 = *mg.addNode(nss0, makeCanonicalQuery(nss0, BSONObj{}), boost::none);
        auto n1 = *mg.addNode(nss1, makeCanonicalQuery(nss1, BSONObj{}), boost::none);
        PathId lp1 = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n0, FieldPath(left1)});
        PathId rp1 = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n1, FieldPath(right1)});
        PathId lp2 = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n0, FieldPath(left2)});
        PathId rp2 = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n1, FieldPath(right2)});
        mg.addEdge(n0, n1, {{JoinPredicate::Eq, lp1, rp1}, {JoinPredicate::Eq, lp2, rp2}});
        return GraphAndResolvedPaths{JoinGraph(std::move(mg)), std::move(rps)};
    }

    // Builds a two-node graph identical to makeTwoNodeGraph's shape (empty filters, single Eq
    // predicate on "id") but with projections attached to node0 and/or node1. Used by the
    // projection-related tests below.
    GraphAndResolvedPaths makeTwoNodeGraphWithProjections(BSONObj proj0,
                                                          BSONObj proj1 = BSONObj{}) {
        std::vector<ResolvedPath> rps;
        MutableJoinGraph mg;
        auto n0 = *mg.addNode(kNss0, makeCanonicalQuery(kNss0, BSONObj{}, proj0), boost::none);
        auto n1 = *mg.addNode(kNss1, makeCanonicalQuery(kNss1, BSONObj{}, proj1), boost::none);
        PathId lp = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n0, FieldPath("id")});
        PathId rp = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n1, FieldPath("id")});
        mg.addEdge(n0, n1, {{JoinPredicate::Eq, lp, rp}});
        return GraphAndResolvedPaths{JoinGraph(std::move(mg)), std::move(rps)};
    }

    // Builds a MultipleCollectionAccessor for the namespaces present in 'g' and calls
    // makeJoinPlanCacheKey. All namespaces in 'g' must have been created in setUp().
    JoinPlanCacheKey makeKey(const JoinGraph& g,
                             const std::vector<ResolvedPath>& rps,
                             const query_settings::QuerySettings& qs = {}) {
        std::vector<NamespaceString> namespaces;
        for (size_t i = 0; i < g.numNodes(); ++i) {
            namespaces.push_back(g.getNode(static_cast<NodeId>(i)).collectionName);
        }
        return makeJoinPlanCacheKey(
            g, rps, multipleCollectionAccessor(operationContext(), namespaces), qs);
    }

    // Builds a JoinPlanCacheKey for the default two-node graph (empty filters, Eq predicate on
    // "id") under the given query settings.
    JoinPlanCacheKey makeKeyWithSettings(const query_settings::QuerySettings& qs = {}) {
        auto [graph, rps] = makeTwoNodeGraph(kNss0, BSONObj{}, kNss1, BSONObj{}, "id", "id");
        return makeKey(graph, rps, qs);
    }

    // Builds a QuerySettings holding only the given knob overrides.
    query_settings::QuerySettings settingsWithKnobs(const BSONObj& knobs) {
        auto overrides = query_settings::QuerySettingsKnobOverrides::fromBSON(knobs);
        // Guard against test setup problems (e.g. an unregistered knob) silently producing empty
        // overrides, which would make the "equal" tests pass and the "different" tests fail.
        ASSERT_FALSE(overrides.hasErrors()) << knobs;
        query_settings::QuerySettings qs;
        qs.setQueryKnobs(std::move(overrides));
        return qs;
    }

    NamespaceSpec makeNsSpec(const NamespaceString& nss) {
        NamespaceSpec ns;
        ns.setDb(DatabaseNameUtil::deserialize(
            boost::none, nss.dbName().toString_forTest(), SerializationContext::stateDefault()));
        ns.setColl(nss.coll());
        return ns;
    }

    const NamespaceString kNss0 = NamespaceString::createNamespaceString_forTest("test.customers");
    const NamespaceString kNss1 = NamespaceString::createNamespaceString_forTest("test.orders");
    // For the 3-node test.
    const NamespaceString kNssC = NamespaceString::createNamespaceString_forTest("test.items");
    // For namespace-differentiation tests.
    const NamespaceString kNssOther =
        NamespaceString::createNamespaceString_forTest("test.products");
};

TEST_F(JoinPlanCacheKeyTest, IdenticalGraphsProduceSameKey) {
    auto f1 = makeTwoNodeGraph(
        kNss0, fromjson("{age: 30}"), kNss1, fromjson("{status: 1}"), "customerId", "customerId");
    auto f2 = makeTwoNodeGraph(
        kNss0, fromjson("{age: 30}"), kNss1, fromjson("{status: 1}"), "customerId", "customerId");
    ASSERT_EQ(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, DifferentConstantsSameStructureProduceSameKey) {
    auto f1 = makeTwoNodeGraph(
        kNss0, fromjson("{age: 30}"), kNss1, fromjson("{status: 1}"), "customerId", "customerId");
    auto f2 = makeTwoNodeGraph(
        kNss0, fromjson("{age: 99}"), kNss1, fromjson("{status: 2}"), "customerId", "customerId");
    ASSERT_EQ(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, DifferentJoinFieldPathsProduceDifferentKeys) {
    auto f1 = makeTwoNodeGraph(
        kNss0, fromjson("{age: 30}"), kNss1, fromjson("{status: 1}"), "customerId", "customerId");
    auto f2 = makeTwoNodeGraph(
        kNss0, fromjson("{age: 30}"), kNss1, fromjson("{status: 1}"), "customerId", "orderId");
    ASSERT_NE(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, DifferentNamespacesProduceDifferentKeys) {
    auto f1 =
        makeTwoNodeGraph(kNss0, fromjson("{age: 30}"), kNss1, fromjson("{status: 1}"), "id", "id");
    auto f2 = makeTwoNodeGraph(
        kNss0, fromjson("{age: 30}"), kNssOther, fromjson("{status: 1}"), "id", "id");
    ASSERT_NE(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, DifferentPredicateOperatorsProduceDifferentKeys) {
    auto f1 = makeTwoNodeGraph(kNss0,
                               fromjson("{age: 30}"),
                               kNss1,
                               fromjson("{status: 1}"),
                               "id",
                               "id",
                               JoinPredicate::Eq);
    auto f2 = makeTwoNodeGraph(kNss0,
                               fromjson("{age: 30}"),
                               kNss1,
                               fromjson("{status: 1}"),
                               "id",
                               "id",
                               JoinPredicate::ExprEq);
    ASSERT_NE(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, DifferentMatchStructureProduceDifferentKeys) {
    auto f1 = makeTwoNodeGraph(kNss0, fromjson("{age: 30}"), kNss1, BSONObj{}, "id", "id");
    auto f2 = makeTwoNodeGraph(kNss0, fromjson("{age: {$gt: 30}}"), kNss1, BSONObj{}, "id", "id");
    ASSERT_NE(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, EmbedPathAffectsKey) {
    auto buildGraph = [&](boost::optional<FieldPath> embedPath) {
        std::vector<ResolvedPath> rps;
        MutableJoinGraph mg;
        auto n0 = *mg.addNode(kNss0, makeCanonicalQuery(kNss0, fromjson("{age: 30}")), embedPath);
        auto n1 = *mg.addNode(kNss1, makeCanonicalQuery(kNss1, BSONObj{}), boost::none);
        PathId lp = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n0, FieldPath("id")});
        PathId rp = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n1, FieldPath("id")});
        mg.addEdge(n0, n1, {{JoinPredicate::Eq, lp, rp}});
        return GraphAndResolvedPaths{JoinGraph(std::move(mg)), std::move(rps)};
    };
    auto f1 = buildGraph(FieldPath("orders"));
    auto f2 = buildGraph(FieldPath("products"));
    ASSERT_NE(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, DifferentProjectionsProduceDifferentKeys) {
    auto f1 = makeTwoNodeGraphWithProjections(fromjson("{a: 1}"));
    auto f2 = makeTwoNodeGraphWithProjections(fromjson("{b: 1}"));
    ASSERT_NE(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, SameProjectionProducesSameKey) {
    auto f1 = makeTwoNodeGraphWithProjections(fromjson("{a: 1}"));
    auto f2 = makeTwoNodeGraphWithProjections(fromjson("{a: 1}"));
    ASSERT_EQ(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, ProjectionPresenceAffectsKey) {
    auto f1 = makeTwoNodeGraphWithProjections(fromjson("{a: 1}"));
    auto f2 = makeTwoNodeGraphWithProjections(BSONObj{});
    ASSERT_NE(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, ProjectionOnDifferentNodesProducesDifferentKeys) {
    auto f1 = makeTwoNodeGraphWithProjections(fromjson("{a: 1}"), BSONObj{});
    auto f2 = makeTwoNodeGraphWithProjections(BSONObj{}, fromjson("{a: 1}"));
    ASSERT_NE(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, ProjectionFieldOrderDoesNotAffectKey) {
    auto f1 = makeTwoNodeGraphWithProjections(fromjson("{a: 1, b: 1}"));
    auto f2 = makeTwoNodeGraphWithProjections(fromjson("{b: 1, a: 1}"));
    ASSERT_EQ(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, MultiplePredicatesOnEdgeProduceDifferentKey) {
    auto f1 = makeTwoNodeGraph(kNss0, BSONObj{}, kNss1, BSONObj{}, "id", "id");
    auto f2 = makeTwoNodeGraphTwoPredicates(kNss0, kNss1, "id", "id", "name", "name");
    ASSERT_NE(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, JoinPredicateSortingProduceDifferentKey) {
    auto buildGraph = [&](bool reverseEdgeOrder) {
        std::vector<ResolvedPath> rps;
        MutableJoinGraph mg;
        auto n0 = *mg.addNode(kNss0, makeCanonicalQuery(kNss0, BSONObj{}), boost::none);
        auto n1 = *mg.addNode(kNss1, makeCanonicalQuery(kNss1, BSONObj{}), boost::none);
        // PathIds are always assigned in the same order to ensure the sort is the only variable.
        PathId idL = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n0, FieldPath("id")});
        PathId idR = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n1, FieldPath("id")});
        PathId nameL = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n0, FieldPath("name")});
        PathId nameR = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n1, FieldPath("name")});
        if (!reverseEdgeOrder) {
            mg.addEdge(n0, n1, {{JoinPredicate::Eq, idL, idR}, {JoinPredicate::Eq, nameL, nameR}});
        } else {
            mg.addEdge(n0, n1, {{JoinPredicate::Eq, nameL, nameR}, {JoinPredicate::Eq, idL, idR}});
        }
        return GraphAndResolvedPaths{JoinGraph(std::move(mg)), std::move(rps)};
    };
    auto f1 = buildGraph(false);
    auto f2 = buildGraph(true);
    ASSERT_NE(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, DifferentOrderResolvedPathsProduceDifferentKey) {
    auto buildGraph = [&](bool reverse) {
        std::vector<ResolvedPath> rps;
        MutableJoinGraph mg;
        auto n0 = *mg.addNode(kNss0, makeCanonicalQuery(kNss0, BSONObj{}), boost::none);
        auto n1 = *mg.addNode(kNss1, makeCanonicalQuery(kNss1, BSONObj{}), boost::none);
        PathId lp = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n0, FieldPath("id")});
        PathId rp = static_cast<PathId>(rps.size());
        rps.push_back(ResolvedPath{n1, FieldPath("id")});
        if (reverse) {
            std::swap(rps[0], rps[1]);
        }
        mg.addEdge(n0, n1, {{JoinPredicate::Eq, lp, rp}});
        return GraphAndResolvedPaths{JoinGraph(std::move(mg)), std::move(rps)};
    };
    auto f1 = buildGraph(false);
    auto f2 = buildGraph(true);
    ASSERT_NE(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

// Smoke test for a 3-node linear join graph (A→B→C) with two edges.
TEST_F(JoinPlanCacheKeyTest, ThreeNodeLinearGraphKey) {
    std::vector<ResolvedPath> rps;
    MutableJoinGraph mg;
    auto n0 = *mg.addNode(kNss0, makeCanonicalQuery(kNss0, fromjson("{age: 30}")), boost::none);
    auto n1 = *mg.addNode(kNss1, makeCanonicalQuery(kNss1, BSONObj{}), boost::none);
    auto n2 = *mg.addNode(kNssC, makeCanonicalQuery(kNssC, BSONObj{}), boost::none);
    PathId l0 = static_cast<PathId>(rps.size());
    rps.push_back(ResolvedPath{n0, FieldPath("id")});
    PathId r0 = static_cast<PathId>(rps.size());
    rps.push_back(ResolvedPath{n1, FieldPath("customerId")});
    PathId l1 = static_cast<PathId>(rps.size());
    rps.push_back(ResolvedPath{n1, FieldPath("itemId")});
    PathId r1 = static_cast<PathId>(rps.size());
    rps.push_back(ResolvedPath{n2, FieldPath("id")});
    mg.addEdge(n0, n1, {{JoinPredicate::Eq, l0, r0}});
    mg.addEdge(n1, n2, {{JoinPredicate::Eq, l1, r1}});
    JoinGraph graph(std::move(mg));
    auto key = makeKey(graph, rps);
    ASSERT_FALSE(key.shape().empty());
    ASSERT_TRUE(key.shape().find("n=3") != std::string::npos);
}

// A sparse index on the join's access path collection must change the key, since the plan
// cache uses indexability discriminators to distinguish index-eligible plans.
TEST_F(JoinPlanCacheKeyTest, SparseIndexDiscriminatorChangesKey) {
    auto f = makeTwoNodeGraph(kNss0, fromjson("{age: 30}"), kNss1, BSONObj{}, "id", "id");

    auto key1 = makeKey(f.graph, f.resolvedPaths);

    // Add a sparse index on `age` to kNss0.
    {
        auto mca = multipleCollectionAccessor(operationContext(), {kNss0, kNss1});
        auto uuid = mca.lookupCollection(kNss0)->uuid();
        auto* coord = IndexBuildsCoordinator::get(operationContext());
        ASSERT_DOES_NOT_THROW(
            coord->createIndex(operationContext(),
                               uuid,
                               BSON("v" << 2 << "key" << BSON("age" << 1) << "name"
                                        << "age_sparse_1"
                                        << "sparse" << true),
                               IndexBuildsManager::IndexConstraints::kRelax,
                               false));
    }

    // Re-acquire collections after the DDL to get updated index state.
    auto key2 = makeKey(f.graph, f.resolvedPaths);

    ASSERT_NE(key1, key2);
}

// A partial index's discriminator encodes whether the query satisfies the partial filter
// expression. Two queries on the same collection produce different keys when one is
// eligible for the partial index and the other is not.
TEST_F(JoinPlanCacheKeyTest, PartialIndexDiscriminatorChangesKey) {
    // Partial index on `score` for documents where score > 50.
    {
        auto mca = multipleCollectionAccessor(operationContext(), {kNss0, kNss1});
        auto uuid = mca.lookupCollection(kNss0)->uuid();
        auto* coord = IndexBuildsCoordinator::get(operationContext());
        ASSERT_DOES_NOT_THROW(coord->createIndex(
            operationContext(),
            uuid,
            BSON("v" << 2 << "key" << BSON("score" << 1) << "name"
                     << "score_partial_1"
                     << "partialFilterExpression" << BSON("score" << BSON("$gt" << 50))),
            IndexBuildsManager::IndexConstraints::kRelax,
            false));
    }

    // f1: {score: {$gt: 100}} — implies score > 50, IS eligible for the partial index.
    auto f1 =
        makeTwoNodeGraph(kNss0, fromjson("{score: {$gt: 100}}"), kNss1, BSONObj{}, "id", "id");

    // f2: {score: {$gt: 20}} — does NOT imply score > 50, NOT eligible.
    auto f2 = makeTwoNodeGraph(kNss0, fromjson("{score: {$gt: 20}}"), kNss1, BSONObj{}, "id", "id");

    ASSERT_NE(makeKey(f1.graph, f1.resolvedPaths), makeKey(f2.graph, f2.resolvedPaths));
}

TEST_F(JoinPlanCacheKeyTest, QueryFrameworkChangesKeyAndHash) {
    query_settings::QuerySettings classic, sbe;
    classic.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);
    sbe.setQueryFramework(QueryFrameworkControlEnum::kTrySbeEngine);
    JoinPlanCacheKey key = makeKeyWithSettings();
    JoinPlanCacheKey classicKey = makeKeyWithSettings(classic);
    JoinPlanCacheKey sbeKey = makeKeyWithSettings(sbe);
    ASSERT_NE(key, classicKey);
    ASSERT_NE(key, sbeKey);
    ASSERT_NE(classicKey, sbeKey);
    ASSERT_NE(key.hash(), classicKey.hash());
    ASSERT_NE(key.hash(), sbeKey.hash());
    ASSERT_NE(classicKey.hash(), sbeKey.hash());
}

TEST_F(JoinPlanCacheKeyTest, RejectChangesKeyAndHash) {
    query_settings::QuerySettings rejectFalse, rejectTrue;
    rejectFalse.setReject(false);
    rejectTrue.setReject(true);
    JoinPlanCacheKey key = makeKeyWithSettings();
    JoinPlanCacheKey rejectFalseKey = makeKeyWithSettings(rejectFalse);
    JoinPlanCacheKey rejectTrueKey = makeKeyWithSettings(rejectTrue);
    ASSERT_EQ(key, rejectFalseKey);
    ASSERT_NE(key, rejectTrueKey);
    ASSERT_NE(rejectTrueKey, rejectFalseKey);
    ASSERT_EQ(key.hash(), rejectFalseKey.hash());
    ASSERT_NE(key.hash(), rejectTrueKey.hash());
    ASSERT_NE(rejectTrueKey.hash(), rejectFalseKey.hash());
}

TEST_F(JoinPlanCacheKeyTest, IndexHintsChangeKeyAndHash) {
    const auto ns = makeNsSpec(kNss0);
    query_settings::QuerySettings a1, a1Rebuilt, b;
    a1.setIndexHints({{query_settings::IndexHintSpec(ns, {IndexHint("a_1")})}});
    a1Rebuilt.setIndexHints({{query_settings::IndexHintSpec(ns, {IndexHint("a_1")})}});
    b.setIndexHints({{query_settings::IndexHintSpec(ns, {IndexHint("b_1")})}});
    JoinPlanCacheKey key = makeKeyWithSettings();
    JoinPlanCacheKey a1Key = makeKeyWithSettings(a1);
    JoinPlanCacheKey a1RebuiltKey = makeKeyWithSettings(a1Rebuilt);
    JoinPlanCacheKey bKey = makeKeyWithSettings(b);
    ASSERT_NE(key, a1Key);
    ASSERT_NE(a1Key, bKey);
    ASSERT_EQ(a1Key, a1RebuiltKey);
    ASSERT_NE(key.hash(), a1Key.hash());
    ASSERT_NE(a1Key.hash(), bKey.hash());
    ASSERT_EQ(a1Key.hash(), a1RebuiltKey.hash());
}

TEST_F(JoinPlanCacheKeyTest, IndexHintsOnDifferentNamespacesChangeKeyAndHash) {
    query_settings::QuerySettings onNss0, onNss1;
    onNss0.setIndexHints({{query_settings::IndexHintSpec(makeNsSpec(kNss0), {IndexHint("a_1")})}});
    onNss1.setIndexHints({{query_settings::IndexHintSpec(makeNsSpec(kNss1), {IndexHint("a_1")})}});
    JoinPlanCacheKey nss0Key = makeKeyWithSettings(onNss0);
    JoinPlanCacheKey nss1Key = makeKeyWithSettings(onNss1);
    ASSERT_NE(nss0Key, nss1Key);
    ASSERT_NE(nss0Key.hash(), nss1Key.hash());
}

TEST_F(JoinPlanCacheKeyTest, IndexHintsWithMultipleIndexesChangeKeyAndHash) {
    const auto ns = makeNsSpec(kNss0);
    query_settings::QuerySettings one, two;
    one.setIndexHints({{query_settings::IndexHintSpec(ns, {IndexHint("a_1")})}});
    two.setIndexHints({{query_settings::IndexHintSpec(ns, {IndexHint("a_1"), IndexHint("b_1")})}});
    JoinPlanCacheKey oneKey = makeKeyWithSettings(one);
    JoinPlanCacheKey twoKey = makeKeyWithSettings(two);
    ASSERT_NE(oneKey, twoKey);
    ASSERT_NE(oneKey.hash(), twoKey.hash());
}

TEST_F(JoinPlanCacheKeyTest, IndexHintSpecOrderChangesKeyAndHash) {
    const auto spec0 = query_settings::IndexHintSpec(makeNsSpec(kNss0), {IndexHint("a_1")});
    const auto spec1 = query_settings::IndexHintSpec(makeNsSpec(kNss1), {IndexHint("b_1")});
    // The encoding is order-sensitive (like the classic key), so a reorder causes a cache miss.
    query_settings::QuerySettings forward, reversed;
    forward.setIndexHints({{spec0, spec1}});
    reversed.setIndexHints({{spec1, spec0}});
    JoinPlanCacheKey forwardKey = makeKeyWithSettings(forward);
    JoinPlanCacheKey reversedKey = makeKeyWithSettings(reversed);
    ASSERT_NE(forwardKey, reversedKey);
    ASSERT_NE(forwardKey.hash(), reversedKey.hash());
}

TEST_F(JoinPlanCacheKeyTest, KnobsChangeKeysAndHash) {
    query_settings::QuerySettings intOneKnob, intTwoKnob, boolKnob;
    intOneKnob = settingsWithKnobs(BSON("testIntKnobWire" << 1));
    intTwoKnob = settingsWithKnobs(BSON("testIntKnobWire" << 2));
    boolKnob = settingsWithKnobs(BSON("testBoolKnobWire" << true));
    JoinPlanCacheKey intOneKnobKey = makeKeyWithSettings(intOneKnob);
    JoinPlanCacheKey intTwoKnobKey = makeKeyWithSettings(intTwoKnob);
    JoinPlanCacheKey boolKnobKey = makeKeyWithSettings(boolKnob);
    ASSERT_NE(intOneKnobKey, intTwoKnobKey);
    ASSERT_NE(intOneKnobKey, boolKnobKey);
    ASSERT_NE(intOneKnobKey.hash(), intTwoKnobKey.hash());
    ASSERT_NE(intOneKnobKey.hash(), boolKnobKey.hash());
}

TEST_F(JoinPlanCacheKeyTest, KnobsAbsentVsPresentChangesKeyAndHash) {
    query_settings::QuerySettings withServerDefault =
        settingsWithKnobs(BSON("testIntKnobWire" << 42));
    JoinPlanCacheKey key = makeKeyWithSettings();
    JoinPlanCacheKey withServerDefaultKey = makeKeyWithSettings(withServerDefault);
    ASSERT_NE(key, withServerDefaultKey);
    ASSERT_NE(key.hash(), withServerDefaultKey.hash());
}

TEST_F(JoinPlanCacheKeyTest, EqualKnobOverridesBuiltDifferentlyProduceSameKeyAndHash) {
    query_settings::QuerySettings intThenBool, boolThenInt, merged;
    intThenBool = settingsWithKnobs(BSON("testIntKnobWire" << 1 << "testBoolKnobWire" << true));
    boolThenInt = settingsWithKnobs(BSON("testBoolKnobWire" << true << "testIntKnobWire" << 1));
    // The encoding relies on QuerySettingsKnobOverrides keeping its entries in a canonical order.
    // Build the same overrides through fromBSON() in different field orders and through merge().
    merged.setQueryKnobs(query_settings::QuerySettingsKnobOverrides::merge(
        query_settings::QuerySettingsKnobOverrides::fromBSON(BSON("testIntKnobWire" << 1)),
        query_settings::QuerySettingsKnobOverrides::fromBSON(BSON("testBoolKnobWire" << true))));
    JoinPlanCacheKey intThenBoolKey = makeKeyWithSettings(intThenBool);
    JoinPlanCacheKey boolThenIntKey = makeKeyWithSettings(boolThenInt);
    JoinPlanCacheKey mergedKey = makeKeyWithSettings(merged);
    ASSERT_EQ(intThenBoolKey, boolThenIntKey);
    ASSERT_EQ(intThenBoolKey, mergedKey);
    ASSERT_EQ(intThenBoolKey.hash(), boolThenIntKey.hash());
    ASSERT_EQ(intThenBoolKey.hash(), mergedKey.hash());
}

TEST_F(JoinPlanCacheKeyTest, MaxTimeMSChangesKeyButNotHash) {
    query_settings::QuerySettings qs;
    qs.setMaxTimeMS(1000);
    JoinPlanCacheKey key = makeKeyWithSettings();
    JoinPlanCacheKey maxTimeKey = makeKeyWithSettings(qs);
    ASSERT_NE(key, maxTimeKey);
    ASSERT_EQ(key.hash(), maxTimeKey.hash());
}

TEST_F(JoinPlanCacheKeyTest, CommentChangesKeyButNotHash) {
    query_settings::QuerySettings qs;
    qs.setComment(query_settings::Comment::parseFromBSON(BSON("c" << "x").firstElement()));
    JoinPlanCacheKey key = makeKeyWithSettings();
    JoinPlanCacheKey commentKey = makeKeyWithSettings(qs);
    ASSERT_NE(key, commentKey);
    ASSERT_EQ(key.hash(), commentKey.hash());
}

// Builds a JoinPlanCacheKey directly from a shape string, without a join graph or catalog.
JoinPlanCacheKey makeTestKey(std::string shape, query_settings::QuerySettings qs = {}) {
    return JoinPlanCacheKey{std::move(shape), std::move(qs)};
}

TEST(JoinPlanCacheKeySizeTest, KeySizeIsObjectPlusShapePlusQuerySettingsBson) {
    const std::string shape(100, 's');
    query_settings::QuerySettings emptySettings, classicSettings;
    classicSettings.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);

    const JoinPlanCacheKey emptySettingsKey = makeTestKey(shape, emptySettings);
    const JoinPlanCacheKey settingsKey = makeTestKey(shape, classicSettings);
    ASSERT_EQ(sizeof(JoinPlanCacheKey) + shape.size() + emptySettings.toBSON().objsize(),
              emptySettingsKey.keySizeInBytes());
    ASSERT_EQ(sizeof(JoinPlanCacheKey) + shape.size() + classicSettings.toBSON().objsize(),
              settingsKey.keySizeInBytes());
}

// Integration test fixture that bridges AggJoinModel pipeline construction
// with the real catalog access required by makeJoinPlanCacheKey.
class JoinPlanCacheKeyAggModelTest : public JoinOrderingTestFixture {
public:
    void setUp() override {
        JoinOrderingTestFixture::setUp();
        createCollection(kMainNss);
        createCollection(kForeignNss);
        createCollection(kForeignNss2);
    }

protected:
    const NamespaceString kMainNss =
        NamespaceString::createNamespaceString_forTest("test.main_coll");
    const NamespaceString kForeignNss =
        NamespaceString::createNamespaceString_forTest("test.foreign_coll");
    const NamespaceString kForeignNss2 =
        NamespaceString::createNamespaceString_forTest("test.foreign_coll2");
    const AggModelBuildParams kBuildParams{.maxNumberNodesConsideredForImplicitEdges = 4};
};

TEST_F(JoinPlanCacheKeyAggModelTest, DifferentSyntacticJoinOrderResultsInDifferentKey) {
    std::string_view pipelineJson = R"([
        {
            "$lookup": {
                "from": "foreign_coll",
                "localField": "a",
                "foreignField": "a",
                "as": "foreign"
            }
        },
        {"$unwind": "$foreign"},
        {
            "$lookup": {
                "from": "foreign_coll2",
                "localField": "b",
                "foreignField": "b",
                "as": "foreign2"
            }
        },
        {"$unwind": "$foreign2"}
    ])";

    std::string_view pipelineDifferentJoinOrder = R"([
        {
            "$lookup": {
                "from": "foreign_coll2",
                "localField": "b",
                "foreignField": "b",
                "as": "foreign2"
            }
        },
        {"$unwind": "$foreign2"},
        {
            "$lookup": {
                "from": "foreign_coll",
                "localField": "a",
                "foreignField": "a",
                "as": "foreign"
            }
        },
        {"$unwind": "$foreign"}
    ])";

    auto buildKey = [&](std::string_view pipelineJson) {
        auto expCtx = make_intrusive<ExpressionContextForTest>(operationContext(), kMainNss);
        auto pipeline =
            makePipelineForTest(pipelineJson, {"foreign_coll", "foreign_coll2"}, expCtx);

        // Mark join field as scalar on all collections so path-arrayness analysis does not block
        // join reordering eligibility.
        AggJoinModelFixture::markFieldsAsScalar(
            *pipeline, {"a", "b"}, {{"foreign_coll", {"a"}}, {"foreign_coll2", {"b"}}});

        ASSERT_TRUE(AggJoinModel::pipelineEligibleForJoinReordering(*pipeline));
        OpDebug::JoinOptimizationMetrics metrics;  // This is unused.
        auto swModel = AggJoinModel::constructJoinModel(*pipeline, kBuildParams, metrics);
        ASSERT_OK(swModel);

        const auto& graph = swModel.getValue().getGraph();
        const auto& resolvedPaths = swModel.getValue().getResolvedPaths();

        std::vector<NamespaceString> namespaces;
        for (size_t i = 0; i < graph.numNodes(); ++i) {
            namespaces.push_back(graph.getNode(static_cast<NodeId>(i)).collectionName);
        }
        return makeJoinPlanCacheKey(graph,
                                    resolvedPaths,
                                    multipleCollectionAccessor(operationContext(), namespaces),
                                    query_settings::QuerySettings{});
    };

    auto key = buildKey(pipelineJson);
    auto keyDifferentJoinOrder = buildKey(pipelineDifferentJoinOrder);

    ASSERT_NE(key, keyDifferentJoinOrder);
}

}  // namespace
}  // namespace mongo
