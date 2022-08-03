/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/util/version/releases.h"

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

AccumulatorN::AccumulatorN(ExpressionContext* const expCtx)
    : AccumulatorState(expCtx), _maxMemUsageBytes(internalQueryTopNAccumulatorBytes.load()) {}

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
        tassert(5787803, "input must be an array when 'merging' is true", input.isArray());
        auto array = input.getArray();
        for (auto&& val : array) {
            _processValue(val);
        }
    } else {
        _processValue(input);
    }
}

AccumulatorMinMaxN::AccumulatorMinMaxN(ExpressionContext* const expCtx, MinMaxSense sense)
    : AccumulatorN(expCtx),
      _set(expCtx->getValueComparator().makeOrderedValueMultiset()),
      _sense(sense) {
    _memUsageBytes = sizeof(*this);
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
                                       bool explain) const {
    MutableDocument args;
    AccumulatorN::serializeHelper(initializer, argument, explain, args);
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

void AccumulatorN::updateAndCheckMemUsage(size_t memAdded) {
    _memUsageBytes += memAdded;
    uassert(ErrorCodes::ExceededMemoryLimit,
            str::stream() << getOpName()
                          << " used too much memory and spilling to disk cannot reduce memory "
                             "consumption any further. Memory limit: "
                          << _maxMemUsageBytes << " bytes",
            _memUsageBytes < _maxMemUsageBytes);
}

void AccumulatorN::serializeHelper(const boost::intrusive_ptr<Expression>& initializer,
                                   const boost::intrusive_ptr<Expression>& argument,
                                   bool explain,
                                   MutableDocument& md) {
    md.addField(kFieldNameN, Value(initializer->serialize(explain)));
    md.addField(kFieldNameInput, Value(argument->serialize(explain)));
}

template <MinMaxSense s>
AccumulationExpression AccumulatorMinMaxN::parseMinMaxN(ExpressionContext* const expCtx,
                                                        BSONElement elem,
                                                        VariablesParseState vps) {
    expCtx->sbeGroupCompatible = false;
    auto name = [] {
        if constexpr (s == MinMaxSense::kMin) {
            return AccumulatorMinN::getName();
        } else {
            return AccumulatorMaxN::getName();
        }
    }();

    uassert(5787900,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::Object);
    BSONObj obj = elem.embeddedObject();

    auto [n, input] = AccumulatorN::parseArgs(expCtx, obj, vps);

    auto factory = [expCtx] {
        if constexpr (s == MinMaxSense::kMin) {
            return AccumulatorMinN::create(expCtx);
        } else {
            return AccumulatorMaxN::create(expCtx);
        }
    };

    return {std::move(n), std::move(input), std::move(factory), name};
}

void AccumulatorMinMaxN::_processValue(const Value& val) {
    // Ignore nullish values.
    if (val.nullish())
        return;

    // Only compare if we have 'n' elements.
    if (static_cast<long long>(_set.size()) == *_n) {
        // Get an iterator to the element we want to compare against.
        auto cmpElem = _sense == MinMaxSense::kMin ? std::prev(_set.end()) : _set.begin();

        auto cmp = getExpressionContext()->getValueComparator().compare(*cmpElem, val) * _sense;
        if (cmp > 0) {
            _memUsageBytes -= cmpElem->getApproximateSize();
            _set.erase(cmpElem);
        } else {
            return;
        }
    }

    updateAndCheckMemUsage(val.getApproximateSize());
    _set.emplace(val);
}

Value AccumulatorMinMaxN::getValue(bool toBeMerged) {
    // Return the values in ascending order for 'kMin' and descending order for 'kMax'.
    return Value(_sense == MinMaxSense::kMin ? std::vector<Value>(_set.begin(), _set.end())
                                             : std::vector<Value>(_set.rbegin(), _set.rend()));
}

void AccumulatorMinMaxN::reset() {
    _set = getExpressionContext()->getValueComparator().makeOrderedValueMultiset();
    _memUsageBytes = sizeof(*this);
}

const char* AccumulatorMinN::getName() {
    return kName.rawData();
}

boost::intrusive_ptr<AccumulatorState> AccumulatorMinN::create(ExpressionContext* const expCtx) {
    return make_intrusive<AccumulatorMinN>(expCtx);
}

const char* AccumulatorMaxN::getName() {
    return kName.rawData();
}

boost::intrusive_ptr<AccumulatorState> AccumulatorMaxN::create(ExpressionContext* const expCtx) {
    return make_intrusive<AccumulatorMaxN>(expCtx);
}

AccumulatorFirstLastN::AccumulatorFirstLastN(ExpressionContext* const expCtx, FirstLastSense sense)
    : AccumulatorN(expCtx), _deque(std::deque<Value>()), _variant(sense) {
    _memUsageBytes = sizeof(*this);
}

// TODO SERVER-59327 Deduplicate with the block in 'AccumulatorMinMaxN::parseMinMaxN'
template <FirstLastSense v>
AccumulationExpression AccumulatorFirstLastN::parseFirstLastN(ExpressionContext* const expCtx,
                                                              BSONElement elem,
                                                              VariablesParseState vps) {
    expCtx->sbeGroupCompatible = false;
    auto name = [] {
        if constexpr (v == Sense::kFirst) {
            return AccumulatorFirstN::getName();
        } else {
            return AccumulatorLastN::getName();
        }
    }();

    uassert(5787801,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::Object);
    auto obj = elem.embeddedObject();

    auto [n, input] = AccumulatorN::parseArgs(expCtx, obj, vps);

    auto factory = [expCtx] {
        if constexpr (v == Sense::kFirst) {
            return AccumulatorFirstN::create(expCtx);
        } else {
            return AccumulatorLastN::create(expCtx);
        }
    };

    return {std::move(n), std::move(input), std::move(factory), name};
}

void AccumulatorFirstLastN::_processValue(const Value& val) {
    // Convert missing values to null.
    auto valToProcess = val.missing() ? Value(BSONNULL) : val;

    // Only insert in the lastN case if we have 'n' elements.
    if (static_cast<long long>(_deque.size()) == *_n) {
        if (_variant == Sense::kLast) {
            _memUsageBytes -= _deque.front().getApproximateSize();
            _deque.pop_front();
        } else {
            // If our deque has 'n' elements and this is $firstN, we don't need to call process
            // anymore.
            _needsInput = false;
            return;
        }
    }

    updateAndCheckMemUsage(valToProcess.getApproximateSize());
    _deque.push_back(valToProcess);
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
                                          bool explain) const {
    MutableDocument args;
    AccumulatorN::serializeHelper(initializer, argument, explain, args);
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
    _deque = std::deque<Value>();
    _memUsageBytes = sizeof(*this);
}

Value AccumulatorFirstLastN::getValue(bool toBeMerged) {
    return Value(std::vector<Value>(_deque.begin(), _deque.end()));
}

const char* AccumulatorFirstN::getName() {
    return kName.rawData();
}

boost::intrusive_ptr<AccumulatorState> AccumulatorFirstN::create(ExpressionContext* const expCtx) {
    return make_intrusive<AccumulatorFirstN>(expCtx);
}

const char* AccumulatorLastN::getName() {
    return kName.rawData();
}

boost::intrusive_ptr<AccumulatorState> AccumulatorLastN::create(ExpressionContext* const expCtx) {
    return make_intrusive<AccumulatorLastN>(expCtx);
}

// TODO SERVER-59327 Refactor other operators to use this parse function.
template <bool single>
std::tuple<boost::intrusive_ptr<Expression>, BSONElement, boost::optional<BSONObj>>
accumulatorNParseArgs(ExpressionContext* expCtx,
                      const BSONElement& elem,
                      const char* name,
                      bool needSortBy,
                      const VariablesParseState& vps) {
    uassert(5788001,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::Object);
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
    }

    return {n, *output, sortBy};
}

