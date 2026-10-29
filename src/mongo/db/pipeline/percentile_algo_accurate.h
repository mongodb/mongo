// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/percentile_algo.h"
#include "mongo/db/sorter/file.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

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
    std::shared_ptr<sorter::File> _spillFile;

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
