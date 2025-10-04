/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cbr_test_utils.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/ce/ce_test_utils.h"
#include "mongo/db/query/compiler/ce/histogram/histogram_test_utils.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/stats/collection_statistics_mock.h"
#include "mongo/platform/compiler.h"

namespace mongo::cost_based_ranker {

CardinalityEstimate makeCard(double d) {
    return CardinalityEstimate(CardinalityType(d), EstimationSource::Code);
}

SelectivityEstimate makeSel(double d) {
    return SelectivityEstimate(SelectivityType(d), EstimationSource::Code);
}

std::unique_ptr<MatchExpression> parse(const BSONObj& bson) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto expr = MatchExpressionParser::parse(
        bson, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(expr);
    return std::move(expr.getValue());
}

BSONObj fieldsToKeyPattern(const std::vector<std::string>& indexFields) {
    BSONObjBuilder bob;
    for (auto& fieldName : indexFields) {
        bob.append(fieldName, 1);
    }
    return bob.obj();
}

IndexEntry buildSimpleIndexEntry(const std::vector<std::string>& indexFields) {
    BSONObj kp = fieldsToKeyPattern(indexFields);
    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
            IndexConfig::kLatestIndexVersion,
            false,
            {},
            {},
            false,
            false,
            CoreIndexInfo::Identifier("test_foo"),
            nullptr,
            {},
            nullptr,
            nullptr};
}

IndexEntry buildMultikeyIndexEntry(const std::vector<std::string>& indexFields,
                                   std::string multikeyField) {
    BSONObj kp = fieldsToKeyPattern(indexFields);
    MultikeyPaths mkp;
    for (auto&& field : indexFields) {
        if (field == multikeyField) {
            mkp.push_back({0U});
        } else {
            mkp.push_back({});
        }
    }
    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
            IndexConfig::kLatestIndexVersion,
            true,
            mkp,
            {},
            false,
            false,
            CoreIndexInfo::Identifier("test_foo"),
            nullptr,
            {},
            nullptr,
            nullptr};
}

CollectionInfo buildCollectionInfo(const std::vector<IndexEntry>& indexes,
                                   std::unique_ptr<stats::CollectionStatistics> collStats) {
    CollectionInfo collInfo;
    collInfo.indexes = indexes;
    collInfo.collStats = std::move(collStats);
    return collInfo;
}

std::unique_ptr<IndexScanNode> makeIndexScan(IndexBounds bounds,
                                             std::vector<std::string> indexFields,
                                             std::unique_ptr<MatchExpression> filter) {
    auto indexScan = std::make_unique<IndexScanNode>(buildSimpleIndexEntry(indexFields));
    indexScan->bounds = std::move(bounds);
    if (filter) {
        indexScan->filter = std::move(filter);
    }
    return indexScan;
}

std::unique_ptr<IndexScanNode> makeMultiKeyIndexScan(IndexBounds bounds,
                                                     std::vector<std::string> indexFields,
                                                     std::string multikeyField,
                                                     std::unique_ptr<MatchExpression> filter) {
    auto indexScan =
        std::make_unique<IndexScanNode>(buildMultikeyIndexEntry(indexFields, multikeyField));
    indexScan->bounds = std::move(bounds);
    if (filter) {
        indexScan->filter = std::move(filter);
    }
    return indexScan;
}

std::unique_ptr<QuerySolution> makeIndexScanFetchPlan(
    IndexBounds bounds,
    std::vector<std::string> indexFields,
    std::unique_ptr<MatchExpression> indexFilter,
    std::unique_ptr<MatchExpression> fetchFilter) {

    auto fetch =
        std::make_unique<FetchNode>(makeIndexScan(bounds, indexFields, std::move(indexFilter)));
    if (fetchFilter) {
        fetch->filter = std::move(fetchFilter);
    }

    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::move(fetch));
    return solution;
}

std::unique_ptr<QuerySolution> makeMultiKeyIndexScanFetchPlan(
    IndexBounds bounds,
    std::vector<std::string> indexFields,
    std::string multikeyField,
    std::unique_ptr<MatchExpression> indexFilter,
    std::unique_ptr<MatchExpression> fetchFilter) {

    auto fetch = std::make_unique<FetchNode>(
        makeMultiKeyIndexScan(bounds, indexFields, multikeyField, std::move(indexFilter)));
    if (fetchFilter) {
        fetch->filter = std::move(fetchFilter);
    }

    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::move(fetch));
    return solution;
}

