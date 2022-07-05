/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <functional>
#include <memory>
#include <set>

#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/util/string_map.h"

namespace mongo {

class MatchExpression;
struct DepsTracker;

namespace expression {

using NodeTraversalFunc = std::function<void(MatchExpression*, std::string)>;

/**
 * Returns true if 'expr' has an $exists predicate on 'path.' Note that this only returns true
 * for an $exists predicatated on the exact path given: it will not return true if there is a
 * $exists predicated on a prefix of the path.
 */
bool hasExistencePredicateOnPath(const MatchExpression& expr, StringData path);

/**
 * Returns true if the documents matched by 'lhs' are a subset of the documents matched by
 * 'rhs', i.e. a document matched by 'lhs' must also be matched by 'rhs', and false otherwise.
 *
 * With respect to partial indexes, 'lhs' corresponds to the query specification and 'rhs'
 * corresponds to the filter specification.
 *
 * e.g.
 *
 *  Suppose that
 *
 *      lhs = { x : 4 }
 *      rhs = { x : { $lte : 5 } }
 *
 *      ==> true
 *
 *  Suppose that
 *
 *      lhs = { x : { $gte: 6 } }
 *      rhs = { x : 7 }
 *
 *      ==> false
 */
bool isSubsetOf(const MatchExpression* lhs, const MatchExpression* rhs);

/**
 * Determine if it is possible to split 'expr' into two MatchExpressions, where one is not
 * dependent on any path from 'pathSet', such that applying the two in sequence is equivalent
 * to applying 'expr'.
 *
 * For example, {a: "foo", b: "bar"} is splittable by "b", while
 * {$or: [{a: {$eq: "foo"}}, {b: {$eq: "bar"}}]} is not splittable by "b", due to the $or.
 */
bool isSplittableBy(const MatchExpression& expr, const OrderedPathSet& pathSet);

/**
 * True if no path in either set is contained by a path in the other.  Does not check for
 * dependencies within each of the sets, just across sets.  Runs in 0(n) time.
 *
 * areIndependent([a.b, b, a], [c]) --> true
 * areIndependent([a.b, b, a], [a.b.f]) --> false
 */
bool areIndependent(const OrderedPathSet& pathSet1, const OrderedPathSet& pathSet2);

/**
 * Return true if any of the fieldPaths in prefixCandidates are identical to or an ancestor of any
 * of the fieldpaths in testSet.  The order of the parameters matters -- it's not commutative.
 */
bool containsDependency(const OrderedPathSet& testSet, const OrderedPathSet& prefixCandidates);

/**
 * Determine if 'expr' is reliant upon any path from 'pathSet'.
 */
bool isIndependentOf(const MatchExpression& expr, const OrderedPathSet& pathSet);

/**
 * Determine if 'expr' is reliant only upon paths from 'pathSet'.
 */
bool isOnlyDependentOn(const MatchExpression& expr, const OrderedPathSet& pathSet);

/**
 * Returns whether the path represented by 'first' is an prefix of the path represented by 'second'.
 * Equality is not considered a prefix. For example:
 *
 * a.b is a prefix of a.b.c
 * a.b is not a prefix of a.balloon
 * a is a prefix of a.b
 * a is not a prefix of a
 * a.b is not a prefix of a
 */
bool isPathPrefixOf(StringData first, StringData second);

/**
 * Returns true if the first path is equal to the second path or if either is a prefix
 * of the other.
 */
bool bidirectionalPathPrefixOf(StringData first, StringData second);

/**
 * Applies 'func' to each node of 'expr', where the first argument is a pointer to that actual node
 * (not a copy), and the second argument is the path to that node. Callers should not depend on the
 * order of the traversal of the nodes.
 */
void mapOver(MatchExpression* expr, NodeTraversalFunc func, std::string path = "");

using ShouldSplitExprFunc = std::function<bool(const MatchExpression&, const OrderedPathSet&)>;

/**
 * Attempt to split 'expr' into two MatchExpressions according to 'func'. 'func' describes the
 * conditions under which its argument can be split from 'expr'. Returns two pointers, where each
 * new MatchExpression contains a portion of 'expr'. The first contains the parts of 'expr' which
 * satisfy 'func', and the second are the remaining parts of 'expr', such that applying the matches
 * in sequence is equivalent to applying 'expr'. If 'expr' cannot be split, returns {nullptr, expr}.
 * If 'expr' can be entirely split, returns {expr, nullptr}. Takes ownership of 'expr'.
 *
 * For example, the default behavior is to split 'match' into two where the first is not reliant
 * upon any path from 'fields', and the second is the remainder.
 *
 * Any paths which should be renamed are encoded in 'renames', which maps from path names in 'expr'
 * to the new values of those paths. If the return value is {exprLeft, exprRight} or {exprLeft,
 * nullptr}, exprLeft will reflect the path renames. For example, suppose the original match
 * expression is {old: {$gt: 3}} and 'renames' contains the mapping "old" => "new". The returned
 * exprLeft value will be {new: {$gt: 3}}, provided that "old" is not in 'fields'.
 *
 * Never returns {nullptr, nullptr}.
 */
std::pair<std::unique_ptr<MatchExpression>, std::unique_ptr<MatchExpression>>
splitMatchExpressionBy(std::unique_ptr<MatchExpression> expr,
                       const OrderedPathSet& fields,
                       const StringMap<std::string>& renames,
                       ShouldSplitExprFunc func = isIndependentOf);

/**
 * Applies the renames specified in 'renames' to 'expr'. 'renames' maps from path names in 'expr'
 * to the new values of those paths. For example, suppose the original match expression is
 * {old: {$gt: 3}} and 'renames' contains the mapping "old" => "new". At the end, 'expr' will be
 * {new: {$gt: 3}}.
 */
void applyRenamesToExpression(MatchExpression* expr, const StringMap<std::string>& renames);

/**
 * Split a MatchExpression into two parts:
 *  - Filters which can be applied to one "column" at a time in a columnstore index. This will be
 *    returned as a map from path to MatchExpression. For this to be safe:
 *    - any predicate which does not  match should disqualify the entire document
 *    - any document which doesn't contain the path should not match.
 *  - A "residual" predicate which captures any pieces of the expression which cannot be pushed down
 *    into a column, either because it would be incorrect to do so, or we're not smart enough to do
 *    so yet.
 */
std::pair<StringMap<std::unique_ptr<MatchExpression>>, std::unique_ptr<MatchExpression>>
splitMatchExpressionForColumns(const MatchExpression* me);

/**
 * Serializes this complex data structure for debugging purposes.
 */
std::string filterMapToString(const StringMap<std::unique_ptr<MatchExpression>>&);

}  // namespace expression
}  // namespace mongo
