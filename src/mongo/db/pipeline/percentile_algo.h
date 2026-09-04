// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cmath>
#include <memory>

#include <boost/optional/optional.hpp>
namespace mongo {

// Computes the 0-based rank for discrete percentile 'p' on a dataset of 'n' values.
//
// We define "percentile" as: value 'P' such that at least ceil(p*n) samples are _less or equal_
// to 'P' and no more than ceil(p*n) samples are strictly _less_ than 'P'. Thus p=0 maps to the
// min and p=1 maps to the max. Ambiguity (e.g. D={1,2,...,10}, P(0.1) in [1,2]) is resolved
// towards the lower rank.
//
// Used by both DiscretePercentile and TDigest, which share this definition.
inline int computeDiscreteRank(int n, double p) {
    if (p >= 1.0) {
        return n - 1;
    }
    const auto ceilRank = std::ceil(n * p);
    // 'p' is validated finite and within [0, 1] in parseP(), so 'ceilRank' is an exact
    // non-negative int. Keep the impossible non-finite case loud (as representAsChecked did)
    // instead of silently returning 0, but without its optional round-trip on this hot path.
    tassert(13448900,
            "non-finite percentile rank computed; 'p' must be validated to [0, 1] upstream",
            std::isfinite(ceilRank));
    return std::max(0, static_cast<int>(ceilRank) - 1);
}

/**
 * Eventually we'll be supporting multiple types of percentiles (discrete, continuous, approximate)
 * and potentially multiple different algorithms for computing the approximate ones.
 *
 * The goal is to keep these algorithms MQL- and engine-agnostic with this interface.
 */
struct PercentileAlgorithm {

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

    /*
     * Provides an interface if the percentile cannot be computed in memory and needs to access
     * disk.
     */
    virtual void spill() = 0;

    /* Provides an interface for resetting algorithm object to newly intialized state, for
     * implementations that may need to do so without destroying the object.*/
    virtual void reset() = 0;
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

std::unique_ptr<PercentileAlgorithm> createTDigest();
std::unique_ptr<PercentileAlgorithm> createTDigestDistributedClassic();
std::unique_ptr<PercentileAlgorithm> createDiscretePercentile(ExpressionContext* expCtx);
std::unique_ptr<PercentileAlgorithm> createContinuousPercentile(ExpressionContext* expCtx);

}  // namespace mongo
