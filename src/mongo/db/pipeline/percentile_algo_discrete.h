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

#include <vector>

#include "mongo/db/pipeline/percentile_algo.h"

namespace mongo {

/**
 * 'DiscretePercentile' algorithm for computing percentiles is accurate and doesn't require
 * specifying a percentile in advance but is only suitable for small datasets. It accumulats all
 * data sent to it and sorts it all when a percentile is requested. Requesting more percentiles
 * after the first one without incorporating more data is fast as it doesn't need to sort again.
 */
class DiscretePercentile : public PercentileAlgorithm {
public:
    DiscretePercentile() = default;  // no config required for this algorithm

    void incorporate(double input) final;
    void incorporate(const std::vector<double>& inputs) final;

    boost::optional<double> computePercentile(double p) final;
    std::vector<double> computePercentiles(const std::vector<double>& ps) final;

    long memUsageBytes() const final {
        return _accumulatedValues.capacity() * sizeof(double);
    }

protected:
    std::vector<double> _accumulatedValues;

    // While infinities do compare and sort correctly, no arithmetics can be done on them so, to
    // allow for implementing quick select in the future and to align with our implementation of
    // t-digest, we are tracking them separately.
    int _negInfCount = 0;
    int _posInfCount = 0;

    bool _shouldSort = true;
};

}  // namespace mongo
