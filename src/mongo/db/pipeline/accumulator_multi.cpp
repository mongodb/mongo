// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/accumulator_multi.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/memory_tracking/memory_usage_limit.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_knob_descriptors_execution.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using FirstLastSense = AccumulatorFirstLastN::Sense;
using MinMaxSense = AccumulatorMinMax::Sense;

// Register macros for the various accumulators/expressions in this file.
REGISTER_ACCUMULATOR(maxN, AccumulatorMinMaxN::parseMinMaxN<MinMaxSense::kMax>);
REGISTER_ACCUMULATOR(minN, AccumulatorMinMaxN::parseMinMaxN<MinMaxSense::kMin>);
REGISTER_STABLE_EXPRESSION(maxN, AccumulatorMinMaxN::parseExpression<MinMaxSense::kMax>);
REGISTER_STABLE_EXPRESSION(minN, AccumulatorMinMaxN::parseExpression<MinMaxSense::kMin>);
REGISTER_ACCUMULATOR(firstN, AccumulatorFirstLastN::parseFirstLastN<FirstLastSense::kFirst>);
REGISTER_ACCUMULATOR(lastN, AccumulatorFirstLastN::parseFirstLastN<FirstLastSense::kLast>);
REGISTER_STABLE_EXPRESSION(firstN, AccumulatorFirstLastN::parseExpression<FirstLastSense::kFirst>);
REGISTER_STABLE_EXPRESSION(lastN, AccumulatorFirstLastN::parseExpression<FirstLastSense::kLast>);
REGISTER_ACCUMULATOR(topN, (AccumulatorTopBottomN<TopBottomSense::kTop, false>::parseTopBottomN));
REGISTER_ACCUMULATOR(bottomN,
                     (AccumulatorTopBottomN<TopBottomSense::kBottom, false>::parseTopBottomN));
REGISTER_ACCUMULATOR(top, (AccumulatorTopBottomN<TopBottomSense::kTop, true>::parseTopBottomN));
REGISTER_ACCUMULATOR(bottom,
                     (AccumulatorTopBottomN<TopBottomSense::kBottom, true>::parseTopBottomN));

namespace {

template <typename AccumulatorState>
Value evaluateAccumulatorN(const ExpressionFromAccumulatorN<AccumulatorState>& expr,
                           const Document& root,
                           Variables* variables,
                           const EvaluationContext& ctx) {
    AccumulatorState accum(expr.getExpressionContext());

    // Evaluate and initialize 'n'.
    accum.startNewGroup(expr.getN()->evaluate(root, variables, ctx));

    // Verify that '_output' produces an array and pass each element to 'process'.
    auto output = expr.getOutput()->evaluate(root, variables, ctx);
    uassert(5788200, "Input must be an array", output.isArray());
    for (const auto& item : output.getArray()) {
        accum.process(item, false);
    }
    return accum.getValue(false);
}

}  // namespace

template <>
Value ExpressionFromAccumulatorN<AccumulatorMinN>::evaluate(const Document& root,
                                                            Variables* variables,
                                                            const EvaluationContext& ctx) const {
    return evaluateAccumulatorN(*this, root, variables, ctx);
}

template <>
Value ExpressionFromAccumulatorN<AccumulatorMaxN>::evaluate(const Document& root,
                                                            Variables* variables,
                                                            const EvaluationContext& ctx) const {
    return evaluateAccumulatorN(*this, root, variables, ctx);
}

template <>
Value ExpressionFromAccumulatorN<AccumulatorFirstN>::evaluate(const Document& root,
                                                              Variables* variables,
                                                              const EvaluationContext& ctx) const {
    return evaluateAccumulatorN(*this, root, variables, ctx);
}

template <>
Value ExpressionFromAccumulatorN<AccumulatorLastN>::evaluate(const Document& root,
                                                             Variables* variables,
                                                             const EvaluationContext& ctx) const {
    return evaluateAccumulatorN(*this, root, variables, ctx);
}

