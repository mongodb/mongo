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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/pipeline/expression_javascript.h"
#include "mongo/db/query/query_knobs_gen.h"

namespace mongo {

REGISTER_EXPRESSION(_internalJsEmit, ExpressionInternalJsEmit::parse);

REGISTER_EXPRESSION(_internalJs, ExpressionInternalJs::parse);
namespace {

/**
 * This function is called from the JavaScript function provided to the expression. Objects are
 * converted from BSON to Document/Value after JS engine has run completely.
 */
BSONObj emitFromJS(const BSONObj& args, void* data) {
    uassert(31220, "emit takes 2 args", args.nFields() == 2);
    auto emitted = static_cast<std::vector<BSONObj>*>(data);
    if (args.firstElement().type() == Undefined) {
        emitted->push_back(BSON("k" << BSONNULL << "v" << args["1"]));
    } else {
        emitted->push_back(BSON("k" << args["0"] << "v" << args["1"]));
    }
    return BSONObj();
}
}  // namespace

ExpressionInternalJsEmit::ExpressionInternalJsEmit(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::intrusive_ptr<Expression> thisRef,
    std::string funcSource)
    : Expression(expCtx, {std::move(thisRef)}),
      _thisRef(_children[0]),
      _funcSource(std::move(funcSource)),
      _byteLimit(internalQueryMaxJsEmitBytes.load()) {}

void ExpressionInternalJsEmit::_doAddDependencies(mongo::DepsTracker* deps) const {
    _children[0]->addDependencies(deps);
}

boost::intrusive_ptr<Expression> ExpressionInternalJsEmit::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vps) {

    uassert(ErrorCodes::BadValue,
            str::stream() << kExpressionName << " not allowed without enabling test commands.",
            getTestCommandsEnabled());

    uassert(31221,
            str::stream() << kExpressionName
                          << " requires an object as an argument, found: " << typeName(expr.type()),
            expr.type() == BSONType::Object);

    BSONElement evalField = expr["eval"];

    uassert(31222, str::stream() << "The map function must be specified.", evalField);
    uassert(31224,
            "The map function must be of type string or code",
            evalField.type() == BSONType::String || evalField.type() == BSONType::Code);

    std::string funcSourceString = evalField._asCode();
    BSONElement thisField = expr["this"];
    uassert(
        31223, str::stream() << kExpressionName << " requires 'this' to be specified", thisField);
    boost::intrusive_ptr<Expression> thisRef = parseOperand(expCtx, thisField, vps);

    return new ExpressionInternalJsEmit(expCtx, std::move(thisRef), std::move(funcSourceString));
}

Value ExpressionInternalJsEmit::serialize(bool explain) const {
    return Value(
        Document{{kExpressionName,
                  Document{{"eval", _funcSource}, {"this", _thisRef->serialize(explain)}}}});
}

Value ExpressionInternalJsEmit::evaluate(const Document& root, Variables* variables) const {
    uassert(31246,
            "Cannot run server-side javascript without the javascript engine enabled",
            getGlobalScriptEngine());

    Value thisVal = _thisRef->evaluate(root, variables);
    uassert(31225, "'this' must be an object.", thisVal.getType() == BSONType::Object);

    // If the scope does not exist and is created by the following call, then make sure to
    // re-bind emit() and the given function to the new scope.
    auto expCtx = getExpressionContext();
    auto [jsExec, newlyCreated] = expCtx->getJsExecWithScope();
    if (newlyCreated) {
        jsExec->getScope()->loadStored(expCtx->opCtx, true);
    }

    // Although inefficient to "create" a new function every time we evaluate, this will usually end
    // up being a simple cache lookup. This is needed because the JS Scope may have been recreated
    // on a new thread if the expression is evaluated across getMores.
    auto func = jsExec->getScope()->createFunction(_funcSource.c_str());
    uassert(31226, "The map function failed to parse in the javascript engine", func);

    // For a given invocation of the user-defined function, this vector holds the results of each
    // call to emit().
    std::vector<BSONObj> emittedObjects;
    jsExec->getScope()->injectNative("emit", emitFromJS, &emittedObjects);

    BSONObj thisBSON = thisVal.getDocument().toBson();
    BSONObj params;
    jsExec->callFunctionWithoutReturn(func, params, thisBSON);

    std::vector<Value> output;

    size_t bytesUsed = 0;
    for (const auto& obj : emittedObjects) {
        bytesUsed += obj.objsize();
        uassert(31292,
                str::stream() << "Size of emitted values exceeds the set size limit of "
                              << _byteLimit << " bytes",
                bytesUsed < _byteLimit);
        output.push_back(Value(obj));
    }

    return Value{std::move(output)};
}

ExpressionInternalJs::ExpressionInternalJs(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           boost::intrusive_ptr<Expression> passedArgs,
                                           std::string funcSource)
    : Expression(expCtx, {std::move(passedArgs)}),
      _passedArgs(_children[0]),
      _funcSource(std::move(funcSource)) {}

Value ExpressionInternalJs::serialize(bool explain) const {
    return Value(
        Document{{kExpressionName,
                  Document{{"eval", _funcSource}, {"args", _passedArgs->serialize(explain)}}}});
}

void ExpressionInternalJs::_doAddDependencies(mongo::DepsTracker* deps) const {
    _children[0]->addDependencies(deps);
}

boost::intrusive_ptr<Expression> ExpressionInternalJs::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vps) {

    uassert(ErrorCodes::BadValue,
            str::stream() << kExpressionName << " not allowed without enabling test commands.",
            getTestCommandsEnabled());

    uassert(31260,
            str::stream() << kExpressionName
                          << " requires an object as an argument, found: " << expr.type(),
            expr.type() == BSONType::Object);

    BSONElement evalField = expr["eval"];

    uassert(31261, "The eval function must be specified.", evalField);
    uassert(31262,
            "The eval function must be of type string or code",
            evalField.type() == BSONType::String || evalField.type() == BSONType::Code);

    BSONElement argsField = expr["args"];
    uassert(31263, "The args field must be specified.", argsField);
    boost::intrusive_ptr<Expression> argsExpr = parseOperand(expCtx, argsField, vps);

    return new ExpressionInternalJs(expCtx, argsExpr, evalField._asCode());
}

Value ExpressionInternalJs::evaluate(const Document& root, Variables* variables) const {
    auto& expCtx = getExpressionContext();
    uassert(31264,
            str::stream() << kExpressionName
                          << " can't be run on this process. Javascript is disabled.",
            getGlobalScriptEngine());

    auto [jsExec, newlyCreated] = expCtx->getJsExecWithScope();
    if (newlyCreated) {
        jsExec->getScope()->loadStored(expCtx->opCtx, true);
    }

    ScriptingFunction func = jsExec->getScope()->createFunction(_funcSource.c_str());
    uassert(31265, "The eval function did not evaluate", func);

    auto argExpressions = _passedArgs->evaluate(root, variables);
    uassert(
        31266, "The args field must be of type array", argExpressions.getType() == BSONType::Array);

    int argNum = 0;
    BSONObjBuilder bob;
    for (const auto& arg : argExpressions.getArray()) {
        arg.addToBsonObj(&bob, "arg" + std::to_string(argNum++));
    }
    return jsExec->callFunction(func, bob.done(), {});
}
}  // namespace mongo
