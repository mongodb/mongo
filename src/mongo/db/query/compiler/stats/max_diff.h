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

#pragma once

#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/db/query/compiler/stats/scalar_histogram.h"
#include "mongo/db/query/compiler/stats/value_utils.h"

#include <cstddef>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace mongo::stats {

constexpr double kInvalidArea = -1.0;
constexpr double kDoubleInf = std::numeric_limits<double>::infinity();

// TODO (SERVER-92732): change the name from ValFreq to ValStats
struct ValFreq {
    ValFreq(size_t idx, size_t freq)
        : _idx(idx),
          _freq(freq),
          _area(kInvalidArea),
          _areaDiff(kInvalidArea),
          _freqDiff(kInvalidArea),
          _normArea(kInvalidArea),
          _normAreaDiff(kInvalidArea) {}

    std::string toString() const {
        std::ostringstream os;
        os << "idx: " << _idx << ", freq: " << _freq << ", area: " << _area
           << ", areaDiff:" << _areaDiff << ", freqDiff:" << _freqDiff
           << ", normArea: " << _normArea << ", normAreaDiff: " << _normAreaDiff;
        return os.str();
    }

    size_t _idx;           // Original index according to value order.
    size_t _freq;          // Frequency of the value.
    double _area;          // Derived as: spread * frequency
    double _areaDiff;      // Difference of two neighboring areas
    double _freqDiff;      // Difference of two neighboring frequencies
    double _normArea;      // Area normalized to the maximum in a type class.
    double _normAreaDiff;  // Area difference normalized to the maximum difference in a type class.
};

/**
 * Store information on data distribution in the form of bucket bounds and associated statistics.
 */
// TODO (SERVER-92732): modify DataDistribution to use one array of buckets which contain the bound
// and statistics information.
struct DataDistribution {
    // TODO (SERVER-92732): rename _bounds to _bucketBounds.
    std::vector<SBEValue> _bounds;  // Tag/value pair for each potential (left) bound of a bucket
    std::vector<ValFreq> _freq;     // Statistics associated with each bound
    // The min/max areas of each type class. The key is the index of the last element of the class.
    // The value is a pair of maximum area, and maximum area difference.
    std::map<size_t, std::pair<double /* area */, double /*area diff*/>> typeClassBounds;
};

/**
 * Given a set of sorted values (and their frequencies), optimal bucket boundaries can be determined
 * based on either area (spread * freq) or area difference. SortArg encapsulates the two possible
 * options and can be used as an argument in the functions used to generate histogram boundaries.
 * Functions below use kArea as a default SortArg value.
 */
enum class SortArg {
    kArea,      // Sorting on kArea: choose (numBuckets - 1) largest areas to determine bucket
                // boundaries
    kAreaDiff,  // Sorting on kAreaDiff: choose (numBuckets - 1) largest difference in areas to
                // determine bucket boundaries
    kFreqDiff   // Sorting on kFreqDiff: choose (numBuckets - 1) largest difference in value
                // frequency to determine bucket boundaries
};


/**
 * Given a data distribution, number of buckets, and a sort argument for priority queue, generate
 * buckets with numBuckets largest areas or area differences (depending on the value of sortArg)
 */
std::vector<ValFreq> generateTopKBuckets(const DataDistribution& dataDistribution,
                                         size_t numBuckets,
                                         SortArg sortArg = SortArg::kAreaDiff);

/**
 * Given a set of values sorted in BSON order, generate a data distribution consisting of counts for
 * each value with the values in sorted order
 */
DataDistribution getDataDistribution(const std::vector<SBEValue>& sortedInput);

/**
 * Given a data distribution, generate a scalar histogram with the supplied number of buckets
 */
ScalarHistogram genMaxDiffHistogram(const DataDistribution& dataDistribution,
                                    size_t numBuckets,
                                    SortArg sortArg = SortArg::kAreaDiff);

/**
 * Given a vector containing SBEValues, generate a set of statistics to summarize the supplied
 * data. Histograms will use the supplied number of buckets.
 */
std::shared_ptr<const CEHistogram> createCEHistogram(const std::vector<SBEValue>& arrayData,
                                                     size_t numBuckets,
                                                     SortArg sortArg = SortArg::kAreaDiff);

}  // namespace mongo::stats