AccumulatorN::AccumulatorN(ExpressionContext* const expCtx)
    : AccumulatorState(expCtx, MemoryUsageLimit{query_knobs::kTopNAccumulatorBytes}) {}

long long AccumulatorN::validateN(const Value& input) {
    // Obtain the value for 'n' and error if it's not a positive integral.
    uassert(5787902,
            str::stream() << "Value for 'n' must be of integral type, but found "
                          << input.toString(),
            input.numeric());
    auto n = input.coerceToLong();
    uassert(5787903,
            str::stream() << "Value for 'n' must be of integral type, but found "
                          << input.toString(),
            n == input.coerceToDouble());
    uassert(5787908, str::stream() << "'n' must be greater than 0, found " << n, n > 0);
    return n;
}
void AccumulatorN::startNewGroup(const Value& input) {
    // TODO SERVER-59327 consider overriding this method in AccumulatorTopBottomN so that
    // sortPattern doesn't need to get passed through the constructor and we can make sure
    // n == 1 for the single variants
    _n = validateN(input);
}

void AccumulatorN::processInternal(const Value& input, bool merging) {
    tassert(5787802, "'n' must be initialized", _n);

    if (merging) {
        assertMergingInputType(input, BSONType::array);
        const auto& array = input.getArray();
        for (auto&& val : array) {
            _processValue(val);
        }
    } else {
        _processValue(input);
    }
}

AccumulatorMinMaxN::AccumulatorMinMaxN(ExpressionContext* const expCtx, MinMaxSense sense)
    : AccumulatorN(expCtx), _sense(sense) {
    _memUsageTracker.set(sizeof(*this));
}

const char* AccumulatorMinMaxN::getOpName() const {
    if (_sense == MinMaxSense::kMin) {
        return AccumulatorMinN::getName();
    } else {
        return AccumulatorMaxN::getName();
    }
}

Document AccumulatorMinMaxN::serialize(boost::intrusive_ptr<Expression> initializer,
                                       boost::intrusive_ptr<Expression> argument,
                                       const query_shape::SerializationOptions& options) const {
    MutableDocument args;
    AccumulatorN::serializeHelper(initializer, argument, options, args);
    return DOC(getOpName() << args.freeze());
}

template <MinMaxSense s>
boost::intrusive_ptr<Expression> AccumulatorMinMaxN::parseExpression(
    ExpressionContext* const expCtx, BSONElement exprElement, const VariablesParseState& vps) {
    auto accExpr = AccumulatorMinMaxN::parseMinMaxN<s>(expCtx, exprElement, vps);
    if constexpr (s == MinMaxSense::kMin) {
        return make_intrusive<ExpressionFromAccumulatorN<AccumulatorMinN>>(
            expCtx, std::move(accExpr.initializer), std::move(accExpr.argument));
    } else {
        return make_intrusive<ExpressionFromAccumulatorN<AccumulatorMaxN>>(
            expCtx, std::move(accExpr.initializer), std::move(accExpr.argument));
    }
}

std::tuple<boost::intrusive_ptr<Expression>, boost::intrusive_ptr<Expression>>
AccumulatorN::parseArgs(ExpressionContext* const expCtx,
                        const BSONObj& args,
                        VariablesParseState vps) {
    boost::intrusive_ptr<Expression> n;
    boost::intrusive_ptr<Expression> input;
    for (auto&& element : args) {
        auto fieldName = element.fieldNameStringData();
        if (fieldName == kFieldNameInput) {
            input = Expression::parseOperand(expCtx, element, vps);
        } else if (fieldName == kFieldNameN) {
            n = Expression::parseOperand(expCtx, element, vps);
        } else {
            uasserted(5787901, str::stream() << "Unknown argument for 'n' operator: " << fieldName);
        }
    }
    uassert(5787906, str::stream() << "Missing value for '" << kFieldNameN << "'", n);
    uassert(5787907, str::stream() << "Missing value for '" << kFieldNameInput << "'", input);
    return std::make_tuple(n, input);
}

