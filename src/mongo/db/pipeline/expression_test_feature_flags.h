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
#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/variables.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
/**
 * The following expressions will be used to validate enabling feature flags on the latest or
 * lastLTS FCV and are only enabled in tests. The expressions are registered behind
 * 'gFeatureFlagBlender', enabled on the latest FCV and 'gFeatureFlagSpoon', enabled on the lastLTS.
 * These expressions are used to write permanent tests that enable feature flags.
 *
 * These expressions only take in and always return the integer 1.
 */
class ExpressionTestFeatureFlags : public Expression {
public:
    ExpressionTestFeatureFlags(ExpressionContext* const expCtx, StringData exprName)
        : Expression(expCtx), _exprName(exprName) {};

    Value evaluate(const Document& root, Variables* variables) const final;

    Value serialize(const SerializationOptions& options = {}) const final {
        return Value(Document{{_exprName, Value(1)}});
    }

protected:
    static void _validateInternal(const BSONElement& expr, StringData testExpressionName);

private:
    const StringData _exprName;
};

class ExpressionTestFeatureFlagLatest final : public ExpressionTestFeatureFlags {
public:
    static constexpr StringData kName = "$_testFeatureFlagLatest"_sd;

    ExpressionTestFeatureFlagLatest(ExpressionContext* const expCtx)
        : ExpressionTestFeatureFlags(expCtx, kName) {};

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};

class ExpressionTestFeatureFlagLastLTS final : public ExpressionTestFeatureFlags {
public:
    static constexpr StringData kName = "$_testFeatureFlagLastLTS"_sd;

    ExpressionTestFeatureFlagLastLTS(ExpressionContext* const expCtx)
        : ExpressionTestFeatureFlags(expCtx, kName) {};

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vpsIn);

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }
};
}  // namespace mongo
