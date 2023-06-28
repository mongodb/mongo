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

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/preprocessor/control/iif.hpp>
#include <cstddef>
#include <functional>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/bool_expression.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer {

using PartialSchemaEntry = std::pair<PartialSchemaKey, PartialSchemaRequirement>;
using PSRExpr = BoolExpr<PartialSchemaEntry>;
using PSRExprBuilder = PSRExpr::Builder<false /*simplifyEmptyOrSingular*/, false /*removeDups*/>;

/**
 * Represents a set of predicates and projections composed in a boolean expression in CNF or DNF.
 * Cannot represent all predicates/projections, only those that can typically be answered
 * efficiently with an index.
 *
 * The predicates take the following form, represented by type PartialSchemaEntry:
 *    {<path, inputProjection>, <interval, outputProjection>}
 *
 * For example, suppose there is a ScanNode which creates a binding 'scan_0' representing the
 * documents in a collection. To represent a conjunction which encodes filtering with array
 * traversal on "a" {$match: {a: {$gt, 1}} combined with a retrieval of the field "b" (without
 * restrictions on its value), the PartialSchemaEntries would look like:
 *      entry1: {<PathGet "a" Traverse Id, scan_0>,    <[1, +inf],     <none>>}
 *      entry2: {<PathGet "b" Id,          scan_0>,    <(-inf, +inf),  "pb">}.
 *
 * These entries could be composed in DNF: OR( AND( entry1, entry2 )). In this case we have a
 * trivial disjunction, where the top-level disjunction only has one child. Or, they could be
 * composed in CNF: AND( OR( entry1 ), OR( entry2 )).
 *
 * When representing a non-trivial disjunction, the PartialSchemaRequirements must not have any
 * output bindings.
 *
 * The default / empty state represents a conjunction of zero predicates, which means always true.
 */
class PartialSchemaRequirements {
public:
    using Entry = std::pair<PartialSchemaKey, PartialSchemaRequirement>;

    // Default PartialSchemaRequirements is a singular DNF of an empty PartialSchemaKey and
    // fully-open PartialSchemaRequirement which does not bind.
    PartialSchemaRequirements();

    explicit PartialSchemaRequirements(PSRExpr::Node requirements);

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

    /**
     * Conjunctively combine 'this' with another PartialSchemaRequirement.
     * Asserts that 'this' is in DNF.
     *
     * For now, we assert that we have only one disjunct. This means we avoid applying
     * the distributive law, which would duplicate the new requirement into each disjunct.
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
    static bool simplify(PSRExpr::Node& expr,
                         std::function<bool(const PartialSchemaKey&, PartialSchemaRequirement&)>);
    static void normalize(PSRExpr::Node& expr);

    /**
     * Given a DNF, try to detect and remove redundant terms.
     *
     * For example, in ((a ^ b) U (z) U (a ^ b ^ c)) the (a ^ b) is redundant because
     * (a ^ b ^ c) implies (a ^ b).
     *
     * TODO SERVER-73827 Consider doing this simplification as part of BoolExpr::Builder.
     */
    static void simplifyRedundantDNF(PSRExpr::Node& expr);

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
