/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <cmath>

#include "mongo/db/jsobj.h"
#include "mongo/util/assert_util.h"

/**
 * These classes provide online descriptive statistics estimator capable
 * of computing the mean, standard deviation and quantiles.
 * Exactness is traded for the ability to obtain reasonable estimates
 * without the need to store all the samples or perform multiple passes
 * over the data.
 *
 * NOTEs on the estimator accessors provide information about accuracy
 * of the approximation.
 *
 * The implementation of the estimators is heavily inspired by the algorithms used in
 * boost.accumulators (www.boost.org/libs/accumulators/).
 * It differs by being tailored for typical descriptive statistics use cases
 * thus providing a simpler (even though less flexible) interface.
 */
namespace mongo {

    /**
     * Collects count, minimum and maximum, calculates mean and standard deviation.
     *
     * The 'Sample' template parameter is the type of the samples. It does not affect the calculated
     * mean and standard deviation as all values are converted to double. 
     * However, setting the correct sample type prevents unnecessary casting or precision loss
     * for min and max.
     */
    template <class Sample>
    class BasicEstimators {
    public:
        BasicEstimators();

        /**
         * Update estimators with another observed value.
         */
        BasicEstimators& operator <<(const Sample sample);

        /**
         * @return number of observations so far
         */
        inline size_t count() const { return _count; }

        /**
         * @return mean of the observations seen so far
         * NOTE: exact (within the limits of IEEE floating point precision).
         */
        inline double mean() const { return _sum / _count; }

        /**
         * @return standard deviation of the observations so far
         * NOTE: exact (within the limits of IEEE floating point precision).
         */
        inline double stddev() const { return std::sqrt(_diff / _count); }

        /**
         * @return minimum observed value so far
         * NOTE: exact.
         */
        inline Sample min() const { return _min; }

        /**
         * @return maximum observed value so far
         * NOTE: exact.
         */
        inline Sample max() const { return _max; }

        /**
         * Appends the basic estimators to the provided BSONObjBuilder.
         */
        void appendBasicToBSONObjBuilder(BSONObjBuilder& b) const;

    private:
        size_t _count;
        double _sum;
        double _diff; // sum of squares of differences from the (then-current) mean
        Sample _min;
        Sample _max;
    };

    /**
     * Computes 'NumQuantiles' quantiles.
     *
     * The quantiles at probability 0 and 1 (minimum and maximum observations) are always computed.
     * Thus DistributionEstimators<3> computes the the 1st, 2nd and 3rd quartiles (probabilities
     * .25, .50, .75) and the default 0th and 5th (min and max).
     *
     * The quantile estimators are mean square consistent (they become a better approximation of the
     * actual quantiles as the sample size grows).
     */
    template <std::size_t NumQuantiles>
    class DistributionEstimators {
    public:
        DistributionEstimators();

        DistributionEstimators& operator <<(const double sample);

        /**
         * Number of computed quantiles, excluding minimum and maximum.
         */
        static const size_t numberOfQuantiles = NumQuantiles;

        /**
         * Updates the estimators with another observed value.
         */
        inline double quantile(std::size_t i) const {
            massert(16476, "the requested value is out of the range of the computed quantiles",
                    i <= NumQuantiles + 1);
            return this->_heights[2 * i];
        }

        /**
         * @return true when enough value has been observed to output sensible quantiles
         */
        inline bool quantilesReady() const {
            return _count >= NumMarkers;
        }

        /**
         * @return estimated minimum
         *
         * NOTE: use SimpleEstimators::min for an exact value.
         */
        inline double min() const {
            return quantile(0);
        }

        /**
         * @return estimated maximum
         *
         * NOTE: use SimpleEstimators::max for an exact value.
         */
        inline double max() const {
            return quantile(NumQuantiles + 1);
        }

        /**
         * @return estimated median (2nd quartile)
         */
        inline double median() const {
            return icdf(.5);
        }

        /**
         * @return probability associated with the i-th quantile
         */
        inline double probability(std::size_t i) const {
            return i * 1. / (NumQuantiles + 1);
        }

        /**
         * @return value for the nearest available quantile for probability 'prob'
         */
        inline double icdf(double prob) const {
            int quant = static_cast<int>(prob * (NumQuantiles + 1) + 0.5);
            return quantile(quant);
        }

        /**
         * Appends the quantiles to the provided BSONArrayBuilder.
         * REQUIRES e.quantilesReady() == true
         */
        void appendQuantilesToBSONArrayBuilder(BSONArrayBuilder& arr) const;

    private:
        inline double _positions_increments(std::size_t i) const;

        int _count;
        enum { NumMarkers = 2 * NumQuantiles + 3 };
        double _heights[NumMarkers];              // q_i
        double _actual_positions[NumMarkers];     // n_i
        double _desired_positions[NumMarkers];    // d_i
    };

    /**
     * Provides the funcionality of both BasicEstimators and DistributionEstimators.
     */
    template <class Sample, std::size_t NumQuantiles>
    class SummaryEstimators :
            // Multiple-inheritance
            public BasicEstimators<Sample>,
            public DistributionEstimators<NumQuantiles> {
    public:
        // Dispatch samples to the inherited estimators
        inline SummaryEstimators& operator<<(const Sample sample) {
            this->BasicEstimators<Sample>::operator<<(sample);
            this->DistributionEstimators<NumQuantiles>::operator<<(sample);
            return *this;
        }

        // Expose the exact values
        inline Sample min() const {
            return this->BasicEstimators<Sample>::min();
        }

        inline Sample max() const {
            return this->BasicEstimators<Sample>::max();
        }

        /**
         * @return a summary of the computed estimators as a BSONObj.
         */
        BSONObj statisticSummaryToBSONObj() const;
    };

} // namespace mongo

#include "mongo/util/descriptive_stats-inl.h"
