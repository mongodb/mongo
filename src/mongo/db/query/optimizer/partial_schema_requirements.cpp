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

#include "mongo/db/query/optimizer/partial_schema_requirements.h"

#include <absl/container/node_hash_map.h>
#include <algorithm>
#include <boost/dynamic_bitset/dynamic_bitset.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <climits>
#include <type_traits>

#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/utils/abt_compare.h"
#include "mongo/db/query/optimizer/utils/abt_hash.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer {

int PartialSchemaEntryComparator::Cmp3W::operator()(const PartialSchemaEntry& e1,
                                                    const PartialSchemaEntry& e2) const {
    const int keyCmp = PartialSchemaKeyComparator::Cmp3W{}(e1.first, e2.first);
    if (keyCmp != 0) {
        return keyCmp;
    }
    return PartialSchemaRequirementComparator::Cmp3W{}(e1.second, e2.second);
}

bool PartialSchemaEntryComparator::Less::operator()(const PartialSchemaEntry& e1,
                                                    const PartialSchemaEntry& e2) const {
    return PartialSchemaEntryComparator::Cmp3W{}(e1, e2) < 0;
}

bool PSRComparator::operator()(const PSRExpr::Node& n1, const PSRExpr::Node& n2) const {
    return comparePartialSchemaRequirementsExpr(n1, n2) < 0;
}

static PartialSchemaEntry makeDefaultEntry(IntervalRequirement interval) {
    return {PartialSchemaKey(),
            PartialSchemaRequirement(boost::none /*boundProjectionName*/,
                                     IntervalReqExpr::makeSingularDNF(std::move(interval)),
                                     false /*isPerfOnly*/)};
}

PartialSchemaEntry makeNoopPartialSchemaEntry() {
    return makeDefaultEntry({/*fully open*/});
}

PartialSchemaEntry makeAlwaysFalsePartialSchemaEntry() {
    return makeDefaultEntry({BoundRequirement::makePlusInf(), BoundRequirement::makeMinusInf()});
}

PSRSimplifier::Result PSRSimplifier::operator()(const BuilderNodeType type,
                                                std::vector<PSRExpr::Node> v,
                                                bool hasTrue,
                                                bool hasFalse) const {
    const bool isConj = type == BuilderNodeType::Conj;

    if (_isDNF == isConj) {
        // Lower level: disjunction for CNF, conjunction for DNF.

        if (!v.empty()) {
            // Try to remove some always-true or always-false elements.
            for (auto it = v.begin(); it != v.end();) {
                auto& [key, req] = it->template cast<PSRExpr::Atom>()->getExpr();

                if (isIntervalReqFullyOpenDNF(req.getIntervals()) &&
                    !req.getBoundProjectionName()) {
                    // If 'req' is always-true then: Under AND, remove the atom. Under OR, remove
                    // the OR.
                    if (isConj) {
                        it = v.erase(it);
                    } else {
                        return {._isTrue = true};
                    }
                } else if (isIntervalReqAlwaysFalseDNF(req.getIntervals())) {
                    // If 'req' is always-false then: Under OR, remove the atom. Under AND, remove
                    // the AND.
                    if (isConj) {
                        return {._isFalse = true};
                    } else {
                        it = v.erase(it);
                    }
                } else {
                    it++;
                }
            }
        }

        if (v.empty()) {
            if (isConj) {
                // All conjuncts were removed because they were always-true: result is always
                // true.
                return {._isTrue = true};
            } else {
                // All disjuncts were removed because they were always-false: result is always
                // false.
                return {._isFalse = true};
            }
        }
    } else {
        // Upper level: conjunction for CNF, disjunction for DNF.

        if (isConj) {
            if (hasFalse) {
                return {psr::makeAlwaysFalseCNF()};
            } else if (v.empty()) {
                return {psr::makeNoOpCNF()};
            }
        } else {
            if (hasTrue) {
                return {psr::makeNoOp()};
            } else if (v.empty()) {
                return {psr::makeAlwaysFalse()};
            }
        }
    }

    // Deduplicate via sort + unique.
    std::sort(v.begin(), v.end(), PSRComparator{});
    auto end = std::unique(v.begin(), v.end());
    v.erase(end, v.end());

    return DefaultSimplifier{}(type, std::move(v), hasTrue, hasFalse);
}

