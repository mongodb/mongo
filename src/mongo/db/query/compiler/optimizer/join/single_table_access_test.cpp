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

#include "mongo/db/query/compiler/optimizer/join/single_table_access.h"

#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_test_utils.h"
#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo::join_ordering {

using namespace mongo::cost_based_ranker;

class SingleTableAccessTestFixture : public CatalogTestFixture {
public:
    std::unique_ptr<ce::SamplingEstimator> samplingEstimator(const MultipleCollectionAccessor& mca,
                                                             NamespaceString nss) {
        auto collPtr = mca.lookupCollection(nss);
        auto size = collPtr->getRecordStore()->numRecords();
        // Sample 10% of the collection
        auto sampleSize = static_cast<long>(size * 0.1);
        auto samplingEstimator = std::make_unique<ce::SamplingEstimatorImpl>(
            operationContext(),
            mca,
            PlanYieldPolicy::YieldPolicy::YIELD_MANUAL,
            sampleSize,
            ce::SamplingEstimatorImpl::SamplingStyle::kRandom,
            boost::none,
            CardinalityEstimate{CardinalityType{static_cast<double>(size)},
                                EstimationSource::Code});
        samplingEstimator->generateSample(ce::NoProjection{});
        return samplingEstimator;
    }

    std::unique_ptr<CanonicalQuery> makeCanonicalQuery(NamespaceString nss, BSONObj filter) {
        auto expCtx = ExpressionContextBuilder{}.opCtx(operationContext()).build();

        auto swFindCmd = ParsedFindCommand::withExistingFilter(
            expCtx,
            nullptr,
            std::move(MatchExpressionParser::parse(filter, expCtx).getValue()),
            std::make_unique<FindCommandRequest>(nss),
            ProjectionPolicies::aggregateProjectionPolicies());

        auto swCq = CanonicalQuery::make(CanonicalQueryParams{
            .expCtx = expCtx,
            .parsedFind = std::move(swFindCmd.getValue()),
        });
        ASSERT_OK(swCq);
        return std::move(swCq.getValue());
    }

    void createIndex(UUID collUUID, BSONObj spec, std::string name) {
        auto indexBuildsCoord = IndexBuildsCoordinator::get(operationContext());
        auto indexConstraints = IndexBuildsManager::IndexConstraints::kRelax;
        ASSERT_DOES_NOT_THROW(indexBuildsCoord->createIndex(
            operationContext(),
            collUUID,
            BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << spec << "name" << name),
            indexConstraints,
            false));
    }
};

void assertQuerySolutionHasEstimate(const QuerySolutionNode* qsn, const EstimateMap& estimates) {
    auto it = estimates.find(qsn);
    ASSERT(it != estimates.end());
    ASSERT_EQ(EstimationSource::Sampling, it->second.outCE.source());
    for (auto&& child : qsn->children) {
        assertQuerySolutionHasEstimate(child.get(), estimates);
    }
}

// Test estimate map is populated for each collection
TEST_F(SingleTableAccessTestFixture, EstimatesPopulated) {
    auto opCtx = operationContext();
    auto nss1 = NamespaceString::createNamespaceString_forTest("test", "coll1");
    auto nss2 = NamespaceString::createNamespaceString_forTest("test", "coll2");

    std::vector<BSONObj> docs;
    for (int i = 0; i < 100; ++i) {
        docs.push_back(BSON("_id" << i << "a" << 1 << "b" << i));
    }

    ce::createCollAndInsertDocuments(opCtx, nss1, docs);
    ce::createCollAndInsertDocuments(opCtx, nss2, docs);

    {
        auto mca = multipleCollectionAccessor(opCtx, {nss1, nss2});
        auto nss1UUID = mca.lookupCollection(nss1)->uuid();
        auto nss2UUID = mca.lookupCollection(nss2)->uuid();

        createIndex(nss1UUID, fromjson("{a: 1}"), "a_1");
        createIndex(nss1UUID, fromjson("{b: 1}"), "b_1");
        createIndex(nss2UUID, fromjson("{a: 1}"), "a_1");
        createIndex(nss2UUID, fromjson("{b: 1}"), "b_1");
    }

    // Get new MultiCollectionAccessor after all DDLs are done.
    auto mca = multipleCollectionAccessor(opCtx, {nss1, nss2});

    SamplingEstimatorMap estimators;
    estimators[nss1] = samplingEstimator(mca, nss1);
    estimators[nss2] = samplingEstimator(mca, nss2);

    auto filter1 = fromjson("{a: 1, b: 1}");
    auto filter2 = fromjson("{a: 1, b: 1}");

    // Mock a JoinGraph for testing purposes.
    JoinGraph graph;
    graph.addNode(nss1, makeCanonicalQuery(nss1, filter1), boost::none);
    graph.addNode(nss2, makeCanonicalQuery(nss2, filter2), boost::none);
    auto swRes = singleTableAccessPlans(opCtx, mca, graph, estimators);
    ASSERT_OK(swRes);

    auto& res = swRes.getValue();
    ASSERT_EQ(2, res.solns.size());

    for (auto&& [_, soln] : res.solns) {
        assertQuerySolutionHasEstimate(soln->root(), res.estimate);
    }
}

}  // namespace mongo::join_ordering
