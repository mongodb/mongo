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
#include "mongo/db/query/optimizer/utils/bool_expression_builder.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer {

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
using PartialSchemaEntry = std::pair<PartialSchemaKey, PartialSchemaRequirement>;
using PSRExpr = BoolExpr<PartialSchemaEntry>;

struct PartialSchemaEntryComparator {
    struct Less {
        bool operator()(const PartialSchemaEntry& e1, const PartialSchemaEntry& e2) const;
    };

    struct Cmp3W {
        int operator()(const PartialSchemaEntry& e1, const PartialSchemaEntry& e2) const;
    };
};

/**
 * A no-op entry has a default key and a requirement that is fully open and does not bind.
 */
PartialSchemaEntry makeNoopPartialSchemaEntry();

/**
 * An always false entry has a default key and a requirement that is unsatisfiable (MaxKey to
 * MinKey) and does not bind.
 */
PartialSchemaEntry makeAlwaysFalsePartialSchemaEntry();

struct PSRComparator {
    bool operator()(const PSRExpr::Node& n1, const PSRExpr::Node& n2) const;
};
struct PSRSimplifier {
    using DefaultSimplifier = DefaultSimplifyAndCreateNode<PartialSchemaEntry>;
    using Result = DefaultSimplifier::Result;

    Result operator()(BuilderNodeType type,
                      std::vector<PSRExpr::Node> v,
                      bool hasTrue,
                      bool hasFalse) const;

    // Informs the simplifier to construct a tree of particular shape when simplifying corner cases.
    bool _isDNF = true;
};
using PSRExprBuilder = BoolExprBuilder<PartialSchemaEntry, PSRSimplifier>;

namespace psr {
/**
 * Default PSRExpr is a singular DNF of an empty PartialSchemaKey and fully-open
 * PartialSchemaRequirement which does not bind.
 */
PSRExpr::Node makeNoOp();

/**
 * Default PSRExpr is a singular CNF of an empty PartialSchemaKey and fully-open
 * PartialSchemaRequirement which does not bind.
 */
PSRExpr::Node makeNoOpCNF();

/**
 * This is a singular DNF partialSchemaRequirements with an empty PartialSchemaKey and always-false
 * PartialSchemaRequirement.
 */
PSRExpr::Node makeAlwaysFalse();

/**
 * This is a singular CNF partialSchemaRequirements with an empty PartialSchemaKey and always-false
 * PartialSchemaRequirement.
 */
PSRExpr::Node makeAlwaysFalseCNF();

/**
 * Return true if there are zero predicates and zero projections, or if there is a single
 * fully-open predicate with no projections.
 */
bool isNoop(const PSRExpr::Node& expr);

/**
 * Returns true if the expression is always false: it has a single always false PSR.
 */
bool isAlwaysFalse(const PSRExpr::Node& expr);

/**
 * Return the bound projection name corresponding to the first conjunct matching the given key.
 * Assert on non-DNF requirements.
 */
boost::optional<ProjectionName> findProjection(const PSRExpr::Node& expr,
                                               const PartialSchemaKey& key);

/**
 * Given a DNF, try to detect and remove redundant terms.
 *
 * For example, in ((a ^ b) U (z) U (a ^ b ^ c)) the (a ^ b) is redundant because
 * (a ^ b ^ c) implies (a ^ b).
 *
 * TODO SERVER-74879: Generalize boolean minimization.
 */
void simplifyRedundantDNF(PSRExpr::Node& expr);

/**
 * Returns a vector of ((input binding, path), output binding). The output binding names
 * are unique and you can think of the vector as a product: every row has all the projections
 * available.
 */
std::vector<std::pair<PartialSchemaKey, ProjectionName>> getBoundProjections(
    const PSRExpr::Node& expr);

}  // namespace psr
}  // namespace mongo::optimizer
