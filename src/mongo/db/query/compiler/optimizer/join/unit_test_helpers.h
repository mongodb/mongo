// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/join/cardinality_estimator.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/join_reordering_context.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry_mock.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_mock.h"
#include "mongo/unittest/golden_test_base.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::join_ordering {

/**
 * Construct acquisition requests for the given namespaces and returns a MultipleCollectionAccessor.
 */
MultipleCollectionAccessor multipleCollectionAccessor(OperationContext* opCtx,
                                                      std::vector<NamespaceString> namespaces);

/**
 * Helpers used to mock index information for unit tests without requiring a
 * MultipleCollectionAccessor.
 */
IndexDescriptor makeIndexDescriptor(BSONObj indexSpec);
IndexCatalogEntryMock makeIndexCatalogEntry(BSONObj indexSpec);
IndexCatalogMock makeIndexCatalog(const std::vector<BSONObj>& keyPatterns);
std::vector<std::shared_ptr<const IndexCatalogEntry>> makeIndexCatalogEntries(
    const std::vector<BSONObj>& keyPatterns);

/**
 * Build UniqueFieldInformation for a single collection based on the given key patterns representing
 * unique index keys.
 */
UniqueFieldInformation buildUniqueFieldInfo(const std::vector<BSONObj>& uniqueKeyPatterns);

/**
 * Text fixture with helpful functions for manipulating the catalog, constructing samples and
 * queries/QSNs.
 * Every test must contains 2 phases.
 * 1. Preparation phase. Build 'graph', add physical plans to 'cbrCqQsns', pouplate indexes in
 * 'perCollIdxs'. The preparation stages always completes with creation of JoinReorderingContext
 * using 'makeContext()' function. This function destroys the graph object and 'cbrCqQsns'.
 * 2. Testing phase. Validate the expected results using JoinReorderingContext created at the end of
 * the preparation stage.
 */
class JoinOrderingTestFixture : public CatalogTestFixture {
public:
    JoinOrderingTestFixture() : goldenTestConfig{"src/mongo/db/test_output/query/join"} {}

    std::unique_ptr<CanonicalQuery> makeCanonicalQuery(NamespaceString nss,
                                                       BSONObj filter = BSONObj::kEmptyObject,
                                                       BSONObj projection = BSONObj::kEmptyObject);

    std::unique_ptr<QuerySolution> makeCollScanPlan(
        NamespaceString nss, std::unique_ptr<MatchExpression> filter = nullptr);

    void createCollection(NamespaceString nss);

    void createIndex(UUID collUUID, BSONObj spec, std::string name);

    std::unique_ptr<ce::SamplingEstimator> samplingEstimator(const MultipleCollectionAccessor& mca,
                                                             NamespaceString nss,
                                                             double sampleProportion = 0.1);

    /**
     * Can be called at most once per test once the join graph is fully built.
     */
    JoinReorderingContext makeContext() {
        joinGraphStorage = JoinGraph(std::move(graph));
        const auto numNodes = joinGraphStorage->numNodes();

        NodeCardinalities collCardinalities = collCards.empty()
            ? NodeCardinalities(numNodes, cost_based_ranker::oneCE)
            : std::move(collCards);

        NodeCardinalities nodeCardinalities =
            nodeCards.empty() ? collCardinalities : std::move(nodeCards);

        NodeCBRCosts costs = nodeCBRCosts.empty()
            ? NodeCBRCosts(numNodes, cost_based_ranker::zeroCost)
            : std::move(nodeCBRCosts);

        SingleTableAccessPlansResult singleTableAccess{
            .cbrCqQsns = std::move(cbrCqQsns),
            .estimate = {},
            .nodeCardinalities = std::move(nodeCardinalities),
            .collCardinalities = std::move(collCardinalities),
            .nodeCBRCosts = std::move(costs)};

        JoinReorderingContext jCtx{.joinGraph = joinGraphStorage.value(),
                                   .resolvedPaths = std::move(resolvedPaths),
                                   .singleTableAccess = std::move(singleTableAccess),
                                   .perCollIdxs = std::move(perCollIdxs),
                                   .catStats = std::move(catStats)};

        return jCtx;
    }

    /**
     * Helper used to initialize the join graph with some number of nodes and some arbitrary
     * cardinalities, as well as populating index information if 'withIndexes' is set to true.
     *
     * NOTE: this does not add any edges to the graph.
     */
    void initGraph(size_t numNodes, bool withIndexes = false);

protected:
    unittest::GoldenTestConfig goldenTestConfig;

    MutableJoinGraph graph;
    boost::optional<JoinGraph> joinGraphStorage;
    std::vector<ResolvedPath> resolvedPaths;
    QuerySolutionMap cbrCqQsns;
    AvailableIndexes perCollIdxs;
    SubsetCardinalities subsetCards;
    NodeCardinalities collCards;
    NodeCardinalities nodeCards;
    NodeCBRCosts nodeCBRCosts;
    CatalogStats catStats;

    std::vector<int> seeds;
    std::vector<NamespaceString> namespaces;

    std::vector<BSONObj> bsonStorage;
};

using namespace cost_based_ranker;

/**
 * Estimator that allows to faking the result of NDV estimation. Asserts on calling any other
 * function.
 */
class FakeNdvEstimator : public ce::SamplingEstimator {
public:
    FakeNdvEstimator(CardinalityEstimate collCard) : _collCard(collCard) {};

