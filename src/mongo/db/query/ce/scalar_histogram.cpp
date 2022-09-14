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

#include "mongo/db/query/ce/scalar_histogram.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"

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

std::string Bucket::toString() const {
    std::ostringstream os;
    os << "equalFreq: " << _equalFreq << ", rangeFreq: " << _rangeFreq
       << ", cumulativeFreq: " << _cumulativeFreq << ", ndv: " << _ndv
       << ", cumulativeNDV: " << _cumulativeNDV;
    return os.str();
}

ScalarHistogram::ScalarHistogram() : ScalarHistogram({}, {}) {}

ScalarHistogram::ScalarHistogram(std::vector<StatsBucket> buckets) {

    for (auto bucket : buckets) {
        Bucket b(bucket.getBoundaryCount(),
                 bucket.getRangeCount(),
                 bucket.getCumulativeCount(),
                 bucket.getRangeDistincts(),
                 bucket.getCumulativeDistincts());
        _buckets.push_back(std::move(b));
        auto value = sbe::bson::convertFrom<1>(bucket.getUpperBoundary().getElement());
        _bounds.push_back(value.first, value.second);
    }
}

ScalarHistogram::ScalarHistogram(value::Array bounds, std::vector<Bucket> buckets)
    : _bounds(std::move(bounds)), _buckets(std::move(buckets)) {
    uassert(6695707, "Invalid sizes", bounds.size() == buckets.size());
}

std::string ScalarHistogram::toString() const {
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

std::string ScalarHistogram::plot() const {
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

const value::Array& ScalarHistogram::getBounds() const {
    return _bounds;
}

const std::vector<Bucket>& ScalarHistogram::getBuckets() const {
    return _buckets;
}

}  // namespace mongo::ce
