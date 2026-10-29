// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_statement.h"

namespace mongo {

WindowFunctionStatement WindowFunctionStatement::parse(BSONElement elem,
                                                       const boost::optional<SortPattern>& sortBy,
                                                       ExpressionContext* expCtx) {
    // 'elem' is a statement like 'v: {$sum: {...}}', whereas the expression is '$sum: {...}'.
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The field '" << elem.fieldName() << "' must be an object",
            elem.type() == BSONType::object);
    return WindowFunctionStatement(
        elem.fieldName(),
        window_function::Expression::parse(elem.embeddedObject(), sortBy, expCtx));
}

void WindowFunctionStatement::serialize(MutableDocument& outputFields,
                                        const query_shape::SerializationOptions& opts) const {
    outputFields[opts.serializeFieldPathFromString(fieldName)] = expr->serialize(opts);
}
}  // namespace mongo