void AccumulatorN::serializeHelper(const boost::intrusive_ptr<Expression>& initializer,
                                   const boost::intrusive_ptr<Expression>& argument,
                                   const query_shape::SerializationOptions& options,
                                   MutableDocument& md) {
    md.addField(kFieldNameN, Value(initializer->serialize(options)));
    md.addField(kFieldNameInput, Value(argument->serialize(options)));
}

template <MinMaxSense s>
AccumulationExpression AccumulatorMinMaxN::parseMinMaxN(ExpressionContext* const expCtx,
                                                        BSONElement elem,
                                                        VariablesParseState vps) {
    auto name = [] {
        if constexpr (s == MinMaxSense::kMin) {
            return AccumulatorMinN::getName();
        } else {
            return AccumulatorMaxN::getName();
        }
    }();

    uassert(5787900,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::object);
    BSONObj obj = elem.embeddedObject();

    auto [n, input] = AccumulatorN::parseArgs(expCtx, obj, vps);

    auto factory = [expCtx] {
        if constexpr (s == MinMaxSense::kMin) {
            return make_intrusive<AccumulatorMinN>(expCtx);
        } else {
            return make_intrusive<AccumulatorMaxN>(expCtx);
        }
    };

    return {std::move(n), std::move(input), std::move(factory), name};
}

void AccumulatorMinMaxN::_processValue(const Value& val) {
    if (val.nullish()) {
        return;
    }
    // For kMin we maintain a max-heap (largest value at front) so we can quickly evict the worst
    // kept element. For kMax we maintain a min-heap (smallest value at front) for the same reason.
    auto heapComp = [&](const std::pair<Value, int64_t>& a, const std::pair<Value, int64_t>& b) {
        return _valueComp.compare(a.first, b.first) * _sense < 0;
    };

    auto sz = static_cast<int64_t>(val.getApproximateSize());

    if (static_cast<long long>(_heap.size()) < *_n) {
        _heap.push_back({val, sz});
        std::push_heap(_heap.begin(), _heap.end(), heapComp);
        _memUsageTracker.add(sz);
        checkMemUsage();
    } else if (_valueComp.compare(_heap.front().first, val) * _sense > 0) {
        // The heap root is strictly worse than val (larger for kMin, smaller for kMax), so evict
        // it and insert val. Adjust memory before modifying the heap so the tracker stays
        // consistent if checkMemUsage throws.
        std::pop_heap(_heap.begin(), _heap.end(), heapComp);
        _memUsageTracker.add(sz - _heap.back().second);
        _heap.back() = {val, sz};
        std::push_heap(_heap.begin(), _heap.end(), heapComp);
        checkMemUsage();
    }
}

Value AccumulatorMinMaxN::getValue(bool toBeMerged) {
    std::vector<Value> vals;
    vals.reserve(_heap.size());
    for (auto& entry : _heap) {
        vals.push_back(entry.first);
    }
    // vals is a valid heap by Value compare: the heap property is preserved.
    std::sort_heap(vals.begin(), vals.end(), [&](const Value& a, const Value& b) {
        return _valueComp.compare(a, b) * _sense < 0;
    });
    return Value{std::move(vals)};
}

void AccumulatorMinMaxN::reset() {
    for (auto& entry : _heap) {
        _memUsageTracker.add(-entry.second);
    }
    _heap.clear();
    // Reserve space is not tracked by _memUsageTracker, so get rid of it.
    _heap.shrink_to_fit();
}

const char* AccumulatorMinN::getName() {
    return kName.data();
}

const char* AccumulatorMaxN::getName() {
    return kName.data();
}

AccumulatorFirstLastN::AccumulatorFirstLastN(ExpressionContext* const expCtx, FirstLastSense sense)
    : AccumulatorN(expCtx), _variant(sense) {
    _memUsageTracker.set(sizeof(*this));
}

