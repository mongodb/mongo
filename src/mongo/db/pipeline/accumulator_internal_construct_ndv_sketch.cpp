// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/accumulator_internal_construct_ndv_sketch.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/ce/ndv/ndv_hashing.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

// 2^14 registers: 16KB per sketch, standard error 1.04/sqrt(2^14) ~= 0.81% (Flajolet et al.,
// "HyperLogLog: the analysis of a near-optimal cardinality estimation algorithm"; see
// hyperloglog.h).
constexpr size_t kNdvSketchPrecision = 14;

ce::HyperLogLog makeSketch() {
    return uassertStatusOK(ce::HyperLogLog::create(kNdvSketchPrecision));
}

}  // namespace

AccumulationExpression parseInternalConstructNdvSketch(ExpressionContext* const expCtx,
                                                       BSONElement elem,
                                                       VariablesParseState vps) {
    expCtx->capSbeCompatibility(SbeCompatibility::notCompatible);

    tassert(13175703,
            "expected the argument to $_internalConstructNdvSketch to be an object",
            elem.isABSONObj());
    auto params = InternalConstructNdvSketchAccumulatorParams::parse(
        elem.Obj(), IDLParserContext("$_internalConstructNdvSketch"));
    tassert(13175704,
            "$_internalConstructNdvSketch requires exactly one field; composite NDV statistics "
            "are not supported yet",
            params.getFields().size() == 1);

    auto initializer = ExpressionConstant::create(expCtx, Value(BSONNULL));
    auto argument = Expression::parseOperand(expCtx, elem, vps);
    return {initializer,
            argument,
            [expCtx, params = std::move(params)]() {
                return make_intrusive<AccumulatorInternalConstructNdvSketch>(expCtx, params);
            },
            "_internalConstructNdvSketch"};
}

// Internal-only, enforced at parse time; the constructor check below is defense in depth.
REGISTER_ACCUMULATOR_CONDITIONALLY(_internalConstructNdvSketch,
                                   parseInternalConstructNdvSketch,
                                   AllowedWithApiStrict::kInternal,
                                   AllowedWithClientType::kInternal,
                                   nullptr /* featureFlag */,
                                   true /* condition */);

AccumulatorInternalConstructNdvSketch::AccumulatorInternalConstructNdvSketch(
    ExpressionContext* const expCtx, const InternalConstructNdvSketchAccumulatorParams& params)
    : AccumulatorState(expCtx), _hll(makeSketch()) {
    assertAllowedInternalIfRequired(
        expCtx->getOperationContext(), kName, AllowedWithClientType::kInternal);
    _fieldPaths.reserve(params.getFields().size());
    for (const auto& field : params.getFields()) {
        _fieldPaths.emplace_back(field);
    }
    _setMemoryUsage();
}

void AccumulatorInternalConstructNdvSketch::_setMemoryUsage() {
    // 'getApproximateSize()' counts sizeof(HyperLogLog) again, which 'sizeof(*this)' already
    // includes.
    _memUsageTracker.set(sizeof(*this) - sizeof(_hll) + _hll.getApproximateSize());
}

void AccumulatorInternalConstructNdvSketch::processInternal(const Value& input, bool merging) {
    // uassert, not tassert: split pipelines (e.g. embedded-router variants) can legitimately
    // request merge output for internal-client requests; that must fail the command, not
    // tripwire the server.
    uassert(13175700, "$_internalConstructNdvSketch does not support merging", !merging);

    // 'input' is the evaluated argument {val: <document>, fields: [...]}. The helper yields
    // boost::none for arrays anywhere along the path and missing for absent fields; a
    // non-document 'val' also degrades to missing.
    const Value doc = input[InternalConstructNdvSketchAccumulatorParams::kValFieldName];
    boost::optional<Value> extracted;
    if (doc.getType() == BSONType::object) {
        extracted = doc.getDocument().getNestedScalarFieldNonCaching(_fieldPaths.front());
    } else {
        extracted = Value();
    }
    uassert(13175701,
            "NDV statistics do not support array values",
            !doc.isArray() && extracted.has_value());
    const Value value = *extracted;

    // hashValueForNdv() takes a BSONElement, so materialize the value. A missing value becomes an
    // empty object whose firstElement() is EOO, which the hasher handles.
    BSONObjBuilder bob;
    if (!value.missing()) {
        value.addToBsonObj(&bob, "");
    }
    const BSONObj obj = bob.obj();
    _hll.addHash(ce::hashValueForNdv(obj.firstElement()));
}

Value AccumulatorInternalConstructNdvSketch::getValue(bool toBeMerged) {
    // uassert for the same reason as in processInternal().
    uassert(13175702, "$_internalConstructNdvSketch does not support merging", !toBeMerged);

    const auto registers = _hll.registers();
    // Real estimates are far below the long long range. Branch rather than clamp: casting
    // LLONG_MAX to double rounds up to 2^63, which is out of range for llround().
    constexpr long long kMaxNdv = std::numeric_limits<long long>::max();
    const double estimate = _hll.estimate();
    NdvSketch sketch;
    sketch.setNdv(estimate >= static_cast<double>(kMaxNdv)
                      ? kMaxNdv
                      : static_cast<long long>(std::llround(estimate)));
    sketch.setRegisters(std::vector<std::uint8_t>(registers.begin(), registers.end()));
    sketch.setPrecision(static_cast<int>(_hll.precision()));
    // One sketch document per null/missing folding variant. A single field needs exactly one
    // ($expr semantics); composite NDV emits the other variants as further array elements.
    return Value(std::vector<Value>{Value(Document(sketch.toBSON()))});
}

void AccumulatorInternalConstructNdvSketch::reset() {
    _hll = makeSketch();
    _setMemoryUsage();
}

}  // namespace mongo
