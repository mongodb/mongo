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
#pragma once

#include <boost/optional/optional.hpp>
#include <vector>

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/percentile_algo.h"

namespace mongo {

/**
 * 'AccuratePercentile' class for common functionality between discrete and continuous
 * percentiles
 */
class AccuratePercentile : public PercentileAlgorithm, public PartialPercentile<Value> {

public:
    AccuratePercentile() = default;  // no config required for this algorithm

    void incorporate(double input) final;
    void incorporate(const std::vector<double>& inputs) final;

    long memUsageBytes() const final {
        return _accumulatedValues.capacity() * sizeof(double);
    }

    Value serialize() final;

    void combine(const Value& partial) final;

    std::vector<double> computePercentiles(const std::vector<double>& ps) final;

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