// TODO SERVER-59327 Deduplicate with the block in 'AccumulatorMinMaxN::parseMinMaxN'
template <FirstLastSense v>
AccumulationExpression AccumulatorFirstLastN::parseFirstLastN(ExpressionContext* const expCtx,
                                                              BSONElement elem,
                                                              VariablesParseState vps) {
    auto name = [] {
        if constexpr (v == Sense::kFirst) {
            return AccumulatorFirstN::getName();
        } else {
            return AccumulatorLastN::getName();
        }
    }();

    uassert(5787801,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::object);
    auto obj = elem.embeddedObject();

    auto [n, input] = AccumulatorN::parseArgs(expCtx, obj, vps);

    auto factory = [expCtx] {
        if constexpr (v == Sense::kFirst) {
            return make_intrusive<AccumulatorFirstN>(expCtx);
        } else {
            return make_intrusive<AccumulatorLastN>(expCtx);
        }
    };

    return {std::move(n), std::move(input), std::move(factory), name};
}

void AccumulatorFirstLastN::_processValue(const Value& val) {
    auto valToProcess = val.missing() ? Value(BSONNULL) : val;

    if (_ringCount == static_cast<size_t>(*_n)) {
        if (_variant == Sense::kFirst) {
            // Once _ring contains 'n' elements and this is $firstN, we don't need to call process
            // anymore.
            _needsInput = false;
            return;
        }
        // $lastN full: evict the oldest slot. The stored size lets us update the tracker without
        // recomputing getApproximateSize() on the outgoing value.
        const int64_t newSize = valToProcess.getApproximateSize();
        _memUsageTracker.add(newSize - _ring[_ringHead].second);
        _ring[_ringHead] = {std::move(valToProcess), newSize};
        _ringHead = (_ringHead + 1) % _ring.size();
    } else {
        const int64_t newSize = valToProcess.getApproximateSize();
        _memUsageTracker.add(newSize);
        _ring.push_back({std::move(valToProcess), newSize});
        ++_ringCount;
    }

    checkMemUsage();
}

const char* AccumulatorFirstLastN::getOpName() const {
    if (_variant == Sense::kFirst) {
        return AccumulatorFirstN::getName();
    } else {
        return AccumulatorLastN::getName();
    }
}

Document AccumulatorFirstLastN::serialize(boost::intrusive_ptr<Expression> initializer,
                                          boost::intrusive_ptr<Expression> argument,
                                          const query_shape::SerializationOptions& options) const {
    MutableDocument args;
    AccumulatorN::serializeHelper(initializer, argument, options, args);
    return DOC(getOpName() << args.freeze());
}

template <FirstLastSense s>
boost::intrusive_ptr<Expression> AccumulatorFirstLastN::parseExpression(
    ExpressionContext* expCtx, BSONElement exprElement, const VariablesParseState& vps) {
    auto accExpr = AccumulatorFirstLastN::parseFirstLastN<s>(expCtx, exprElement, vps);
    if constexpr (s == FirstLastSense::kFirst) {
        return make_intrusive<ExpressionFromAccumulatorN<AccumulatorFirstN>>(
            expCtx, std::move(accExpr.initializer), std::move(accExpr.argument));
    } else {
        return make_intrusive<ExpressionFromAccumulatorN<AccumulatorLastN>>(
            expCtx, std::move(accExpr.initializer), std::move(accExpr.argument));
    }
}

void AccumulatorFirstLastN::reset() {
    _ring.clear();
    // memoryUsageTracker doesn't track reserved space so get rid of it.
    _ring.shrink_to_fit();
    _memUsageTracker.set(sizeof(*this));
    _ringHead = 0;
    _ringCount = 0;
}

