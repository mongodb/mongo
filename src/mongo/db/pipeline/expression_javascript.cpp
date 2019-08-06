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

namespace mongo {

REGISTER_EXPRESSION(_internalJsEmit, ExpressionInternalJsEmit::parse);

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
      _funcSource(std::move(funcSource)) {}

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
            "The map function must be of type string, code, or code w/ scope",
            evalField.type() == BSONType::String || evalField.type() == BSONType::Code ||
                evalField.type() == BSONType::CodeWScope);

    std::string funcSourceString = evalField._asCode();
    boost::intrusive_ptr<Expression> thisRef = parseOperand(expCtx, expr["this"], vps);


    uassert(31223, str::stream() << kExpressionName << " requires 'this' to be specified", thisRef);

    return new ExpressionInternalJsEmit(expCtx, std::move(thisRef), std::move(funcSourceString));
}

Value ExpressionInternalJsEmit::serialize(bool explain) const {
    return Value(
        Document{{kExpressionName,
                  Document{{"eval", _funcSource}, {"this", _thisRef->serialize(explain)}}}});
}

Value ExpressionInternalJsEmit::evaluate(const Document& root, Variables* variables) const {
    auto expCtx = getExpressionContext();
    uassert(31234,
            str::stream() << kExpressionName << " is not allowed to run on mongos",
            !expCtx->inMongos);

    Value thisVal = _thisRef->evaluate(root, variables);
    uassert(31225, "'this' must be an object.", thisVal.getType() == BSONType::Object);

    // If the scope does not exist and is created by the call to ExpressionContext::getJsExec(),
    // then make sure to re-bind emit() and the given function to the new scope.
    auto [jsExec, newlyCreated] = expCtx->mongoProcessInterface->getJsExec();
    if (newlyCreated) {
        jsExec->getScope()->loadStored(expCtx->opCtx, true);

        const_cast<ExpressionInternalJsEmit*>(this)->_func =
            jsExec->getScope()->createFunction(_funcSource.c_str());
        uassert(31226, "The map function did not evaluate", _func);
        jsExec->getScope()->injectNative(
            "emit", emitFromJS, &const_cast<ExpressionInternalJsEmit*>(this)->_emittedObjects);
    }

    BSONObj thisBSON = thisVal.getDocument().toBson();
    BSONObj params;
    jsExec->callFunctionWithoutReturn(_func, params, thisBSON);

    std::vector<Value> output;

    for (const BSONObj& obj : _emittedObjects) {
        output.push_back(Value(std::move(obj)));
    }

    // Need to const_cast here in order to clean out _emittedObjects which were added in the call to
    // JS in this function. This is so _emittedObjects is empty again for the next JS invocation.
    const_cast<ExpressionInternalJsEmit*>(this)->_emittedObjects.clear();

    return Value{std::move(output)};
}
}  // namespace mongo
