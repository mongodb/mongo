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

#include "mongo/db/query/optimizer/index_bounds.h"

namespace mongo::optimizer {
/**
 * Represents a set of predicates and projections. Cannot represent all predicates/projections:
 * only those that can typically be answered efficiently with an index.
 *
 * Only one instance of a path without Traverse elements (non-multikey) is allowed. By contrast
 * several instances of paths with Traverse elements (multikey) are allowed. For example: Get "a"
 * Get "b" Id is allowed just once while Get "a" Traverse Get "b" Id is allowed multiple times.
 *
 * The default / empty state represents a conjunction of zero predicates, which means always true.
 */
class PartialSchemaRequirements {
public:
    using Entry = std::pair<PartialSchemaKey, PartialSchemaRequirement>;
    struct ConstIter {
        auto begin() const {
            return _begin;
        }
        auto end() const {
            return _end;
        }
        auto cbegin() const {
            return _begin;
        }
        auto cend() const {
            return _end;
        }

        std::vector<Entry>::const_iterator _begin;
        std::vector<Entry>::const_iterator _end;
    };

    struct Iter {
        auto begin() const {
            return _begin;
        }
        auto end() const {
            return _end;
        }
        auto cbegin() const {
            return _begin;
        }
        auto cend() const {
            return _end;
        }

        std::vector<Entry>::iterator _begin;
        std::vector<Entry>::iterator _end;
    };

    PartialSchemaRequirements() = default;
    PartialSchemaRequirements(std::vector<Entry>);
    PartialSchemaRequirements(std::initializer_list<Entry> entries)
        : PartialSchemaRequirements(std::vector<Entry>(entries)) {}

    bool operator==(const PartialSchemaRequirements& other) const;

    /**
     * Return true if there are zero predicates and zero projections.
     */
    bool empty() const;

    size_t numLeaves() const;
    size_t numConjuncts() const;

    std::set<ProjectionName> getBoundNames() const;

    boost::optional<ProjectionName> findProjection(const PartialSchemaKey&) const;

    /**
     * Picks the first top-level conjunct matching the given key.
     *
     * Result includes the index of the top-level conjunct.
     */
    boost::optional<std::pair<size_t, PartialSchemaRequirement>> findFirstConjunct(
        const PartialSchemaKey&) const;

    ConstIter conjuncts() const {
        return {_repr.begin(), _repr.end()};
    }

    Iter conjuncts() {
        return {_repr.begin(), _repr.end()};
    }

    using Bindings = std::vector<std::pair<PartialSchemaKey, ProjectionName>>;
    Bindings iterateBindings() const;

    void add(PartialSchemaKey, PartialSchemaRequirement);

    /**
     * Apply an in-place transformation to each PartialSchemaRequirement.
     *
     * Since the key is only exposed read-only to the callback, we don't need to
     * worry about restoring the no-Traverseless-duplicates invariant.
     */
    void transform(std::function<void(const PartialSchemaKey&, PartialSchemaRequirement&)>);

    /**
     * Apply a simplification to each PartialSchemaRequirement.
     *
     * The callback can return false if an individual PartialSchemaRequirement
     * simplifies to an always-false predicate.
     *
     * This method returns false if the overall result is an always-false predicate.
     *
     * This method will also remove any predicates that are trivially true (those will
     * a fully open DNF interval).
     */
    bool simplify(std::function<bool(const PartialSchemaKey&, PartialSchemaRequirement&)>);

private:
    // Restore the invariant that the entries are sorted by key.
    void normalize();

    using Repr = std::vector<Entry>;
    Repr _repr;
};

}  // namespace mongo::optimizer
