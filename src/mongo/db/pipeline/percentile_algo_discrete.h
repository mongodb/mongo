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

    // We define "percentile" as:
    //   Percentile P(p) where 'p' is from [0.0, 1.0] on dataset 'D' with 'n', possibly duplicated,
    //   samples is value 'P' such that at least ceil(p*n) samples from 'D' are _less or equal_ to
    //   'P' and no more than ceil(p*n) samples that are strictly _less_ than 'P'. Thus, p = 0 maps
    //   to the min of 'D' and p = 1 maps to the max of 'D'.
    //
    // Notice, that this definition is ambiguous. For example, on D = {1.0, 2.0, ..., 10.0} P(0.1)
    // could be any value in [1.0, 2.0] range. For discrete percentiles the value 'P' _must_ be one
    // of the samples from 'D' but it's still ambiguous as either 1.0 or 2.0 can be used.
    //
    // This definiton leads to the following computation of 0-based rank for percentile 'p' while
    // resolving the ambiguity towards the lower rank.
    static int computeTrueRank(int n, double p) {
        if (p >= 1.0) {
            return n - 1;
        }
        return std::max(0, static_cast<int>(std::ceil(n * p)) - 1);
    }

    boost::optional<double> computePercentile(double p) final;

    void reset() final;

private:
    // Only used if we spilled to disk.
    boost::optional<double> computeSpilledPercentile(double p) final;
    boost::optional<double> _previousValue = boost::none;
};

}  // namespace mongo
