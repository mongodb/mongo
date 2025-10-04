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

#include "mongo/base/string_data.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/util/string_map.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mongo {

class PathMatchExpression;

namespace expression {

using NodeTraversalFunc = std::function<void(MatchExpression*, std::string)>;

/**
 * Returns true if 'expr' has a 'searchType' predicated on a path in 'paths' or a descendent of a
 * path in 'paths'.
 *
 * For example, with 'searchType'=EXISTS and 'paths'=["a", "b.c"], then various 'expr's result in:
 * - {a: {$exists: true}}                     ---> true
 * - {$and: [{a: {$exists: false}}, {d: 5}]}  ---> true
 * - {'a.b': {$exists: true}}}                ---> true
 * - {'b.d': {$exists: true}}                 ---> false
 * - {'a': {$type: 'long'}}                   ---> false
 */
bool hasPredicateOnPaths(const MatchExpression& expr,
                         mongo::MatchExpression::MatchType searchType,
                         const OrderedPathSet& paths);

using PathOrExprMatchExpression = std::variant<PathMatchExpression*, ExprMatchExpression*>;
using Renameables = std::vector<std::pair<PathOrExprMatchExpression, std::string>>;

/**
 * Checks if 'expr' has any subexpression which does not have renaming implemented or has renaming
 * implemented but may fail to rename for any one of 'renames'. If there's any such subexpression,
 * we should not proceed with renaming.
 */
bool hasOnlyRenameableMatchExpressionChildren(const MatchExpression& expr,
                                              const StringMap<std::string>& renames);

/**
 * Checks if 'expr' has any subexpression which does not have renaming implemented or has renaming
 * implemented but may fail to rename for any one of 'renames'. If there's any such subexpression,
 * we should not proceed with renaming.
 *
 * This function also fills out 'renameables' with the renameable subexpressions. For
 * PathMatchExpression, the new path is returned in the second element of the pair. For
 * ExprMatchExpression, the second element should be ignored and renames must be applied based on
 * the full 'renames' map.
 *
 * Note: The 'renameables' is filled out while traversing the tree and so, designated as an output
 * parameter as an optimization to avoid traversing the tree again unnecessarily.
 */
bool hasOnlyRenameableMatchExpressionChildren(MatchExpression& expr,
                                              const StringMap<std::string>& renames,
                                              Renameables& renameables);

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
 * Return true if any of the paths in 'prefixCandidates' are identical to or an ancestor of any
 * of the paths in 'testSet'.  The order of the parameters matters -- it's not commutative.
 */
bool containsDependency(const OrderedPathSet& testSet, const OrderedPathSet& prefixCandidates);

/**
 * Returns elements from testSet which:
 *  * Do not appear in toRemove
 *  * Are not prefixed by an element of toRemove
 *  * Are not a prefix of an element of toRemove
 *
 * The remaining elements are paths which would be unaltered if all paths in toRemove were (e.g.,)
 * erased.
 *
 * Post-condition: areIndependent(returnValue, toRemove) == true
 */
OrderedPathSet makeIndependent(OrderedPathSet testSet, const OrderedPathSet& toRemove);

/**
 * Returns true if any of the paths in 'testSet' are an ancestor of any of the other paths in
 * 'testSet'. Examples:
 * containsOverlappingPaths([a.b, a]) --> true
 * containsOverlappingPaths([ab, a, a-b]) --> false
 */
bool containsOverlappingPaths(const OrderedPathSet& testSet);

/**
 * Returns true if any of the paths in 'testSet' contain empty path components.
 */
bool containsEmptyPaths(const OrderedPathSet& testSet);

/**
 * Determines if 'expr' is reliant upon any path from 'pathSet' and can be renamed by 'renames'.
 *
 * In other words: if writing to any of the paths in 'pathSet' (with say $addFields) could change
 * the evaluation of the expression, then it's not independent.
 */
bool isIndependentOfConst(const MatchExpression& expr,
                          const OrderedPathSet& pathSet,
                          const StringMap<std::string>& renames = {});

/**
 * Determines if 'expr' is reliant upon any path from 'pathSet' and can be renamed by 'renames'.
 *
 * In other words: if writing to any of the paths in 'pathSet' (with say $addFields) could change
 * the evaluation of the expression, then it's not independent.
 *
 * Note: For a description of the expected value returned in the 'renameables' output parameter, see
 * the documentation for the 'hasOnlyRenameableMatchExpressionChildren()' function.
 */
bool isIndependentOf(MatchExpression& expr,
                     const OrderedPathSet& pathSet,
                     const StringMap<std::string>& renames,
                     Renameables& renameables);

/**
 * Determines if 'expr' is reliant only upon paths from 'pathSet' and can be renamed by 'renames'.
 */
bool isOnlyDependentOnConst(const MatchExpression& expr,
                            const OrderedPathSet& pathSet,
                            const StringMap<std::string>& renames = {});

/**
 * Determines if 'expr' is reliant only upon paths from 'pathSet' and can be renamed by 'renames'.
 *
 * Note: For a description of the expected value returned in the 'renameables' output parameter, see
 * the documentation for the 'hasOnlyRenameableMatchExpressionChildren()' function.
 */
bool isOnlyDependentOn(MatchExpression& expr,
                       const OrderedPathSet& pathSet,
                       const StringMap<std::string>& renames,
                       Renameables& renameables);

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
 * Applies 'func' to each node of 'expr', where the first argument is a pointer to that actual node
 * (not a copy), and the second argument is the path to that node. Callers should not depend on the
 * order of the traversal of the nodes.
 */
void mapOver(MatchExpression* expr, NodeTraversalFunc func, std::string path = "");

/**
 * Rewrites the match expression to assume that InternalExpr* match expressions return true. These
 * expressions are "imprecise" in that they can return true even for non-passing documents, and
 * rely on a second layer of filtering being applied later.
 */
std::unique_ptr<MatchExpression> assumeImpreciseInternalExprNodesReturnTrue(
    std::unique_ptr<MatchExpression> expr);

using ShouldSplitExprFunc = std::function<bool(
    MatchExpression&, const OrderedPathSet&, const StringMap<std::string>&, Renameables&)>;

/**
 * Attempt to split 'expr' into two MatchExpressions according to 'func'. 'func' describes the
 * conditions under which its argument can be split from 'expr'. Returns two pointers, where each
 * new MatchExpression contains a portion of 'expr'. The first (split out expression) contains the
 * parts of 'expr' which satisfy 'func', and the second (residual expression) are the remaining
 * parts of 'expr', such that applying the matches in sequence is equivalent to applying 'expr'. If
 * 'expr' cannot be split, returns {nullptr, expr}. If 'expr' can be entirely split, returns {expr,
 * nullptr}. Takes ownership of 'expr'.
 *
 * For example, the default behavior is to split 'expr' into two where the split out expression is
 * not reliant upon any path from 'fields', and the residual expression is the remainder.
 *
 * Any paths which might be renamed are encoded in 'renames', which maps from path names in 'expr'
 * to the new values of those paths. If the return value is {splitOutExpr, residualExpr} or
 * {splitOutExpr, nullptr}, splitOutExpr will reflect the path renames. For example, suppose the
 * original match expression is {old: {$gt: 3}} and 'renames' contains the mapping "old" => "new".
 * The returned exprLeft value will be {new: {$gt: 3}}, provided that "old" is not in 'fields'.
 *
 * Never returns {nullptr, nullptr}.
 */
std::pair<std::unique_ptr<MatchExpression>, std::unique_ptr<MatchExpression>>
splitMatchExpressionBy(std::unique_ptr<MatchExpression> expr,
                       const OrderedPathSet& fields,
                       const StringMap<std::string>& renames,
                       ShouldSplitExprFunc func = isIndependentOf);

/**
 * Applies the renames specified in 'renames' & 'renameables'. 'renames' maps from path names in
 * 'expr' to the new values of those paths. For example, suppose the original match expression is
 * {old: {$gt: 3}} and 'renames' contains the mapping "old" => "new". At the end, 'expr' will be
 * {new: {$gt: 3}}.
 *
 * In order to do an in-place renaming of the match expression tree, the caller should first call
 * 'hasOnlyRenameableMatchExpressionChildren()' and then call this function if it returns true,
 * passing through the resulting 'renameables'.
 *
 * Note: To enforce the above precondition, the caller should pass in the output of the call to
 * hasOnlyRenameableMatchExpressionChildren() as the 'renameables' argument. To avoid passing empty
 * vector for 'renameables' like applyRenamesToExpression(expr, {}, {}), the parameter is defined as
 * a pointer.
 */
void applyRenamesToExpression(const StringMap<std::string>& renames,
                              const Renameables* renameables);

/**
 * Copies the 'expr' and applies the renames specified in 'renames' to the copy of 'expr' and
 * returns the renamed copy of 'expr' if renaming is successful. Otherwise, returns nullptr.
 * 'renames' maps from path names in 'expr' to the new values of those paths. For example, suppose
 * the original match expression is {old: {$gt: 3}} and 'renames' contains the mapping "old" =>
 * "new". The returned expression will be {new: {$gt: 3}}.
 */
std::unique_ptr<MatchExpression> copyExpressionAndApplyRenames(
    const MatchExpression* expr, const StringMap<std::string>& renames);

/**
 * Serializes this complex data structure for debugging purposes.
 */
std::string filterMapToString(const StringMap<std::unique_ptr<MatchExpression>>&);

}  // namespace expression
}  // namespace mongo
