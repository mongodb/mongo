/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/stats/max_diff.h"
#include "mongo/db/query/stats/value_utils.h"
#include "mongo/logv2/log.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using boost::intrusive_ptr;

REGISTER_ACCUMULATOR(
    _internalConstructStats,
    genericParseSBEUnsupportedSingleExpressionAccumulator<AccumulatorInternalConstructStats>);


AccumulatorInternalConstructStats::AccumulatorInternalConstructStats(
    ExpressionContext* const expCtx)
    : AccumulatorState(expCtx), _count(0.0) {
    assertAllowedInternalIfRequired(
        expCtx->opCtx, "_internalConstructStats", AllowedWithClientType::kInternal);
    _memUsageBytes = sizeof(*this);
}

intrusive_ptr<AccumulatorState> AccumulatorInternalConstructStats::create(
    ExpressionContext* const expCtx) {
    return new AccumulatorInternalConstructStats(expCtx);
}

void AccumulatorInternalConstructStats::processInternal(const Value& input, bool merging) {
    uassert(8423375, "Can not merge analyze pipelines", !merging);

    const auto& doc = input.getDocument();
    auto val = doc["val"];

    LOGV2_DEBUG(6735800, 4, "Extracted document", "val"_attr = val);
    _values.emplace_back(stats::SBEValue(mongo::optimizer::convertFrom(val)));

    _count++;
    _memUsageBytes = sizeof(*this);
}

Value AccumulatorInternalConstructStats::getValue(bool toBeMerged) {
    uassert(8423374, "Can not merge analyze pipelines", !toBeMerged);

    // Generate and serialize maxdiff histogram for scalar and array values.
    auto arrayHistogram = stats::createArrayEstimator(_values, stats::ScalarHistogram::kMaxBuckets);
    auto stats = stats::makeStatistics(_count, arrayHistogram);

    return Value(stats);
}

void AccumulatorInternalConstructStats::reset() {
    _count = 0.0;
    _values.clear();
    _memUsageBytes = sizeof(*this);
}

}  // namespace mongo
