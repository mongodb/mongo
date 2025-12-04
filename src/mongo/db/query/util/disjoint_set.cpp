/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
