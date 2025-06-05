/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/exec/projection_executor_utils.h"

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionInternalFindPositional& expr,
               const Document& root,
               Variables* variables) {
    auto& children = expr.getChildren();

    auto preImage = children[0]->evaluate(root, variables);
    auto postImage = children[1]->evaluate(root, variables);
    uassert(51255,
            fmt::format("Positional operator pre-image can only be an object, but got {}",
                        typeName(preImage.getType())),
            preImage.getType() == BSONType::object);
    uassert(51258,
            fmt::format("Positional operator post-image can only be an object, but got {}",
                        typeName(postImage.getType())),
            postImage.getType() == BSONType::object);
    return Value{projection_executor_utils::applyFindPositionalProjection(preImage.getDocument(),
                                                                          postImage.getDocument(),
                                                                          *expr.getMatchExpr(),
                                                                          expr.getFieldPath())};
}

Value evaluate(const ExpressionInternalFindSlice& expr,
               const Document& root,
               Variables* variables) {
    auto postImage = expr.getChildren()[0]->evaluate(root, variables);
    uassert(51256,
            fmt::format("$slice operator can only be applied to an object, but got {}",
                        typeName(postImage.getType())),
            postImage.getType() == BSONType::object);
    return Value{projection_executor_utils::applyFindSliceProjection(
        postImage.getDocument(), expr.getFieldPath(), expr.getSkip(), expr.getLimit())};
}

Value evaluate(const ExpressionInternalFindElemMatch& expr,
               const Document& root,
               Variables* variables) {
    auto input = expr.getChildren()[0]->evaluate(root, variables);
    invariant(input.getType() == BSONType::object);
    return projection_executor_utils::applyFindElemMatchProjection(
        input.getDocument(), *expr.getMatchExpr(), expr.getFieldPath());
}

}  // namespace exec::expression
}  // namespace mongo
