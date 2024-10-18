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

#include "mongo/db/query/ce/sampling_estimator.h"

#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/platform/basic.h"

namespace mongo {

namespace {

/*
 * This helper creates a CanonicalQuery for the sampling plan.
 */
std::unique_ptr<CanonicalQuery> makeCanonicalQuery(const NamespaceString& nss,
                                                   OperationContext* opCtx,
                                                   const size_t sampleSize) {
    auto findCommand = std::make_unique<FindCommandRequest>(NamespaceStringOrUUID(nss));
    findCommand->setLimit(sampleSize);

    auto expCtx = makeExpressionContext(opCtx, *findCommand);

    auto statusWithCQ = CanonicalQuery::make(
        {.expCtx = expCtx,
         .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)}});

    return std::move(statusWithCQ.getValue());
}
}  // namespace

namespace optimizer::ce {
/*
 * The sample size is calculated based on the confidence level and margin of error required.
 */
size_t SamplingEstimator::calculateSampleSize() {
    // TODO SERVER-94063: Calculate the sample size.
    return 500;
}

void SamplingEstimator::generateRandomSample() {
    // TODO SERVER-93728: Build a sampling SBE plan to randomly draw samples.
    return;
}

void SamplingEstimator::generateChunkSample() {
    // TODO SERVER-93729: Implement chunk-based sampling CE approach.
    return;
}

Cardinality SamplingEstimator::estimateCardinality(const MatchExpression* expr) {
    // TODO SERVER-93730: Evaluate MatchExpression against a sample.
    return 10.0;
}

std::vector<Cardinality> SamplingEstimator::estimateCardinality(
    const std::vector<MatchExpression*>& expr) {
    // TODO SERVER-93730: Evaluate MatchExpression against a sample.
    return {10.0};
}

SamplingEstimator::SamplingEstimator(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const MultipleCollectionAccessor& collections,
                                     SamplingStyle samplingStyle)
    : _opCtx(opCtx), _collections(collections) {
    _sampleSize = calculateSampleSize();

    // Create a CanonicalQuery for the sampling plan.
    _cq = makeCanonicalQuery(nss, _opCtx, _sampleSize);
    if (samplingStyle == SamplingStyle::kRandom) {
        generateRandomSample();
    } else {
        generateChunkSample();
    }
}

SamplingEstimator::~SamplingEstimator() {}

}  // namespace optimizer::ce
}  // namespace mongo
