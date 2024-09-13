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

#include <algorithm>
#include <boost/none.hpp>
#include <boost/optional.hpp>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/utils/abt_compare.h"
#include "mongo/db/query/optimizer/utils/abt_hash.h"
#include "mongo/db/query/optimizer/utils/strong_alias.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/util/assert_util.h"


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

bool IntervalRequirement::isAlwaysFalse() const {
    return _lowBound.isPlusInf() && _highBound.isMinusInf();
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

bool isIntervalReqAlwaysFalseDNF(const IntervalReqExpr::Node& n) {
    if (auto singular = IntervalReqExpr::getSingularDNF(n); singular && singular->isAlwaysFalse()) {
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

}  // namespace mongo::optimizer
