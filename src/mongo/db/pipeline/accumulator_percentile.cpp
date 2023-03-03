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

namespace mongo {

using boost::intrusive_ptr;

REGISTER_ACCUMULATOR_WITH_FEATURE_FLAG(percentile,
                                       AccumulatorPercentile::parseArgs,
                                       feature_flags::gFeatureFlagApproxPercentiles);

namespace {
// Checks that 'pv' is an array of valid percentile specifications.
std::vector<double> validatePercentileArg(const Value& pv) {
    const auto msg = "'p' must be an array of numeric values from [0.0, 1.0] range, but found "_sd;
    uassert(7429700, str::stream() << msg << pv.toString(), pv.isArray());

    std::vector<double> ps;
    for (const Value& p : pv.getArray()) {
        uassert(7429701, str::stream() << msg << pv.toString(), p.numeric());
        auto pd = p.coerceToDouble();
        uassert(7429702, str::stream() << msg << pv.toString(), pd >= 0 && pd <= 1);
        ps.push_back(pd);
    }
    return ps;
}
}  // namespace

// TODO SERVER-74556: Move parsing of the args into IDL.
AccumulationExpression AccumulatorPercentile::parseArgs(ExpressionContext* const expCtx,
                                                        BSONElement elem,
                                                        VariablesParseState vps) {
    expCtx->sbeGroupCompatible = false;

    uassert(7429703,
            str::stream() << "specification must be an object; found " << elem,
            elem.type() == BSONType::Object);
    BSONObj obj = elem.embeddedObject();

    boost::intrusive_ptr<Expression> input;
    std::vector<double> ps;
    std::string algo = "";
    for (auto&& element : obj) {
        auto fieldName = element.fieldNameStringData();
        if (fieldName == kFieldNameInput) {
            input = Expression::parseOperand(expCtx, element, vps);
        } else if (fieldName == kFieldNameP) {
            auto pv = Value(element);
            ps = validatePercentileArg(pv);
        } else if (fieldName == kFieldNameAlgo) {
            if (element.type() == BSONType::String) {
                algo = element.String();
            }
            // More types of percentiles will be supported in the future: PM-1883
            uassert(7429704,
                    str::stream()
                        << "the valid specifications for 'algorithm' are: 'approximate'; found "
                        << element,
                    algo == "approximate");
        } else {
            uasserted(7429705,
                      str::stream() << "Unknown argument for 'percentile' operator: " << fieldName);
        }
    }
    uassert(7429706, str::stream() << "Missing value for '" << kFieldNameP << "'", !ps.empty());
    uassert(7429707, str::stream() << "Missing value for '" << kFieldNameInput << "'", input);
    uassert(
        7429708, str::stream() << "Missing value for '" << kFieldNameAlgo << "'", !algo.empty());

    auto factory = [expCtx, ps] {
        // Temporary implementation! To be replaced based on the user's choice of algorithm.
        auto algo = PercentileAlgorithm::createDiscreteSortAndRank();

        return AccumulatorPercentile::create(expCtx, ps, std::move(algo));
    };

    return {ExpressionConstant::create(expCtx, Value(BSONNULL)) /*initializer*/,
            std::move(input) /*argument*/,
            std::move(factory),
            "$percentile"_sd /*name*/};
}

void AccumulatorPercentile::processInternal(const Value& input, bool merging) {
    if (merging) {
        // Merging percentiles from different shards. For approximate percentiels 'input' is a
        // document with a digest.
        // TODO SERVER-74358: Support sharded collections
        uasserted(7429709, "Sharded collections are not yet supported by $percentile");
        return;
    }

    if (!input.numeric()) {
        return;
    }

    _algo->incorporate(input.coerceToDouble());

    // TODO SERVER-74558: Could/should we update the memory usage less often?
    _memUsageBytes = sizeof(*this) + _algo->memUsageBytes();
}

Value AccumulatorPercentile::getValue(bool toBeMerged) {
    if (toBeMerged) {
        // Return a document, containing the whole digest, because they would need to be merged
        // on mongos to compute the final result.
        // TODO SERVER-74358: Support sharded collections
        uasserted(7429710, "Sharded collections are not yet supported by $percentile");
        return Value(Document{});
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

AccumulatorPercentile::AccumulatorPercentile(ExpressionContext* const expCtx,
                                             const std::vector<double>& ps,
                                             std::unique_ptr<PercentileAlgorithm> algo)
    : AccumulatorState(expCtx), _percentiles(ps), _algo(std::move(algo)) {
    _memUsageBytes = sizeof(*this) + _algo->memUsageBytes();
}

void AccumulatorPercentile::reset() {
    // Temporary implementation! To be replaced based on the user's choice of algorithm.
    auto algo = PercentileAlgorithm::createDiscreteSortAndRank();

    _memUsageBytes = sizeof(*this) + _algo->memUsageBytes();
}

intrusive_ptr<AccumulatorState> AccumulatorPercentile::create(
    ExpressionContext* const expCtx,
    const std::vector<double>& ps,
    std::unique_ptr<PercentileAlgorithm> algo) {
    return new AccumulatorPercentile(expCtx, ps, std::move(algo));
}
}  // namespace mongo
