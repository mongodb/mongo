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

#include "mongo/db/query/compiler/stats/scalar_histogram.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <utility>

namespace mongo::stats {
namespace {
void validate(const sbe::value::Array& bounds, const std::vector<Bucket>& buckets) {
    uassert(6695707,
            "ScalarHistogram buckets and bounds must have equal sizes.",
            bounds.size() == buckets.size());

    // Pair-wise comparison validating that bounds are unique and sorted in increasing order.
    size_t l = 0;
    for (size_t r = 1; r < bounds.size(); r++) {
        const auto [lt, lv] = bounds.getAt(l);
        const auto [rt, rv] = bounds.getAt(r);
        const auto comp = compareValues(lt, lv, rt, rv);
        if (comp > 0) {
            uasserted(7131006, "Scalar histogram must have sorted bound values");
        } else if (comp == 0) {
            uasserted(7131007, "Scalar histogram must have unique bound values");
        }
        l = r;
    }

    // Validate buckets.
    double cumulFreq = 0.0;
    double cumulNdv = 0.0;
    for (const auto& b : buckets) {
        // Validate bucket fields.
        uassert(6695702, "Invalid equalFreq", b._equalFreq >= 0.0);
        uassert(6695703, "Invalid rangeFreq", b._rangeFreq >= 0.0);
        uassert(6695704, "Invalid ndv", b._ndv <= b._rangeFreq);
        uassert(6695705,
                "Invalid cumulative frequency",
                b._cumulativeFreq >= b._equalFreq + b._rangeFreq);
        uassert(6695706, "Invalid cumulative ndv", b._cumulativeNDV >= b._ndv + 1.0);

        // Validate that cumulative frequency of each bucket is sum of preceding frequencies.
        cumulFreq += b._equalFreq + b._rangeFreq;
        if (cumulFreq != b._cumulativeFreq) {
            uasserted(7131008,
                      str::stream() << "Cumulative ndv of bucket " << b._cumulativeFreq
                                    << " is invalid, expecting " << cumulFreq);
        }

        // Validate that cumulative ndv of each bucket is sum of preceding ndvs (including bounds).
        cumulNdv += b._ndv + 1.0;
        if (cumulNdv != b._cumulativeNDV) {
            uasserted(7131009,
                      str::stream() << "Cumulative ndv of bucket " << b._cumulativeNDV
                                    << " is invalid, expecting " << cumulNdv);
        }
    }
}
}  // namespace

Bucket::Bucket(
    double equalFreq, double rangeFreq, double cumulativeFreq, double ndv, double cumulativeNDV)
    : _equalFreq(equalFreq),
      _rangeFreq(rangeFreq),
      _cumulativeFreq(cumulativeFreq),
      _ndv(ndv),
      _cumulativeNDV(cumulativeNDV) {}

std::string Bucket::toString() const {
    std::ostringstream os;
    os << "equalFreq: " << _equalFreq << ", rangeFreq: " << _rangeFreq
       << ", cumulativeFreq: " << _cumulativeFreq << ", ndv: " << _ndv
       << ", cumulativeNDV: " << _cumulativeNDV;
    return os.str();
}

std::string Bucket::dump() const {
    std::ostringstream os;
    os << _equalFreq << ", " << _rangeFreq << ", " << _ndv;
    return os.str();
}

BSONObj Bucket::serialize() const {
    BSONObjBuilder bob;
    bob.appendNumber("boundaryCount", _equalFreq);
    bob.appendNumber("rangeCount", _rangeFreq);
    bob.appendNumber("rangeDistincts", _ndv);
    bob.appendNumber("cumulativeCount", _cumulativeFreq);
    bob.appendNumber("cumulativeDistincts", _cumulativeNDV);
    bob.doneFast();
    return bob.obj();
}

ScalarHistogram ScalarHistogram::make() {
    return ScalarHistogram();
}

ScalarHistogram ScalarHistogram::make(const StatsHistogram& histogram) {
    std::vector<Bucket> buckets;
    for (const auto& bucket : histogram.getBuckets()) {
        Bucket b(bucket.getBoundaryCount(),
                 bucket.getRangeCount(),
                 bucket.getCumulativeCount(),
                 bucket.getRangeDistincts(),
                 bucket.getCumulativeDistincts());
        buckets.push_back(std::move(b));
    }

    sbe::value::Array bounds;
    for (const auto& bound : histogram.getBounds()) {
        // We cannot insert a view here, because the lifetime of the of the bound is shorter than
        // that of the histogram. In the case of a larger type, e.g. BigString/bsonString, we need
        // to copy over the entire string as well, not just a pointer to memory which may be
        // deallocated before we need it.
        auto value = sbe::bson::convertFrom<false>(bound.getElement());
        bounds.push_back(value.first, value.second);
    }

    // Note: no need to validate this histogram when its being loaded from the stats collection, as
    // it was already validated at creation time.
    return ScalarHistogram(std::move(bounds), std::move(buckets));
}

ScalarHistogram ScalarHistogram::make(sbe::value::Array bounds,
                                      std::vector<Bucket> buckets,
                                      bool doValidation) {
    if (doValidation) {
        validate(bounds, buckets);
    }
    return ScalarHistogram(std::move(bounds), std::move(buckets));
}

ScalarHistogram::ScalarHistogram() : ScalarHistogram({}, {}) {}

ScalarHistogram::ScalarHistogram(sbe::value::Array bounds, std::vector<Bucket> buckets)
    : _bounds(std::move(bounds)), _buckets(std::move(buckets)) {}

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

const sbe::value::Array& ScalarHistogram::getBounds() const {
    return _bounds;
}

const std::vector<Bucket>& ScalarHistogram::getBuckets() const {
    return _buckets;
}

BSONObj ScalarHistogram::serialize() const {
    BSONObjBuilder histogramBuilder;

    // Construct bucket BSON.
    auto buckets = getBuckets();
    BSONArrayBuilder bucketsBuilder(histogramBuilder.subarrayStart("buckets"));
    for (const auto& bucket : buckets) {
        bucketsBuilder.append(bucket.serialize());
    }
    bucketsBuilder.doneFast();

    // Construct bucket bounds BSON.
    auto bounds = getBounds();
    BSONArrayBuilder boundsBuilder(histogramBuilder.subarrayStart("bounds"));
    sbe::bson::convertToBsonArr(boundsBuilder, &bounds);
    boundsBuilder.doneFast();

    histogramBuilder.doneFast();
    return histogramBuilder.obj();
}

}  // namespace mongo::stats