Value AccumulatorFirstLastN::getValue(bool toBeMerged) {
    std::vector<Value> result;
    result.reserve(_ringCount);
    for (size_t i = 0; i < _ringCount; ++i) {
        result.emplace_back(_ring[(_ringHead + i) % _ring.size()].first);
    }
    return Value{std::move(result)};
}

const char* AccumulatorFirstN::getName() {
    return kName.data();
}

const char* AccumulatorLastN::getName() {
    return kName.data();
}

// TODO SERVER-59327 Refactor other operators to use this parse function.
template <bool single>
std::tuple<boost::intrusive_ptr<Expression>, BSONElement, boost::optional<BSONObj>>
accumulatorNParseArgs(ExpressionContext* expCtx,
                      const BSONElement& elem,
                      std::string_view name,
                      bool needSortBy,
                      const VariablesParseState& vps) {
    uassert(5788001,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::object);
    BSONObj obj = elem.embeddedObject();

    // Extract fields from specification object. sortBy and output are not immediately parsed into
    // Expressions so that they can easily still be manipulated and processed in the special case of
    // AccumulatorTopBottomN.
    boost::optional<BSONObj> sortBy;
    boost::optional<BSONElement> output;
    boost::intrusive_ptr<Expression> n;
    for (auto&& element : obj) {
        auto fieldName = element.fieldNameStringData();
        if constexpr (!single) {
            if (fieldName == AccumulatorN::kFieldNameN) {
                n = Expression::parseOperand(expCtx, element, vps);
                continue;
            }
        }
        if (fieldName == AccumulatorN::kFieldNameOutput) {
            output = element;
        } else if (fieldName == AccumulatorN::kFieldNameSortBy && needSortBy) {
            sortBy = element.Obj();
        } else {
            uasserted(5788002,
                      str::stream() << "Unknown argument to " << name << " '" << fieldName << "'");
        }
    }

    // Make sure needed arguments were found.
    if constexpr (single) {
        n = ExpressionConstant::create(expCtx, Value(1));
    } else {
        uassert(
            5788003, str::stream() << "Missing value for '" << AccumulatorN::kFieldNameN << "'", n);
    }
    uassert(5788004,
            str::stream() << "Missing value for '" << AccumulatorN::kFieldNameOutput << "'",
            output);
    if (needSortBy) {
        uassert(5788005,
                str::stream() << "Missing value for '" << AccumulatorN::kFieldNameSortBy << "'",
                sortBy);
        uassert(9657900,
                str::stream() << "Value for '" << AccumulatorN::kFieldNameSortBy
                              << "' must be a non-empty object",
                !sortBy->isEmpty());
    }

    return {n, *output, sortBy};
}

template <TopBottomSense sense, bool single>
AccumulatorTopBottomN<sense, single>::AccumulatorTopBottomN(ExpressionContext* const expCtx,
                                                            BSONObj sortBy,
                                                            bool isRemovable)
    : AccumulatorTopBottomN(expCtx,
                            std::get<0>(parseAccumulatorTopBottomNSortBy<sense>(expCtx, sortBy)),
                            isRemovable) {}

template <TopBottomSense sense, bool single>
AccumulatorTopBottomN<sense, single>::AccumulatorTopBottomN(ExpressionContext* const expCtx,
                                                            SortPattern sp,
                                                            bool isRemovable)
    : AccumulatorN(expCtx), _isRemovable(isRemovable), _sortPattern(std::move(sp)) {

    // Make a copy of _sortPattern to sort based on fields where they are in the evaluated argument
    // instead of where they would be in the raw document received by $group and friends.
    std::vector<SortPattern::SortPatternPart> parts;
    parts.reserve(_sortPattern.size());
    int sortOrder = 0;
    for (auto part : _sortPattern) {
        const auto newFieldName =
            (StringBuilder() << AccumulatorN::kFieldNameSortFields << sortOrder).str();
        part.fieldPath.reset(FieldPath(newFieldName));

        if (part.expression) {
            // $meta based sorting is handled earlier in the sortFields expression. See comment in
            // parseAccumulatorTopBottomNSortBy().
            part.expression = nullptr;
        }
        parts.push_back(std::move(part));
        sortOrder++;
    }
    SortPattern internalSortPattern(std::move(parts));

    _sortKeyComparator.emplace(internalSortPattern);
    _sortKeyGenerator.emplace(std::move(internalSortPattern), expCtx->getCollator());

    _memUsageTracker.set(sizeof(*this));

    // STL expects a less-than function not a 3-way compare function so this lambda wraps
    // SortKeyComparator.
    _map.emplace([&, this](const Value& lhs, const Value& rhs) {
        return (*this->_sortKeyComparator)(lhs, rhs) < 0;
    });
}

