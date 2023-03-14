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
#include "mongo/db/query/optimizer/syntax/expr.h"

namespace mongo::optimizer {

using PartialSchemaEntry = std::pair<PartialSchemaKey, PartialSchemaRequirement>;
using PSRExpr = BoolExpr<PartialSchemaEntry>;
using PSRExprBuilder = PSRExpr::Builder<false /*simplifyEmptyOrSingular*/, false /*removeDups*/>;

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

    // TODO SERVER-69026: Remove these iterator constructs.
    using ConstNodeVecIter = std::vector<PSRExpr::Node>::const_iterator;
    using NodeVecIter = std::vector<PSRExpr::Node>::iterator;

    template <bool IsConst>
    using MaybeConstNodeVecIter =
        typename std::conditional<IsConst, ConstNodeVecIter, NodeVecIter>::type;

    template <bool IsConst>
    struct PSRIterator {
        using iterator_category = std::forward_iterator_tag;
        using difference_type = ptrdiff_t;
        using value_type = Entry;
        using pointer = typename std::conditional<IsConst, const Entry*, Entry*>::type;
        using reference = typename std::conditional<IsConst, const Entry&, Entry&>::type;

        PSRIterator(MaybeConstNodeVecIter<IsConst> atomsIt) : _atomsIt(atomsIt) {}

        reference operator*() const {
            return _atomsIt->template cast<PSRExpr::Atom>()->getExpr();
        }
        pointer operator->() {
            return &(_atomsIt->template cast<PSRExpr::Atom>()->getExpr());
        }

        PSRIterator& operator++() {
            _atomsIt++;
            return *this;
        }

        PSRIterator operator++(int) {
            PSRIterator tmp = *this;
            ++(*this);
            return tmp;
        }

        friend bool operator==(const PSRIterator& a, const PSRIterator& b) {
            return a._atomsIt == b._atomsIt;
        };
        friend bool operator!=(const PSRIterator& a, const PSRIterator& b) {
            return a._atomsIt != b._atomsIt;
        };

        MaybeConstNodeVecIter<IsConst> _atomsIt;
    };

    template <bool IsConst>
    struct Range {
        auto begin() const {
            return PSRIterator<IsConst>(_begin);
        }
        auto end() const {
            return PSRIterator<IsConst>(_end);
        }
        auto cbegin() const {
            return PSRIterator<true>(_begin);
        }
        auto cend() const {
            return PSRIterator<true>(_end);
        }

        MaybeConstNodeVecIter<IsConst> _begin;
        MaybeConstNodeVecIter<IsConst> _end;
    };

    // Default PartialSchemaRequirements is a singular DNF of an empty PartialSchemaKey and
    // fully-open PartialSchemaRequirement which does not bind.
    PartialSchemaRequirements();

    PartialSchemaRequirements(PSRExpr::Node requirements);

    bool operator==(const PartialSchemaRequirements& other) const;

    /**
     * Return true if there are zero predicates and zero projections, or if there is a single
     * fully-open predicate with no projections.
     */
    bool isNoop() const;

    /**
     * Return the bound projection name corresponding to the first conjunct matching the given key.
     * Assert on non-DNF requirements.
     */
    boost::optional<ProjectionName> findProjection(const PartialSchemaKey&) const;

    /**
     * Pick the first conjunct matching the given key. Assert on non-DNF requirements.
     *
     * Result includes the index of the conjunct.
     */
    boost::optional<std::pair<size_t, PartialSchemaRequirement>> findFirstConjunct(
        const PartialSchemaKey&) const;

    // TODO SERVER-69026: Remove these methods in favor of visitDis/Conjuncts().
    Range<true> conjuncts() const {
        tassert(7453905,
                "Expected PartialSchemaRequirement to be a singleton disjunction",
                PSRExpr::isSingletonDisjunction(_expr));
        const auto& atoms = _expr.cast<PSRExpr::Disjunction>()
                                ->nodes()
                                .begin()
                                ->cast<PSRExpr::Conjunction>()
                                ->nodes();
        return {atoms.begin(), atoms.end()};
    }

    Range<false> conjuncts() {
        tassert(7453904,
                "Expected PartialSchemaRequirement to be a singleton disjunction",
                PSRExpr::isSingletonDisjunction(_expr));
        auto& atoms = _expr.cast<PSRExpr::Disjunction>()
                          ->nodes()
                          .begin()
                          ->cast<PSRExpr::Conjunction>()
                          ->nodes();
        return {atoms.begin(), atoms.end()};
    }

    /**
     * Conjunctively combine 'this' with another PartialSchemaRequirement.
     * Asserts that 'this' is in DNF.
     *
     * For now, we assert that we have only one disjunct. This means we avoid applying
     * the distributive law, which would duplicate the new requirement into each disjunct.
     *
     * TODO SERVER-69026 Consider applying the distributive law to allow contained-OR in
     * SargableNode.
     */
    void add(PartialSchemaKey, PartialSchemaRequirement);

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
     *
     * TODO SERVER-73827: Consider applying this simplification during BoolExpr building.
     */
    bool simplify(std::function<bool(const PartialSchemaKey&, PartialSchemaRequirement&)>);

    const auto& getRoot() const {
        return _expr;
    }

    auto& getRoot() {
        return _expr;
    }

private:
    // Restore the invariant that the entries are sorted by key.
    // TODO SERVER-73827: Consider applying this normalization during BoolExpr building.
    void normalize();

    // _expr is currently always in DNF.
    PSRExpr::Node _expr;
};

/**
 * Returns a vector of ((input binding, path), output binding). The output binding names
 * are unique and you can think of the vector as a product: every row has all the projections
 * available.
 */
std::vector<std::pair<PartialSchemaKey, ProjectionName>> getBoundProjections(
    const PartialSchemaRequirements& reqs);

}  // namespace mongo::optimizer
