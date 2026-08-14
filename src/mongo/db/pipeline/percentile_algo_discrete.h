// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/percentile_algo_accurate.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * 'DiscretePercentile' algorithm for computing percentiles is accurate and doesn't require
 * specifying a percentile in advance but is only suitable for small datasets. It accumulats all
 * data sent to it and sorts it all when a percentile is requested. Requesting more percentiles
 * after the first one without incorporating more data is fast as it doesn't need to sort again.
 */
class DiscretePercentile : public AccuratePercentile {
public:
    DiscretePercentile(ExpressionContext* expCtx);

    static int computeTrueRank(int n, double p) {
        return computeDiscreteRank(n, p);
    }

    boost::optional<double> computePercentile(double p) final;

    void reset() final;

private:
    // Only used if we spilled to disk.
    boost::optional<double> computeSpilledPercentile(double p) final;
    boost::optional<double> _previousValue = boost::none;
};

}  // namespace mongo
