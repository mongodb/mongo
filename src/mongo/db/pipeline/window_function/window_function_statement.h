/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/pipeline/window_function/window_function_expression.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"

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

    void serialize(MutableDocument& outputFields, const SerializationOptions& opts) const;
};
}  // namespace mongo
