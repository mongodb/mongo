// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/db/query/compiler/stats/max_diff.h"
#include "mongo/db/query/compiler/stats/stats_for_histograms_gen.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <vector>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

AccumulationExpression parseInternalConstructStats(ExpressionContext* const expCtx,
                                                   BSONElement elem,
                                                   VariablesParseState vps) {
    expCtx->capSbeCompatibility(SbeCompatibility::notCompatible);

    IDLParserContext parser("$_internalConstructStats");
    tassert(7261401,
            "expected $_internalConstructStats in the analyze pipeline to an object",
            elem.isABSONObj());
    auto params = InternalConstructStatsAccumulatorParams::parse(elem.Obj(), parser);

    auto initializer = ExpressionConstant::create(expCtx, Value(BSONNULL));
    auto argument = Expression::parseOperand(expCtx, elem, vps);
    return {initializer,
            argument,
            [expCtx, params]() {
                return make_intrusive<AccumulatorInternalConstructStats>(expCtx, params);
            },
            "_internalConstructStats"};
}

REGISTER_ACCUMULATOR(_internalConstructStats, parseInternalConstructStats);

AccumulatorInternalConstructStats::AccumulatorInternalConstructStats(
    ExpressionContext* const expCtx, InternalConstructStatsAccumulatorParams params)
    : AccumulatorState(expCtx), _count(0.0), _params(params) {
    assertAllowedInternalIfRequired(
        expCtx->getOperationContext(), "_internalConstructStats", AllowedWithClientType::kInternal);
    _memUsageTracker.set(sizeof(*this));
}

void AccumulatorInternalConstructStats::processInternal(const Value& input, bool merging) {
    uassert(8423375, "Can not merge analyze pipelines", !merging);

    const auto& doc = input.getDocument();
    // The $project stage in the analyze pipeline outputs a document of the form:
    //      {val: "some value from the collection"}
    // The $_internalConstructStats accumulator looks like:
    //      {$_internalConstructStats: {val: "$$ROOT", sampleRate: 0.5, ...}}
    // Thus the `input` parameter has the form:
    //      {val: {_id: ..., val: "some value from the collection"}, sampleRate: 0.5}
    auto val = doc["val"][InternalConstructStatsAccumulatorParams::kValFieldName];

    LOGV2_DEBUG(6735800, 4, "Extracted document", "val"_attr = redact(val.toString()));
    _values.emplace_back(stats::SBEValue(sbe::value::makeValue(val)));

    _count++;
    _memUsageTracker.set(sizeof(*this));
}

Value AccumulatorInternalConstructStats::getValue(bool toBeMerged) {
    uassert(8423374, "Can not merge analyze pipelines", !toBeMerged);

    // Generate and serialize maxdiff histogram for scalar and array values.
    auto ceHistogram = stats::createCEHistogram(_values, _params.getNumberBuckets());
    auto stats = stats::makeStatistics(_count, _params.getSampleRate(), ceHistogram);

    return Value(stats);
}

void AccumulatorInternalConstructStats::reset() {
    _count = 0.0;
    _values.clear();
    _memUsageTracker.set(sizeof(*this));
}

}  // namespace mongo
