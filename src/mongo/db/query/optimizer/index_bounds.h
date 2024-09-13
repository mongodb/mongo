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

#include <cstddef>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/db/query/optimizer/bool_expression.h"
#include "mongo/db/query/optimizer/containers.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/const_fold_interface.h"


namespace mongo::optimizer {

/**
 * Generic bound.
 */
template <class T>
class Bound {
public:
    Bound(bool inclusive, T bound) : _inclusive(inclusive), _bound(std::move(bound)) {}

    bool operator==(const Bound& other) const {
        return _inclusive == other._inclusive && _bound == other._bound;
    }

    bool isInclusive() const {
        return _inclusive;
    }

    const T& getBound() const {
        return _bound;
    }
    T& getBound() {
        return _bound;
    }

protected:
    bool _inclusive;
    T _bound;
};

/**
 * Generic interval.
 */
template <class T>
class Interval {
public:
    Interval(T lowBound, T highBound)
        : _lowBound(std::move(lowBound)), _highBound(std::move(highBound)) {}

    bool operator==(const Interval& other) const {
        return _lowBound == other._lowBound && _highBound == other._highBound;
    }

    bool isEquality() const {
        return _lowBound.isInclusive() && _highBound.isInclusive() && _lowBound == _highBound;
    }

    void reverse() {
        std::swap(_lowBound, _highBound);
    }

    const T& getLowBound() const {
        return _lowBound;
    }
    T& getLowBound() {
        return _lowBound;
    }

    const T& getHighBound() const {
        return _highBound;
    }
    T& getHighBound() {
        return _highBound;
    }

protected:
    T _lowBound;
    T _highBound;
};

/**
 * Represents a bound in an simple interval (interval over one projection). The bound can be a
 * constant or an expression (e.g. a formula). This is a logical abstraction.
 */
class BoundRequirement : public Bound<ABT> {
    using Base = Bound<ABT>;

public:
    static BoundRequirement makeMinusInf();
    static BoundRequirement makePlusInf();

    BoundRequirement(bool inclusive, ABT bound);

    bool isMinusInf() const;
    bool isPlusInf() const;
};

/**
 * Represents a simple interval (interval over one projection). This is a logical abstraction. It
 * counts low and high bounds which may be inclusive or exclusive.
 */
class IntervalRequirement : public Interval<BoundRequirement> {
    using Base = Interval<BoundRequirement>;

public:
    IntervalRequirement();
    IntervalRequirement(BoundRequirement lowBound, BoundRequirement highBound);

    bool isFullyOpen() const;

    /**
     * Checks whether the interval is exactly [MaxKey, MinKey]. Although this is not the
     * only always-false interval, it is the canonical one we use after simplifying.
     */
    bool isAlwaysFalse() const;

    bool isConstant() const;
};

/**
 * Represents an expression (consisting of possibly nested unions and intersections) over an
 * interval.
 */
using IntervalReqExpr = BoolExpr<IntervalRequirement>;

/**
 * Checks if the interval is always true: it contains all possible values. This is encoded as
 * [MinKey, MaxKey].
 */
bool isIntervalReqFullyOpenDNF(const IntervalReqExpr::Node& n);

/**
 * Checks if the interval is always false: it does not contain any values. This is encoded as
 * [MaxKey, MinKey]
 */
bool isIntervalReqAlwaysFalseDNF(const IntervalReqExpr::Node& n);

/**
 * Represents a bound in a compound interval, which encodes an equality prefix. It consists of a
 * vector of expressions, which represents an index bound. This is a physical abstraction.
 */
class CompoundBoundRequirement : public Bound<ABTVector> {
    using Base = Bound<ABTVector>;

public:
    CompoundBoundRequirement(bool inclusive, ABTVector bound);

    bool isMinusInf() const;
    bool isPlusInf() const;
    bool isConstant() const;

    size_t size() const;

    // Extend the current compound bound with a simple bound. It is the caller's responsibility to
    // ensure we confirm to an equality prefix.
    void push_back(BoundRequirement bound);
};

/**
 * An interval of compound keys: each endpoint is a compound key, with one expression per index key.
 * This is a physical primitive tied to a specific index.
 */
class CompoundIntervalRequirement : public Interval<CompoundBoundRequirement> {
    using Base = Interval<CompoundBoundRequirement>;

public:
    CompoundIntervalRequirement();
    CompoundIntervalRequirement(CompoundBoundRequirement lowBound,
                                CompoundBoundRequirement highBound);

    bool isFullyOpen() const;

    size_t size() const;
    void push_back(IntervalRequirement interval);
};

// Unions and conjunctions of individual compound intervals.
using CompoundIntervalReqExpr = BoolExpr<CompoundIntervalRequirement>;

}  // namespace mongo::optimizer
