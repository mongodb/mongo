// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

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
