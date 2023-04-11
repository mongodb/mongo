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

#include <algorithm>
#include <cmath>

#include "mongo/db/pipeline/percentile_algo_discrete.h"

#include "mongo/db/query/query_knobs_gen.h"

namespace mongo {
using std::vector;

void DiscretePercentile::incorporate(double input) {
    if (std::isnan(input)) {
        return;
    }
    if (std::isinf(input)) {
        if (input < 0) {
            _negInfCount++;
        } else {
            _posInfCount++;
        }
        return;
    }

    // Take advantage of already sorted input -- avoid resorting it later.
    if (!_shouldSort && !_accumulatedValues.empty() && input < _accumulatedValues.back()) {
        _shouldSort = true;
    }

    _accumulatedValues.push_back(input);
}

void DiscretePercentile::incorporate(const std::vector<double>& inputs) {
    _accumulatedValues.reserve(_accumulatedValues.size() + inputs.size());
    for (double val : inputs) {
        incorporate(val);
    }
}

boost::optional<double> DiscretePercentile::computePercentile(double p) {
    if (_accumulatedValues.empty() && _negInfCount == 0 && _posInfCount == 0) {
        return boost::none;
    }

    const int n = _accumulatedValues.size();
    int rank = PercentileAlgorithm::computeTrueRank(n + _posInfCount + _negInfCount, p);
    if (_negInfCount > 0 && rank < _negInfCount) {
        return -std::numeric_limits<double>::infinity();
    } else if (_posInfCount > 0 && rank >= n + _negInfCount) {
        return std::numeric_limits<double>::infinity();
    }
    rank -= _negInfCount;

    // The data might be sorted either by construction or because too many percentiles have been
    // requested at once, in which case we can just return the corresponding element, otherwise,
    // use std::nth_element algorithm which does partial sorting of the range and places n_th order
    // statistic at its place in the vector. The algorithm runs in O(_accumulatedValues.size()).
    if (_shouldSort) {
        auto it = _accumulatedValues.begin() + rank;
        std::nth_element(_accumulatedValues.begin(), it, _accumulatedValues.end());
        return *it;
    }
    return _accumulatedValues[rank];
}

vector<double> DiscretePercentile::computePercentiles(const vector<double>& ps) {
    if (_accumulatedValues.empty() && _negInfCount == 0 && _posInfCount == 0) {
        return {};
    }

    vector<double> pctls;
    pctls.reserve(ps.size());

    // When sufficiently many percentiles are requested at once, it becomes more efficient to sort
    // the data rather than compute each percentile separately. The tipping point depends on both
    // the size of the data and the number of percentiles, but to keep the model simple for the knob
    // we only consider the latter.
    if (_shouldSort &&
        static_cast<int>(ps.size()) > internalQueryPercentileExprSelectToSortThreshold.load()) {
        std::sort(_accumulatedValues.begin(), _accumulatedValues.end());
        _shouldSort = false;
    }

    for (double p : ps) {
        pctls.push_back(*computePercentile(p));
    }
    return pctls;
}

std::unique_ptr<PercentileAlgorithm> createDiscretePercentile() {
    return std::make_unique<DiscretePercentile>();
}

}  // namespace mongo
