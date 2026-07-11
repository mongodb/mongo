// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo::projection_executor_utils {
/**
 * Applies the projection to a single field name. Returns whether or not the projection would
 * allow that field to remain in a document.
 **/
bool applyProjectionToOneField(projection_executor::ProjectionExecutor* executor,
                               std::string_view field);

/**
 * Applies the projection to each field from the 'fields' set and stores it in the returned set
 * if the projection would allow that field to remain in a document.
 **/
template <typename Container>
std::set<std::string> applyProjectionToFields(projection_executor::ProjectionExecutor* executor,
                                              Container const& fields) {
    std::set<std::string> out;
    for (const auto& field : fields) {
        if (applyProjectionToOneField(executor, field)) {
            out.insert(field);
        }
    }
    return out;
}

/**
 * Applies a positional projection on the first array found in the 'path' on a projection
 * 'preImage' document. The applied projection is merged with a projection 'postImage' document.
 * The 'matchExpr' specifies a condition to locate the first matching element in the array and must
 * match the input document. Note that the match expression must be applied to the projection
 * post-image, as it may contain conditions on fields which are not included into the projection
 * post-image. So, the pre-image document will be used to match an array and record a position of
 * the matching element, whilst the actual result will be merged into the post-image.
 *
 * For example, given:
 *
 *    - the 'preImage' document {bar: 1, foo: {bar: [1,2,6,10]}}
 *    - the 'postImage' document {foo: {bar: [1,2,6,10]}}
 *    - the 'matchExpr' condition {bar: 1, 'foo.bar': {$gte: 5}}
 *    - and the 'path' for the positional projection of 'foo.bar'
 *
 * The resulting document will contain the following element: {foo: {bar: [6]}}
 *
 * Throws an AssertionException if 'matchExpr' matches the input document, but an array element
 * satisfying positional projection requirements cannot be found.
 */
Document applyFindPositionalProjection(const Document& preImage,
                                       const Document& postImage,
                                       const MatchExpression& matchExpr,
                                       const FieldPath& path);
/**
 * Applies an $elemMatch projection on the array at the given 'path' on the 'input' document. The
 * applied projection is stored in the output Value. The 'matchExpr' specifies a condition to
 * locate the first matching element in the array and must contain the $elemMatch operator. This
 * function doesn't enforce this requirement and the caller must ensure that the valid match
 * expression is passed. For example, given:
 *
 *   - the 'input' document {foo: [{bar: 1}, {bar: 2}, {bar: 3}]}
 *   - the 'matchExpr' condition {foo: {$elemMatch: {bar: {$gt: 1}}}}
 *   - and the 'path' of 'foo'
 *
 * The resulting value will be: [{bar: 2}]
 *
 * If the 'matchExpr' does not match the input document, the function returns a missing value.
 *
 * Since the $elemMatch projection cannot be used with a nested field, the 'path' value must not
 * be a dotted path, otherwise a tassert will be triggered.
 */
Value applyFindElemMatchProjection(const Document& input,
                                   const MatchExpression& matchExpr,
                                   const FieldPath& path);
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
Document applyFindSliceProjection(const Document& input,
                                  const FieldPath& path,
                                  boost::optional<int> skip,
                                  int limit);
}  // namespace mongo::projection_executor_utils
