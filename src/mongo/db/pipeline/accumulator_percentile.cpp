/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/accumulator_percentile.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulator_percentile_gen.h"
#include "mongo/db/pipeline/expression_from_accumulator_quantile.h"
#include "mongo/db/pipeline/percentile_algo.h"
#include "mongo/db/pipeline/percentile_algo_accurate.h"
#include "mongo/db/version_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <type_traits>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_ACCUMULATOR(percentile, AccumulatorPercentile::parseArgs);
REGISTER_STABLE_EXPRESSION(percentile, AccumulatorPercentile::parseExpression);

REGISTER_ACCUMULATOR(median, AccumulatorMedian::parseArgs);
REGISTER_STABLE_EXPRESSION(median, AccumulatorMedian::parseExpression);

namespace {

template <typename TAccumulator>
Value evaluateAccumulatorQuantile(const ExpressionFromAccumulatorQuantile<TAccumulator>& expr,
                                  const Document& root,
                                  Variables* variables) {
    auto input = expr.getInput()->evaluate(root, variables);
    if (input.numeric()) {
        // On a scalar value, all percentiles are the same for all methods.
        return TAccumulator::formatFinalValue(
            expr.getPs().size(), std::vector<double>(expr.getPs().size(), input.coerceToDouble()));
    }

    if (input.isArray() && input.getArrayLength() > 0) {
        std::vector<double> samples;
        samples.reserve(input.getArrayLength());
        for (const auto& item : input.getArray()) {
            if (item.numeric()) {
                samples.push_back(item.coerceToDouble());
            }
        }
        if (expr.getMethod() != PercentileMethodEnum::kContinuous) {
            // On small datasets, which are likely to be the inputs for the expression, creating
            // t-digests is inefficient, so instead we use DiscretePercentile algo directly for
            // both "discrete" and "approximate" methods.
            DiscretePercentile dp(expr.getExpressionContext());
            dp.incorporate(samples);
            return TAccumulator::formatFinalValue(expr.getPs().size(),
                                                  dp.computePercentiles(expr.getPs()));
        } else {
            ContinuousPercentile cp(expr.getExpressionContext());
            cp.incorporate(samples);
            return TAccumulator::formatFinalValue(expr.getPs().size(),
                                                  cp.computePercentiles(expr.getPs()));
        }
    }

    // No numeric values have been found for the expression to process.
    return TAccumulator::formatFinalValue(expr.getPs().size(), {});
}

}  // namespace

template <>
Value ExpressionFromAccumulatorQuantile<AccumulatorPercentile>::evaluate(
    const Document& root, Variables* variables) const {
    return evaluateAccumulatorQuantile(*this, root, variables);
}

template <>
Value ExpressionFromAccumulatorQuantile<AccumulatorMedian>::evaluate(const Document& root,
                                                                     Variables* variables) const {
    return evaluateAccumulatorQuantile(*this, root, variables);
}

