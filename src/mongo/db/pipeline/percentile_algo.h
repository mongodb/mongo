// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/util/modules.h"

#include <cmath>
#include <memory>

#include <boost/optional/optional.hpp>
namespace mongo {

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
