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
    const char* getOpName() const final {
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

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx,
                                                         const std::vector<double>& ps,
                                                         std::unique_ptr<PercentileAlgorithm> algo);

    AccumulatorPercentile(ExpressionContext* expCtx,
                          const std::vector<double>& ps,
                          std::unique_ptr<PercentileAlgorithm> algo);

    /**
     * Ingressing values and computing the requested percentiles.
     */
    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;

    /**
     * Other infra for the accumulators.
     */
    void reset() final;

private:
    std::vector<double> _percentiles;
    std::unique_ptr<PercentileAlgorithm> _algo;
};

}  // namespace mongo
