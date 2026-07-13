// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cardinality_estimator.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::cost_based_ranker {

CardinalityEstimate makeCard(double d);

SelectivityEstimate makeSel(double d);

std::unique_ptr<MatchExpression> parse(const BSONObj& bson);

IndexEntry buildSimpleIndexEntry(const std::vector<std::string>& indexFields);

IndexEntry buildMultikeyIndexEntry(const std::vector<std::string>& indexFields,
                                   std::string multikeyField);

CollectionInfo buildCollectionInfo(const std::vector<IndexEntry>& indexes,
                                   std::unique_ptr<stats::CollectionStatistics> collStats);

std::unique_ptr<IndexScanNode> makeIndexScan(const NamespaceString& nss,
                                             IndexBounds bounds,
                                             std::vector<std::string> indexFields,
                                             std::unique_ptr<MatchExpression> filter = nullptr);

std::unique_ptr<QuerySolution> makeIndexScanFetchPlan(
    const NamespaceString& nss,
    IndexBounds bounds,
    std::vector<std::string> indexFields,
    std::unique_ptr<MatchExpression> indexFilter = nullptr,
    std::unique_ptr<MatchExpression> fetchFilter = nullptr);

std::unique_ptr<QuerySolution> makeMultiKeyIndexScanFetchPlan(
    const NamespaceString& nss,
    IndexBounds bounds,
    std::vector<std::string> indexFields,
    std::string multikeyField,
    std::unique_ptr<MatchExpression> indexFilter = nullptr,
    std::unique_ptr<MatchExpression> fetchFilter = nullptr);

std::unique_ptr<QuerySolution> makeCollScanPlan(std::unique_ptr<MatchExpression> filter);

std::unique_ptr<QuerySolution> makeVirtualCollScanPlan(size_t size,
                                                       std::unique_ptr<MatchExpression> filter);

OrderedIntervalList makePointInterval(double point, std::string fieldName);
OrderedIntervalList makePointInterval(std::string_view str, std::string fieldName);
OrderedIntervalList makePointInterval(const BSONObj& obj, std::string fieldName);

IndexBounds makePointIntervalBounds(double point, std::string fieldName);
IndexBounds makePointIntervalBounds(std::string_view str, std::string fieldName);
IndexBounds makePointIntervalBounds(const BSONObj& obj, std::string fieldName);

IndexBounds makeRangeIntervalBounds(const BSONObj& range,
                                    BoundInclusion boundInclusion,
                                    std::string fieldName);

CEResult getPlanCE(const QuerySolution& plan,
                   const CollectionInfo& collInfo,
                   QueryCBRCEModeEnum ceMode);

CardinalityEstimate getPlanHeuristicCE(const QuerySolution& plan, double collCard);
CardinalityEstimate getPlanHeuristicCE(const QuerySolution& plan, const CollectionInfo& collInfo);

CardinalityEstimate getPlanHistogramCE(const QuerySolution& plan, const CollectionInfo& collInfo);

CardinalityEstimate getPlanSamplingCE(const QuerySolution& plan,
                                      double collCard,
                                      ce::SamplingEstimator* samplingEstimator);

std::unique_ptr<stats::CollectionStatistics> makeCollStats(double collCard);
std::unique_ptr<stats::CollectionStatistics> makeCollStatsWithHistograms(
    const std::vector<std::string>& histFields, double collCard);

}  // namespace mongo::cost_based_ranker
