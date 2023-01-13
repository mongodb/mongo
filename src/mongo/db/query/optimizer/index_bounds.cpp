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

#include "mongo/db/query/optimizer/index_bounds.h"

#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/utils/abt_compare.h"
#include "mongo/db/query/optimizer/utils/utils.h"


namespace mongo::optimizer {

BoundRequirement BoundRequirement::makeMinusInf() {
    return {true /*inclusive*/, Constant::minKey()};
}

BoundRequirement BoundRequirement::makePlusInf() {
    return {true /*inclusive*/, Constant::maxKey()};
}

BoundRequirement::BoundRequirement(bool inclusive, ABT bound)
    : _inclusive(inclusive), _bound(std::move(bound)) {
    assertExprSort(_bound);
}

bool BoundRequirement::operator==(const BoundRequirement& other) const {
    return _inclusive == other._inclusive && _bound == other._bound;
}

bool BoundRequirement::isInclusive() const {
    return _inclusive;
}

bool BoundRequirement::isMinusInf() const {
    return _inclusive && _bound == Constant::minKey();
}

bool BoundRequirement::isPlusInf() const {
    return _inclusive && _bound == Constant::maxKey();
}

const ABT& BoundRequirement::getBound() const {
    return _bound;
}

IntervalRequirement::IntervalRequirement()
    : IntervalRequirement(BoundRequirement::makeMinusInf(), BoundRequirement::makePlusInf()) {}

IntervalRequirement::IntervalRequirement(BoundRequirement lowBound, BoundRequirement highBound)
    : _lowBound(std::move(lowBound)), _highBound(std::move(highBound)) {}

bool IntervalRequirement::operator==(const IntervalRequirement& other) const {
    return _lowBound == other._lowBound && _highBound == other._highBound;
}

bool IntervalRequirement::isFullyOpen() const {
    return _lowBound.isMinusInf() && _highBound.isPlusInf();
}

bool IntervalRequirement::isEquality() const {
    return _lowBound.isInclusive() && _highBound.isInclusive() && _lowBound == _highBound;
}

const BoundRequirement& IntervalRequirement::getLowBound() const {
    return _lowBound;
}

BoundRequirement& IntervalRequirement::getLowBound() {
    return _lowBound;
}

const BoundRequirement& IntervalRequirement::getHighBound() const {
    return _highBound;
}

BoundRequirement& IntervalRequirement::getHighBound() {
    return _highBound;
}

void IntervalRequirement::reverse() {
    std::swap(_lowBound, _highBound);
}

bool IntervalRequirement::isConstant() const {
    return getLowBound().getBound().is<Constant>() && getHighBound().getBound().is<Constant>();
}

PartialSchemaKey::PartialSchemaKey(ABT path) : PartialSchemaKey(boost::none, std::move(path)) {}

PartialSchemaKey::PartialSchemaKey(ProjectionName projectionName, ABT path)
    : PartialSchemaKey(boost::optional<ProjectionName>{std::move(projectionName)},
                       std::move(path)) {}

PartialSchemaKey::PartialSchemaKey(boost::optional<ProjectionName> projectionName, ABT path)
    : _projectionName(std::move(projectionName)), _path(std::move(path)) {
    assertPathSort(_path);
}

bool PartialSchemaKey::operator==(const PartialSchemaKey& other) const {
    return _projectionName == other._projectionName && _path == other._path;
}

bool isIntervalReqFullyOpenDNF(const IntervalReqExpr::Node& n) {
    if (auto singular = IntervalReqExpr::getSingularDNF(n); singular && singular->isFullyOpen()) {
        return true;
    }
    return false;
}

PartialSchemaRequirement::PartialSchemaRequirement(
    boost::optional<ProjectionName> boundProjectionName,
    IntervalReqExpr::Node intervals,
    const bool isPerfOnly)
    : _boundProjectionName(std::move(boundProjectionName)),
      _intervals(std::move(intervals)),
      _isPerfOnly(isPerfOnly) {
    tassert(6624154,
            "Cannot have perf only requirement which also binds",
            !_isPerfOnly || !_boundProjectionName);
}

bool PartialSchemaRequirement::operator==(const PartialSchemaRequirement& other) const {
    return _boundProjectionName == other._boundProjectionName && _intervals == other._intervals &&
        _isPerfOnly == other._isPerfOnly;
}

const boost::optional<ProjectionName>& PartialSchemaRequirement::getBoundProjectionName() const {
    return _boundProjectionName;
}

const IntervalReqExpr::Node& PartialSchemaRequirement::getIntervals() const {
    return _intervals;
}

bool PartialSchemaRequirement::getIsPerfOnly() const {
    return _isPerfOnly;
}

bool PartialSchemaRequirement::mayReturnNull(const ConstFoldFn& constFold) const {
    return _boundProjectionName && checkMaybeHasNull(getIntervals(), constFold);
};

bool IndexPath3WComparator::operator()(const ABT& path1, const ABT& path2) const {
    return compareExprAndPaths(path1, path2) < 0;
}

bool PartialSchemaKeyLessComparator::operator()(const PartialSchemaKey& k1,
                                                const PartialSchemaKey& k2) const {
    if (const auto& p1 = k1._projectionName) {
        if (const auto& p2 = k2._projectionName) {
            const int projCmp = p1->compare(*p2);
            if (projCmp != 0) {
                return projCmp < 0;
            }
            // Fallthrough to comparison below.
        } else {
            return false;
        }
    } else if (k2._projectionName) {
        return false;
    }

    return compareExprAndPaths(k1._path, k2._path) < 0;
}

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


ResidualRequirement::ResidualRequirement(PartialSchemaKey key,
                                         PartialSchemaRequirement req,
                                         size_t entryIndex)
    : _key(std::move(key)), _req(std::move(req)), _entryIndex(entryIndex) {}

bool ResidualRequirement::operator==(const ResidualRequirement& other) const {
    return _key == other._key && _req == other._req && _entryIndex == other._entryIndex;
}

ResidualRequirementWithCE::ResidualRequirementWithCE(PartialSchemaKey key,
                                                     PartialSchemaRequirement req,
                                                     CEType ce)
    : _key(std::move(key)), _req(std::move(req)), _ce(ce) {}

EqualityPrefixEntry::EqualityPrefixEntry(const size_t startPos)
    : _startPos(startPos), _interval(CompoundIntervalReqExpr::makeSingularDNF()), _predPosSet() {}

bool EqualityPrefixEntry::operator==(const EqualityPrefixEntry& other) const {
    return _startPos == other._startPos && _interval == other._interval &&
        _predPosSet == other._predPosSet;
}

CandidateIndexEntry::CandidateIndexEntry(std::string indexDefName)
    : _indexDefName(std::move(indexDefName)),
      _fieldProjectionMap(),
      _eqPrefixes(),
      _correlatedProjNames(),
      _residualRequirements(),
      _predTypes(),
      _intervalPrefixSize(0) {}

bool CandidateIndexEntry::operator==(const CandidateIndexEntry& other) const {
    return _indexDefName == other._indexDefName &&
        _fieldProjectionMap == other._fieldProjectionMap && _eqPrefixes == other._eqPrefixes &&
        _correlatedProjNames == other._correlatedProjNames &&
        _residualRequirements == other._residualRequirements && _predTypes == other._predTypes &&
        _intervalPrefixSize == other._intervalPrefixSize;
}

bool ScanParams::operator==(const ScanParams& other) const {
    return _fieldProjectionMap == other._fieldProjectionMap &&
        _residualRequirements == other._residualRequirements;
}

}  // namespace mongo::optimizer
