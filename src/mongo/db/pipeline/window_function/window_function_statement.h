// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/util/modules.h"

namespace mongo {

struct WindowFunctionStatement {
    std::string fieldName;  // top-level fieldname, not a path
    boost::intrusive_ptr<window_function::Expression> expr;

    WindowFunctionStatement(std::string fieldName,
                            boost::intrusive_ptr<window_function::Expression> expr)
        : fieldName(std::move(fieldName)), expr(std::move(expr)) {}

    static WindowFunctionStatement parse(BSONElement elem,
                                         const boost::optional<SortPattern>& sortBy,
                                         ExpressionContext* expCtx);

    void addDependencies(DepsTracker* deps) const {
        if (expr) {
            expr->addDependencies(deps);
        }

        const FieldPath path(fieldName);

        // We do this because acting on "a.b" where a is an object also depends on "a" not being
        // changed (e.g. to a non-object).
        for (size_t i = 0; i < path.getPathLength() - 1; i++) {
            deps->fields.insert(std::string{path.getSubpath(i)});
        }
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const {
        if (expr) {
            expr->addVariableRefs(refs);
        }
    }

    void serialize(MutableDocument& outputFields,
                   const query_shape::SerializationOptions& opts) const;
};
}  // namespace mongo