template <TopBottomSense sense, bool single>
AccumulatorTopBottomN<sense, single>::AccumulatorTopBottomN(ExpressionContext* const expCtx,
                                                            SortPattern sp,
                                                            bool isRemovable)
    : AccumulatorN(expCtx), _isRemovable(isRemovable), _sortPattern(std::move(sp)) {

    // Modify sortPattern to sort based on fields where they are in the evaluated argument instead
    // of where they would be in the raw document received by $group and friends.
    std::vector<SortPattern::SortPatternPart> parts;
    int sortOrder = 0;
    for (auto part : _sortPattern) {
        const auto newFieldName =
            (StringBuilder() << AccumulatorN::kFieldNameSortFields << "." << sortOrder).str();
        part.fieldPath.reset(FieldPath(newFieldName));

        if (part.expression) {
            // $meta based sorting is handled earlier in the sortFields expression. See comment in
            // parseAccumulatorTopBottomNSortBy().
            part.expression = nullptr;
        }
        parts.push_back(part);
        sortOrder++;
    }
    SortPattern internalSortPattern(parts);

    _sortKeyComparator.emplace(internalSortPattern);
    _sortKeyGenerator.emplace(std::move(internalSortPattern), expCtx->getCollator());

    _memUsageBytes = sizeof(*this);

    // STL expects a less-than function not a 3-way compare function so this lambda wraps
    // SortKeyComparator.
    _map.emplace([&, this](const Value& lhs, const Value& rhs) {
        return (*this->_sortKeyComparator)(lhs, rhs) < 0;
    });
}

