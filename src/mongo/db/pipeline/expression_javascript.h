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

#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/javascript_execution.h"

/**
 * This file contains all expressions which make use of JavaScript execution and depend on the JS
 * engine to operate.
 */
namespace mongo {

/**
 * This expression takes in a JavaScript function and a "this" reference, and returns an array of
 * key/value objects which are the results of calling emit() from the provided JS function.
 */
class ExpressionInternalJsEmit final : public Expression {
public:
    Value evaluate(const Document& root, Variables* variables) const final;
    static boost::intrusive_ptr<Expression> parse(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        BSONElement expr,
        const VariablesParseState& vps);
    Value serialize(bool explain) const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

private:
    const boost::intrusive_ptr<Expression>& _thisRef;

    ExpressionInternalJsEmit(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             boost::intrusive_ptr<Expression> thisRef,
                             std::string funcSourceString);

    // Vector needs to be a member on the class so that it can be attached to the JS scope in the
    // constructor and stay alive through the lifetime of the Expression object.
    std::vector<BSONObj> _emittedObjects{};

    void _doAddDependencies(DepsTracker* deps) const final override;

    std::string _funcSource;
    ScriptingFunction _func;

    static constexpr auto kExpressionName = "$_internalJsEmit"_sd;
};
}  // namespace mongo
