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

namespace mongo {
/**
 * This expression takes a function, an array of arguments to pass to it, and the language
 * specifier (currently limited to JavaScript). It returns the return value of the function with
 * the given arguments.
 */
class ExpressionFunction final : public Expression {
public:
    static boost::intrusive_ptr<Expression> parse(ExpressionContext* const expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    static boost::intrusive_ptr<ExpressionFunction> create(
        ExpressionContext* const expCtx,
        boost::intrusive_ptr<Expression> passedArgs,
        std::string funcSourceString,
        std::string lang) {
        return new ExpressionFunction{expCtx,
                                      passedArgs,
                                      false /* don't assign first argument to 'this' */,
                                      std::move(funcSourceString),
                                      std::move(lang)};
    }

    // This method is intended for use when you want to bind obj to an argument for desugaring
    // $where.
    static boost::intrusive_ptr<ExpressionFunction> createForWhere(
        ExpressionContext* const expCtx,
        boost::intrusive_ptr<Expression> passedArgs,
        std::string funcSourceString,
        std::string lang) {
        return new ExpressionFunction{
            expCtx, passedArgs, true, std::move(funcSourceString), std::move(lang)};
    }

    Value evaluate(const Document& root, Variables* variables) const final;

    Value serialize(bool explain) const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

    static constexpr auto kExpressionName = "$function"_sd;
    static constexpr auto kJavaScript = "js";

private:
    ExpressionFunction(ExpressionContext* const expCtx,
                       boost::intrusive_ptr<Expression> passedArgs,
                       bool assignFirstArgToThis,
                       std::string funcSourceString,
                       std::string lang);
    void _doAddDependencies(DepsTracker* deps) const final override;

    const boost::intrusive_ptr<Expression>& _passedArgs;
    bool _assignFirstArgToThis;
    std::string _funcSource;
    std::string _lang;
};
}  // namespace mongo
