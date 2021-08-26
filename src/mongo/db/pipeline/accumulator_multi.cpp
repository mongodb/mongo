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

namespace mongo {
using FirstLastSense = AccumulatorFirstLastN::Sense;
using MinMaxSense = AccumulatorMinMax::Sense;

REGISTER_ACCUMULATOR_WITH_MIN_VERSION(
    maxN,
    AccumulatorMinMaxN::parseMinMaxN<MinMaxSense::kMax>,
    ServerGlobalParams::FeatureCompatibility::Version::kVersion51);
REGISTER_ACCUMULATOR_WITH_MIN_VERSION(
    minN,
    AccumulatorMinMaxN::parseMinMaxN<MinMaxSense::kMin>,
    ServerGlobalParams::FeatureCompatibility::Version::kVersion51);
REGISTER_EXPRESSION_WITH_MIN_VERSION(maxN,
                                     AccumulatorMinMaxN::parseExpression<MinMaxSense::kMax>,
                                     AllowedWithApiStrict::kNeverInVersion1,
                                     AllowedWithClientType::kAny,
                                     ServerGlobalParams::FeatureCompatibility::Version::kVersion51);
REGISTER_EXPRESSION_WITH_MIN_VERSION(minN,
                                     AccumulatorMinMaxN::parseExpression<MinMaxSense::kMin>,
                                     AllowedWithApiStrict::kNeverInVersion1,
                                     AllowedWithClientType::kAny,
                                     ServerGlobalParams::FeatureCompatibility::Version::kVersion51);
REGISTER_ACCUMULATOR_WITH_MIN_VERSION(
    firstN,
    AccumulatorFirstLastN::parseFirstLastN<FirstLastSense::kFirst>,
    ServerGlobalParams::FeatureCompatibility::Version::kVersion51);
REGISTER_ACCUMULATOR_WITH_MIN_VERSION(
    lastN,
    AccumulatorFirstLastN::parseFirstLastN<FirstLastSense::kLast>,
    ServerGlobalParams::FeatureCompatibility::Version::kVersion51);
REGISTER_EXPRESSION_WITH_MIN_VERSION(firstN,
                                     AccumulatorFirstLastN::parseExpression<FirstLastSense::kFirst>,
                                     AllowedWithApiStrict::kNeverInVersion1,
                                     AllowedWithClientType::kAny,
                                     ServerGlobalParams::FeatureCompatibility::Version::kVersion51);
REGISTER_EXPRESSION_WITH_MIN_VERSION(lastN,
                                     AccumulatorFirstLastN::parseExpression<FirstLastSense::kLast>,
                                     AllowedWithApiStrict::kNeverInVersion1,
                                     AllowedWithClientType::kAny,
                                     ServerGlobalParams::FeatureCompatibility::Version::kVersion51);
// TODO SERVER-57885 Add $minN/$maxN as window functions.
// TODO SERVER-57884 Add $firstN/$lastN as window functions.

AccumulatorN::AccumulatorN(ExpressionContext* const expCtx)
    : AccumulatorState(expCtx), _maxMemUsageBytes(internalQueryMaxNAccumulatorBytes.load()) {}

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
    _n = validateN(input);
}

void AccumulatorN::processInternal(const Value& input, bool merging) {
    tassert(5787802, "'n' must be initialized", _n);

    if (merging) {
        tassert(5787803, "input must be an array when 'merging' is true", input.isArray());
        auto array = input.getArray();
        for (auto&& val : array) {
            processValue(val);
        }
    } else {
        processValue(input);
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
    boost::intrusive_ptr<Expression> output;
    for (auto&& element : args) {
        auto fieldName = element.fieldNameStringData();
        if (fieldName == kFieldNameOutput) {
            output = Expression::parseOperand(expCtx, element, vps);
        } else if (fieldName == kFieldNameN) {
            n = Expression::parseOperand(expCtx, element, vps);
        } else {
            uasserted(5787901, str::stream() << "Unknown argument for 'n' operator: " << fieldName);
        }
    }
    uassert(5787906, str::stream() << "Missing value for " << kFieldNameN << "'", n);
    uassert(5787907, str::stream() << "Missing value for " << kFieldNameOutput << "'", output);
    return std::make_tuple(n, output);
}

void AccumulatorN::serializeHelper(const boost::intrusive_ptr<Expression>& initializer,
                                   const boost::intrusive_ptr<Expression>& argument,
                                   bool explain,
                                   MutableDocument& md) {
    md.addField(kFieldNameN, Value(initializer->serialize(explain)));
    md.addField(kFieldNameOutput, Value(argument->serialize(explain)));
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

    // TODO SERVER-58379 Remove this uassert once the FCV constants are upgraded and the REGISTER
    // macros above are updated accordingly.
    uassert(5787909,
            str::stream() << "Cannot create " << name << " accumulator if feature flag is disabled",
            feature_flags::gFeatureFlagExactTopNAccumulator.isEnabledAndIgnoreFCV());
    uassert(5787900,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::Object);
    BSONObj obj = elem.embeddedObject();

    auto [n, output] = AccumulatorN::parseArgs(expCtx, obj, vps);

    auto factory = [expCtx] {
        if constexpr (s == MinMaxSense::kMin) {
            return AccumulatorMinN::create(expCtx);
        } else {
            return AccumulatorMaxN::create(expCtx);
        }
    };

    return {std::move(n), std::move(output), std::move(factory), name};
}

void AccumulatorMinMaxN::processValue(const Value& val) {
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
    _memUsageBytes += val.getApproximateSize();
    uassert(ErrorCodes::ExceededMemoryLimit,
            str::stream() << getOpName()
                          << " used too much memory and cannot spill to disk. Memory limit: "
                          << _maxMemUsageBytes << " bytes",
            _memUsageBytes < _maxMemUsageBytes);
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
    auto name = [] {
        if constexpr (v == Sense::kFirst) {
            return AccumulatorFirstN::getName();
        } else {
            return AccumulatorLastN::getName();
        }
    }();

    // TODO SERVER-58379 Remove this uassert once the FCV constants are upgraded and the REGISTER
    // macros above are updated accordingly.
    uassert(5787800,
            str::stream() << "Cannot create " << name << " accumulator if feature flag is disabled",
            feature_flags::gFeatureFlagExactTopNAccumulator.isEnabledAndIgnoreFCV());
    uassert(5787801,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::Object);
    auto obj = elem.embeddedObject();

    auto [n, output] = AccumulatorN::parseArgs(expCtx, obj, vps);

    auto factory = [expCtx] {
        if constexpr (v == Sense::kFirst) {
            return AccumulatorFirstN::create(expCtx);
        } else {
            return AccumulatorLastN::create(expCtx);
        }
    };

    return {std::move(n), std::move(output), std::move(factory), name};
}

void AccumulatorFirstLastN::processValue(const Value& val) {
    // Only insert in the lastN case if we have 'n' elements.
    if (static_cast<long long>(_deque.size()) == *_n) {
        if (_variant == Sense::kLast) {
            _memUsageBytes -= _deque.front().getApproximateSize();
            _deque.pop_front();
        } else {
            return;
        }
    }

    _memUsageBytes += val.getApproximateSize();
    uassert(ErrorCodes::ExceededMemoryLimit,
            str::stream() << getOpName()
                          << " used too much memory and cannot spill to disk. Memory limit: "
                          << _maxMemUsageBytes << " bytes",
            _memUsageBytes < _maxMemUsageBytes);
    _deque.push_back(val);
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

}  // namespace mongo
