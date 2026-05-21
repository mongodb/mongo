/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where_base.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class OperationContext;

// This forward declaration is necessary because some translation units that include this header do
// not link with MozJS and must not transitively include js_function.h.
class JsFunction;

class WhereMatchExpression final : public WhereMatchExpressionBase {
public:
    WhereMatchExpression(OperationContext* opCtx, WhereParams params, const DatabaseName& dbName);

    // Out-of-line destructor required because _jsFunction holds an incomplete JsFunction type.
    ~WhereMatchExpression() final;

    std::unique_ptr<MatchExpression> clone() const final;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    const JsFunction& getPredicate() const;
    std::unique_ptr<JsFunction> extractPredicate();
    void setPredicate(std::unique_ptr<JsFunction> jsFunction);
    void validateState() const;

    bool runPredicate(const BSONObj& doc) const final;

private:
    OperationContext* const _opCtx;
    std::unique_ptr<JsFunction> _jsFunction;
};

}  // namespace mongo
