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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/find_projection_executor.h"

namespace mongo {
namespace projection_executor {
namespace {
/**
 * Extracts an element from the array 'arr' at position 'elemIndex'. The 'elemIndex' string
 * parameter must hold a value which can be converted to an unsigned integer. If 'elemIndex' is not
 * within array boundaries, an empty Value is returned.
 */
Value extractArrayElement(const Value& arr, const std::string& elemIndex) {
    auto index = str::parseUnsignedBase10Integer(elemIndex);
    invariant(index);
    return arr[*index];
}
}  // namespace

void applyPositionalProjection(const Document& input,
                               const MatchExpression& matchExpr,
                               const FieldPath& path,
                               MutableDocument* output) {
    invariant(output);

    // Try to find the first matching array element from the 'input' document based on the condition
    // specified as 'matchExpr'. If such an element is found, its position within an array will be
    // recorded in the 'details' object. Since 'matchExpr' used with the positional projection is
    // the very same selection filter expression in the find command, the input document passed to
    // this function should have already been matched against this expression, so we'll use an
    // invariant to make sure this is the case indeed.
    MatchDetails details;
    details.requestElemMatchKey();
    invariant(matchExpr.matchesBSON(input.toBson(), &details));

    // At this stage we know that the 'input' document matches against the specified condition,
    // but the matching array element may not be found. This can happen if the field, specified
    // in the match condition is not an array. For example, if the match condition is {foo: 3}
    // and the document is {_id: 1, foo: 3}, then we will match this document but the matching
    // array element position won't be recorded. In this case, we don't want to error out but
    // to exclude the positional projection path from the output document. So, we will walk the
    // 'path' on the 'input' document trying to locate the first array element. If it can be
    // found, then we will extract the matching element from this array and will store it as
    // the current sub-path in the 'output' document. Otherwise, just leave the 'output'
    // document untouched.
    for (auto [ind, subDoc] = std::pair{0ULL, input}; ind < path.getPathLength(); ind++) {
        switch (auto val = subDoc[path.getFieldName(ind)]; val.getType()) {
            case BSONType::Array: {
                // Raise an error if we found the first array on the 'path', but the matching array
                // element index wasn't recorded in the 'details' object. This can happen when the
                // match expression doesn't conform to the positional projection requirements. E.g.,
                // when it contains multiple conditions on the array field being projected, which
                // may override each other, making it impossible to correctly locate the matching
                // element.
                uassert(51246,
                        "positional operator '.$' couldn't find a matching element in the array",
                        details.hasElemMatchKey());

                auto elemMatchKey = details.elemMatchKey();
                auto matchingElem = extractArrayElement(val, elemMatchKey);
                uassert(
                    51247, "positional operator '.$' element mismatch", !matchingElem.missing());

                output->setNestedField(path.getSubpath(ind),
                                       Value{std::vector<Value>{matchingElem}});
                return;
            }
            case BSONType::Object:
                subDoc = val.getDocument();
                break;
            default:
                break;
        }
    }
}
}  // namespace projection_executor
}  // namespace mongo
