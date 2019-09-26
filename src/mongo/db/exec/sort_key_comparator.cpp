/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/exec/sort_key_comparator.h"

namespace mongo {

SortKeyComparator::SortKeyComparator(const SortPattern& sortPattern) {
    _pattern.reserve(sortPattern.size());
    std::transform(sortPattern.begin(),
                   sortPattern.end(),
                   std::back_inserter(_pattern),
                   [](const SortPattern::SortPatternPart& part) {
                       return part.isAscending ? SortDirection::kAscending
                                               : SortDirection::kDescending;
                   });
}

int SortKeyComparator::operator()(const Value& lhsKey, const Value& rhsKey) const {
    // Note that 'comparator' must use binary comparisons here, as both 'lhs' and 'rhs' are
    // collation comparison keys.
    ValueComparator comparator;
    const size_t n = _pattern.size();
    if (n == 1) {  // simple fast case
        if (_pattern[0] == SortDirection::kAscending)
            return comparator.compare(lhsKey, rhsKey);
        else
            return -comparator.compare(lhsKey, rhsKey);
    }

    // compound sort
    for (size_t i = 0; i < n; i++) {
        int cmp = comparator.compare(lhsKey[i], rhsKey[i]);
        if (cmp) {
            // If necessary, adjust the return value by the key ordering.
            if (_pattern[i] == SortDirection::kDescending)
                cmp = -cmp;

            return cmp;
        }
    }

    // If we got here, everything matched (or didn't exist), so we'll consider the documents equal
    // for purposes of this sort.
    return 0;
}

SortKeyComparator::SortKeyComparator(const BSONObj& sortPattern) {
    _pattern.reserve(sortPattern.nFields());
    std::transform(sortPattern.begin(),
                   sortPattern.end(),
                   std::back_inserter(_pattern),
                   [](const BSONElement& part) {
                       return (part.number() >= 0) ? SortDirection::kAscending
                                                   : SortDirection::kDescending;
                   });
}

}  // namespace mongo
