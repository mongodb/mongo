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

#include "mongo/db/query/compiler/optimizer/join/unit_test_helpers.h"

#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"

namespace mongo::join_ordering {

using namespace cost_based_ranker;

MultipleCollectionAccessor multipleCollectionAccessor(OperationContext* opCtx,
                                                      std::vector<NamespaceString> namespaces) {
    auto mainAcquisitionReq = CollectionAcquisitionRequest::fromOpCtx(
        opCtx, namespaces.front(), AcquisitionPrerequisites::kRead);
    auto mainAcquisition = acquireCollection(opCtx, mainAcquisitionReq, LockMode::MODE_X);

    CollectionOrViewAcquisitionRequests acquisitionReqs;
    for (size_t i = 1; i < namespaces.size(); ++i) {
        acquisitionReqs.push_back(CollectionAcquisitionRequest::fromOpCtx(
            opCtx, namespaces[i], AcquisitionPrerequisites::kRead));
    }

    auto acquisitions = acquireCollectionsOrViews(opCtx, acquisitionReqs, LockMode::MODE_X);
    auto acquisitionMap = makeAcquisitionMap(acquisitions);
    return MultipleCollectionAccessor(std::move(mainAcquisition), std::move(acquisitionMap), false);
}

std::unique_ptr<CanonicalQuery> JoinOrderingTestFixture::makeCanonicalQuery(NamespaceString nss,
                                                                            BSONObj filter) {
    auto expCtx = ExpressionContextBuilder{}.opCtx(operationContext()).build();
    if (!filter.isEmpty()) {
        auto swFindCmd = ParsedFindCommand::withExistingFilter(
            expCtx,
            nullptr,
            std::move(MatchExpressionParser::parse(filter, expCtx).getValue()),
            std::make_unique<FindCommandRequest>(nss),
            ProjectionPolicies::aggregateProjectionPolicies());
        ASSERT_OK(swFindCmd.getStatus());
        return std::make_unique<CanonicalQuery>(
            CanonicalQueryParams{.expCtx = expCtx, .parsedFind = std::move(swFindCmd.getValue())});
    }
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = expCtx, .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
}

std::unique_ptr<QuerySolution> JoinOrderingTestFixture::makeCollScanPlan(
    NamespaceString nss, std::unique_ptr<MatchExpression> filter) {
    auto scan = std::make_unique<CollectionScanNode>();
    scan->nss = nss;
    scan->filter = std::move(filter);
    auto soln = std::make_unique<QuerySolution>();
    soln->setRoot(std::move(scan));
    return soln;
}

void JoinOrderingTestFixture::createCollection(NamespaceString nss) {
    ASSERT_OK(storageInterface()->createCollection(operationContext(), nss, CollectionOptions()));
}

void JoinOrderingTestFixture::createIndex(UUID collUUID, BSONObj spec, std::string name) {
    auto indexBuildsCoord = IndexBuildsCoordinator::get(operationContext());
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kRelax;
    ASSERT_DOES_NOT_THROW(indexBuildsCoord->createIndex(
        operationContext(),
        collUUID,
        BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << spec << "name" << name),
        indexConstraints,
        false));
}

std::unique_ptr<ce::SamplingEstimator> JoinOrderingTestFixture::samplingEstimator(
    const MultipleCollectionAccessor& mca, NamespaceString nss, double sampleProportion) {
    auto collPtr = mca.lookupCollection(nss);
    auto size = collPtr->getRecordStore()->numRecords();
    auto sampleSize = static_cast<long>(size * sampleProportion);
    auto samplingEstimator = std::make_unique<ce::SamplingEstimatorImpl>(
        operationContext(),
        mca,
        nss,
        PlanYieldPolicy::YieldPolicy::YIELD_MANUAL,
        sampleSize,
        ce::SamplingEstimatorImpl::SamplingStyle::kRandom,
        boost::none,
        CardinalityEstimate{CardinalityType{static_cast<double>(size)}, EstimationSource::Code});
    samplingEstimator->generateSample(ce::NoProjection{});
    return samplingEstimator;
}

NamespaceString makeNSS(StringData collName) {
    return NamespaceString::makeLocalCollection(collName);
}
}  // namespace mongo::join_ordering
