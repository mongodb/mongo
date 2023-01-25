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

#include "mongo/db/query/optimizer/index_bounds.h"

#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {

/**
 * Constant fold the bounds of a DNF interval.
 */
void constFoldInterval(IntervalRequirement& interval, const ConstFoldFn& constFold);

/**
 * Intersects or unions two intervals without simplification which might depend on multi-keyness.
 * Currently assumes intervals are in DNF.
 * TODO: handle generic interval expressions (not necessarily DNF).
 */
void combineIntervalsDNF(bool intersect,
                         IntervalReqExpr::Node& target,
                         const IntervalReqExpr::Node& source);

/**
 * Simplifies two unioned intervals, variable or constant.
 */
std::vector<IntervalRequirement> unionTwoIntervals(const IntervalRequirement& int1,
                                                   const IntervalRequirement& int2,
                                                   const ConstFoldFn& constFold);

/**
 * Union DNF intervals. Analyzes constant intervals, and merges them if possible. Requires interval
 * to be normalized, as well as intersection simplification to be run before this is called.
 */
boost::optional<IntervalReqExpr::Node> unionDNFIntervals(const IntervalReqExpr::Node& intervalDNF,
                                                         const ConstFoldFn& constFold);

/**
 * Intersect all intervals within each conjunction of intervals in a disjunction of intervals.
 * Notice that all intervals reference the same path (which is an index field).
 * Return a DNF of the intersected intervals, where there is at most one interval inside each
 * conjunct. If the resulting interval is empty, return boost::none.
 * The intervals themselves can contain Constants, Variables, or arbitrary arithmetic expressions.
 * TODO: handle generic interval expressions (not necessarily DNF).
 */
boost::optional<IntervalReqExpr::Node> intersectDNFIntervals(
    const IntervalReqExpr::Node& intervalDNF, const ConstFoldFn& constFold);

/**
 * Simplify DNF intervals by analyzing intervals within each conjunction to intersect, and also
 * combining conjunctions with one child, to simplify unions. Returns boost::none if result is
 * empty.
 */
boost::optional<IntervalReqExpr::Node> simplifyDNFIntervals(const IntervalReqExpr::Node& interval,
                                                            const ConstFoldFn& constFold);

/**
 * Combines a source interval over a single path with a target multi-component interval. The
 * multi-component interval is extended to contain an extra field. The resulting multi-component
 * interval defined the boundaries over the index component used by the index access execution
 * operator. If we fail to combine, the target compound interval is left unchanged.
 * Currently we only support a single "equality prefix": 0+ equalities followed by at most
 * inequality, and trailing open intervals.
 * reverseSource flag indicates the sourceInterval corresponds to a descending index, so the bounds
 * are flipped before combining with the target.
 */
bool combineCompoundIntervalsDNF(CompoundIntervalReqExpr::Node& targetIntervals,
                                 const IntervalReqExpr::Node& sourceIntervals,
                                 bool reverseSource);

/**
 * Similar to combineCompoundIntervalsDNF in the sense that the source interval can be considered a
 * fully open one. We want to extend the target multi-component interval with one more entry, such
 * that do not constrain the result. If the last entry's lower bound was inclusive, then we append
 * MinKey, otherwise MaxKey. We do the opposite for the high bound. We also reverse high and low if
 * the reverseSource flag is set.
 */
void padCompoundIntervalsDNF(CompoundIntervalReqExpr::Node& targetIntervals, bool reverseSource);

/**
 * Converts intervals into a normal form. Atoms are compared using the ABT comparison order. Atoms
 * precede Cojunctions and those precede Disjunctions.
 *
 * For example:
 *         Disj
 *   Conj    3    Disj
 *  1   2        Conj  4
 *               6  5
 *
 *   becomes:
 *            Disj
 *      3    Conj    Disj
 *           1  2   4  Conj
 *                      5  6
 */
void normalizeIntervals(IntervalReqExpr::Node& intervals);

/**
 * Analyze the given interval, and convert it into a PathCompare EqMember if possible.
 */
boost::optional<ABT> coerceIntervalToPathCompareEqMember(const IntervalReqExpr::Node& interval);


/**
 * Returns true if all components of the compound interval are equalities.
 */
bool areCompoundIntervalsEqualities(const CompoundIntervalRequirement& intervals);

/**
 * Returns true if the interval corresponds to a simple range (e.g >10 as opposed to a point
 * equality or complex boolean expression of intervals).
 */
bool isSimpleRange(const CompoundIntervalReqExpr::Node& interval);

}  // namespace mongo::optimizer