template <TopBottomSense sense, bool single>
const char* AccumulatorTopBottomN<sense, single>::getOpName() const {
    return AccumulatorTopBottomN<sense, single>::getName().data();
}

template <TopBottomSense sense, bool single>
Document AccumulatorTopBottomN<sense, single>::serialize(
    boost::intrusive_ptr<Expression> initializer,
    boost::intrusive_ptr<Expression> argument,
    const query_shape::SerializationOptions& options) const {
    MutableDocument args;

    if constexpr (!single) {
        args.addField(kFieldNameN, Value(initializer->serialize(options)));
    }

    // If 'argument' is either an ExpressionObject or an ExpressionConstant of object type, then
    // we are serializing the original expression under the 'output' field of the object. Otherwise,
    // we're serializing a custom expression for merging group.
    if (auto argObj = dynamic_cast<ExpressionObject*>(argument.get())) {
        bool foundOutputField = false;
        for (auto& child : argObj->getChildExpressions()) {
            if (child.first == kFieldNameOutput) {
                auto output = child.second->serialize(options);
                args.addField(kFieldNameOutput, output);
                foundOutputField = true;
                break;
            }
        }
        tassert(7773700, "'output' field should be present.", foundOutputField);
    } else if (auto argConst = dynamic_cast<ExpressionConstant*>(argument.get())) {
        auto output = argConst->getValue().getDocument()[kFieldNameOutput];
        tassert(7773701, "'output' field should be present.", !output.missing());
        args.addField(kFieldNameOutput, output);
    } else {
        auto serializedArg = argument->serialize(options);
        args.addField(kFieldNameOutput, serializedArg);
    }

    args.addField(kFieldNameSortBy,
                  Value(_sortPattern.serialize(
                      SortPattern::SortKeySerialization::kForPipelineSerialization, options)));
    return DOC(getOpName() << args.freeze());
}

template <TopBottomSense sense>
std::tuple<SortPattern, BSONArray, bool> parseAccumulatorTopBottomNSortBy(
    ExpressionContext* const expCtx, BSONObj sortBy) {

    SortPattern sortPattern(sortBy, expCtx);
    BSONArrayBuilder sortFieldsExpBab;
    BSONObjIterator sortByBoi(sortBy);
    bool hasMeta = false;
    for (const auto& part : sortPattern) {
        const auto fieldName = sortByBoi.next().fieldNameStringData();
        if (part.expression) {
            // In a scenario where we are sorting by metadata (for example if sortBy is
            // {text: {$meta: "textScore"}}) we cant use ["$text"] as the sortFields expression
            // since the evaluated argument wouldn't have the same metadata as the original
            // document. Instead we use [{$meta: "textScore"}] as the sortFields expression so the
            // sortFields array contains the data we need for sorting.
            const auto serialized = part.expression->serialize();
            sortFieldsExpBab.append(serialized.getDocument().toBson());
            hasMeta = true;
        } else {
            sortFieldsExpBab.append((StringBuilder() << "$" << fieldName).str());
        }
    }
    return {sortPattern, sortFieldsExpBab.arr(), hasMeta};
}

