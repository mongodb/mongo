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
 * Holds various parameters required to apply a $slice projection. Populated from the arguments
 * to 'applySliceProjection()'.
 */
struct SliceParams {
    const FieldPath& path;
    const boost::optional<int> skip;
    const int limit;
};

Value applySliceProjectionHelper(const Document& input,
                                 const SliceParams& params,
                                 size_t fieldPathIndex);

/**
 * Returns a portion of the 'array', skipping a number of elements as indicated by the 'skip'
 * parameter, either from the beginning of the array (if 'skip' is positive), or from the end
 * of the array (if 'skip' is negative). The 'limit' indicates the number of items to return.
 * If 'limit' is negative, the last 'limit' number of items in the array are returned.
 *
 * If the 'skip' is specified, the 'limit' cannot be negative.
 */
Value sliceArray(const std::vector<Value>& array, boost::optional<int> skip, int limit) {
    auto start = 0ll;
    auto forward = 0ll;
    const long long len = array.size();

    if (!skip) {
        if (limit < 0) {
            start = std::max(0ll, len + limit);
            forward = len - start;
        } else {
            forward = std::min(len, static_cast<long long>(limit));
        }
    } else {
        // We explicitly disallow a negative limit when skip is specified.
        invariant(limit >= 0);

        if (*skip < 0) {
            start = std::max(0ll, len + *skip);
            forward = std::min(len - start, static_cast<long long>(limit));
        } else {
            start = std::min(len, static_cast<long long>(*skip));
            forward = std::min(len - start, static_cast<long long>(limit));
        }
    }

    invariant(start + forward >= 0);
    invariant(start + forward <= len);
    return Value{std::vector<Value>(array.cbegin() + start, array.cbegin() + start + forward)};
}

/**
 * Applies a $slice projection to the array at the given 'params.path'. For each array element,
 * recursively calls 'applySliceProjectionHelper' if the element is a Document, storing the result
 * in the output array, otherwise just stores the element in the output unmodified.
 *
 * Note we do not expand arrays within arrays this way. For example, {a: [[{b: 1}]]} has no values
 * on the path "a.b", but {a: [{b: 1}]} does, so nested arrays are stored within the output array
 * as regular values.
 */
Value applySliceProjectionToArray(const std::vector<Value>& array,
                                  const SliceParams& params,
                                  size_t fieldPathIndex) {
    std::vector<Value> output;
    output.reserve(array.size());

    for (const auto& elem : array) {
        output.push_back(
            elem.getType() == BSONType::Object
                ? applySliceProjectionHelper(elem.getDocument(), params, fieldPathIndex)
                : elem);
    }

    return Value{output};
}

/**
 * This is a helper function which implements the $slice projection. The strategy for applying a
 * $slice projection is as follows:
 *     * Pick the current path component from the current 'params.path' and store the value from the
 *       'input' doc at this sub-path in 'val'.
 *     * If 'val' is an array and we're at the last component in the 'params.path' - slice the array
 *       and exit recursion, otherwise recursively apply the $slice projection to each element
 *       in the array, and store the result in 'val'.
 *     * If the field value is a document, apply the $slice projection to this document, and store
 *       the result in 'val'.
 *     * Store the computed 'val' in the 'output' document under the current field name.
 */
Value applySliceProjectionHelper(const Document& input,
                                 const SliceParams& params,
                                 size_t fieldPathIndex) {
    invariant(fieldPathIndex < params.path.getPathLength());

    auto fieldName = params.path.getFieldName(fieldPathIndex);
    Value val{input[fieldName]};

    switch (val.getType()) {
        case BSONType::Array:
            val = (fieldPathIndex + 1 == params.path.getPathLength())
                ? sliceArray(val.getArray(), params.skip, params.limit)
                : applySliceProjectionToArray(val.getArray(), params, fieldPathIndex + 1);
            break;
        case BSONType::Object:
            val = applySliceProjectionHelper(val.getDocument(), params, fieldPathIndex + 1);
            break;
        default:
            break;
    }

    MutableDocument output(input);
    output.setField(fieldName, val);
    return Value{output.freeze()};
}
}  // namespace

Value extractArrayElement(const Value& arr, const std::string& elemIndex) {
    auto index = str::parseUnsignedBase10Integer(elemIndex);
    invariant(index);
    return arr[*index];
}

Document applyPositionalProjection(const Document& preImage,
                                   const Document& postImage,
                                   const MatchExpression& matchExpr,
                                   const FieldPath& path) {
    MutableDocument output(postImage);

    // Try to find the first matching array element from the 'input' document based on the condition
    // specified as 'matchExpr'. If such an element is found, its position within an array will be
    // recorded in the 'details' object. Since 'matchExpr' used with the positional projection is
    // the very same selection filter expression in the find command, the input document passed to
    // this function should have already been matched against this expression, so we'll use an
    // invariant to make sure this is the case indeed.
    MatchDetails details;
    details.requestElemMatchKey();
    invariant(matchExpr.matchesBSON(preImage.toBson(), &details));

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
    for (auto [ind, subDoc] = std::pair{0ULL, postImage}; ind < path.getPathLength(); ind++) {
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

                output.setNestedField(path.getSubpath(ind),
                                      Value{std::vector<Value>{matchingElem}});
                return output.freeze();
            }
            case BSONType::Object:
                subDoc = val.getDocument();
                break;
            default:
                break;
        }
    }

    return output.freeze();
}

Value applyElemMatchProjection(const Document& input,
                               const MatchExpression& matchExpr,
                               const FieldPath& path) {
    invariant(path.getPathLength() == 1);

    // Try to find the first matching array element from the 'input' document based on the condition
    // specified as 'matchExpr'. If such an element is found, its position within an array will be
    // recorded in the 'details' object.
    MatchDetails details;
    details.requestElemMatchKey();
    if (!matchExpr.matchesBSON(input.toBson(), &details)) {
        return {};
    }

    auto val = input[path.fullPath()];
    invariant(val.getType() == BSONType::Array);
    auto elemMatchKey = details.elemMatchKey();
    invariant(details.hasElemMatchKey());
    auto matchingElem = extractArrayElement(val, elemMatchKey);
    invariant(!matchingElem.missing());

    return Value{std::vector<Value>{matchingElem}};
}

Document applySliceProjection(const Document& input,
                              const FieldPath& path,
                              boost::optional<int> skip,
                              int limit) {
    auto params = SliceParams{path, skip, limit};
    auto val = applySliceProjectionHelper(input, params, 0);
    invariant(val.getType() == BSONType::Object);
    return val.getDocument();
}
}  // namespace projection_executor
}  // namespace mongo
