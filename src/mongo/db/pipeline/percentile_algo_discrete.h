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

#include "mongo/db/pipeline/percentile_algo_accurate.h"

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
