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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_percentile_enum_gen.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/percentile_algo.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/sorter/sorter.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
/**
 * Accumulator for computing $percentile.
 */
class AccumulatorPercentile : public AccumulatorState {
public:
    static constexpr auto kApproximate = "approximate"_sd;
    static constexpr auto kContinuous = "continuous"_sd;
    static constexpr auto kDiscrete = "discrete"_sd;

    static constexpr auto kName = "$percentile"_sd;
    const char* getOpName() const override {
        return kName.data();
    }

    /**
     * Parsing and creating the accumulator. A separate accumulator object is created per group.
     */
    static AccumulationExpression parseArgs(ExpressionContext* expCtx,
                                            BSONElement elem,
                                            VariablesParseState vps);

    static boost::intrusive_ptr<Expression> parseExpression(ExpressionContext* expCtx,
                                                            BSONElement elem,
                                                            VariablesParseState vps);

    /**
     * Necessary for supporting $percentile as window functions and/or as expression.
     */
    static std::pair<std::vector<double> /*ps*/, PercentileMethodEnum> parsePercentileAndMethod(
        ExpressionContext* expCtx, BSONElement elem, VariablesParseState vps);
    static Value formatFinalValue(int nPercentiles, const std::vector<double>& pctls);

    /**
     * maxMemoryUsageBytes sets the accumulator's memory limit before it will spill to disk. Passing
     * in boost::none will result in the default query knob value
     * internalQueryMaxPercentileAccumulatorBytes being used.
     */
    AccumulatorPercentile(ExpressionContext* expCtx,
                          const std::vector<double>& ps,
                          PercentileMethodEnum method,
                          boost::optional<int> maxMemoryUsageBytes = boost::none);

    /**
     * Ingressing values and computing the requested percentiles.
     */
    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) override;

    /**
     * Other infra for the accumulators.
     */
    void reset() final;


    /**
     * Serializes this accumulator to a valid MQL accumulation statement that would be legal
     * inside a $group. When executing on a sharded cluster for approximate percentiles,
     * the result of this function will be sent to each individual shard. When executing on a
     * sharded cluster for accurate percentiles, the current implementation prevents the $group from
     * executing on the shards, but allows a $project on the fields specified for $percentile to be
     * pushed down to the shards.
     *
     * The implementation in 'AccumulatorState' assumes the accumulator has the simple syntax {
     * <name>: <argument> }, such as { $sum: <argument> }. Because $percentile's syntax is more
     * complex ({$percentile: {p: [0.5, 0.8], input: "$x", method: "approximate"}}) we have to
     * override this method.
     */
    Document serialize(boost::intrusive_ptr<Expression> initializer,
                       boost::intrusive_ptr<Expression> argument,
                       const SerializationOptions& options) const override;

    /**
     * Helper that allows both the accumulator and expression $percentile to serialize their
     * corresponding instance variables.
     */
    static void serializeHelper(const boost::intrusive_ptr<Expression>& argument,
                                const SerializationOptions& options,
                                std::vector<double> percentiles,
                                PercentileMethodEnum method,
                                MutableDocument& md);

    /**
     * Getter for method
     */
    PercentileMethodEnum getMethod() const {
        return _method;
    }

    std::unique_ptr<PercentileAlgorithm> createPercentileAlgorithm(PercentileMethodEnum method);

protected:
    std::vector<double> _percentiles;
    std::unique_ptr<PercentileAlgorithm> _algo;
    const PercentileMethodEnum _method;
};

/*
 * Accumulator for computing $median. $median has the same semantics as $percentile with the 'p'
 * field set to [0.5].
 */
class AccumulatorMedian : public AccumulatorPercentile {
public:
    static constexpr auto kName = "$median"_sd;
    const char* getOpName() const final {
        return kName.data();
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

    /**
     * We are matching the signature of the AccumulatorPercentile for the purpose of using
     * ExpressionFromAccumulatorQuantile as a template for both $median and $percentile. This is the
     * reason for passing in `unused` and it will not be referenced.
     *
     * maxMemoryUsageBytes sets the accumulator's memory limit before it will spill to disk. Passing
     * in boost::none will result in the default query knob value
     * internalQueryMaxPercentileAccumulatorBytes being used.
     */
    AccumulatorMedian(ExpressionContext* expCtx,
                      const std::vector<double>& unused,
                      PercentileMethodEnum method,
                      boost::optional<int> maxMemoryUsageBytes = boost::none);

    /**
     * Necessary for supporting $median as window functions and/or as expression.
     */
    static std::pair<std::vector<double> /*ps*/, PercentileMethodEnum> parsePercentileAndMethod(
        ExpressionContext* expCtx, BSONElement elem, VariablesParseState vps);
    static Value formatFinalValue(int nPercentiles, const std::vector<double>& pctls);

    /**
     * Modify the base-class implementation to return a single value rather than a single-element
     * array.
     */
    Value getValue(bool toBeMerged) final;

    Document serialize(boost::intrusive_ptr<Expression> initializer,
                       boost::intrusive_ptr<Expression> argument,
                       const SerializationOptions& options) const override;

    /**
     * Helper that allows both the accumulator and expression $median to serialize their
     * corresponding instance variables.
     */
    static void serializeHelper(const boost::intrusive_ptr<Expression>& argument,
                                const SerializationOptions& options,
                                std::vector<double> percentiles,
                                PercentileMethodEnum method,
                                MutableDocument& md);
};
}  // namespace mongo
