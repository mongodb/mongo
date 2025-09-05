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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cardinality_estimator.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"

namespace mongo::cost_based_ranker {

CardinalityEstimate makeCard(double d);

SelectivityEstimate makeSel(double d);

std::unique_ptr<MatchExpression> parse(const BSONObj& bson);

IndexEntry buildSimpleIndexEntry(const std::vector<std::string>& indexFields);

IndexEntry buildMultikeyIndexEntry(const std::vector<std::string>& indexFields,
                                   std::string multikeyField);

CollectionInfo buildCollectionInfo(const std::vector<IndexEntry>& indexes,
                                   std::unique_ptr<stats::CollectionStatistics> collStats);

std::unique_ptr<IndexScanNode> makeIndexScan(IndexBounds bounds,
                                             std::vector<std::string> indexFields,
                                             std::unique_ptr<MatchExpression> filter = nullptr);

std::unique_ptr<QuerySolution> makeIndexScanFetchPlan(
    IndexBounds bounds,
    std::vector<std::string> indexFields,
    std::unique_ptr<MatchExpression> indexFilter = nullptr,
    std::unique_ptr<MatchExpression> fetchFilter = nullptr);

std::unique_ptr<QuerySolution> makeMultiKeyIndexScanFetchPlan(
    IndexBounds bounds,
    std::vector<std::string> indexFields,
    std::string multikeyField,
    std::unique_ptr<MatchExpression> indexFilter = nullptr,
    std::unique_ptr<MatchExpression> fetchFilter = nullptr);

std::unique_ptr<QuerySolution> makeCollScanPlan(std::unique_ptr<MatchExpression> filter);

std::unique_ptr<QuerySolution> makeVirtualCollScanPlan(size_t size,
                                                       std::unique_ptr<MatchExpression> filter);

OrderedIntervalList makePointInterval(double point, std::string fieldName);

IndexBounds makePointIntervalBounds(double point, std::string fieldName);

IndexBounds makeRangeIntervalBounds(const BSONObj& range,
                                    BoundInclusion boundInclusion,
                                    std::string fieldName);

CEResult getPlanCE(const QuerySolution& plan,
                   const CollectionInfo& collInfo,
                   QueryPlanRankerModeEnum ceMode);

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
