// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/util/disjoint_set.h"

#include <algorithm>
#include <numeric>

namespace mongo {
DisjointSet::DisjointSet(size_t size) : _parents(size), _ranks(size) {
    clear();
}

void DisjointSet::unite(size_t lhs, size_t rhs) {
    if (!isValidElement(lhs) || !isValidElement(rhs)) {
        return;
    }

    const auto left = unsafeFind(lhs);
    const auto right = unsafeFind(rhs);

    // If both elements already belong to the same set, do nothing.
    if (left == right) {
        return;
    }

    // Union by rank heuristic: joining smaller tree to larger tree.
    if (_ranks[left] < _ranks[right]) {
        _parents[left] = right;
    } else {
        _parents[right] = left;
        if (_ranks[right] == _ranks[left]) {
            // If both sets are of the same size, increase the size of the tree.
            ++_ranks[right];
        }
    }
}

void DisjointSet::clear() {
    // Initialize each element's parent to itself because each element is in its own disjoint set.
    std::iota(_parents.begin(), _parents.end(), 0);
    std::fill(_ranks.begin(), _ranks.end(), 1);
}

size_t DisjointSet::unsafeFind(size_t element) {
    if (_parents[element] != element) {
        // Path compression heuristic.
        _parents[element] = find(_parents[element]).value();
    }
    return _parents[element];
}
}  // namespace mongo
