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

#include <boost/optional/optional.hpp>
#include <cmath>
#include <memory>

namespace mongo {

/**
 * Eventually we'll be supporting multiple types of percentiles (discrete, continuous, approximate)
 * and potentially multiple different algorithms for computing the approximate ones.
 *
 * The goal is to keep these algorithms MQL- and engine-agnostic with this interface.
 */
struct PercentileAlgorithm {
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

    virtual ~PercentileAlgorithm() {}

    virtual void incorporate(double input) = 0;
    virtual void incorporate(const std::vector<double>& inputs) = 0;

    /**
     * 'p' must be from [0, 1] range.
     *
     * It is always valid to call 'computePercentile()', however, if no input has been incorporated
     * yet, all implementations must return 'boost::none'. It is allowed to incorporate more inputs
     * after calling 'computePercentile()' and call it again (naturally, the result might differ
     * depending on the new data).
     *
     * Note 1: the implementations are free to either return "none" or throw if they require setting
     * up for computing a specific percentile but a different one is requested here.
     *
     * Note 2: the implementations might have different tradeoffs regarding balancing performance of
     * ingress vs computing the percentile, so this interface provides no perfomance guarantees.
     * Refer to the documentation of the specific implementations for details.
     */
    virtual boost::optional<double> computePercentile(double p) = 0;

    /**
     * Computes multiple percentiles at once and might be more efficient than computing them one at
     * a time. Same constraints apply as for 'computePercentile(double p)'. Returns an empty vector
     * if no inputs have been incorporated.
     */
    virtual std::vector<double> computePercentiles(const std::vector<double>& ps) = 0;

    /*
     * The owner might need a rough estimate of how much memory the algorithm is using.
     */
    virtual long memUsageBytes() const = 0;
};

/**
 * In sharded environment percentiles need to be partially computed on each shard and then combined
 * together to compute the final result. 'TValue' type used to communicate the partial computation
 * depends on the engine.
 */
template <typename TValue>
struct PartialPercentile {
    virtual TValue serialize() = 0;
    virtual void combine(const TValue& partial) = 0;
};

/**
 * Factory methods for instantiating concrete algorithms.
 */
std::unique_ptr<PercentileAlgorithm> createDiscretePercentile();

std::unique_ptr<PercentileAlgorithm> createTDigest();
std::unique_ptr<PercentileAlgorithm> createTDigestDistributedClassic();

}  // namespace mongo
