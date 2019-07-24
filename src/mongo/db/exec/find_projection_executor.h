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
}  // namespace projection_executor
}  // namespace mongo
