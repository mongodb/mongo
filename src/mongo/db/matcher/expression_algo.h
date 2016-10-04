// expression_algo.h

/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/stdx/functional.h"

namespace mongo {

class MatchExpression;
struct DepsTracker;

namespace expression {

using NodeTraversalFunc = stdx::function<void(MatchExpression*, std::string)>;

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
bool isSplittableBy(const MatchExpression& expr, const std::set<std::string>& pathSet);

/**
 * Determine if 'expr' is reliant upon any path from 'pathSet'.
 */
bool isIndependentOf(const MatchExpression& expr, const std::set<std::string>& pathSet);

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
 * (not a copy), and the second argument is the path to that node.
 */
void mapOver(MatchExpression* expr, NodeTraversalFunc func, std::string path = "");

/**
 * Attempt to split 'expr' into two MatchExpressions, where the first is not reliant upon any
 * path from 'fields', such that applying the matches in sequence is equivalent to applying
 * 'expr'. Takes ownership of 'expr'.
 *
 * If 'expr' cannot be split, returns {nullptr, expr}. If 'expr' is entirely independent of
 * 'fields', returns {expr, nullptr}. If 'expr' is partially dependent on 'fields', and partially
 * independent, returns {exprLeft, exprRight}, where each new MatchExpression contains a portion of
 * 'expr'.
 *
 * Never returns {nullptr, nullptr}.
 */
std::pair<std::unique_ptr<MatchExpression>, std::unique_ptr<MatchExpression>>
splitMatchExpressionBy(std::unique_ptr<MatchExpression> expr, const std::set<std::string>& fields);

}  // namespace expression
}  // namespace mongo
