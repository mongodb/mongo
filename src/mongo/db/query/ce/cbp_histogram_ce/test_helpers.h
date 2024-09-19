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

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/ce/cbp_histogram_ce/histogram_common.h"
#include "mongo/db/query/stats/array_histogram.h"
#include "mongo/db/query/stats/value_utils.h"

namespace mongo::optimizer::cbp::ce {

/**
 * Test utility for helping with creation of manual histograms in the unit tests.
 */
struct BucketData {
    Value _v;
    double _equalFreq;
    double _rangeFreq;
    double _ndv;

    BucketData(Value v, double equalFreq, double rangeFreq, double ndv)
        : _v(v), _equalFreq(equalFreq), _rangeFreq(rangeFreq), _ndv(ndv) {}
    BucketData(const std::string& v, double equalFreq, double rangeFreq, double ndv)
        : BucketData(Value(v), equalFreq, rangeFreq, ndv) {}
    BucketData(int v, double equalFreq, double rangeFreq, double ndv)
        : BucketData(Value(v), equalFreq, rangeFreq, ndv) {}
};

stats::ScalarHistogram createHistogram(const std::vector<BucketData>& data);

double estimateCardinalityScalarHistogramInteger(const stats::ScalarHistogram& hist,
                                                 int v,
                                                 cbp::ce::EstimationType type);
}  // namespace mongo::optimizer::cbp::ce