template <TopBottomSense sense, bool single>
AccumulationExpression AccumulatorTopBottomN<sense, single>::parseTopBottomN(
    ExpressionContext* const expCtx, BSONElement elem, VariablesParseState vps) {
    auto name = AccumulatorTopBottomN<sense, single>::getName();
    const auto [n, output, sortBy] = accumulatorNParseArgs<single>(expCtx, elem, name, true, vps);
    auto [sortPattern, sortFieldsExp, hasMeta] =
        parseAccumulatorTopBottomNSortBy<sense>(expCtx, *sortBy);

    if (hasMeta) {
        expCtx->setSbeGroupCompatibility(SbeCompatibility::notCompatible);
    }

    // Construct argument expression. If given sortBy: {field1: 1, field2: 1} it will be shaped like
    // {output: <output expression>, sortFields0: "$field1", sortFields1: "$field2"}. This projects
    // out only the fields we need for sorting so we can use SortKeyComparator without copying the
    // entire document. This argument expression will be evaluated and become the input to
    // _processValue.
    BSONObjBuilder argumentBuilder;
    argumentBuilder.append(output);
    int sortOrder = 0;
    for (const auto& sortField : sortFieldsExp) {
        argumentBuilder.appendAs(
            sortField, (StringBuilder() << AccumulatorN::kFieldNameSortFields << sortOrder).str());
        sortOrder++;
    }
    boost::intrusive_ptr<Expression> argument =
        Expression::parseObject(expCtx, argumentBuilder.obj(), vps);
    auto factory = [expCtx, sortPattern = std::move(sortPattern)] {
        return make_intrusive<AccumulatorTopBottomN<sense, single>>(
            expCtx, sortPattern, /* isRemovable */ false);
    };

    return {std::move(n), std::move(argument), std::move(factory), name};
}

template <TopBottomSense sense, bool single>
std::pair<Value, Value> AccumulatorTopBottomN<sense, single>::_genKeyOutPair(const Value& val) {
    tassert(5788014,
            str::stream() << getName()
                          << " tried to get a sort key on something that wasn't a BSON object",
            val.isObject());

    Value output = val[kFieldNameOutput];

    // Upconvert to 'null' if the output field is missing.
    if (output.missing())
        output = Value(BSONNULL);

    Value sortKey;

    // In the case that _processValue() is getting called in the context of merging, a previous
    // _processValue has already generated the sortKey for us, so we don't need to regenerate it.
    Value generatedSortKey = val[kFieldNameGeneratedSortKey];
    if (!generatedSortKey.missing()) {
        sortKey = generatedSortKey;
    } else {
        sortKey = _sortKeyGenerator->computeSortKeyFromDocument(val.getDocument());
    }
    return {sortKey, output};
}

template <TopBottomSense sense, bool single>
void AccumulatorTopBottomN<sense, single>::_processValue(const Value& val) {
    auto keyOutPair = _genKeyOutPair(val);

    // Only compare if we have 'n' elements.
    if (static_cast<long long>(_map->size()) == *_n && !_isRemovable) {
        // Get an iterator to the element we want to compare against. In particular, $top will
        // insert items less than the max, and $bottom will insert greater than the min.
        auto [cmpElem, cmp] = [&]() {
            if constexpr (sense == TopBottomSense::kTop) {
                auto elem = std::prev(_map->end());
                auto res = (*_sortKeyComparator)(elem->first, keyOutPair.first);
                return std::make_pair(elem, res);
            } else {
                auto elem = _map->begin();
                auto res = (*_sortKeyComparator)(keyOutPair.first, elem->first);
                return std::make_pair(elem, res);
            }
        }();

        // When the sort key produces a tie we keep the first value seen.
        if (cmp > 0) {
            _map->erase(cmpElem);
        } else {
            return;
        }
    }

    const auto memUsage = keyOutPair.first.getApproximateSize() +
        keyOutPair.second.getApproximateSize() + sizeof(KeyOutPair);
    _map->emplace(keyOutPair.first,
                  SimpleMemoryUsageTokenWith<Value>{
                      SimpleMemoryUsageToken{memUsage, &_memUsageTracker}, keyOutPair.second});
    checkMemUsage();
}