std::unique_ptr<QuerySolution> makeCollScanPlan(std::unique_ptr<MatchExpression> filter) {
    auto scan = std::make_unique<CollectionScanNode>();
    if (filter) {
        scan->filter = std::move(filter);
    }

    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::move(scan));
    return solution;
}

std::unique_ptr<QuerySolution> makeVirtualCollScanPlan(size_t size,
                                                       std::unique_ptr<MatchExpression> filter) {
    // In a virtual collection, each "document" is a BSONArray with a single element representing
    // the document.
    std::vector<BSONArray> docs(size, BSON_ARRAY(BSON("a" << 1)));
    auto scan = std::make_unique<VirtualScanNode>(
        std::move(docs), VirtualScanNode::ScanType::kCollScan, false);
    if (filter) {
        scan->filter = std::move(filter);
    }

    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::move(scan));
    return solution;
}

OrderedIntervalList makePointInterval(double point, std::string fieldName) {
    OrderedIntervalList oil(fieldName);
    oil.intervals.emplace_back(IndexBoundsBuilder::makePointInterval(point));
    return oil;
}

IndexBounds makePointIntervalBounds(double point, std::string fieldName) {
    IndexBounds bounds;
    bounds.fields.emplace_back(makePointInterval(point, fieldName));
    return bounds;
}

IndexBounds makeRangeIntervalBounds(const BSONObj& range,
                                    BoundInclusion boundInclusion,
                                    std::string fieldName) {
    OrderedIntervalList oilRange(fieldName);
    oilRange.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(range, boundInclusion));
    IndexBounds rangeBounds;
    rangeBounds.fields.push_back(oilRange);
    return rangeBounds;
}

CEResult getPlanCE(const QuerySolution& plan,
                   const CollectionInfo& collInfo,
                   QueryPlanRankerModeEnum ceMode) {
    EstimateMap qsnEstimates;
    CardinalityEstimator estimator{collInfo, nullptr /*samplingEstimator*/, qsnEstimates, ceMode};
    return estimator.estimatePlan(plan);
}

CardinalityEstimate getPlanHeuristicCE(const QuerySolution& plan, double collCard) {
    return getPlanHeuristicCE(plan,
                              buildCollectionInfo({}, makeCollStatsWithHistograms({}, collCard)));
}

CardinalityEstimate getPlanHeuristicCE(const QuerySolution& plan, const CollectionInfo& collInfo) {
    const auto ceRes = getPlanCE(plan, collInfo, QueryPlanRankerModeEnum::kHeuristicCE);
    ASSERT(ceRes.isOK());
    return ceRes.getValue();
}

CardinalityEstimate getPlanHistogramCE(const QuerySolution& plan, const CollectionInfo& collInfo) {
    const auto ceRes = getPlanCE(plan, collInfo, QueryPlanRankerModeEnum::kHistogramCE);
    ASSERT(ceRes.isOK());
    return ceRes.getValue();
}

CardinalityEstimate getPlanSamplingCE(const QuerySolution& plan,
                                      double collCard,
                                      ce::SamplingEstimator* samplingEstimator) {
    EstimateMap qsnEstimates;
    auto collInfo = buildCollectionInfo({}, makeCollStatsWithHistograms({}, collCard));
    CardinalityEstimator estimator{
        collInfo, samplingEstimator, qsnEstimates, QueryPlanRankerModeEnum::kSamplingCE};
    const auto ceRes = estimator.estimatePlan(plan);
    ASSERT(ceRes.isOK());
    return ceRes.getValue();
}

std::unique_ptr<stats::CollectionStatistics> makeCollStats(double collCard) {
    return makeCollStatsWithHistograms({}, collCard);
}

std::unique_ptr<stats::CollectionStatistics> makeCollStatsWithHistograms(
    const std::vector<std::string>& histFields, double collCard) {
    std::unique_ptr<stats::CollectionStatistics> stats =
        std::make_unique<stats::CollectionStatisticsMock>(collCard);
    std::vector<ce::BucketData> data{
        {0 /*bucketBoundary*/, 10 /*equalFreq*/, 90 /*rangeFreq*/, 5 /*ndv*/},
        {5, 100, 100, 0},
        {6, 700, 0, 0}};
    for (const auto& field : histFields) {
        stats->addHistogram(field,
                            stats::CEHistogram::make(
                                ce::createHistogram(data),
                                stats::TypeCounts{{sbe::value::TypeTags::NumberInt64, collCard}},
                                collCard));
    }
    return stats;
}

}  // namespace mongo::cost_based_ranker