    CardinalityEstimate estimateCardinality(const MatchExpression* expr) const override {
        MONGO_UNREACHABLE;
    }
    std::vector<CardinalityEstimate> estimateCardinality(
        const std::vector<const MatchExpression*>& expr) const override {
        MONGO_UNREACHABLE;
    }
    CardinalityEstimate estimateKeysScanned(const IndexBounds& bounds) const override {
        MONGO_UNREACHABLE;
    }
    std::vector<CardinalityEstimate> estimateKeysScanned(
        const std::vector<const IndexBounds*>& bounds) const override {
        MONGO_UNREACHABLE;
    }
    CardinalityEstimate estimateRIDs(const IndexBounds& bounds,
                                     const MatchExpression* expr) const override {
        MONGO_UNREACHABLE;
    }
    std::vector<CardinalityEstimate> estimateRIDs(
        const std::vector<const IndexBounds*>& bounds,
        const std::vector<const MatchExpression*>& expressions) const override {
        MONGO_UNREACHABLE;
    }
    void generateSample(ce::ProjectionParams projectionParams) override {
        MONGO_UNREACHABLE;
    }

    /*
     * Add a fake response to 'estimateNDV()' for the given 'fields'.
     */
    void addFakeNDVEstimate(std::vector<FieldPath> fields, CardinalityEstimate estimate) {
        _fakeEstimates.insert_or_assign(fields, estimate);
    }

    /*
     * Uses the results assigned from 'addFakeNDVEstimate()'. If an estimate for a particular
     * 'fieldNames' is not set, this function will throw an exception.
     */
    CardinalityEstimate estimateNDV(
        const std::vector<ce::FieldPathAndEqSemantics>& fields,
        boost::optional<std::span<const OrderedIntervalList>> bounds) const override {
        ASSERT_FALSE(bounds.has_value()) << "FakeNdvEstimator doesn't handle bounds";
        std::vector<FieldPath> fieldPaths;
        for (auto&& field : fields) {
            fieldPaths.push_back(field.path);
        }
        return _fakeEstimates.at(fieldPaths);
    }

    CardinalityEstimate estimateNDVMultiKey(
        const std::vector<ce::FieldPathAndEqSemantics>& fields,
        boost::optional<std::span<const OrderedIntervalList>> bounds) const override {
        // Not yet required in these tests.
        MONGO_UNIMPLEMENTED;
    }

    CardinalityEstimate getCollCard() const override {
        return _collCard;
    }

    size_t getSampleSize() const override {
        MONGO_UNREACHABLE;
    }

    ce::SamplingMetadata getSamplingMetadata() const override {
        MONGO_UNREACHABLE;
    }

private:
    CardinalityEstimate _collCard;
    stdx::unordered_map<std::vector<FieldPath>, CardinalityEstimate> _fakeEstimates;
};

/**
 * Fake implementation of JoinCardinalityEstimator useful for tests which need to inject artificial
 * cardinality estimates to verify the behavior of other components in isolation.
 */
class FakeJoinCardinalityEstimator final : public JoinCardinalityEstimator {
public:
    /**
     * This constructor causes the fake to return consistent but not meaningful CE results for
     * 'getOrEstimateSubsetCardinality()'. This is useful for enumeration tests.
     */
    FakeJoinCardinalityEstimator(const JoinReorderingContext& jCtx)
        : JoinCardinalityEstimator(
              jCtx, EdgeSelectivities(jCtx.joinGraph.numEdges(), cost_based_ranker::zeroSel)) {
        for (uint64_t i = 0; i < std::pow(2, jCtx.joinGraph.numNodes()); ++i) {
            _subsetCardinalities.emplace(
                NodeSet::fromUIntBitSet(i),
                // Convert the bitset to a double to get a consistent estimate.
                CardinalityEstimate{CardinalityType{static_cast<double>(i)},
                                    EstimationSource::Code});
        }
    }

    /**
     * This constructor allows the caller to control the results of
     * 'getOrEstimateSubsetCardinality()'. Per-node and collection cardinalities are read from
     * 'jCtx.singleTableAccess', which tests can populate directly.
     */
    FakeJoinCardinalityEstimator(const JoinReorderingContext& jCtx, SubsetCardinalities subsetCards)
        : JoinCardinalityEstimator(
              jCtx, EdgeSelectivities(jCtx.joinGraph.numEdges(), cost_based_ranker::zeroSel)) {
        _subsetCardinalities = std::move(subsetCards);
    }

    /**
     * This constuctor initializes base node subsets and generates estimates for composite subsets.
     */
    FakeJoinCardinalityEstimator(const JoinReorderingContext& jCtx,
                                 SubsetCardinalities subsetCards,
                                 EdgeSelectivities edgeSels)
        : JoinCardinalityEstimator(jCtx, std::move(edgeSels)) {
        _subsetCardinalities = std::move(subsetCards);
    }
};

/**
 * Small utility function to make a namepace string from collection name.
 */
NamespaceString makeNSS(std::string_view collName);

/**
 * Pipeline construction helpers for use in tests.
 */
std::unique_ptr<Pipeline> makePipelineForTest(
    std::vector<BSONObj> bsonStages,
    std::vector<std::string_view> collNames,
    boost::intrusive_ptr<ExpressionContextForTest> expCtx);
std::unique_ptr<Pipeline> makePipelineForTest(
    std::string_view query,
    std::vector<std::string_view> collNames,
    boost::intrusive_ptr<ExpressionContextForTest> expCtx);
}  // namespace mongo::join_ordering