namespace {
PercentileMethodEnum methodNameToEnum(const VersionContext& vCtx, StringData method) {
    uassert(ErrorCodes::BadValue,
            "Currently only 'approximate' can be used as a percentile 'method'.",
            feature_flags::gFeatureFlagAccuratePercentiles.isEnabled(
                vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ||
                method == AccumulatorPercentile::kApproximate);

    if (method == AccumulatorPercentile::kApproximate) {
        return PercentileMethodEnum::kApproximate;
    }
    if (method == AccumulatorPercentile::kDiscrete) {
        return PercentileMethodEnum::kDiscrete;
    }
    if (method == AccumulatorPercentile::kContinuous) {
        return PercentileMethodEnum::kContinuous;
    }

    // The idl should have validated the input string (see 'validatePercentileMethod()').
    uasserted(
        9157900,
        "Currently only 'approximate', 'discrete', and 'continuous' percentiles are supported");
}

StringData percentileMethodEnumToString(PercentileMethodEnum method) {
    switch (method) {
        case PercentileMethodEnum::kApproximate:
            return AccumulatorPercentile::kApproximate;
        case PercentileMethodEnum::kDiscrete:
            return AccumulatorPercentile::kDiscrete;
        case PercentileMethodEnum::kContinuous:
            return AccumulatorPercentile::kContinuous;
    }
    MONGO_UNREACHABLE;
}

// Deal with the 'p' field. It's allowed to use constant expressions and variables as long as it
// evaluates to an array of doubles from the range [0.0, 1.0].
std::vector<double> parseP(ExpressionContext* const expCtx,
                           BSONElement elem,
                           VariablesParseState vps) {
    auto expr = Expression::parseOperand(expCtx, elem, vps)->optimize();
    ExpressionConstant* constExpr = dynamic_cast<ExpressionConstant*>(expr.get());
    uassert(7750300,
            str::stream() << "The $percentile 'p' field must be an array of "
                             "constant values, but found value: "
                          << elem.toString(false, false) << ".",
            constExpr);
    Value pVals = constExpr->getValue();

    constexpr StringData msg =
        "The $percentile 'p' field must be an array of numbers from [0.0, 1.0], but found: "_sd;
    if (!pVals.isArray() || pVals.getArrayLength() == 0) {
        uasserted(7750301, str::stream() << msg << pVals.toString());
    }

    std::vector<double> ps;
    ps.reserve(pVals.getArrayLength());
    for (const Value& pVal : pVals.getArray()) {
        if (!pVal.numeric()) {
            uasserted(7750302, str::stream() << msg << pVal.toString());
        }
        double p = pVal.coerceToDouble();
        if (p < 0 || p > 1) {
            uasserted(7750303, str::stream() << msg << p);
        }
        ps.push_back(p);
    }
    return ps;
}
}  // namespace

AccumulationExpression AccumulatorPercentile::parseArgs(ExpressionContext* const expCtx,
                                                        BSONElement elem,
                                                        VariablesParseState vps) {
    expCtx->setSbeGroupCompatibility(SbeCompatibility::notCompatible);

    uassert(7429703,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::object);

    auto spec = AccumulatorPercentileSpec::parse(elem.Obj(), IDLParserContext(kName));

    boost::intrusive_ptr<Expression> input =
        Expression::parseOperand(expCtx, spec.getInput().getElement(), vps);

    std::vector<double> ps = parseP(expCtx, spec.getP().getElement(), vps);

    const PercentileMethodEnum method = methodNameToEnum(
        VersionContext::getDecoration(expCtx->getOperationContext()), spec.getMethod());

    auto factory = [expCtx, ps, method] {
        return make_intrusive<AccumulatorPercentile>(expCtx, ps, method);
    };

    return {ExpressionConstant::create(expCtx, Value(BSONNULL)) /*initializer*/,
            std::move(input) /*argument*/,
            std::move(factory),
            "$percentile"_sd /*name*/};
}

std::pair<std::vector<double> /*ps*/, PercentileMethodEnum>
AccumulatorPercentile::parsePercentileAndMethod(ExpressionContext* expCtx,
                                                BSONElement elem,
                                                VariablesParseState vps) {
    auto spec = AccumulatorPercentileSpec::parse(elem.Obj(), IDLParserContext(kName));
    return std::make_pair(
        parseP(expCtx, spec.getP().getElement(), vps),
        methodNameToEnum(VersionContext::getDecoration(expCtx->getOperationContext()),
                         spec.getMethod()));
}

boost::intrusive_ptr<Expression> AccumulatorPercentile::parseExpression(
    ExpressionContext* const expCtx, BSONElement elem, VariablesParseState vps) {
    expCtx->setSbeGroupCompatibility(SbeCompatibility::notCompatible);
    uassert(7436200,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::object);

    auto spec = AccumulatorPercentileSpec::parse(elem.Obj(), IDLParserContext(kName));

    boost::intrusive_ptr<Expression> input =
        Expression::parseOperand(expCtx, spec.getInput().getElement(), vps);
    std::vector<double> ps = parseP(expCtx, spec.getP().getElement(), vps);
    const PercentileMethodEnum method = methodNameToEnum(
        VersionContext::getDecoration(expCtx->getOperationContext()), spec.getMethod());

    return make_intrusive<ExpressionFromAccumulatorQuantile<AccumulatorPercentile>>(
        expCtx, ps, input, method);
}

void AccumulatorPercentile::processInternal(const Value& input, bool merging) {
    if (merging) {
        assertMergingInputType(input, BSONType::array);
        dynamic_cast<PartialPercentile<Value>*>(_algo.get())->combine(input);

        _memUsageTracker.set(sizeof(*this) + _algo->memUsageBytes());
        if (!_memUsageTracker.withinMemoryLimit()) {
            _algo->spill();
            _memUsageTracker.set(sizeof(*this) + _algo->memUsageBytes());
        }
        return;
    }
    if (!input.numeric()) {
        return;
    }
    _algo->incorporate(input.coerceToDouble());
    _memUsageTracker.set(sizeof(*this) + _algo->memUsageBytes());
}

Value AccumulatorPercentile::formatFinalValue(int nPercentiles, const std::vector<double>& pctls) {
    if (pctls.empty()) {
        std::vector<Value> nulls;
        nulls.insert(nulls.end(), nPercentiles, Value(BSONNULL));
        return Value(nulls);
    }
    return Value(std::vector<Value>(pctls.begin(), pctls.end()));
}

Value AccumulatorPercentile::getValue(bool toBeMerged) {
    if (toBeMerged) {
        return dynamic_cast<PartialPercentile<Value>*>(_algo.get())->serialize();
    }
    return AccumulatorPercentile::formatFinalValue(_percentiles.size(),
                                                   _algo->computePercentiles(_percentiles));
}

std::unique_ptr<PercentileAlgorithm> AccumulatorPercentile::createPercentileAlgorithm(
    PercentileMethodEnum method) {
    switch (method) {
        case PercentileMethodEnum::kApproximate:
            return createTDigestDistributedClassic();
        case PercentileMethodEnum::kDiscrete:
            return createDiscretePercentile(this->getExpressionContext());
        case PercentileMethodEnum::kContinuous:
            return createContinuousPercentile(this->getExpressionContext());
        default:
            uasserted(
                7435800,
                str::stream()
                    << "Only approximate, discrete, and continuous percentiles are supported");
    }
    return nullptr;
}

AccumulatorPercentile::AccumulatorPercentile(ExpressionContext* const expCtx,
                                             const std::vector<double>& ps,
                                             PercentileMethodEnum method,
                                             boost::optional<int> maxMemoryUsageBytes)
    : AccumulatorState(
          expCtx, maxMemoryUsageBytes.value_or(internalQueryMaxPercentileAccumulatorBytes.load())),
      _percentiles(ps),
      _algo(createPercentileAlgorithm(method)),
      _method(method) {
    _memUsageTracker.set(sizeof(*this) + _algo->memUsageBytes());
}

void AccumulatorPercentile::reset() {
    if (_method == PercentileMethodEnum::kApproximate) {
        _algo = createPercentileAlgorithm(_method);
    }
    // Reset the algorithm's internal state, in case the algorithm has spilled to disk.
    // Note that the approximate percentile case has a no-op reset, because it cannot spill to disk.
    _algo->reset();

    _memUsageTracker.set(sizeof(*this) + _algo->memUsageBytes());
}

Document AccumulatorPercentile::serialize(boost::intrusive_ptr<Expression> initializer,
                                          boost::intrusive_ptr<Expression> argument,
                                          const SerializationOptions& options) const {
    ExpressionConstant const* ec = dynamic_cast<ExpressionConstant const*>(initializer.get());
    invariant(ec);
    invariant(ec->getValue().nullish());

    MutableDocument md;
    AccumulatorPercentile::serializeHelper(argument, options, _percentiles, _method, md);

    return DOC(getOpName() << md.freeze());
}

void AccumulatorPercentile::serializeHelper(const boost::intrusive_ptr<Expression>& argument,
                                            const SerializationOptions& options,
                                            std::vector<double> percentiles,
                                            PercentileMethodEnum method,
                                            MutableDocument& md) {
    md.addField(AccumulatorPercentileSpec::kInputFieldName, Value(argument->serialize(options)));
    md.addField(AccumulatorPercentileSpec::kPFieldName,
                Value(std::vector<Value>(percentiles.begin(), percentiles.end())));
    md.addField(AccumulatorPercentileSpec::kMethodFieldName,
                Value(percentileMethodEnumToString(method)));
}

AccumulationExpression AccumulatorMedian::parseArgs(ExpressionContext* const expCtx,
                                                    BSONElement elem,
                                                    VariablesParseState vps) {
    expCtx->setSbeGroupCompatibility(SbeCompatibility::notCompatible);

    uassert(7436100,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::object);

    auto spec = AccumulatorMedianSpec::parse(elem.Obj(), IDLParserContext(kName));
    boost::intrusive_ptr<Expression> input =
        Expression::parseOperand(expCtx, spec.getInput().getElement(), vps);

    const PercentileMethodEnum method = methodNameToEnum(
        VersionContext::getDecoration(expCtx->getOperationContext()), spec.getMethod());

    auto factory = [expCtx, method] {
        return make_intrusive<AccumulatorMedian>(
            expCtx, std::vector<double>{} /* unused */, method);
    };

    return {ExpressionConstant::create(expCtx, Value(BSONNULL)) /*initializer*/,
            std::move(input) /*argument*/,
            std::move(factory),
            "$median"_sd /*name*/};
}

std::pair<std::vector<double> /*ps*/, PercentileMethodEnum>
AccumulatorMedian::parsePercentileAndMethod(ExpressionContext* expCtx,
                                            BSONElement elem,
                                            VariablesParseState /*vps*/) {
    auto spec = AccumulatorMedianSpec::parse(elem.Obj(), IDLParserContext(kName));
    return std::make_pair(
        std::vector<double>({0.5}),
        methodNameToEnum(VersionContext::getDecoration(expCtx->getOperationContext()),
                         spec.getMethod()));
}

boost::intrusive_ptr<Expression> AccumulatorMedian::parseExpression(ExpressionContext* const expCtx,
                                                                    BSONElement elem,
                                                                    VariablesParseState vps) {
    expCtx->setSbeGroupCompatibility(SbeCompatibility::notCompatible);
    uassert(7436201,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::object);

    auto spec = AccumulatorMedianSpec::parse(elem.Obj(), IDLParserContext(kName));

    boost::intrusive_ptr<Expression> input =
        Expression::parseOperand(expCtx, spec.getInput().getElement(), vps);

    std::vector<double> p = {0.5};

    const PercentileMethodEnum method = methodNameToEnum(
        VersionContext::getDecoration(expCtx->getOperationContext()), spec.getMethod());

    return make_intrusive<ExpressionFromAccumulatorQuantile<AccumulatorMedian>>(
        expCtx, p, input, method);
}

AccumulatorMedian::AccumulatorMedian(ExpressionContext* expCtx,
                                     const std::vector<double>& /* unused */,
                                     PercentileMethodEnum method,
                                     boost::optional<int> maxMemoryUsageBytes)
    : AccumulatorPercentile(
          expCtx,
          {0.5} /* Median is equivalent to asking for the 50th percentile */,
          method,
          maxMemoryUsageBytes.value_or(internalQueryMaxPercentileAccumulatorBytes.load())) {}

Value AccumulatorMedian::formatFinalValue(int nPercentiles, const std::vector<double>& pctls) {
    if (pctls.empty()) {
        return Value(BSONNULL);
    }

    tassert(7436101,
            "the percentile method for median must return a single result.",
            pctls.size() == 1);
    return Value(pctls.front());
}

Value AccumulatorMedian::getValue(bool toBeMerged) {
    // $median only adjusts the output of the final result, the internal logic for merging is up to
    // the implementation of $percentile.
    if (toBeMerged) {
        return AccumulatorPercentile::getValue(toBeMerged);
    }

    return AccumulatorMedian::formatFinalValue(_percentiles.size(),
                                               _algo->computePercentiles(_percentiles));
}

Document AccumulatorMedian::serialize(boost::intrusive_ptr<Expression> initializer,
                                      boost::intrusive_ptr<Expression> argument,
                                      const SerializationOptions& options) const {
    ExpressionConstant const* ec = dynamic_cast<ExpressionConstant const*>(initializer.get());
    invariant(ec);
    invariant(ec->getValue().nullish());

    MutableDocument md;
    AccumulatorMedian::serializeHelper(argument, options, _percentiles, _method, md);

    return DOC(getOpName() << md.freeze());
}

void AccumulatorMedian::serializeHelper(const boost::intrusive_ptr<Expression>& argument,
                                        const SerializationOptions& options,
                                        std::vector<double> percentiles,
                                        PercentileMethodEnum method,
                                        MutableDocument& md) {
    md.addField(AccumulatorPercentileSpec::kInputFieldName, Value(argument->serialize(options)));
    md.addField(AccumulatorPercentileSpec::kMethodFieldName,
                Value(percentileMethodEnumToString(method)));
}
}  // namespace mongo
