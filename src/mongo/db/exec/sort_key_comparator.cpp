// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sort_key_comparator.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/value_comparator.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

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
