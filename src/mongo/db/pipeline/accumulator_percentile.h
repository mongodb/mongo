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

#pragma once

#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator_percentile_gen.h"
#include "mongo/db/pipeline/percentile_algo.h"

namespace mongo {
/**
 * Accumulator for computing $percentile.
 */
class AccumulatorPercentile : public AccumulatorState {
public:
    static constexpr auto kName = "$percentile"_sd;
    const char* getOpName() const {
        return kName.rawData();
    }

    /**
     * Checks that 'pv' is an array of valid percentile specifications. Called by the IDL file.
     */
    static Status validatePercentileArg(const std::vector<double>& pv);

    /**
     * Parsing and creating the accumulator. A separate accumulator object is created per group.
     */
    static AccumulationExpression parseArgs(ExpressionContext* expCtx,
                                            BSONElement elem,
                                            VariablesParseState vps);

    static boost::intrusive_ptr<Expression> parseExpression(ExpressionContext* expCtx,
                                                            BSONElement elem,
                                                            VariablesParseState vps);

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx,
                                                         const std::vector<double>& ps,
                                                         int32_t method);

    AccumulatorPercentile(ExpressionContext* expCtx, const std::vector<double>& ps, int32_t method);

    /**
     * Ingressing values and computing the requested percentiles.
     */
    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged);

    /**
     * Other infra for the accumulators.
     */
    void reset() final;

    /**
     * Necessary for supporting $percentile as window functions.
     */
    static std::pair<std::vector<double> /*ps*/, int32_t /*method*/> parsePercentileAndMethod(
        BSONElement elem);

    /**
     * Serializes this accumulator to a valid MQL accumulation statement that would be legal
     * inside a $group. When executing on a sharded cluster, the result of this function will be
     * sent to each individual shard.
     *
     * The implementation in 'AccumulatorState' assumes the accumulator has the simple syntax {
     * <name>: <argument> }, such as { $sum: <argument> }. Because $percentile's syntax is more
     * complex ({$percentile: {p: [0.5, 0.8], input: "$x", method: "approximate"}}) we have to
     * override this method.
     */
    Document serialize(boost::intrusive_ptr<Expression> initializer,
                       boost::intrusive_ptr<Expression> argument,
                       SerializationOptions options) const;

    /**
     * Helper that allows both the accumulator and expression $percentile to serialize their
     * corresponding instance variables.
     */
    static void serializeHelper(const boost::intrusive_ptr<Expression>& argument,
                                SerializationOptions options,
                                std::vector<double> percentiles,
                                int32_t method,
                                MutableDocument& md);

protected:
    std::vector<double> _percentiles;
    std::unique_ptr<PercentileAlgorithm> _algo;

    // TODO SERVER-74894: This should have been 'PercentileMethodEnum' but the generated
    // header from the IDL includes this header, creating a dependency.
    const int32_t _method;
};

/*
 * Accumulator for computing $median. $median has the same semantics as $percentile with the 'p'
 * field set to [0.5].
 */
class AccumulatorMedian : public AccumulatorPercentile {
public:
    static constexpr auto kName = "$median"_sd;
    const char* getOpName() const final {
        return kName.rawData();
    }

    /**
     * Parsing and creating the accumulator.
     */
    static AccumulationExpression parseArgs(ExpressionContext* expCtx,
                                            BSONElement elem,
                                            VariablesParseState vps);

    static boost::intrusive_ptr<Expression> parseExpression(ExpressionContext* expCtx,
                                                            BSONElement elem,
                                                            VariablesParseState vps);

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx,
                                                         const std::vector<double>& unused,
                                                         int32_t method);

    /**
     * We are matching the signature of the AccumulatorPercentile for the purpose of using
     * ExpressionFromAccumulatorQuantile as a template for both $median and $percentile. This is the
     * reason for passing in `unused` and it will not be referenced.
     */
    AccumulatorMedian(ExpressionContext* expCtx, const std::vector<double>& unused, int32_t method);

    /**
     * Necessary for supporting $median as window functions.
     */
    static std::pair<std::vector<double> /*ps*/, int32_t /*method*/> parsePercentileAndMethod(
        BSONElement elem);
    /**
     * Modify the base-class implementation to return a single value rather than a single-element
     * array.
     */
    Value getValue(bool toBeMerged) final;

    Document serialize(boost::intrusive_ptr<Expression> initializer,
                       boost::intrusive_ptr<Expression> argument,
                       SerializationOptions options) const;

    /**
     * Helper that allows both the accumulator and expression $median to serialize their
     * corresponding instance variables.
     */
    static void serializeHelper(const boost::intrusive_ptr<Expression>& argument,
                                SerializationOptions options,
                                std::vector<double> percentiles,
                                int32_t method,
                                MutableDocument& md);
};
}  // namespace mongo
