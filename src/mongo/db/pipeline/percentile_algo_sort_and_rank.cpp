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
#include <cmath>
#include <vector>

#include "mongo/db/pipeline/percentile_algo.h"

#include "mongo/db/exec/document_value/value.h"

namespace mongo {

/**
 * 'SortAndRank' algorithm for computing percentiles is accurate and doesn't require specifying a
 * percentile in advance but is only suitable for small datasets. It accumulats all data sent to it
 * and sorts it all when a percentile is requested. Requesting more percentiles after the first one
 * without incorporating more data is fast as it doesn't need to sort again.
 */
class DiscreteSortAndRank : public PercentileAlgorithm {
public:
    DiscreteSortAndRank() = default;  // no config required for this algorithm

    void incorporate(double input) final {
        // Take advantage of already sorted input -- avoid resorting it later.
        if (!_shouldSort && !_accumulatedValues.empty() && input < _accumulatedValues.back()) {
            _shouldSort = true;
        }

        _accumulatedValues.push_back(input);
    }
    void incorporate(const std::vector<double>& inputs) final {
        _shouldSort = true;
        _accumulatedValues.reserve(_accumulatedValues.size() + inputs.size());
        _accumulatedValues.insert(_accumulatedValues.end(), inputs.begin(), inputs.end());
    }

    boost::optional<double> computePercentile(double p) final {
        if (_accumulatedValues.empty()) {
            return boost::none;
        }

        if (_shouldSort) {
            std::sort(_accumulatedValues.begin(), _accumulatedValues.end());
            _shouldSort = false;
        }

        const double n = _accumulatedValues.size();
        const size_t rank = static_cast<size_t>(std::max<double>(0, std::ceil(p * n) - 1));
        return _accumulatedValues[rank];
    }

    long memUsageBytes() const final {
        return _accumulatedValues.capacity() * sizeof(double);
    }

protected:
    std::vector<double> _accumulatedValues;
    bool _shouldSort = true;
};

class DiscreteSortAndRankDistributedClassic : public DiscreteSortAndRank,
                                              public PartialPercentile<Value> {
public:
    Value serialize() final {
        if (_shouldSort) {
            std::sort(_accumulatedValues.begin(), _accumulatedValues.end());
            _shouldSort = false;
        }

        return Value(std::vector<Value>(_accumulatedValues.begin(), _accumulatedValues.end()));
    }

    // 'partial' should be a sorted array created by 'serialize()'
    void combine(const Value& partial) final {
        if (_shouldSort) {
            std::sort(_accumulatedValues.begin(), _accumulatedValues.end());
            _shouldSort = false;
        }

        std::vector<double> other;
        other.reserve(partial.getArrayLength());
        for (const auto& v : partial.getArray()) {
            other.push_back(v.coerceToDouble());
        }

        std::vector<double> temp;
        temp.reserve(_accumulatedValues.size() + other.size());
        std::merge(_accumulatedValues.begin(),
                   _accumulatedValues.end(),
                   other.begin(),
                   other.end(),
                   std::back_inserter(temp));

        _accumulatedValues.swap(temp);
    }
};

std::unique_ptr<PercentileAlgorithm> createDiscreteSortAndRank() {
    return std::make_unique<DiscreteSortAndRank>();
}

std::unique_ptr<PercentileAlgorithm> createDiscreteSortAndRankDistributedClassic() {
    return std::make_unique<DiscreteSortAndRankDistributedClassic>();
}

}  // namespace mongo
