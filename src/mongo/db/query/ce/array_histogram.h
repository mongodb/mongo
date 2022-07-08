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

#include "mongo/db/query/ce/histogram.h"

namespace mongo::ce {

class ArrayHistogram {
public:
    // Constructs an empty scalar histogram.
    ArrayHistogram();

    // Constructor for scalar field histograms.
    ArrayHistogram(Histogram scalar, std::map<value::TypeTags, size_t> typeCounts);

    // Constructor for array field histograms. We have to initialize all array fields in this case.
    ArrayHistogram(Histogram scalar,
                   std::map<value::TypeTags, size_t> typeCounts,
                   Histogram arrayUnique,
                   Histogram arrayMin,
                   Histogram arrayMax,
                   std::map<value::TypeTags, size_t> arrayTypeCounts);

    ArrayHistogram(const ArrayHistogram&) = delete;

    // Returns whether or not this histogram includes array data points.
    bool isArray() const;

    const std::string toString() const;
    const Histogram& getScalar() const;
    const Histogram& getArrayUnique() const;
    const Histogram& getArrayMin() const;
    const Histogram& getArrayMax() const;
    const std::map<value::TypeTags, size_t> getTypeCounts() const;
    const std::map<value::TypeTags, size_t> getArrayTypeCounts() const;

private:
    /* Histogram fields for all paths. */

    // Contains values which appeared originally as scalars on the path.
    Histogram _scalar;
    std::map<value::TypeTags, size_t> _typeCounts;

    /* Histogram fields for array paths (only initialized if arrays are present). */

    // Contains unique scalar values originating from arrays.
    boost::optional<Histogram> _arrayUnique;
    // Contains minimum values originating from arrays **per class**.
    boost::optional<Histogram> _arrayMin;
    // Contains maximum values originating from arrays **per class**.
    boost::optional<Histogram> _arrayMax;
    boost::optional<std::map<value::TypeTags, size_t>> _arrayTypeCounts;
};


}  // namespace mongo::ce
