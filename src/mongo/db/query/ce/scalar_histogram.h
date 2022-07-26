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

#include <string>
#include <utility>
#include <vector>

#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::ce {

/**
 * Statistics related to a single ScalarHistogram bucket. The boundary value is kept in a separate
 * array, so that each bucket has a corresponding boundary value. The reason for this to manage the
 * memory of values.
 */
struct Bucket {
    Bucket(double equalFreq,
           double rangeFreq,
           double cumulativeFreq,
           double ndv,
           double cumulativeNDV);

    std::string toString() const;

    // Frequency of the bound value itself.
    double _equalFreq;

    // Frequency of other values.
    double _rangeFreq;

    // Sum of frequencies of preceding buckets to avoid recomputing. Includes both _equalFreq and
    // _rangeFreq.
    double _cumulativeFreq;

    // Number of distinct values in this bucket, excludes the bound.
    double _ndv;

    // Sum of distinct values in preceding buckets including this bucket.
    double _cumulativeNDV;
};

/**
 * A ScalarHistogram over a set of values. The ScalarHistogram consists of two parallel vectors -
 * one with the individual value statistics, and another one with the actual boundary values.
 */
class ScalarHistogram {
public:
    ScalarHistogram();
    ScalarHistogram(sbe::value::Array bounds, std::vector<Bucket> buckets);

    std::string toString() const;
    std::string plot() const;

    const sbe::value::Array& getBounds() const;
    const std::vector<Bucket>& getBuckets() const;

    bool empty() const {
        return _buckets.empty();
    }

private:
    // Bucket bounds representing the **highest** value in each bucket.
    sbe::value::Array _bounds;

    std::vector<Bucket> _buckets;
};

}  // namespace mongo::ce
