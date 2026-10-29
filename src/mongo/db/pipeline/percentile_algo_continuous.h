// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#pragma once

#include "mongo/db/pipeline/percentile_algo_accurate.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 *'ContinuousPercentile' algorithm for computing accurate continuous percentiles
 */
class ContinuousPercentile : public AccuratePercentile {
public:
    ContinuousPercentile(ExpressionContext* expCtx);

    static double computeTrueRank(int n, double p) {
        return p * (n - 1);
    }

    static double linearInterpolate(
        double rank, int rank_ceil, int rank_floor, double value_ceil, double value_floor);

    boost::optional<double> computePercentile(double p) final;

    void reset() final;

private:
    // Only used if we spilled to disk.
    boost::optional<double> computeSpilledPercentile(double p) final;
    std::pair<boost::optional<double>, boost::optional<double>> _previousValues = {boost::none,
                                                                                   boost::none};
};

}  // namespace mongo
