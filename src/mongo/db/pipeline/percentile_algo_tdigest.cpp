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
#include <iostream>

#include "mongo/db/pipeline/percentile_algo_tdigest.h"

#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/util/assert_util.h"

namespace mongo {

std::unique_ptr<PercentileAlgorithm> createTDigest() {
    return std::make_unique<TDigest>(TDigest::k2_limit, internalQueryTdigestDelta.load());
}

using Centroid = TDigest::Centroid;
using std::vector;

TDigest::TDigest(ScalingFunction k_limit, int delta)
    : _k_limit(k_limit), _delta(delta), _maxBufferSize(bufferCoeff * delta) {}

TDigest::TDigest(int64_t negInfCount,
                 int64_t posInfCount,
                 double min,
                 double max,
                 vector<Centroid> centroids,
                 ScalingFunction k_limit,
                 int delta)
    : _k_limit(k_limit),
      _delta(delta),
      _maxBufferSize(bufferCoeff * delta),
      _n(0),
      _negInfCount(negInfCount),
      _posInfCount(posInfCount),
      _min(min),
      _max(max) {
    _centroids.swap(centroids);
    for (const Centroid& c : _centroids) {
        _n += c.weight;
    }
}

long TDigest::memUsageBytes() const {
    return sizeof(TDigest) + _buffer.capacity() * sizeof(double) +
        _centroids.capacity() * sizeof(Centroid);
}

void TDigest::incorporate(double input) {
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
    _buffer.push_back(input);
    if (_buffer.size() >= _maxBufferSize) {
        flushBuffer();
    }
}

void TDigest::incorporate(const vector<double>& inputs) {
    _buffer.reserve(_buffer.size() + inputs.size());
    for (double v : inputs) {
        if (std::isnan(v)) {
            continue;
        }
        if (std::isinf(v)) {
            if (v < 0) {
                _negInfCount++;
            } else {
                _posInfCount++;
            }
            continue;
        }
        _buffer.push_back((v));
    }
    if (_buffer.size() >= _maxBufferSize) {
        flushBuffer();
    }
}

void TDigest::flushBuffer() {
    if (_buffer.empty()) {
        return;
    }

    // TODO SERVER-75565: 'boost::sort::spreadsort::spreadsort' shows an observable perf improvement
    // over std::sort on large datasets. If switching to boost's spreadsort would need to re-tune
    // the default delta setting and the size of the buffer.
    std::sort(_buffer.begin(), _buffer.end());
    merge(_buffer);
    _buffer.clear();
}

boost::optional<double> TDigest::computePercentile(double p) {
    if (!_buffer.empty()) {
        flushBuffer();
    }

    if (_centroids.empty() && _negInfCount == 0 && _posInfCount == 0) {
        return boost::none;
    }

    // We compute accurate min and max values. This check must be done _before_ dealing with
    // infinites.
    if (p >= 1.0) {
        return max();
    } else if (p <= 0) {
        return min();
    }

    int rank = PercentileAlgorithm::computeTrueRank(_n + _posInfCount + _negInfCount, p);
    if (_negInfCount > 0 && rank < _negInfCount) {
        return -std::numeric_limits<double>::infinity();
    } else if (_posInfCount > 0 && rank >= _n + _negInfCount) {
        return std::numeric_limits<double>::infinity();
    }
    rank -= _negInfCount;

    // Even though strict ordering of centroids isn't guaranteed, the algorithm assumes it when
    // computing percentiles (this is the reason t-digest cannot guarantee the accuracy bounds). So,
    // under this assumption, let's find the centroid an input with rank 'rank' would have
    // contributed to.
    size_t i = 0;  // index of the target centroid
    double r = 0;  // cumulative weight of all centroids up to, and including, i_th one

    // We are not optimizing traversing the set of centroids for higher percentiles or when
    // multiple percentiles have been requested because our benchmarks don't show this to be a
    // problem in the accumulator context, and for expressions, where it might matter, we are not
    // using t-digest.
    for (; i < _centroids.size(); i++) {
        r += _centroids[i].weight;
        if (r > rank) {
            break;
        }
    }

    // If the i_th centroid has weight exactly 1, it hasn't lost any information and we can give it
    // out as the answer (if the centroids are strictly ordered, this answer would be accurate).
    if (_centroids[i].weight == 1) {
        return _centroids[i].mean;
    }

    // We also assume that the inputs are uniformly distributed within each centroid so we can do
    // linear interpolation between the means of the centroids to compute the percentile. Basically,
    // given centroids {w: 10, m: 2.4} and {w: 16, m: 3.7} we assume that there are 10/2 + 16/2 = 13
    // evenly distributed points in [2.4, 3.7). NB: we do _not_ interpolate with infinities.
    const double wCur = _centroids[i].weight;

    double left = 0;
    double right = 0;
    double wLeft = 0;
    double wRight = 0;
    double doubledInnerRank = 0;

    // (r - rank) is in (0.0, wCur] by construction of 'r'
    if (r - rank >= wCur / 2) {
        // The target point sits between the previous and i_th centroids' means.
        left = (i == 0) ? _min : _centroids[i - 1].mean;
        right = _centroids[i].mean;
        wLeft = (i == 0 ? 0 : _centroids[i - 1].weight);
        wRight = wCur;
        doubledInnerRank = wLeft + 2 * wRight - 2 * (r - 1 - rank);  // [wLeft, wLeft + wRight]
    } else {
        // The target point sits between the i_th and the next centroids' means (or _max).
        left = _centroids[i].mean;
        right = (i == _centroids.size() - 1) ? _max : _centroids[i + 1].mean;
        wLeft = wCur;
        wRight = (i == _centroids.size() - 1 ? 0 : _centroids[i + 1].weight);
        doubledInnerRank = wLeft - 2 * (r - 1 - rank);  // [0.0, wLeft]
    }
    const double innerP = doubledInnerRank / (wLeft + wRight);  // [0.0, 1.0]

    // Both (right - left) and innerP are non-negative, so the computation below is guaranteed to be
    // to the right of 'left' but the precision error might make it greater than 'right'...
    double res = left + (right - left) * innerP;
    return (res > right) ? right : res;
}

vector<double> TDigest::computePercentiles(const vector<double>& ps) {
    vector<double> pctls;
    pctls.reserve(ps.size());
    for (double p : ps) {
        auto pctl = computePercentile(p);
        if (pctl) {
            pctls.push_back(*pctl);
        } else {
            return {};
        }
    }
    return pctls;
}

void TDigest::merge(const vector<double>& sorted) {
    if (sorted.empty()) {
        return;
    }

    _n += sorted.size();
    _min = std::min(_min, sorted.front());
    _max = std::max(_max, sorted.back());

    vector<Centroid> temp;
    temp.reserve(std::min<size_t>(2 * _delta, _centroids.size() + sorted.size()));

    // Invariant: after the merge, the weights of all centroids should add up to the new _n.
    int64_t tn = 0;

    auto itCentroids = _centroids.begin();
    auto itSorted = sorted.begin();
    double n = _n;  // to ensure floating point math below

    // Conceptually, the algorithm treats 'sorted' as a t-digest with single-point centroids, merges
    // the two sorted lists of centroids into one sorted list and then compacts it according to
    // the scaling function (that is, merges the adjacent centroids if the size of the resulting
    // centroid is allowed by the scaling function). We do this in a single pass, compacting the
    // centroids as we are merging the sorted centroids with the sorted data.

    int64_t w = 0;  // cumulative weights of the centroids up to (but not including) the current one

    while (itCentroids != _centroids.end() && itSorted != sorted.end()) {
        Centroid cur =
            (itCentroids->mean > *itSorted) ? Centroid{1, *(itSorted++)} : *(itCentroids++);
        const double qLimit = n * _k_limit(w / n, _delta);
        while (itCentroids != _centroids.end() && itSorted != sorted.end()) {
            if (itCentroids->mean > *itSorted) {
                if (w + cur.weight + 1 <= qLimit) {
                    cur.add(*(itSorted++));
                } else {
                    break;
                }
            } else {
                if (w + cur.weight + itCentroids->weight <= qLimit) {
                    cur.add(*(itCentroids++));
                } else {
                    break;
                }
            }
        }
        temp.push_back(cur);
        tn += cur.weight;
        w += cur.weight;
    }

    // Have run out of the sorted data => merge the remaining tail of centoids (some of them might
    // need to be compacted).
    while (itCentroids != _centroids.end()) {
        Centroid cur = *(itCentroids++);
        const double qLimit = n * _k_limit(w / n, _delta);

        while (itCentroids != _centroids.end() && w + cur.weight + itCentroids->weight <= qLimit) {
            cur.add(*(itCentroids++));
        }
        temp.push_back(cur);
        tn += cur.weight;
        w += cur.weight;
    }

    // Have run out of the centroids => merge in the remainging tail of the sorted data.
    while (itSorted != sorted.end()) {
        Centroid cur{1, *(itSorted++)};
        const double qLimit = n * _k_limit(w / n, _delta);
        while (itSorted != sorted.end() && w + cur.weight + 1 <= qLimit) {
            cur.add(*(itSorted++));
        }
        temp.push_back(cur);
        tn += cur.weight;
        w += cur.weight;
    }

    tassert(
        7429503,
        "Merging a batch of data into current digest either lost or duplicated some of the inputs",
        tn == _n);

    temp.shrink_to_fit();
    _centroids.swap(temp);
}

void TDigest::merge(const TDigest& other) {
    tassert(7429504,
            "Digests that use different scaling functions or delta parameters aren't mergeable",
            _k_limit == other._k_limit && _delta == other._delta);
    tassert(7429505, "Cannot merge a digest with itself", &other != this);

    _n += other._n;
    _negInfCount += other._negInfCount;
    _posInfCount += other._posInfCount;
    _min = std::min(_min, other._min);
    _max = std::max(_max, other._max);

    if (other.centroids().empty()) {
        return;
    }

    const vector<Centroid>& c1 = _centroids;
    const vector<Centroid>& c2 = other._centroids;

    vector<Centroid> temp;
    temp.reserve(std::min<size_t>(2 * _delta, c1.size() + c2.size()));

    // Invariant: after the merge, the weights of all centroids should add up to the new _n.
    int64_t tn = 0;

    auto it1 = c1.begin();
    auto it2 = c2.begin();
    const double n = _n;  // to ensure floating point division below when it involves '_n'

    // Conceptually, the algorithm merges the two sorted lists of centroids into one sorted list
    // and then compacts it according to the scaling function (that is, merges the adjacent
    // centroids if the size of the resulting centroid is allowed by the scaling function). We do
    // this in a single pass, compacting the centroids as we are merging the sorted lists.

    int64_t w = 0;  // cumulative weights of centroids up to (not including) the current one
    while (it1 != c1.end() && it2 != c2.end()) {
        Centroid cur = (it1->mean > it2->mean) ? *(it2++) : *(it1++);
        const double qLimit = n * _k_limit(w / n, _delta);

        while (it1 != c1.end() && it2 != c2.end()) {
            auto& next = (it2->mean < it1->mean) ? it2 : it1;
            if (w + cur.weight + next->weight <= qLimit) {
                cur.add(*(next++));
            } else {
                break;
            }
        }
        temp.push_back(cur);
        tn += cur.weight;
        w += cur.weight;
    }

    // Process the remaining tail.
    const auto& end = (it1 != c1.end()) ? c1.end() : c2.end();
    auto& tail = (it1 != c1.end()) ? it1 : it2;
    while (tail != end) {
        Centroid cur = *(tail++);
        const double qLimit = n * _k_limit(w / n, _delta);

        while (tail != end && w + cur.weight + tail->weight <= qLimit) {
            cur.add(*(tail++));
        }
        temp.push_back(cur);
        tn += cur.weight;
        w += cur.weight;
    }

    tassert(7429506, "Merging two digests either lost or duplicated some of the inputs", tn == _n);

    temp.shrink_to_fit();
    _centroids.swap(temp);
}

std::ostream& operator<<(std::ostream& os, const TDigest& digest) {
    os << "{n: " << digest.n() << ", min: " << digest.min() << ", max: " << digest.max();
    os << ", posInf: " << digest.posInfCount() << ", negInf: " << digest.negInfCount();
    os << ", s: " << digest.centroids().size() << ", centroids: [";
    for (const Centroid& c : digest.centroids()) {
        os << c << ",";
    }
    os << "]}";
    return os;
}

std::ostream& operator<<(std::ostream& os, const Centroid& centroid) {
    os << " {w: " << centroid.weight << " , m: " << centroid.mean << "}";
    return os;
}

}  // namespace mongo
