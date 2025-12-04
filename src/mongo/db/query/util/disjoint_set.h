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

#pragma once

#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
/**
 * Implementation of DisjointSet for integer values using UnionFind algorithm
 * (CLRS, "Data Structures for Disjoint Sets";
 * https://www.cs.ox.ac.uk/files/2831/union-find-easy.pdf;
 * https://dl.acm.org/doi/10.1145/321879.321884).
 */
class DisjointSet {
public:
    /**
     * Initialize a DistinctSet that can hold elements from '0' to 'size-1' including.
     */
    explicit DisjointSet(size_t size);

    /**
     * Returns the root element of the disjoint-tree the 'element' belong to, returns none if the
     * element is out of range.
     */
    boost::optional<size_t> find(size_t element) {
        if (isValidElement(element)) {
            return unsafeFind(element);
        }
        return boost::none;
    }

    /**
     * Unite 2 sets represented by their elements: 'lhs' and 'rhs', do nothing if at least one of
     * the elements are out of range of the disjoint set.
     */
    void unite(size_t lhs, size_t rhs);

    /**
     * Reset the disjoint set to its initial state.
     */
    void clear();

    size_t size() const {
        return _parents.size();
    }

private:
    size_t unsafeFind(size_t element);

    /**
     * Returns false for out of range elements.
     */
    bool isValidElement(size_t element) {
        return element < _parents.size();
    }

    // Represents a disjoint-set forest, where every disjoint-set tree is identified by the index of
    // its root element.
    std::vector<size_t> _parents;

    // Used by the union by rank heuristics, see 'unite' function for details.
    std::vector<size_t> _ranks;
};
}  // namespace mongo
