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

#include <cmath>
#include <limits>
#include <vector>

#include "mongo/db/pipeline/percentile_algo.h"

namespace mongo {

/**
 * For the description of t-digest algorithm see
 * https://github.com/tdunning/t-digest/blob/main/docs/t-digest-paper/histo.pdf
 */
class TDigest : public PercentileAlgorithm {
public:
    typedef double (*ScalingFunction)(double /* q */, double /* delta */);

    //----------------------------------------------------------------------------------------------
    // PercentileAlgorithm interface
    //----------------------------------------------------------------------------------------------

    /**
     * The raw input is buffered by TDigest and, when the buffer is full, merged into the
     * datastructure that represents prior inputs. The ammortized runtime complexity is
     * O(log(delta)) where 'delta' is the compaction paramenter of t-digest.
     * NaN values are ignored.
     */
    void incorporate(double input) final;

    /**
     * All values from 'inputs' are merged with the current buffer and processed at _once_. This
     * allows the clients to increase the default buffer size.
     */
    void incorporate(const std::vector<double>& inputs) final;

    /**
     * We compute percentile by linearly scanning centroids to find the one that matches the rank of
     * the requested percentile and then doing a linear interpolation between centroid means. We are
     * currently not optimizing for accessing multiple percentiles as we don't think that would
     * result in noticeable performance gains for accumulators and expressions should not be using
     * t-digest.
     */
    boost::optional<double> computePercentile(double p) final;
    std::vector<double> computePercentiles(const std::vector<double>& ps) final;

    long memUsageBytes() const final;

    //----------------------------------------------------------------------------------------------
    // Implementation details of t-digest
    //
    // All methods are public to enable unit testing. The production clients should be accessing
    // this class through the PercentileAlgorithm interface.
    //----------------------------------------------------------------------------------------------

    /**
     * Each centroid represents a "summary" of a few processed datapoints by storing the number of
     * the datapoints (the 'weight' of the centroid) and the average of their values (the 'mean').
     */
    struct Centroid {
        Centroid(double w, double m) : weight(w), mean(m) {}

        // The computation below ensures that the interpolated mean is within the bounds of the
        // source means so long as the source weights aren't wildly imbalanced.
        //
        // Proof: let m,m1,m2 be the means of the combined, original and other centroid, and
        // similarly w,w1,w2 for the weights.
        //
        // Let dm = m2-m1 and let d = (m2-m1)x(w2/w) be the correction to the mean m1 as computed
        // by machine double operations, that is, m = m1+d. Rounding and multiplication by a
        // positive constant can not change the sign of an operation so sgn(dm)=sgn(d) and therefore
        // m lies on the correct side of m1. Thus computed m would lie on the correct side of m2 if
        // |d| ≤ |dm|, which holds when the maximum relative error (1+eps) introduced by the
        // rounding in (m2-m1) is compensated for by the multiplication by the interpolation
        // parameter (w2/w), where eps is the maximum relative error for double float calculations,
        // 2^-53. Since the maximum combined relative error introduced by the two machine
        // calculations in (w2/w) is (1+eps)^2, require that (w2/w)(1+eps)^2 ≤ 1/(1+eps), or (w2/w)
        // ≤ (1+eps)^3 ~ 1+3ε. Thus the caclculated combined mean can only lay outside the bounds of
        // the source means if the original centroid weight w1 is less than about 3/2^53 of the
        // combined weight w. We can avoid this restriction by flipping the computation based on
        // which of the weights is higher.
        void add(const Centroid& other) {
            if (other.weight < weight) {
                weight += other.weight;
                mean += (other.mean - mean) * (other.weight / weight);
            } else {
                double new_weight = weight + other.weight;
                mean = other.mean + (mean - other.mean) * (weight / new_weight);
                weight = new_weight;
            }
        }

        void add(double value) {
            weight += 1;
            mean += (value - mean) / weight;
        }

        // Semantically, 'weight' should be an integer. But 1) there's probably a cost to converting
        // to doubles for each division, and 2) the weight range over which the calculations for
        // adding centroids are accurate are (roughly) the range of doubles.
        double weight;
        double mean;
    };

    // The scaling functions. 'q' must be from [0.0, 1.0] and 'delta' must be positive. While there
    // are no specific restrictions on 'delta', its expected to fall between 10^2 and 10^6.
    //
    // The inverse is on 'q' and treats 'delta' as parameter. The "limit" function is also
    // parametrized by 'delta' and is equal to k_inverse(1 + k(q)) but might be folded for some of
    // the scaling functions for better performance.
    //
    // k0 - a linear scaling function. Fast but not biased to give accurate results for extreme
    // percentiles. Limits weight of all centroids to 2n/delta, where n is the number of processed
    // inputs. Limits the number of centroids to delta.
    //
    // k1 - the scaling function from the paper. Expensive to compute. k1_limit() isn't foldable to
    // a closed form. Limits the number of centroids to delta.
    //
    // k2 - a scaling function with asymptotic behaviour at 0 and 1 similar to k1, but cheaper to
    // compute. Limits the number of centroids to 2*delta. Origin: Meta's Folly library.
    //
    // Perf notes: changing delta and buffer size affects how often k*_limit has to be computed so
    // the gains from using a particular scaling function might vary considerably depending on the
    // data pattern, delta and the buffer size. According to the micro-benchmarks run on normally
    // distributed unsorted data with delta = 1000 and buffer_size = 5*delta:
    //    1. The win from using k2 instead of k1 is ~6%.
    //    2. The win from using k0 instead of k2 is ~3%.
    static inline double k0(double q, double delta) {
        return 0.5 * delta * q;
    }
    static inline double k0_inverse(double k, double delta) {
        return 2.0 * k / delta;
    }
    static inline double k0_limit(double q, double delta) {
        return q + 2.0 / delta;
    }

