/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <cmath>
#include <limits>
#include <memory>

#include <boost/optional/optional.hpp>

#include "mongo/db/pipeline/percentile_algo_continuous.h"

namespace mongo {

double ContinuousPercentile::linearInterpolate(double rank, int rank_ceil, int rank_floor) {
    return (rank_ceil - rank) * _accumulatedValues[rank_floor] +
        (rank - rank_floor) * _accumulatedValues[rank_ceil];
}

boost::optional<double> ContinuousPercentile::computePercentile(double p) {
    if (_accumulatedValues.empty() && _negInfCount == 0 && _posInfCount == 0) {
        return boost::none;
    }

    const int n = _accumulatedValues.size();
    double rank = computeTrueRank(n + _posInfCount + _negInfCount, p);
    if (_negInfCount > 0 && rank < _negInfCount) {
        return -std::numeric_limits<double>::infinity();
    } else if (_posInfCount > 0 && rank > n + _negInfCount - 1) {
        return std::numeric_limits<double>::infinity();
    }
    rank -= _negInfCount;

    int rank_ceil = ceil(rank);
    int rank_floor = floor(rank);

    // The data might be sorted either by construction or because too many percentiles have been
    // requested at once, in which case we can just return the corresponding element, otherwise,
    // use std::nth_element algorithm which does partial sorting of the range and places n_th order
    // statistic at its place in the vector. The algorithm runs in O(_accumulatedValues.size()).
    if (_shouldSort) {
        if (rank_ceil == rank && rank == rank_floor) {
            auto it = _accumulatedValues.begin() + rank;
            std::nth_element(_accumulatedValues.begin(), it, _accumulatedValues.end());
            return *it;
        } else {
            auto ceil_it = _accumulatedValues.begin() + rank_ceil;
            auto floor_it = _accumulatedValues.begin() + rank_floor;
            std::nth_element(_accumulatedValues.begin(), ceil_it, _accumulatedValues.end());
            std::nth_element(_accumulatedValues.begin(), floor_it, _accumulatedValues.end());
            return linearInterpolate(rank, rank_ceil, rank_floor);
        }
    }

    if (rank_ceil == rank && rank == rank_floor) {
        return _accumulatedValues[(int)rank];
    } else {
        return linearInterpolate(rank, rank_ceil, rank_floor);
    }
}

std::unique_ptr<PercentileAlgorithm> createContinuousPercentile() {
    return std::make_unique<ContinuousPercentile>();
}

}  // namespace mongo
