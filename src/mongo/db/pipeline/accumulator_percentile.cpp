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

#include "mongo/db/exec/document_value/value.h"
#include "mongo/idl/idl_parser.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_ACCUMULATOR_WITH_FEATURE_FLAG(percentile,
                                       AccumulatorPercentile::parseArgs,
                                       feature_flags::gFeatureFlagApproxPercentiles);


REGISTER_ACCUMULATOR_WITH_FEATURE_FLAG(median,
                                       AccumulatorMedian::parseArgs,
                                       feature_flags::gFeatureFlagApproxPercentiles);

Status AccumulatorPercentile::validatePercentileArg(const std::vector<double>& pv) {
    if (pv.empty()) {
        return {ErrorCodes::BadValue, "'p' cannot be an empty array"};
    }
    for (const double& p : pv) {
        if (p < 0 || p > 1) {
            return {ErrorCodes::BadValue,
                    str::stream() << "'p' must be an array of numeric values from [0.0, 1.0] "
                                     "range, but received incorrect value: "
                                  << p};
        }
    }
    return Status::OK();
}

AccumulationExpression AccumulatorPercentile::parseArgs(ExpressionContext* const expCtx,
                                                        BSONElement elem,
                                                        VariablesParseState vps) {
    expCtx->sbeGroupCompatible = false;

    uassert(7429703,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::Object);

    auto spec = AccumulatorPercentileSpec::parse(IDLParserContext(kName), elem.Obj());
    boost::intrusive_ptr<Expression> input =
        Expression::parseOperand(expCtx, spec.getInput().getElement(), vps);
    std::vector<double> ps = spec.getP();

    PercentileAlgorithmTypeEnum algoType = spec.getAlgorithm();

    auto factory = [expCtx, ps, algoType] {
        return AccumulatorPercentile::create(expCtx, ps, static_cast<int32_t>(algoType));
    };

    return {ExpressionConstant::create(expCtx, Value(BSONNULL)) /*initializer*/,
            std::move(input) /*argument*/,
            std::move(factory),
            "$percentile"_sd /*name*/};
}

void AccumulatorPercentile::processInternal(const Value& input, bool merging) {
    if (merging) {
        dynamic_cast<PartialPercentile<Value>*>(_algo.get())->combine(input);
        return;
    }

    if (!input.numeric()) {
        return;
    }
    _algo->incorporate(input.coerceToDouble());
    _memUsageBytes = sizeof(*this) + _algo->memUsageBytes();
}

Value AccumulatorPercentile::getValue(bool toBeMerged) {
    if (toBeMerged) {
        return dynamic_cast<PartialPercentile<Value>*>(_algo.get())->serialize();
    }

    // Compute the percentiles for each requested one in the order listed. Computing a percentile
    // can only fail if there have been no numeric inputs, then all percentiles would fail. Rather
    // than returning an array of nulls in this case, we return a single null value.
    std::vector<double> pctls;
    for (double p : _percentiles) {
        auto res = _algo->computePercentile(p);
        if (!res) {
            return Value(BSONNULL);
        }
        pctls.push_back(res.value());
    }

    return Value(std::vector<Value>(pctls.begin(), pctls.end()));
}

namespace {
std::unique_ptr<PercentileAlgorithm> createPercentileAlgorithm(int32_t algoType) {
    switch (static_cast<PercentileAlgorithmTypeEnum>(algoType)) {
        case PercentileAlgorithmTypeEnum::Approximate:
            // Temporary implementation!
            return createDiscreteSortAndRankParallelClassic();
        default:
            tasserted(7435800,
                      str::stream() << "Currently only approximate percentiles are supported");
    }
    return nullptr;
}
}  // namespace

AccumulatorPercentile::AccumulatorPercentile(ExpressionContext* const expCtx,
                                             const std::vector<double>& ps,
                                             int32_t algoType)
    : AccumulatorState(expCtx),
      _percentiles(ps),
      _algo(createPercentileAlgorithm(algoType)),
      _algoType(algoType) {
    _memUsageBytes = sizeof(*this) + _algo->memUsageBytes();
}