template <TopBottomSense sense, bool single>
const char* AccumulatorTopBottomN<sense, single>::getOpName() const {
    return AccumulatorTopBottomN<sense, single>::getName().rawData();
}

template <TopBottomSense sense, bool single>
Document AccumulatorTopBottomN<sense, single>::serialize(
    boost::intrusive_ptr<Expression> initializer,
    boost::intrusive_ptr<Expression> argument,
    bool explain) const {
    MutableDocument args;

    if constexpr (!single) {
        args.addField(kFieldNameN, Value(initializer->serialize(explain)));
    }
    auto serializedArg = argument->serialize(explain);

    // If 'argument' contains a field named 'output', this means that we are serializing the
    // accumulator's original output expression under the field name 'output'. Otherwise, we are
    // serializing a custom argument under the field name 'output'. For instance, a merging $group
    // will provide an argument that merges multiple partial groups.
    if (auto output = serializedArg[kFieldNameOutput]; !output.missing()) {
        args.addField(kFieldNameOutput, Value(output));
    } else {
        args.addField(kFieldNameOutput, serializedArg);
    }
    args.addField(kFieldNameSortBy,
                  Value(_sortPattern.serialize(
                      SortPattern::SortKeySerialization::kForPipelineSerialization)));
    return DOC(getOpName() << args.freeze());
}

template <TopBottomSense sense>
std::pair<SortPattern, BSONArray> parseAccumulatorTopBottomNSortBy(ExpressionContext* const expCtx,
                                                                   BSONObj sortBy) {
    SortPattern sortPattern(sortBy, expCtx);
    BSONArrayBuilder sortFieldsExpBab;
    BSONObjIterator sortByBoi(sortBy);
    for (const auto& part : sortPattern) {
        const auto fieldName = sortByBoi.next().fieldNameStringData();
        if (part.expression) {
            // In a scenario where we are sorting by metadata (for example if sortBy is
            // {text: {$meta: "textScore"}}) we cant use ["$text"] as the sortFields expression
            // since the evaluated argument wouldn't have the same metadata as the original
            // document. Instead we use [{$meta: "textScore"}] as the sortFields expression so the
            // sortFields array contains the data we need for sorting.
            const auto serialized = part.expression->serialize(false);
            sortFieldsExpBab.append(serialized.getDocument().toBson());
        } else {
            sortFieldsExpBab.append((StringBuilder() << "$" << fieldName).str());
        }
    }
    return {sortPattern, sortFieldsExpBab.arr()};
}