namespace psr {

PSRExpr::Node makeNoOp() {
    return PSRExpr::makeSingularDNF(makeNoopPartialSchemaEntry());
}

PSRExpr::Node makeNoOpCNF() {
    return PSRExpr::makeSingularCNF(makeNoopPartialSchemaEntry());
}

PSRExpr::Node makeAlwaysFalse() {
    return PSRExpr::makeSingularDNF(makeAlwaysFalsePartialSchemaEntry());
}

PSRExpr::Node makeAlwaysFalseCNF() {
    return PSRExpr::makeSingularCNF(makeAlwaysFalsePartialSchemaEntry());
}

static bool isInSpecialForm(const PSRExpr::Node& expr, PartialSchemaEntry specialForm) {
    // We are in special form if we have exactly 1 predicate.
    const size_t numPreds = PSRExpr::numLeaves(expr);
    if (numPreds == 0) {
        return true;
    } else if (numPreds > 1) {
        return false;
    }

    bool result = false;
    auto checkFn = [&](const PartialSchemaEntry& entry, const PSRExpr::VisitorContext&) {
        result = (entry == specialForm);
    };
    if (PSRExpr::isCNF(expr)) {
        PSRExpr::visitCNF(expr, checkFn);
    } else if (PSRExpr::isDNF(expr)) {
        PSRExpr::visitDNF(expr, checkFn);
    }
    return result;
}

bool isNoop(const PSRExpr::Node& expr) {
    // A PartialSchemaRequirements is a no-op if it has exactly zero predicates/projections or if it
    // has exactly one predicate which is a no-op.
    return isInSpecialForm(expr, makeNoopPartialSchemaEntry());
}

bool isAlwaysFalse(const PSRExpr::Node& expr) {
    // A PartialSchemaRequirements is always false if it has exactly zero predicates/projections or
    // if it has exactly one predicate which is always false.
    return isInSpecialForm(expr, makeAlwaysFalsePartialSchemaEntry());
}

boost::optional<ProjectionName> findProjection(const PSRExpr::Node& expr,
                                               const PartialSchemaKey& key) {
    tassert(7453908,
            "Expected PartialSchemaRequirement to be a singleton disjunction",
            PSRExpr::isSingletonDisjunction(expr));

    boost::optional<ProjectionName> proj;
    PSRExpr::visitDNF(expr, [&](const PartialSchemaEntry& entry, const PSRExpr::VisitorContext&) {
        if (!proj && entry.first == key) {
            proj = entry.second.getBoundProjectionName();
        }
    });
    return proj;
}

void simplifyRedundantDNF(PSRExpr::Node& expr) {
    tassert(6902601, "simplifyRedundantDNF expects DNF", PSRExpr::isDNF(expr));

    struct TermSimplifier {
        using DefaultSimplifier = DefaultSimplifyAndCreateNode<PartialSchemaEntry>;
        using Mask = boost::dynamic_bitset<size_t>;

        DefaultSimplifier::Result operator()(const BuilderNodeType type,
                                             std::vector<PSRExpr::Node> v,
                                             const bool hasTrue,
                                             const bool hasFalse) {
            // Now remove terms that are subsumed by some other term. This means try to remove terms
            // whose atoms are a superset of some other term: (a^b) subsumes (a^b^c), so remove
            // (a^b^c). Since there are no duplicate atoms, we're looking to remove terms whose
            // 'nodes().size()' is large.

            if (type == BuilderNodeType::Conj) {
                Mask mask;
                mask.resize(_exprs.size());

                for (const auto& n : v) {
                    const size_t label = _exprs.at(n.cast<PSRExpr::Atom>()->getExpr());
                    mask[label] = 1;
                }

                // Does any previously-seen mask subsume this one?
                for (const auto& prev : _seenMasks) {
                    if (prev.is_subset_of(mask)) {
                        // Do not add to parent.
                        return {boost::none};
                    }
                }
                _seenMasks.push_back(std::move(mask));
            }

            return DefaultSimplifier{}(type, std::move(v), hasTrue, hasFalse);
        }

        struct Hasher {
            size_t operator()(const PartialSchemaEntry& entry) const {
                return ABTHashGenerator::generate(entry);
            }
        };
        opt::unordered_map<PartialSchemaEntry, size_t, Hasher> _exprs;

        std::vector<Mask> _seenMasks;
    };
    TermSimplifier instance;

    // First give each unique atom a label. Store each atom by value.
    PSRExpr::visitAnyShape(
        expr, [&](const PartialSchemaEntry& expr, const PSRExpr::VisitorContext& /*ctx*/) {
            // Insert all atoms in the map to obtain the size of the bit vectors.
            instance._exprs.emplace(expr, instance._exprs.size());
        });

    BoolExprBuilder<PartialSchemaEntry, TermSimplifier> builder(std::move(instance));
    expr = std::move(*builder.subtree(std::move(expr)).finish());
}

/**
 * Returns a vector of ((input binding, path), output binding). The output binding names
 * are unique and you can think of the vector as a product: every row has all the projections
 * available.
 */
std::vector<std::pair<PartialSchemaKey, ProjectionName>> getBoundProjections(
    const PSRExpr::Node& expr) {
    // For now we assume no projections inside a nontrivial disjunction.
    std::vector<std::pair<PartialSchemaKey, ProjectionName>> result;
    PSRExpr::visitAnyShape(expr, [&](const PartialSchemaEntry& e, const PSRExpr::VisitorContext&) {
        const auto& [key, req] = e;
        if (auto proj = req.getBoundProjectionName()) {
            result.emplace_back(key, *proj);
        }
    });
    tassert(7453906,
            "Expected no bound projections in a nontrivial disjunction",
            result.empty() || PSRExpr::isSingletonDisjunction(expr));
    return result;
}

}  // namespace psr
}  // namespace mongo::optimizer
