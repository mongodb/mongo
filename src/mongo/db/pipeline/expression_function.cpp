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

#include "mongo/db/pipeline/expression_function.h"

namespace mongo {

REGISTER_EXPRESSION_WITH_MIN_VERSION(
    function,
    ExpressionFunction::parse,
    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44);

ExpressionFunction::ExpressionFunction(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       boost::intrusive_ptr<Expression> passedArgs,
                                       std::string funcSource,
                                       std::string lang)
    : Expression(expCtx, {std::move(passedArgs)}),
      _passedArgs(_children[0]),
      _funcSource(std::move(funcSource)),
      _lang(std::move(lang)) {}

Value ExpressionFunction::serialize(bool explain) const {
    return Value(Document{{kExpressionName,
                           Document{{"body", _funcSource},
                                    {"args", _passedArgs->serialize(explain)},
                                    {"lang", _lang}}}});
}

void ExpressionFunction::_doAddDependencies(mongo::DepsTracker* deps) const {
    _children[0]->addDependencies(deps);
}

boost::intrusive_ptr<Expression> ExpressionFunction::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vps) {

    uassert(31260,
            str::stream() << kExpressionName
                          << " requires an object as an argument, found: " << expr.type(),
            expr.type() == BSONType::Object);

    BSONElement bodyField = expr["body"];

    uassert(31261, "The body function must be specified.", bodyField);

    boost::intrusive_ptr<Expression> bodyExpr = Expression::parseOperand(expCtx, bodyField, vps);

    auto bodyConst = dynamic_cast<ExpressionConstant*>(bodyExpr.get());
    uassert(31432, "The body function must be a constant expression", bodyConst);

    auto bodyValue = bodyConst->getValue();
    uassert(31262,
            "The body function must evaluate to type string or code",
            bodyValue.getType() == BSONType::String || bodyValue.getType() == BSONType::Code);

    BSONElement argsField = expr["args"];
    uassert(31263, "The args field must be specified.", argsField);
    boost::intrusive_ptr<Expression> argsExpr = parseOperand(expCtx, argsField, vps);

    BSONElement langField = expr["lang"];
    uassert(31418, "The lang field must be specified.", langField);
    uassert(31419,
            "Currently the only supported language specifier is 'js'.",
            langField.type() == BSONType::String && langField.str() == kJavaScript);

    return new ExpressionFunction(expCtx, argsExpr, bodyValue.coerceToString(), langField.str());
}

Value ExpressionFunction::evaluate(const Document& root, Variables* variables) const {
    auto jsExec = getExpressionContext()->getJsExecWithScope();

    ScriptingFunction func = jsExec->getScope()->createFunction(_funcSource.c_str());
    uassert(31265, "The body function did not evaluate", func);

    auto argValue = _passedArgs->evaluate(root, variables);
    uassert(31266, "The args field must be of type array", argValue.getType() == BSONType::Array);

    int argNum = 0;
    BSONObjBuilder bob;
    for (const auto& arg : argValue.getArray()) {
        arg.addToBsonObj(&bob, "arg" + std::to_string(argNum++));
    }
    return jsExec->callFunction(func, bob.done(), {});
};
}  // namespace mongo