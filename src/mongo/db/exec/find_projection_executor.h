/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/field_path.h"

namespace mongo {
namespace projection_executor {
/**
 * Applies a positional projection on the first array found in the 'path' on the 'input' document.
 * The applied projection is stored in the 'output' document. If the output document contains a
 * field under which the projection is saved, it will be overwritten with the projection value.
 * The 'matchExpr' specifies a condition to locate the first matching element in the array and must
 * match the input document. For example, given:
 *
 *    - the 'input' document {bar: 1, foo: {bar: [1,2,6,10]}}
 *    - the 'matchExpr' condition {bar: 1, 'foo.bar': {$gte: 5}}
 *    - and the 'path' for the positional projection of 'foo.bar'
 *
 * The resulting document will contain the following element: {foo: {bar: [6]}}
 *
 * Throws an AssertionException if 'matchExpr' matches the input document, but an array element
 * satisfying positional projection requirements cannot be found.
 */
void applyPositionalProjection(const Document& input,
                               const MatchExpression& matchExpr,
                               const FieldPath& path,
                               MutableDocument* output);
/**
 * Applies an $elemMatch projection on the array at the given 'path' on the 'input' document. The
 * applied projection is stored in the output document. If the output document contains a field
 * under which the projection is saved, it will be overwritten with the projection value. The
 * 'matchExpr' specifies a condition to locate the first matching element in the array and must
 * contain the $elemMatch operator. This function doesn't enforce this requirement and the caller
 * must ensure that the valid match expression is passed. For example, given:
 *
 *   - the 'input' document {foo: [{bar: 1}, {bar: 2}, {bar: 3}]}
 *   - the 'matchExpr' condition {foo: {$elemMatch: {bar: {$gt: 1}}}}
 *   - and the 'path' of 'foo'
 *
 * The resulting document will contain the following element: {foo: [{bar: 2}]}
 *
 * If the 'matchExpr' does not match the input document, the function returns without modifying
 * the output document.
 *
 * Since the $elemMatch projection cannot be used with a nested field, the 'path' value must not
 * be a dotted path, otherwise an invariant will be triggered.
 */
void applyElemMatchProjection(const Document& input,
                              const MatchExpression& matchExpr,
                              const FieldPath& path,
                              MutableDocument* output);
/**
 * Applies a $slice projection on the array at the given 'path' on the 'input' document. The applied
 * projection is returned as a Document. The 'skip' parameter indicates the number of items in the
 * array to skip and the 'limit' indicates the number of items to return.
 *
 * If any of the 'skip' or 'limit' is negative, it is applied to the last items in the array (e.g.,
 * {$slice: -5} returns the last five items in array, and {$slice: [-20, 10]} returns 10 items,
 * beginning with the item that is 20th from the last item of the array.
 *
 * If the 'skip' is specified, the 'limit' cannot be negative.
 *
 * For example, given:
 *
 *   - the 'input' document {foo: [{bar: 1}, {bar: 2}, {bar: 3}, {bar: 4}]}
 *   - the path of 'foo'
 *   - the 'skip' of -3 and 'limit' of 2
 *
 * The resulting document will contain the following element: {foo: [{bar: 2}, {bar: 3}]}.
 */
Document applySliceProjection(const Document& input,
                              const FieldPath& path,
                              boost::optional<int> skip,
                              int limit);

}  // namespace projection_executor
}  // namespace mongo
