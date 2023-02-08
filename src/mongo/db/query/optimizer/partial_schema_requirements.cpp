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

namespace mongo::optimizer {
void PartialSchemaRequirements::normalize() {
    std::stable_sort(
        _repr.begin(),
        _repr.end(),
        [lt = PartialSchemaKeyLessComparator{}](const auto& entry1, const auto& entry2) -> bool {
            return lt(entry1.first, entry2.first);
        });
}

PartialSchemaRequirements::PartialSchemaRequirements(
    std::vector<PartialSchemaRequirements::Entry> entries) {
    for (Entry& entry : entries) {
        _repr.push_back(std::move(entry));
    }

    normalize();
}

std::set<ProjectionName> PartialSchemaRequirements::getBoundNames() const {
    std::set<ProjectionName> names;
    for (auto&& [key, b] : iterateBindings()) {
        names.insert(b);
    }
    return names;
}

bool PartialSchemaRequirements::operator==(const PartialSchemaRequirements& other) const {
    return _repr == other._repr;
}

bool PartialSchemaRequirements::empty() const {
    return _repr.empty();
}

size_t PartialSchemaRequirements::numLeaves() const {
    return _repr.size();
}

size_t PartialSchemaRequirements::numConjuncts() const {
    return _repr.size();
}

boost::optional<ProjectionName> PartialSchemaRequirements::findProjection(
    const PartialSchemaKey& key) const {
    for (auto [k, req] : _repr) {
        if (k == key) {
            return req.getBoundProjectionName();
        }
    }
    return {};
}

boost::optional<std::pair<size_t, PartialSchemaRequirement>>
PartialSchemaRequirements::findFirstConjunct(const PartialSchemaKey& key) const {
    size_t i = 0;
    for (auto [k, req] : _repr) {
        if (k == key) {
            return {{i, req}};
        }
        ++i;
    }
    return {};
}

PartialSchemaRequirements::Bindings PartialSchemaRequirements::iterateBindings() const {
    Bindings result;
    for (auto&& [key, req] : _repr) {
        if (auto binding = req.getBoundProjectionName()) {
            result.emplace_back(key, *binding);
        }
    }
    return result;
}

void PartialSchemaRequirements::add(PartialSchemaKey key, PartialSchemaRequirement req) {
    _repr.emplace_back(std::move(key), std::move(req));

    normalize();
}

void PartialSchemaRequirements::transform(
    std::function<void(const PartialSchemaKey&, PartialSchemaRequirement&)> func) {
    for (auto& [key, req] : _repr) {
        func(key, req);
    }
}

bool PartialSchemaRequirements::simplify(
    std::function<bool(const PartialSchemaKey&, PartialSchemaRequirement&)> func) {
    for (auto it = _repr.begin(); it != _repr.end();) {
        auto& [key, req] = *it;

        if (!func(key, req)) {
            return false;
        }
        if (isIntervalReqFullyOpenDNF(it->second.getIntervals()) && !req.getBoundProjectionName()) {
            it = _repr.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

}  // namespace mongo::optimizer
