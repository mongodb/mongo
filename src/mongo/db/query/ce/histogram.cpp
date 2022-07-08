/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/ce/histogram.h"

namespace mongo::ce {

using namespace sbe;

Bucket::Bucket(
    double equalFreq, double rangeFreq, double cumulativeFreq, double ndv, double cumulativeNDV)
    : _equalFreq(equalFreq),
      _rangeFreq(rangeFreq),
      _cumulativeFreq(cumulativeFreq),
      _ndv(ndv),
      _cumulativeNDV(cumulativeNDV) {
    uassert(6695702, "Invalid equalFreq", _equalFreq >= 0.0);
    uassert(6695703, "Invalid rangeFreq", _rangeFreq >= 0.0);
    uassert(6695704, "Invalid ndv", _ndv <= _rangeFreq);
    uassert(6695705, "Invalid cumulative frequency", _cumulativeFreq >= _equalFreq + _rangeFreq);
    uassert(6695706, "Invalid cumulative ndv", _cumulativeNDV >= _ndv + 1.0);
}

const std::string Bucket::toString() const {
    std::ostringstream os;
    os << "equalFreq: " << _equalFreq << ", rangeFreq: " << _rangeFreq
       << ", cumulativeFreq: " << _cumulativeFreq << ", ndv: " << _ndv
       << ", cumulativeNDV: " << _cumulativeNDV;
    return os.str();
}

Histogram::Histogram() : Histogram({}, {}) {}

Histogram::Histogram(value::Array bounds, std::vector<Bucket> buckets)
    : _bounds(std::move(bounds)), _buckets(std::move(buckets)) {
    uassert(6695707, "Invalid sizes", bounds.size() == buckets.size());
}

const std::string Histogram::toString() const {
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < _buckets.size(); i++) {
        os << "{val: " << _bounds.getAt(i) << ", " << _buckets.at(i).toString() << "}";
        if (_buckets.size() - i > 1)
            os << ",";
    }
    os << "]";
    return os.str();
}

const std::string Histogram::plot() const {
    std::ostringstream os;
    double maxFreq = 0;
    const double maxBucketSize = 100;

    for (const auto& bucket : _buckets) {
        double maxBucketFreq = std::max(bucket._equalFreq, bucket._rangeFreq);
        maxFreq = std::max(maxFreq, maxBucketFreq);
    }

    std::vector<std::pair<double, std::string>> headers;
    size_t maxHeaderSize = 0;
    for (size_t i = 0; i < _buckets.size(); ++i) {
        std::ostringstream rngHeader;
        std::ostringstream eqlHeader;
        double scaledRngF = maxBucketSize * _buckets[i]._rangeFreq / maxFreq;
        double scaledEqlF = maxBucketSize * _buckets[i]._equalFreq / maxFreq;
        rngHeader << _bounds.getAt(i) << ": " << _buckets[i]._rangeFreq;
        eqlHeader << _bounds.getAt(i) << ": " << _buckets[i]._equalFreq;
        auto rngStr = rngHeader.str();
        maxHeaderSize = std::max(maxHeaderSize, rngStr.size());
        headers.emplace_back(scaledRngF, rngStr);
        auto eqlStr = eqlHeader.str();
        maxHeaderSize = std::max(maxHeaderSize, eqlStr.size());
        headers.emplace_back(scaledEqlF, eqlStr);
    }

    const std::string maxLine(maxBucketSize + maxHeaderSize + 3, '-');
    os << maxLine << "\n";
    for (size_t j = 0; j < headers.size(); ++j) {
        auto header = headers.at(j);
        header.second.resize(maxHeaderSize, ' ');
        const std::string bar(std::round(header.first), '*');
        os << header.second << " | " << bar << "\n";
    }
    os << maxLine << "\n";

    return os.str();
}

EstimationResult Histogram::getTotals() const {
    if (_buckets.empty()) {
        return {0.0, 0.0};
    }

    const Bucket& last = _buckets.back();
    return {last._cumulativeFreq, last._cumulativeNDV};
}

EstimationResult Histogram::estimate(value::TypeTags tag,
                                     value::Value val,
                                     EstimationType type) const {
    switch (type) {
        case EstimationType::kGreater:
            return getTotals() - estimate(tag, val, EstimationType::kLessOrEqual);

        case EstimationType::kGreaterOrEqual:
            return getTotals() - estimate(tag, val, EstimationType::kLess);

        default:
            // Continue.
            break;
    }

    size_t bucketIndex = 0;
    {
        size_t len = _buckets.size();
        while (len > 0) {
            const size_t half = len >> 1;
            const auto [boundTag, boundVal] = _bounds.getAt(bucketIndex + half);

            if (compareValues3w(boundTag, boundVal, tag, val) < 0) {
                bucketIndex += half + 1;
                len -= half + 1;
            } else {
                len = half;
            }
        }
    }
    if (bucketIndex == _buckets.size()) {
        // Value beyond the largest endpoint.
        switch (type) {
            case EstimationType::kEqual:
                return {0.0, 0.0};

            case EstimationType::kLess:
            case EstimationType::kLessOrEqual:
                return getTotals();

            default:
                MONGO_UNREACHABLE;
        }
    }

    const Bucket& bucket = _buckets.at(bucketIndex);
    const auto [boundTag, boundVal] = _bounds.getAt(bucketIndex);
    const bool isEndpoint = compareValues3w(boundTag, boundVal, tag, val) == 0;

    switch (type) {
        case EstimationType::kEqual: {
            if (isEndpoint) {
                return {bucket._equalFreq, 1.0};
            }
            return {(bucket._ndv == 0.0) ? 0.0 : bucket._rangeFreq / bucket._ndv, 1.0};
        }

        case EstimationType::kLess: {
            double resultCard = bucket._cumulativeFreq - bucket._equalFreq;
            double resultNDV = bucket._cumulativeNDV - 1.0;

            if (!isEndpoint) {
                // TODO: consider value interpolation instead of assigning 50% of the weight.
                resultCard -= bucket._rangeFreq / 2.0;
                resultNDV -= bucket._ndv / 2.0;
            }
            return {resultCard, resultNDV};
        }

        case EstimationType::kLessOrEqual: {
            double resultCard = bucket._cumulativeFreq;
            double resultNDV = bucket._cumulativeNDV;

            if (!isEndpoint) {
                // TODO: consider value interpolation instead of assigning 50% of the weight.
                resultCard -= bucket._equalFreq + bucket._rangeFreq / 2.0;
                resultNDV -= 1.0 + bucket._ndv / 2.0;
            }
            return {resultCard, resultNDV};
        }

        default:
            MONGO_UNREACHABLE;
    }
}

const value::Array& Histogram::getBounds() const {
    return _bounds;
}

const std::vector<Bucket>& Histogram::getBuckets() const {
    return _buckets;
}

}  // namespace mongo::ce