void AccumulatorPercentile::reset() {
    _algo = createPercentileAlgorithm(_algoType);
    _memUsageBytes = sizeof(*this) + _algo->memUsageBytes();
}

Document AccumulatorPercentile::serialize(boost::intrusive_ptr<Expression> initializer,
                                          boost::intrusive_ptr<Expression> argument,
                                          SerializationOptions options) const {
    ExpressionConstant const* ec = dynamic_cast<ExpressionConstant const*>(initializer.get());
    invariant(ec);
    invariant(ec->getValue().nullish());

    MutableDocument md;
    md.addField(AccumulatorPercentileSpec::kInputFieldName, Value(argument->serialize(options)));

    md.addField(AccumulatorPercentileSpec::kPFieldName,
                Value(std::vector<Value>(_percentiles.begin(), _percentiles.end())));

    md.addField(AccumulatorPercentileSpec::kAlgorithmFieldName,
                Value(PercentileAlgorithmType_serializer(
                    static_cast<PercentileAlgorithmTypeEnum>(_algoType))));

    return DOC(getOpName() << md.freeze());
}

intrusive_ptr<AccumulatorState> AccumulatorPercentile::create(ExpressionContext* const expCtx,
                                                              const std::vector<double>& ps,
                                                              int32_t algoType) {
    return new AccumulatorPercentile(expCtx, ps, algoType);
}

AccumulationExpression AccumulatorMedian::parseArgs(ExpressionContext* const expCtx,
                                                    BSONElement elem,
                                                    VariablesParseState vps) {
    expCtx->sbeGroupCompatible = false;

    uassert(7436100,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::Object);

    auto spec = AccumulatorMedianSpec::parse(IDLParserContext(kName), elem.Obj());
    boost::intrusive_ptr<Expression> input =
        Expression::parseOperand(expCtx, spec.getInput().getElement(), vps);

    PercentileAlgorithmTypeEnum algoType = spec.getAlgorithm();

    auto factory = [expCtx, algoType] {
        return AccumulatorMedian::create(expCtx, static_cast<int32_t>(algoType));
    };

    return {ExpressionConstant::create(expCtx, Value(BSONNULL)) /*initializer*/,
            std::move(input) /*argument*/,
            std::move(factory),
            "$ median"_sd /*name*/};
}

AccumulatorMedian::AccumulatorMedian(ExpressionContext* expCtx, int32_t algoType)
    : AccumulatorPercentile(expCtx, {0.5} /* ps */, algoType){};

intrusive_ptr<AccumulatorState> AccumulatorMedian::create(ExpressionContext* expCtx,
                                                          int32_t algoType) {
    return new AccumulatorMedian(expCtx, algoType);
}

Value AccumulatorMedian::getValue(bool toBeMerged) {
    auto result = AccumulatorPercentile::getValue(toBeMerged);

    // $median only adjusts the output of the final result, the internal logic for merging is up to
    // the implementation of $percentile.
    if (toBeMerged) {
        return result;
    }

    // Currently $percentile returns _scalar_ null if all inputs were non-numeric.
    if (result.getType() == jstNULL) {
        return result;
    }

    tassert(7436101,
            "the percentile algorithm for median must return a single result.",
            result.getArrayLength() == 1);

    return Value(result.getArray().front());
}

Document AccumulatorMedian::serialize(boost::intrusive_ptr<Expression> initializer,
                                      boost::intrusive_ptr<Expression> argument,
                                      SerializationOptions options) const {
    ExpressionConstant const* ec = dynamic_cast<ExpressionConstant const*>(initializer.get());
    invariant(ec);
    invariant(ec->getValue().nullish());

    MutableDocument md;
    md.addField(AccumulatorPercentileSpec::kInputFieldName, Value(argument->serialize(options)));

    md.addField(AccumulatorPercentileSpec::kAlgorithmFieldName,
                Value(PercentileAlgorithmType_serializer(
                    static_cast<PercentileAlgorithmTypeEnum>(_algoType))));

    return DOC(getOpName() << md.freeze());
}
}  // namespace mongo
