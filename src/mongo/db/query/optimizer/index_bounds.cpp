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


namespace mongo::optimizer {

BoundRequirement BoundRequirement::makeMinusInf() {
    return {true /*inclusive*/, Constant::minKey()};
}

BoundRequirement BoundRequirement::makePlusInf() {
    return {true /*inclusive*/, Constant::maxKey()};
}

BoundRequirement::BoundRequirement(bool inclusive, ABT bound)
    : _inclusive(inclusive), _bound(std::move(bound)) {}

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

PartialSchemaKey::PartialSchemaKey() : PartialSchemaKey({}, make<PathIdentity>()) {}

PartialSchemaKey::PartialSchemaKey(ProjectionName projectionName)
    : PartialSchemaKey(std::move(projectionName), make<PathIdentity>()) {}

PartialSchemaKey::PartialSchemaKey(ProjectionName projectionName, ABT path)
    : _projectionName(std::move(projectionName)), _path(std::move(path)) {
    assertPathSort(_path);
}

bool PartialSchemaKey::operator==(const PartialSchemaKey& other) const {
    return _projectionName == other._projectionName && _path == other._path;
}

bool PartialSchemaKey::emptyPath() const {
    return _path.is<PathIdentity>();
}

bool isIntervalReqFullyOpenDNF(const IntervalReqExpr::Node& n) {
    if (auto singular = IntervalReqExpr::getSingularDNF(n); singular && singular->isFullyOpen()) {
        return true;
    }
    return false;
}

PartialSchemaRequirement::PartialSchemaRequirement()
    : _intervals(IntervalReqExpr::makeSingularDNF()) {}

PartialSchemaRequirement::PartialSchemaRequirement(ProjectionName boundProjectionName,
                                                   IntervalReqExpr::Node intervals)
    : _boundProjectionName(std::move(boundProjectionName)), _intervals(std::move(intervals)) {}

bool PartialSchemaRequirement::operator==(const PartialSchemaRequirement& other) const {
    return _boundProjectionName == other._boundProjectionName && _intervals == other._intervals;
}

bool PartialSchemaRequirement::hasBoundProjectionName() const {
    return !_boundProjectionName.empty();
}

const ProjectionName& PartialSchemaRequirement::getBoundProjectionName() const {
    return _boundProjectionName;
}

void PartialSchemaRequirement::setBoundProjectionName(ProjectionName boundProjectionName) {
    _boundProjectionName = std::move(boundProjectionName);
}

const IntervalReqExpr::Node& PartialSchemaRequirement::getIntervals() const {
    return _intervals;
}

IntervalReqExpr::Node& PartialSchemaRequirement::getIntervals() {
    return _intervals;
}

/**
 * Helper class used to compare PartialSchemaKey objects.
 */
class Path3WCompare {
public:
    Path3WCompare() {}

    int compareTags(const ABT& n, const ABT& other) {
        const auto t1 = n.tagOf();
        const auto t2 = other.tagOf();
        return (t1 == t2) ? 0 : ((t1 < t2) ? -1 : 1);
    }

    int operator()(const ABT& n, const PathGet& node, const ABT& other) {
        if (auto otherGet = other.cast<PathGet>(); otherGet != nullptr) {
            const int varCmp = node.name().compare(otherGet->name());
            return (varCmp == 0) ? node.getPath().visit(*this, otherGet->getPath()) : varCmp;
        }
        return compareTags(n, other);
    }

    int operator()(const ABT& n, const PathTraverse& node, const ABT& other) {
        if (auto otherTraverse = other.cast<PathTraverse>(); otherTraverse != nullptr) {
            return node.getPath().visit(*this, otherTraverse->getPath());
        }
        return compareTags(n, other);
    }

    int operator()(const ABT& n, const PathIdentity& node, const ABT& other) {
        return compareTags(n, other);
    }

    template <typename T, typename... Ts>
    int operator()(const ABT& /*n*/, const T& /*node*/, Ts&&...) {
        uasserted(6624079, "Unexpected node type");
        return 0;
    }

    static int compare(const ABT& node, const ABT& other) {
        Path3WCompare instance;
        return node.visit(instance, other);
    }
};

bool PartialSchemaKeyLessComparator::operator()(const PartialSchemaKey& k1,
                                                const PartialSchemaKey& k2) const {
    const int projCmp = k1._projectionName.compare(k2._projectionName);
    if (projCmp != 0) {
        return projCmp < 0;
    }
    return Path3WCompare::compare(k1._path, k2._path) < 0;
}

ResidualRequirement::ResidualRequirement(PartialSchemaKey key,
                                         PartialSchemaRequirement req,
                                         CEType ce)
    : _key(std::move(key)), _req(std::move(req)), _ce(ce) {}

bool CandidateIndexEntry::operator==(const CandidateIndexEntry& other) const {
    return _fieldProjectionMap == other._fieldProjectionMap && _intervals == other._intervals &&
        _residualRequirements == other._residualRequirements &&
        _fieldsToCollate == other._fieldsToCollate &&
        _intervalPrefixSize == other._intervalPrefixSize;
}

IndexSpecification::IndexSpecification(std::string scanDefName,
                                       std::string indexDefName,
                                       MultiKeyIntervalRequirement interval,
                                       bool reverseOrder)
    : _scanDefName(std::move(scanDefName)),
      _indexDefName(std::move(indexDefName)),
      _interval(std::move(interval)),
      _reverseOrder(reverseOrder) {}

bool IndexSpecification::operator==(const IndexSpecification& other) const {
    return _scanDefName == other._scanDefName && _indexDefName == other._indexDefName &&
        _interval == other._interval && _reverseOrder == other._reverseOrder;
}

const std::string& IndexSpecification::getScanDefName() const {
    return _scanDefName;
}

const std::string& IndexSpecification::getIndexDefName() const {
    return _indexDefName;
}

const MultiKeyIntervalRequirement& IndexSpecification::getInterval() const {
    return _interval;
}

bool IndexSpecification::isReverseOrder() const {
    return _reverseOrder;
}

}  // namespace mongo::optimizer
