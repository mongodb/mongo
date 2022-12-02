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

#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer {

static ProjectionNameMap<size_t> createMapFromVector(const ProjectionNameVector& v) {
    ProjectionNameMap<size_t> result;
    for (size_t i = 0; i < v.size(); i++) {
        result.emplace(v.at(i), i);
    }
    return result;
}

ProjectionNameOrderPreservingSet::ProjectionNameOrderPreservingSet(ProjectionNameVector v)
    : _map(createMapFromVector(v)), _vector(std::move(v)) {}

ProjectionNameOrderPreservingSet::ProjectionNameOrderPreservingSet(
    const ProjectionNameOrderPreservingSet& other)
    : _map(other._map), _vector(other._vector) {}

ProjectionNameOrderPreservingSet::ProjectionNameOrderPreservingSet(
    ProjectionNameOrderPreservingSet&& other) noexcept
    : _map(std::move(other._map)), _vector(std::move(other._vector)) {}

bool ProjectionNameOrderPreservingSet::operator==(
    const ProjectionNameOrderPreservingSet& other) const {
    return _vector == other._vector;
}

std::pair<size_t, bool> ProjectionNameOrderPreservingSet::emplace_back(
    ProjectionName projectionName) {
    if (const auto index = find(projectionName)) {
        return {*index, false};
    }

    const size_t id = _vector.size();
    _vector.emplace_back(std::move(projectionName));
    _map.emplace(_vector.back(), id);
    return {id, true};
}

boost::optional<size_t> ProjectionNameOrderPreservingSet::find(
    const ProjectionName& projectionName) const {
    auto it = _map.find(projectionName);
    if (it == _map.end()) {
        return boost::none;
    }

    return it->second;
}

bool ProjectionNameOrderPreservingSet::erase(const ProjectionName& projectionName) {
    auto index = find(projectionName);
    if (!index) {
        return false;
    }

    if (*index < _vector.size() - 1) {
        // Repoint map.
        _map.at(_vector.back()) = *index;
        // Fill gap with last element.
        _vector.at(*index) = std::move(_vector.back());
    }

    _map.erase(projectionName);
    _vector.pop_back();

    return true;
}

bool ProjectionNameOrderPreservingSet::isEqualIgnoreOrder(
    const ProjectionNameOrderPreservingSet& other) const {
    size_t numMatches = 0;
    for (const auto& projectionName : _vector) {
        if (other.find(projectionName)) {
            numMatches++;
        } else {
            return false;
        }
    }

    return numMatches == other._vector.size();
}

const ProjectionNameVector& ProjectionNameOrderPreservingSet::getVector() const {
    return _vector;
}

bool FieldProjectionMap::operator==(const FieldProjectionMap& other) const {
    return _ridProjection == other._ridProjection && _rootProjection == other._rootProjection &&
        _fieldProjections == other._fieldProjections;
}

bool MemoLogicalNodeId::operator==(const MemoLogicalNodeId& other) const {
    return _groupId == other._groupId && _index == other._index;
}

size_t NodeIdHash::operator()(const MemoLogicalNodeId& id) const {
    size_t result = 17;
    updateHash(result, std::hash<GroupIdType>()(id._groupId));
    updateHash(result, std::hash<size_t>()(id._index));
    return result;
}

bool MemoPhysicalNodeId::operator==(const MemoPhysicalNodeId& other) const {
    return _groupId == other._groupId && _index == other._index;
}

DebugInfo DebugInfo::kDefaultForTests =
    DebugInfo(true, DebugInfo::kDefaultDebugLevelForTests, DebugInfo::kIterationLimitForTests);
DebugInfo DebugInfo::kDefaultForProd = DebugInfo(false, 0, -1);

DebugInfo::DebugInfo(const bool debugMode, const int debugLevel, const int iterationLimit)
    : _debugMode(debugMode), _debugLevel(debugLevel), _iterationLimit(iterationLimit) {}

bool DebugInfo::isDebugMode() const {
    return _debugMode;
}

bool DebugInfo::hasDebugLevel(const int debugLevel) const {
    return _debugLevel >= debugLevel;
}

bool DebugInfo::exceedsIterationLimit(const int iterations) const {
    return _iterationLimit >= 0 && iterations > _iterationLimit;
}

CostType CostType::kInfinity = CostType(true /*isInfinite*/, 0.0);
CostType CostType::kZero = CostType(false /*isInfinite*/, 0.0);

CostType::CostType(const bool isInfinite, const double cost)
    : _isInfinite(isInfinite), _cost(cost) {
    uassert(6624346, "Cost is negative", _cost >= 0.0);
}

bool CostType::operator<(const CostType& other) const {
    return !_isInfinite && (other._isInfinite || _cost + kPrecision < other._cost);
}

CostType CostType::operator+(const CostType& other) const {
    return (_isInfinite || other._isInfinite) ? kInfinity : fromDouble(_cost + other._cost);
}

CostType CostType::operator-(const CostType& other) const {
    uassert(6624001, "Cannot subtract an infinite cost", !other.isInfinite());
    return _isInfinite ? kInfinity : fromDouble(std::max(0.0, _cost - other._cost));
}

CostType& CostType::operator+=(const CostType& other) {
    *this = (*this + other);
    return *this;
}

CostType CostType::fromDouble(const double cost) {
    uassert(8423327, "Invalid cost.", !std::isnan(cost) && cost >= 0.0);
    return CostType(false /*isInfinite*/, cost);
}

std::string CostType::toString() const {
    std::ostringstream os;
    if (_isInfinite) {
        os << "{Infinite cost}";
    } else {
        os << _cost;
    }
    return os.str();
}

double CostType::getCost() const {
    uassert(6624002, "Attempted to coerce infinite cost to a double", !_isInfinite);
    return _cost;
}

bool CostType::isInfinite() const {
    return _isInfinite;
}

CollationOp reverseCollationOp(const CollationOp op) {
    switch (op) {
        case CollationOp::Ascending:
            return CollationOp::Descending;
        case CollationOp::Descending:
            return CollationOp::Ascending;
        case CollationOp::Clustered:
            return CollationOp::Clustered;

        default:
            MONGO_UNREACHABLE;
    }
}

bool collationOpsCompatible(const CollationOp availableOp, const CollationOp requiredOp) {
    return requiredOp == CollationOp::Clustered || requiredOp == availableOp;
}

bool collationsCompatible(const ProjectionCollationSpec& available,
                          const ProjectionCollationSpec& required) {
    // Check if required is more restrictive than available. If yes, reject.
    if (available.size() < required.size()) {
        return false;
    }

    for (size_t i = 0; i < required.size(); i++) {
        const auto& requiredEntry = required.at(i);
        const auto& availableEntry = available.at(i);

        if (requiredEntry.first != availableEntry.first ||
            !collationOpsCompatible(availableEntry.second, requiredEntry.second)) {
            return false;
        }
    }

    // Available is at least as restrictive as required.
    return true;
}

}  // namespace mongo::optimizer
