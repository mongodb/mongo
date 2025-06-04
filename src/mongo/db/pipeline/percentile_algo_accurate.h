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

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/percentile_algo.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/platform/compiler.h"

#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * 'AccuratePercentile' class for common functionality between discrete and continuous
 * percentiles
 */
class AccuratePercentile : public PercentileAlgorithm, public PartialPercentile<Value> {

public:
    AccuratePercentile() = default;  // no config required for this algorithm

    MONGO_COMPILER_ALWAYS_INLINE_GCC14 void incorporate(double input) final {
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

    void incorporate(const std::vector<double>& inputs) final;

    long memUsageBytes() const final {
        return _accumulatedValues.capacity() * sizeof(double);
    }

    Value serialize() final;

    void combine(const Value& partial) final;

    std::vector<double> computePercentiles(const std::vector<double>& ps) final;

    /*
     * Accurate percentile implementations may need to spill to disk if the amount of data involved
     * exceeds AccumulatorPercentile's memory limit.
     */
    void spill() final;

protected:
    void reset() override;

    void emptyMemory();

    virtual boost::optional<double> computeSpilledPercentile(double p) = 0;

    std::vector<double> _accumulatedValues;

    // While infinities do compare and sort correctly, no arithmetics can be done on them so, to
    // allow for implementing quick select in the future and to align with our implementation of
    // t-digest, we are tracking them separately.
    int _negInfCount = 0;
    int _posInfCount = 0;

    bool _shouldSort = true;

    // Only used if the accumulator needs to spill to disk after $group spills are merged together.
    // File where spilled data will be written to disk.
    std::unique_ptr<SorterFileStats> _spillStats;
    std::shared_ptr<Sorter<Value, Value>::File> _spillFile;

    // Vector of file iterators tracking each sorted segment that is written to disk when we spill.
    std::vector<std::shared_ptr<Sorter<Value, Value>::Iterator>> _spilledSortedSegments;

    // Sorter iterator used to merge sorted segments back together in sorted order.
    std::unique_ptr<Sorter<Value, Value>::Iterator> _sorterIterator;

    // Position of next value in sorted spill.
    int _indexNextSorted = 0;

    // Total number of values written to disk.
    int _numTotalValuesSpilled = 0;

    ExpressionContext* _expCtx;
};

}  // namespace mongo
