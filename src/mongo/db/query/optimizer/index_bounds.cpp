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

BoundRequirement::BoundRequirement(bool inclusive, ABT bound) : Base(inclusive, std::move(bound)) {
    assertExprSort(_bound);
}

bool BoundRequirement::isMinusInf() const {
    return _inclusive && _bound == Constant::minKey();
}

bool BoundRequirement::isPlusInf() const {
    return _inclusive && _bound == Constant::maxKey();
}

IntervalRequirement::IntervalRequirement()
    : IntervalRequirement(BoundRequirement::makeMinusInf(), BoundRequirement::makePlusInf()) {}

IntervalRequirement::IntervalRequirement(BoundRequirement lowBound, BoundRequirement highBound)
    : Base(std::move(lowBound), std::move(highBound)) {}

bool IntervalRequirement::isFullyOpen() const {
    return _lowBound.isMinusInf() && _highBound.isPlusInf();
}

bool IntervalRequirement::isConstant() const {
    return getLowBound().getBound().is<Constant>() && getHighBound().getBound().is<Constant>();
}

bool isIntervalReqFullyOpenDNF(const IntervalReqExpr::Node& n) {
    if (auto singular = IntervalReqExpr::getSingularDNF(n); singular && singular->isFullyOpen()) {
        return true;
    }
    return false;
}

CompoundBoundRequirement::CompoundBoundRequirement(bool inclusive, ABTVector bound)
    : Base(inclusive, std::move(bound)) {
    for (const auto& expr : _bound) {
        assertExprSort(expr);
    }
}

bool CompoundBoundRequirement::isMinusInf() const {
    return _inclusive && std::all_of(_bound.cbegin(), _bound.cend(), [](const ABT& element) {
               return element == Constant::minKey();
           });
}

bool CompoundBoundRequirement::isPlusInf() const {
    return _inclusive && std::all_of(_bound.cbegin(), _bound.cend(), [](const ABT& element) {
               return element == Constant::maxKey();
           });
}

bool CompoundBoundRequirement::isConstant() const {
    return std::all_of(
        _bound.cbegin(), _bound.cend(), [](const ABT& element) { return element.is<Constant>(); });
}

size_t CompoundBoundRequirement::size() const {
    return _bound.size();
}

void CompoundBoundRequirement::push_back(BoundRequirement bound) {
    _inclusive &= bound.isInclusive();
    _bound.push_back(std::move(bound.getBound()));
}

CompoundIntervalRequirement::CompoundIntervalRequirement()
    : CompoundIntervalRequirement({true /*inclusive*/, {}}, {true /*inclusive*/, {}}) {}

CompoundIntervalRequirement::CompoundIntervalRequirement(CompoundBoundRequirement lowBound,
                                                         CompoundBoundRequirement highBound)
    : Base(std::move(lowBound), std::move(highBound)) {}

bool CompoundIntervalRequirement::isFullyOpen() const {
    return _lowBound.isMinusInf() && _highBound.isPlusInf();
}

size_t CompoundIntervalRequirement::size() const {
    return _lowBound.size();
}

void CompoundIntervalRequirement::push_back(IntervalRequirement interval) {
    _lowBound.push_back(std::move(interval.getLowBound()));
    _highBound.push_back(std::move(interval.getHighBound()));
}

PartialSchemaKey::PartialSchemaKey() : PartialSchemaKey(make<PathIdentity>()) {}

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

ResidualRequirement::ResidualRequirement(PartialSchemaKey key,
                                         PartialSchemaRequirement req,
                                         size_t entryIndex)
    : _key(std::move(key)), _req(std::move(req)), _entryIndex(entryIndex) {}

bool ResidualRequirement::operator==(const ResidualRequirement& other) const {
    return _key == other._key && _req == other._req && _entryIndex == other._entryIndex;
}

ResidualRequirementWithOptionalCE::ResidualRequirementWithOptionalCE(PartialSchemaKey key,
                                                                     PartialSchemaRequirement req,
                                                                     boost::optional<CEType> ce)
    : _key(std::move(key)), _req(std::move(req)), _ce(ce) {}

bool ResidualRequirementWithOptionalCE::operator==(
    const ResidualRequirementWithOptionalCE& other) const {
    return _key == other._key && _req == other._req && _ce == other._ce;
}

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