template <TopBottomSense sense, bool single>
AccumulationExpression AccumulatorTopBottomN<sense, single>::parseTopBottomN(
    ExpressionContext* const expCtx, BSONElement elem, VariablesParseState vps) {
    auto name = AccumulatorTopBottomN<sense, single>::getName();

    const auto [n, output, sortBy] =
        accumulatorNParseArgs<single>(expCtx, elem, name.rawData(), true, vps);

    auto [sortPattern, sortFieldsExp] = parseAccumulatorTopBottomNSortBy<sense>(expCtx, *sortBy);

    // Construct argument expression. If given sortBy: {field1: 1, field2: 1} it will be shaped like
    // {output: <output expression>, sortFields: ["$field1", "$field2"]}. This projects out only the
    // fields we need for sorting so we can use SortKeyComparator without copying the entire
    // document. This argument expression will be evaluated and become the input to _processValue.
    boost::intrusive_ptr<Expression> argument = Expression::parseObject(
        expCtx, BSON(output << AccumulatorN::kFieldNameSortFields << sortFieldsExp), vps);
    auto factory = [expCtx, sortPattern = std::move(sortPattern)] {
        return make_intrusive<AccumulatorTopBottomN<sense, single>>(
            expCtx, sortPattern, /* isRemovable */ false);
    };

    return {std::move(n), std::move(argument), std::move(factory), name};
}

template <TopBottomSense sense, bool single>
boost::intrusive_ptr<AccumulatorState> AccumulatorTopBottomN<sense, single>::create(
    ExpressionContext* expCtx, BSONObj sortBy, bool isRemovable) {
    return make_intrusive<AccumulatorTopBottomN<sense, single>>(
        expCtx, parseAccumulatorTopBottomNSortBy<sense>(expCtx, sortBy).first, isRemovable);
}

template <TopBottomSense sense, bool single>
boost::intrusive_ptr<AccumulatorState> AccumulatorTopBottomN<sense, single>::create(
    ExpressionContext* expCtx, SortPattern sortPattern) {
    return make_intrusive<AccumulatorTopBottomN<sense, single>>(
        expCtx, sortPattern, /* isRemovable */ false);
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
            _memUsageBytes -= cmpElem->first.getApproximateSize() +
                cmpElem->second.getApproximateSize() + sizeof(KeyOutPair);
            _map->erase(cmpElem);
        } else {
            return;
        }
    }

    // TODO SERVER-61281 consider removing this call to fillCache().
    // Since Document caches fields the size of this cache and getApproximateSize() can vary
    // depending on access. In order to avoid this and make sure we subtract the right amount if
    // remove() ever gets called, we can fill the cache to get a consistent view of the size.
    // Normally the outer window function code handles this, but _genKeyOutPair() makes a new
    // document for sortKey, so its cache get reset.
    keyOutPair.first.fillCache();
    const auto memUsage = keyOutPair.first.getApproximateSize() +
        keyOutPair.second.getApproximateSize() + sizeof(KeyOutPair);
    updateAndCheckMemUsage(memUsage);
    _map->emplace(keyOutPair);
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

    // TODO SERVER-61281 consider removing this comment if its no longer relevant.
    // After calling lower_bound() it uses SortKeyComparator and the sortKey's field cache should be
    // fully populated so no need to call fillCache() again.
    _memUsageBytes -= keyOutPair.first.getApproximateSize() +
        keyOutPair.second.getApproximateSize() + sizeof(KeyOutPair);
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
            tassert(5872600, "Expected 'output' field to contain an array", vals.isArray());
            for (auto&& val : vals.getArray()) {
                _processValue(val);
            }
        } else {
            tasserted(5872602,
                      "argument to top/bottom processInternal must be an array or an "
                      "object when merging");
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
            result.emplace_back(BSON(kFieldNameGeneratedSortKey
                                     << keyOutPair.first << kFieldNameOutput << keyOutPair.second));
        } else {
            result.push_back(keyOutPair.second);
        }
    };

    if constexpr (!single) {
        return Value(result);
    } else {
        if (toBeMerged) {
            return Value(result);
        } else {
            if (result.empty()) {
                // This only occurs in a window function scenario, an accumulator will always have
                // at least one value processed.
                return Value(BSONNULL);
            }
            return Value(result[0]);
        }
    }
}

template <TopBottomSense sense, bool single>
void AccumulatorTopBottomN<sense, single>::reset() {
    _map->clear();
    _memUsageBytes = sizeof(*this);
}

// Explicitly specify the following classes should generated and should live in this compilation
// unit.
template class AccumulatorTopBottomN<TopBottomSense::kBottom, false>;
template class AccumulatorTopBottomN<TopBottomSense::kBottom, true>;
template class AccumulatorTopBottomN<TopBottomSense::kTop, false>;
template class AccumulatorTopBottomN<TopBottomSense::kTop, true>;

}  // namespace mongo
