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
#include "mongo/db/query/compiler/ce/ndv/field_stats.h"
#include "mongo/db/query/compiler/ce/ndv/ndv_hashing.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/inlined_vector.h>
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

/**
 * Validates the 'fields' argument. Uasserts if the number of fields is not between 1 and
 * kNdvMaxFields, or if the fields are not sorted and distinct. Called from parsing and
 * re-checked by the constructor, since it is also constructible directly.
 */
void validateFields(const std::vector<std::string_view>& fields) {
    uassert(13176301,
            str::stream() << "$_internalConstructNdvSketch requires between 1 and "
                          << ce::kNdvMaxFields << " fields",
            !fields.empty() && fields.size() <= ce::kNdvMaxFields);
    uassert(13176302,
            "$_internalConstructNdvSketch requires canonically sorted, distinct fields",
            std::is_sorted(fields.begin(), fields.end()) &&
                std::adjacent_find(fields.begin(), fields.end()) == fields.end());
}

}  // namespace

AccumulationExpression parseInternalConstructNdvSketch(ExpressionContext* const expCtx,
                                                       BSONElement elem,
                                                       VariablesParseState vps) {
    expCtx->capSbeCompatibility(SbeCompatibility::notCompatible);

    uassert(13175703,
            "expected the argument to $_internalConstructNdvSketch to be an object",
            elem.isABSONObj());
    auto params = InternalConstructNdvSketchAccumulatorParams::parse(
        elem.Obj(), IDLParserContext("$_internalConstructNdvSketch"));
    validateFields(params.getFields());

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
    : AccumulatorState(expCtx) {
    assertAllowedInternalIfRequired(
        expCtx->getOperationContext(), kName, AllowedWithClientType::kInternal);
    const auto fields = params.getFields();
    validateFields(fields);
    _fieldPaths.reserve(fields.size());
    for (const auto& field : fields) {
        _fieldPaths.emplace_back(field);
    }
    // One folding variant per sketch: a single field needs only the strict one, n >= 2 fields
    // add one folded variant per field (see the class comment).
    const size_t numSketches = _fieldPaths.size() == 1 ? 1 : _fieldPaths.size() + 1;
    _sketches.reserve(numSketches);
    for (size_t i = 0; i < numSketches; ++i) {
        _sketches.push_back(makeSketch());
    }
    _setMemoryUsage();
}

void AccumulatorInternalConstructNdvSketch::_setMemoryUsage() {
    // 'sizeof(*this)' covers the vector headers; the sketches and path strings live on the
    // heap.
    size_t memUsage = sizeof(*this);
    for (const auto& sketch : _sketches) {
        memUsage += sketch.getApproximateSize();
    }
    for (const auto& fieldPath : _fieldPaths) {
        memUsage += fieldPath.fullPath().size();
    }
    _memUsageTracker.set(memUsage);
}

void AccumulatorInternalConstructNdvSketch::processInternal(const Value& input, bool merging) {
    // uassert, not tassert: split pipelines (e.g. embedded-router variants) can legitimately
    // request merge output for internal-client requests; that must fail the command, not
    // tripwire the server.
    uassert(13175700, "$_internalConstructNdvSketch does not support merging", !merging);

    // 'input' is the evaluated argument {val: <document>, fields: [...]}. The helper yields
    // boost::none for arrays anywhere along the path and missing for absent fields; a
    // non-document 'val' also degrades to missing. The hasher takes BSONElements, so each value
    // is materialized into a single-element object; a missing value becomes an empty object
    // whose firstElement() is EOO, which the hasher handles.
    //
    // Note: the Value round trip normalizes some deprecated representations, e.g. it truncates
    // a DBRef namespace at an embedded NUL byte (Value::Value uses dbrefNS()), so two such
    // pathological DBRefs can conflate here even though the hasher itself keeps them apart.
    const Value doc = input[InternalConstructNdvSketchAccumulatorParams::kValFieldName];
    const auto extractField = [&](const FieldPath& fieldPath) {
        boost::optional<Value> extracted;
        if (doc.getType() == BSONType::object) {
            extracted = doc.getDocument().getNestedScalarFieldNonCaching(fieldPath);
        } else {
            extracted = Value();
        }
        uassert(13175701,
                "NDV statistics do not support array values",
                !doc.isArray() && extracted.has_value());
        BSONObjBuilder bob;
        if (!extracted->missing()) {
            extracted->addToBsonObj(&bob, "");
        }
        return bob.obj();
    };

    if (_fieldPaths.size() == 1) {
        // Single-field fast path: no per-document containers, and the one-value hash is an
        // on-disk contract (golden values in ndv_hashing_test.cpp).
        const BSONObj valueObj = extractField(_fieldPaths.front());
        _sketches.front().addHash(ce::hashValueForNdv(valueObj.firstElement()));
        return;
    }

    // This runs for every document of a full collection scan; the inlined capacity (bounded by
    // kNdvMaxFields) keeps it allocation-free.
    absl::InlinedVector<BSONObj, ce::kNdvMaxFields> valueObjs;
    absl::InlinedVector<BSONElement, ce::kNdvMaxFields> elements;
    for (const auto& fieldPath : _fieldPaths) {
        valueObjs.push_back(extractField(fieldPath));
        elements.push_back(valueObjs.back().firstElement());
    }

    // sketches[0]: the strict tuple, all fields under $expr semantics (missing distinct from
    // null, encoded as Undefined by the hasher).
    const uint64_t strictHash = ce::hashValuesForNdv(elements);
    _sketches[0].addHash(strictHash);

    // sketches[i + 1]: field i folded, missing counted as null. Folding only changes the
    // encoding when the value actually is missing; otherwise the strict hash is the same bytes.
    static const BSONObj kNullValueObj = BSON("" << BSONNULL);
    for (size_t i = 0; i < elements.size(); ++i) {
        if (!elements[i].eoo()) {
            _sketches[i + 1].addHash(strictHash);
            continue;
        }
        const BSONElement original = std::exchange(elements[i], kNullValueObj.firstElement());
        _sketches[i + 1].addHash(ce::hashValuesForNdv(elements));
        elements[i] = original;
    }
}

Value AccumulatorInternalConstructNdvSketch::getValue(bool toBeMerged) {
    // uassert for the same reason as in processInternal().
    uassert(13175702, "$_internalConstructNdvSketch does not support merging", !toBeMerged);

    // One sketch document per null/missing folding variant, in the positional convention
    // described in the class comment: the strict sketch first, then one folded variant per
    // (canonically sorted) field for composite statistics.
    std::vector<Value> sketchDocs;
    sketchDocs.reserve(_sketches.size());
    for (const auto& hll : _sketches) {
        const auto registers = hll.registers();
        // Real estimates are far below the long long range. Branch rather than clamp: casting
        // LLONG_MAX to double rounds up to 2^63, which is out of range for llround().
        constexpr long long kMaxNdv = std::numeric_limits<long long>::max();
        const double estimate = hll.estimate();
        NdvSketch sketch;
        sketch.setNdv(estimate >= static_cast<double>(kMaxNdv)
                          ? kMaxNdv
                          : static_cast<long long>(std::llround(estimate)));
        sketch.setRegisters(std::vector<std::uint8_t>(registers.begin(), registers.end()));
        sketch.setPrecision(static_cast<int>(hll.precision()));
        sketchDocs.push_back(Value(Document(sketch.toBSON())));
    }
    return Value(std::move(sketchDocs));
}

void AccumulatorInternalConstructNdvSketch::reset() {
    for (auto& sketch : _sketches) {
        sketch = makeSketch();
    }
    _setMemoryUsage();
}

}  // namespace mongo
