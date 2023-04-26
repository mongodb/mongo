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

#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/utils/abt_compare.h"

namespace mongo::optimizer {
namespace {
class PSRNormalizeTransporter {
public:
    void transport(const PSRExpr::Atom& node) {
        // Noop.
    }

    void transport(PSRExpr::Conjunction& node, std::vector<PSRExpr::Node>& children) {
        sortChildren(children);
    }

    void transport(PSRExpr::Disjunction& node, std::vector<PSRExpr::Node>& children) {
        sortChildren(children);
    }

    void normalize(PSRExpr::Node& node) {
        return algebra::transport<false>(node, *this);
    }

private:
    void sortChildren(std::vector<PSRExpr::Node>& children) {
        struct Comparator {
            bool operator()(const PSRExpr::Node& i1, const PSRExpr::Node& i2) const {
                return comparePartialSchemaRequirementsExpr(i1, i2) < 0;
            }
        };
        std::sort(children.begin(), children.end(), Comparator{});
    }
};

// A no-op entry has a default key and a requirement that is fully open and does not bind.
PartialSchemaEntry makeNoopPartialSchemaEntry() {
    return {PartialSchemaKey(),
            PartialSchemaRequirement(
                boost::none /*boundProjectionName*/,
                IntervalReqExpr::makeSingularDNF(IntervalRequirement{/*fully open*/}),
                false /*isPerfOnly*/)};
}
}  // namespace

void PartialSchemaRequirements::normalize() {
    PSRNormalizeTransporter{}.normalize(_expr);
}

PartialSchemaRequirements::PartialSchemaRequirements(PSRExpr::Node requirements)
    : _expr(std::move(requirements)) {
    tassert(7016403,
            "PartialSchemaRequirements must be in CNF or DNF",
            PSRExpr::isCNF(_expr) || PSRExpr::isDNF(_expr));

    normalize();
}

PartialSchemaRequirements::PartialSchemaRequirements()
    : PartialSchemaRequirements(PSRExpr::makeSingularDNF(makeNoopPartialSchemaEntry())) {}

bool PartialSchemaRequirements::operator==(const PartialSchemaRequirements& other) const {
    return _expr == other._expr;
}

bool PartialSchemaRequirements::isNoop() const {
    // A PartialSchemaRequirements is a no-op if it has exactly zero predicates/projections...
    const size_t numPreds = PSRExpr::numLeaves(getRoot());
    if (numPreds == 0) {
        return true;
    } else if (numPreds > 1) {
        return false;
    }

    // ...or if it has exactly one predicate which is a no-op.
    auto reqIsNoop = false;

    auto checkNoop = [&](const Entry& entry) {
        reqIsNoop = (entry == makeNoopPartialSchemaEntry());
    };
    if (PSRExpr::isCNF(_expr)) {
        PSRExpr::visitCNF(_expr, checkNoop);
    } else {
        PSRExpr::visitDNF(_expr, checkNoop);
    }

    return reqIsNoop;
}

boost::optional<ProjectionName> PartialSchemaRequirements::findProjection(
    const PartialSchemaKey& key) const {
    tassert(7453908,
            "Expected PartialSchemaRequirement to be a singleton disjunction",
            PSRExpr::isSingletonDisjunction(getRoot()));

    boost::optional<ProjectionName> proj;
    PSRExpr::visitDNF(_expr, [&](const Entry& entry) {
        if (!proj && entry.first == key) {
            proj = entry.second.getBoundProjectionName();
        }
    });
    return proj;
}

boost::optional<std::pair<size_t, PartialSchemaRequirement>>
PartialSchemaRequirements::findFirstConjunct(const PartialSchemaKey& key) const {
    tassert(7453907,
            "Expected PartialSchemaRequirement to be a singleton disjunction",
            PSRExpr::isSingletonDisjunction(getRoot()));

    size_t i = 0;
    boost::optional<std::pair<size_t, PartialSchemaRequirement>> res;
    PSRExpr::visitDNF(_expr, [&](const PartialSchemaEntry& entry) {
        if (!res && entry.first == key) {
            res = {{i, entry.second}};
        }
        ++i;
    });
    return res;
}

void PartialSchemaRequirements::add(PartialSchemaKey key, PartialSchemaRequirement req) {
    tassert(7016406, "Expected a PartialSchemaRequirements in DNF form", PSRExpr::isDNF(_expr));
    // TODO SERVER-69026 Consider applying the distributive law.
    tassert(7453912, "Expected a singleton disjunction", PSRExpr::isSingletonDisjunction(_expr));

    // Add an entry to the first conjunction
    PSRExpr::visitDisjuncts(_expr, [&](PSRExpr::Node& disjunct, const size_t i) {
        if (i == 0) {
            const auto& conjunction = disjunct.cast<PSRExpr::Conjunction>();
            conjunction->nodes().emplace_back(
                PSRExpr::make<PSRExpr::Atom>(Entry(std::move(key), std::move(req))));
        }
    });
    normalize();
}

namespace {
// TODO SERVER-73827: Apply this simplification during BoolExpr building.
template <bool isCNF,
          class TopLevel = std::conditional_t<isCNF, PSRExpr::Conjunction, PSRExpr::Disjunction>,
          class SecondLevel = std::conditional_t<isCNF, PSRExpr::Disjunction, PSRExpr::Conjunction>>
static bool simplifyExpr(
    PSRExpr::Node& n,
    std::function<bool(const PartialSchemaKey&, PartialSchemaRequirement&)> func) {
    auto& children = n.template cast<TopLevel>()->nodes();
    for (auto it = children.begin(); it != children.end();) {
        auto& atoms = (*it).template cast<SecondLevel>()->nodes();

        bool removeChild = false;
        for (auto atomIt = atoms.begin(); atomIt != atoms.end();) {
            auto& [key, req] = (*atomIt).template cast<PSRExpr::Atom>()->getExpr();
            // If 'req' is always-false then: Under OR, remove the atom. Under AND, remove the AND.
            if (!func(key, req)) {
                if (isCNF) {
                    atomIt = atoms.erase(atomIt);
                    continue;
                } else {
                    removeChild = true;
                    break;
                }
            }

            // If 'req' is always-true then: Under AND, remove the atom. Under OR, remove the OR.
            if (isIntervalReqFullyOpenDNF(req.getIntervals()) && !req.getBoundProjectionName()) {
                if (isCNF) {
                    removeChild = true;
                    break;
                } else {
                    atomIt = atoms.erase(atomIt);
                    continue;
                }
            }

            ++atomIt;
        }

        if (atoms.empty() && isCNF) {
            // We have an OR of nothing-- so we have simplified to always-false
            return false;
        }

        if (removeChild) {
            it = children.erase(it);
            continue;
        }
        ++it;
    }

    if (children.empty() && !isCNF) {
        // We have an OR of nothing-- so we have simplified to always-false
        return false;
    }
    return true;
}
}  // namespace

bool PartialSchemaRequirements::simplify(
    std::function<bool(const PartialSchemaKey&, PartialSchemaRequirement&)> func) {
    if (PSRExpr::isCNF(_expr)) {
        return simplifyExpr<true /*isCNF*/>(_expr, func);
    }
    return simplifyExpr<false /*isCNF*/>(_expr, func);
}

/**
 * Returns a vector of ((input binding, path), output binding). The output binding names
 * are unique and you can think of the vector as a product: every row has all the projections
 * available.
 */
std::vector<std::pair<PartialSchemaKey, ProjectionName>> getBoundProjections(
    const PartialSchemaRequirements& reqs) {
    // For now we assume no projections inside a nontrivial disjunction.
    std::vector<std::pair<PartialSchemaKey, ProjectionName>> result;
    PSRExpr::visitAnyShape(reqs.getRoot(), [&](const PartialSchemaEntry& e) {
        const auto& [key, req] = e;
        if (auto proj = req.getBoundProjectionName()) {
            result.emplace_back(key, *proj);
        }
    });
    tassert(7453906,
            "Expected no bound projections in a nontrivial disjunction",
            result.empty() || PSRExpr::isSingletonDisjunction(reqs.getRoot()));
    return result;
}

}  // namespace mongo::optimizer
