/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
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
#include "mongo/util/modules.h"

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
                                                       BSONObj filter = BSONObj::kEmptyObject);

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

        JoinReorderingContext jCtx{
            .joinGraph = joinGraphStorage.value(),
            .resolvedPaths = std::move(resolvedPaths),
            .cbrCqQsns = std::move(cbrCqQsns),
            .perCollIdxs = std::move(perCollIdxs),
        };

        return jCtx;
    }

protected:
    unittest::GoldenTestConfig goldenTestConfig;

    MutableJoinGraph graph;
    boost::optional<JoinGraph> joinGraphStorage;
    std::vector<ResolvedPath> resolvedPaths;
    QuerySolutionMap cbrCqQsns;
    AvailableIndexes perCollIdxs;

    std::vector<int> seeds;
    std::vector<NamespaceString> namespaces;
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
    CardinalityEstimate estimateNDV(const std::vector<FieldPath>& fieldNames) const override {
        return _fakeEstimates.at(fieldNames);
    }

    double getCollCard() const override {
        return _collCard.toDouble();
    }

private:
    CardinalityEstimate _collCard;
    stdx::unordered_map<std::vector<FieldPath>, CardinalityEstimate> _fakeEstimates;
};

/**
 * Tests that assert on join order enumeration but not the winning plan typically need
 * statistics that will provide consistent CE results, but they need not be meaningful.
 * This helper gives all node sets representing bitset 'b' a CE of b.to_ullong().
 */
class FakeJoinCardinalityEstimator : public JoinCardinalityEstimator {
public:
    FakeJoinCardinalityEstimator(const JoinReorderingContext& jCtx)
        : JoinCardinalityEstimator(
              jCtx,
              EdgeSelectivities(jCtx.joinGraph.numEdges(), cost_based_ranker::zeroSel),
              NodeCardinalities(jCtx.joinGraph.numNodes(), cost_based_ranker::oneCE)) {};

    cost_based_ranker::CardinalityEstimate getOrEstimateSubsetCardinality(
        const NodeSet& nodes) override {
        return cost_based_ranker::CardinalityEstimate(
            cost_based_ranker::CardinalityType(nodes.to_ullong()),
            cost_based_ranker::EstimationSource::Code);
    }
};


/**
 * Small utility function to make a namepace string from collection name.
 */
NamespaceString makeNSS(StringData collName);
}  // namespace mongo::join_ordering