template <TopBottomSense sense, bool single>
void AccumulatorTopBottomN<sense, single>::remove(const Value& val) {
    tassert(5788605,
            str::stream() << "Tried to remove() from a non-removable " << getName(),
            _isRemovable);
    tassert(5788600, str::stream() << "Can't remove from an empty " << getName(), !_map->empty());
    auto keyOutPair = _genKeyOutPair(val);

    // std::multimap::insert is guaranteed to put the element after any equal elements
    // already in the container. So lower_bound() / erase() will remove the oldest equal element,
    // which is what we want, to satisfy "remove() undoes add() when called in FIFO order".
    auto it = _map->lower_bound(keyOutPair.first);
    _map->erase(it);
}

template <TopBottomSense sense, bool single>
void AccumulatorTopBottomN<sense, single>::processInternal(const Value& input, bool merging) {
    if (merging) {
        if (input.isArray()) {
            // In the simplest case, we are merging arrays. This happens when we are merging
            // results that were spilled to disk or on mongos.
            for (auto&& val : input.getArray()) {
                _processValue(val);
            }
        } else if (input.isObject()) {
            // In the more complicated case, we are merging objects of the form {output: <output
            // array>, sortFields: <...>}, where <output array> contains already generated <output
            // value, sort pattern part array> pairs. This happens when we have to merge on a
            // shard because we may need to spill to disk.
            auto doc = input.getDocument();
            auto vals = doc[kFieldNameOutput];
            assertMergingInputType(vals, BSONType::array);
            for (auto&& val : vals.getArray()) {
                _processValue(val);
            }
        } else {
            uasserted(ErrorCodes::TypeMismatch,
                      "argument to top/bottom processInternal must be an array or an object when "
                      "merging");
        }
    } else {
        _processValue(input);
    }
}

template <TopBottomSense sense, bool single>
Value AccumulatorTopBottomN<sense, single>::getValueConst(bool toBeMerged) const {
    std::vector<Value> result;
    auto begin = _map->begin();
    auto end = _map->end();
    if constexpr (sense == TopBottomSense::kBottom) {
        // If this accumulator is removable there may be more than n elements in the map, so we must
        // skip elements that shouldn't be in the result.
        if (static_cast<long long>(_map->size()) > *_n) {
            std::advance(begin, _map->size() - *_n);
        }
    }

    // Insert at most _n values into result.
    auto it = begin;
    for (auto inserted = 0; inserted < *_n && it != end; ++inserted, ++it) {
        const auto& keyOutPair = *it;
        if (toBeMerged) {
            result.emplace_back(BSON(kFieldNameGeneratedSortKey << keyOutPair.first
                                                                << kFieldNameOutput
                                                                << keyOutPair.second.value()));
        } else {
            result.push_back(keyOutPair.second.value());
        }
    };

    if constexpr (!single) {
        return Value(std::move(result));
    } else {
        if (toBeMerged) {
            return Value(std::move(result));
        } else {
            if (result.empty()) {
                // This only occurs in a window function scenario, an accumulator will always have
                // at least one value processed.
                return Value(BSONNULL);
            }
            return Value(std::move(result[0]));
        }
    }
}

template <TopBottomSense sense, bool single>
void AccumulatorTopBottomN<sense, single>::reset() {
    _map->clear();
}

// Explicitly specify the following classes should generated and should live in this compilation
// unit.
template class AccumulatorTopBottomN<TopBottomSense::kBottom, false>;
template class AccumulatorTopBottomN<TopBottomSense::kBottom, true>;
template class AccumulatorTopBottomN<TopBottomSense::kTop, false>;
template class AccumulatorTopBottomN<TopBottomSense::kTop, true>;

}  // namespace mongo