    static inline double k1(double q, double delta) {
        static const double coeff = 1 / (4 * std::asin(1.0));  // 1 / (2 * pi) = 0.159154943...
        return coeff * delta * std::asin(2 * q - 1);
    }
    static inline double k1_inverse(double s, double delta) {
        static const double pi = 2 * std::asin(1.0);
        return 0.5 * (1 + std::sin(2 * pi * s / delta));
    }
    static inline double k1_limit(double q, double delta) {
        return k1_inverse(1 + k1(q, delta), delta);
    }

    static inline double k2(double q, double delta) {
        static const double sqrtOf2_recip = 1 / std::sqrt(2.0);
        if (q <= 0.5) {
            return delta * sqrtOf2_recip * std::sqrt(q);
        }
        return delta - delta * sqrtOf2_recip * std::sqrt(1 - q);
    }
    static inline double k2_inverse(double s, double delta) {
        const double r = s / delta;
        if (r <= 0.5) {
            return 2 * r * r;
        }
        const double b = 1 - r;
        return 1 - 2 * b * b;
    }
    static inline double k2_limit(double q, double delta) {
        return k2_inverse(1 + k2(q, delta), delta);
    }

    // Creates an empty digest.
    explicit TDigest(ScalingFunction k_limit, int delta);

    // Creates a digest from the provided parts. It's the responsibility of the caller to
    // ensure the resulting digest is valid.
    explicit TDigest(int64_t negInfCount,
                     int64_t posInfCount,
                     double min,
                     double max,
                     std::vector<Centroid> centroids,
                     ScalingFunction k_limit,
                     int delta);

    // The ability to merge digests is needed for sharding, but when building a digest on a single
    // node, merging the sorted data directly is faster. The micro-benchmarks show ~8% improvement
    // on a dataset of size 10^7 when merging directly from a buffer (however, this amounts to ~65
    // milliseconds, compared to ~7500 milliseconds of running the accumulator on a collection with
    // 10^7 documents).
    //
    // The paper glosses over _why_ merging would produce a valid t-digest. Essentially, merging
    // shouldn't be able to shift a centroid into an area of a fast growth of the scaling function.
    // Some scaling functions support mergeability and others don't but the paper doesn't seem to
    // formalize the exact criteria for mergeability.
    void merge(const TDigest& other);

    // The input is assumed to be already sorted and not contain NaN or Infinity values. Neither of
    // the assumptions are checked.
    void merge(const std::vector<double>& sorted);

    // Sorts data in the pending buffer and merges it with the prior centroids.
    void flushBuffer();

    const std::vector<Centroid>& centroids() const {
        return _centroids;
    }
    double min() const {
        return (_negInfCount > 0) ? -std::numeric_limits<double>::infinity() : _min;
    }
    double max() const {
        return (_posInfCount > 0) ? std::numeric_limits<double>::infinity() : _max;
    }
    int64_t n() const {
        return _n;
    }
    int64_t negInfCount() const {
        return _negInfCount;
    }
    int64_t posInfCount() const {
        return _posInfCount;
    }

protected:
    // The sizes of centroids are controlled by the scaling function and, conceptually, the
    // algorithm can be implemented using k() alone. However, the scaling functions that allow for
    // more accuracy of extreme percentiles are expensive to compute and the runtime can be
    // improved by using during merge a derived from k() "limit" function instead of k() itself.
    ScalingFunction _k_limit = nullptr;

    // The "compaction parameter". Defines the upper bound on the number of centroids and their
    // sizes (see the specific scaling functions above for details).
    const int _delta = 1000;

    // Buffer for the incoming inputs. When the buffer is full, the inputs are sorted and merged
    // into '_centroids'. The max size is set in constructors to bufferCoeff * delta. The
    // coefficient has been determined empirically from benchmarks.
    static constexpr int bufferCoeff = 3;
    const size_t _maxBufferSize;
    std::vector<double> _buffer;

    // Centroids are ordered by their means. The ordering is maintained during merges.
    std::vector<Centroid> _centroids;

    // The number of inputs that are represented by '_centroids' (the number of incorporated inputs
    // can be higher, if '_buffer' isn't empty and if any infinite values have been encountered).
    int64_t _n = 0;

    // We are tracking infinities separately because, while they can be compared to other doubles
    // with a mathematically expected result, no arithmetics can be done on them.
    int64_t _negInfCount = 0;
    int64_t _posInfCount = 0;

    // Min and max values of the inputs that have been already merged into the centroids. We need to
    // track these to interpolate the values of the extreme centroids and to answer 0.0 and 1.0
    // percentiles accurately. The min and max of all incorporated inputs can be different, if
    // '_buffer' isn't empty.
    double _min = std::numeric_limits<double>::max();
    double _max = std::numeric_limits<double>::min();
};

/**
 * Outputs json-like representation of the given digest, similar to:
 * {n: 6, min: -0.2, max: 9.2, s: 3, centroids: [{w: 2, m: 1.5}, {w: 3, m: 7.81}, {w: 1, m: 9.2}]}
 */
std::ostream& operator<<(std::ostream& os, const TDigest& tdigest);
std::ostream& operator<<(std::ostream& os, const TDigest::Centroid& centroid);

}  // namespace mongo
