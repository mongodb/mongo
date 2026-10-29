// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/percentile_algo_continuous.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

ContinuousPercentile::ContinuousPercentile(ExpressionContext* expCtx) {
    _expCtx = expCtx;
}

void ContinuousPercentile::reset() {
    AccuratePercentile::reset();
    _previousValues = {boost::none, boost::none};
}

double ContinuousPercentile::linearInterpolate(
    double rank, int rank_ceil, int rank_floor, double value_ceil, double value_floor) {
    return (rank_ceil - rank) * value_floor + (rank - rank_floor) * value_ceil;
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

    // The data might be already sorted either by construction or because too many percentiles have
    // been requested at once, in which case we can just return the corresponding element without
    // any prior sorting. Otherwise, use some partial sort algorithm of the range such that the
    // rank_ceil_th and rank_floor_th elements are in the correct place in the vector.

    if (rank_ceil == rank && rank == rank_floor) {
        // The correct percentile is exactly one of the accumulated values. Simply return this value
        // for the continuous percentile.
        auto it = _accumulatedValues.begin() + rank;
        if (_shouldSort) {
            // std::nth_element runs in O(_accumulatedValues.size()).
            std::nth_element(_accumulatedValues.begin(), it, _accumulatedValues.end());
        }
        return *it;
    }

    // The correct percentile lies in between two of the accumulated values. Linear interpolate
    // between them for a continuous percentile.
    if (_shouldSort) {
        // std::partial_sort(first, middle, last) sorts the collection such that the [first,
        // middle) contains the sorted (middle - first) smallest elements in the range [first,
        // last). Sort the collection from [begin, ceiling iterator), such that accesses to the
        // rank_floor-th and rank_ceil-th elements give the linear interpolation bounds.
        // Note that std::nth_element cannot be used because it only partitions the data around
        // one iterator - it does not sort either side. std::partial_sort runs in approximately
        // O(_accumulatedValues.size() * log(rank_ceil)).
        auto ceil_it = _accumulatedValues.begin() + rank_ceil;
        std::partial_sort(_accumulatedValues.begin(), ceil_it + 1, _accumulatedValues.end());
    }

    return linearInterpolate(
        rank, rank_ceil, rank_floor, _accumulatedValues[rank_ceil], _accumulatedValues[rank_floor]);
}


boost::optional<double> ContinuousPercentile::computeSpilledPercentile(double p) {
    double rank = computeTrueRank(_numTotalValuesSpilled + _posInfCount + _negInfCount, p);
    if (_negInfCount > 0 && rank < _negInfCount) {
        return -std::numeric_limits<double>::infinity();
    } else if (_posInfCount > 0 && rank > _numTotalValuesSpilled + _negInfCount - 1) {
        return std::numeric_limits<double>::infinity();
    }
    rank -= _negInfCount;

    int rank_ceil = ceil(rank);
    int rank_floor = floor(rank);

    tassert(9299401,
            "Successive calls to computeSpilledPercentile() must have nondecreasing values of p",
            _indexNextSorted - rank < 2);

    while (_indexNextSorted <= rank_ceil) {
        _previousValues.first = _previousValues.second;
        _previousValues.second = _sorterIterator->next().first.getDouble();
        _indexNextSorted += 1;
    }

    if (rank_ceil == rank && rank == rank_floor) {
        return _previousValues.second;
    }

    return linearInterpolate(
        rank, rank_ceil, rank_floor, *_previousValues.second, *_previousValues.first);
}

std::unique_ptr<PercentileAlgorithm> createContinuousPercentile(ExpressionContext* expCtx) {
    return std::make_unique<ContinuousPercentile>(expCtx);
}

}  // namespace mongo
