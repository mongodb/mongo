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

#include "mongo/db/query/ce/array_histogram.h"

namespace mongo::ce {
using namespace sbe;

ArrayHistogram::ArrayHistogram() : ArrayHistogram(ScalarHistogram(), {}) {}

ArrayHistogram::ArrayHistogram(ScalarHistogram scalar,
                               std::map<value::TypeTags, size_t> typeCounts,
                               ScalarHistogram arrayUnique,
                               ScalarHistogram arrayMin,
                               ScalarHistogram arrayMax,
                               std::map<value::TypeTags, size_t> arrayTypeCounts)
    : _scalar(std::move(scalar)),
      _typeCounts(std::move(typeCounts)),
      _arrayUnique(std::move(arrayUnique)),
      _arrayMin(std::move(arrayMin)),
      _arrayMax(std::move(arrayMax)),
      _arrayTypeCounts(std::move(arrayTypeCounts)) {
    invariant(isArray());
}

ArrayHistogram::ArrayHistogram(ScalarHistogram scalar, std::map<value::TypeTags, size_t> typeCounts)
    : _scalar(std::move(scalar)),
      _typeCounts(std::move(typeCounts)),
      _arrayUnique(boost::none),
      _arrayMin(boost::none),
      _arrayMax(boost::none),
      _arrayTypeCounts(boost::none) {
    invariant(!isArray());
}

bool ArrayHistogram::isArray() const {
    return _arrayUnique && _arrayMin && _arrayMax && _arrayTypeCounts;
}

std::string typeCountsToString(const std::map<value::TypeTags, size_t>& typeCounts) {
    std::ostringstream os;
    os << "{";
    bool first = true;
    for (auto [tag, count] : typeCounts) {
        if (!first)
            os << ", ";
        os << tag << ": " << count;
        first = false;
    }
    os << "}";
    return os.str();
}

std::string ArrayHistogram::toString() const {
    std::ostringstream os;
    os << "{\n";
    os << " scalar: " << _scalar.toString();
    os << ",\n typeCounts: " << typeCountsToString(_typeCounts);
    if (isArray()) {
        os << ",\n arrayUnique: " << _arrayUnique->toString();
        os << ",\n arrayMin: " << _arrayMin->toString();
        os << ",\n arrayMax: " << _arrayMax->toString();
        os << ",\n arrayTypeCounts: " << typeCountsToString(*_arrayTypeCounts);
    }
    os << "\n}\n";
    return os.str();
}

const ScalarHistogram& ArrayHistogram::getScalar() const {
    return _scalar;
}

const ScalarHistogram& ArrayHistogram::getArrayUnique() const {
    invariant(isArray());
    return *_arrayUnique;
}

const ScalarHistogram& ArrayHistogram::getArrayMin() const {
    invariant(isArray());
    return *_arrayMin;
}

const ScalarHistogram& ArrayHistogram::getArrayMax() const {
    invariant(isArray());
    return *_arrayMax;
}

const std::map<value::TypeTags, size_t>& ArrayHistogram::getTypeCounts() const {
    return _typeCounts;
}

const std::map<value::TypeTags, size_t>& ArrayHistogram::getArrayTypeCounts() const {
    invariant(isArray());
    return *_arrayTypeCounts;
}

}  // namespace mongo::ce
