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
#include "mongo/db/pipeline/expression_js_emit.h"
#include "mongo/db/pipeline/make_js_function.h"
#include "mongo/db/query/query_knobs_gen.h"

namespace mongo {

REGISTER_EXPRESSION(_internalJsEmit, ExpressionInternalJsEmit::parse);

namespace {
/**
 * Helper for extracting fields from a BSONObj in one pass. This is a hot path
 * for some map reduce workloads so be careful when changing.
 *
 * 'elts' must be an array of size >= 2.
 */
void extract2Args(const BSONObj& args, BSONElement* elts) {
    const size_t nToExtract = 2;

    auto fail = []() { uasserted(31220, "emit takes 2 args"); };
    BSONObjIterator it(args);
    for (size_t i = 0; i < nToExtract; ++i) {
        if (!it.more()) {
            fail();
        }
        elts[i] = it.next();
    }

    // There should be exactly two arguments, no more.
    if (it.more()) {
        fail();
    }
}

/**
 * This function is called from the JavaScript function provided to the expression.
 */
BSONObj emitFromJS(const BSONObj& args, void* data) {
    BSONElement elts[2];
    extract2Args(args, elts);

    auto emitState = static_cast<ExpressionInternalJsEmit::EmitState*>(data);
    if (elts[0].type() == Undefined) {
        MutableDocument md;
        // Note: Using MutableDocument::addField() is considerably faster than using
        // MutableDocument::setField() or building a document by hand with the DOC() macros.
        md.addField("k", Value(BSONNULL));
        md.addField("v", Value(elts[1]));
        emitState->emit(md.freeze());
    } else {
        MutableDocument md;
        md.addField("k", Value(elts[0]));
        md.addField("v", Value(elts[1]));
        emitState->emit(md.freeze());
    }
    return BSONObj();
}
}  // namespace

ExpressionInternalJsEmit::ExpressionInternalJsEmit(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::intrusive_ptr<Expression> thisRef,
    std::string funcSource)
    : Expression(expCtx, {std::move(thisRef)}),
      _emitState{{}, internalQueryMaxJsEmitBytes.load(), 0},
      _thisRef(_children[0]),
      _funcSource(std::move(funcSource)) {}

void ExpressionInternalJsEmit::_doAddDependencies(mongo::DepsTracker* deps) const {
    _children[0]->addDependencies(deps);
}

boost::intrusive_ptr<Expression> ExpressionInternalJsEmit::parse(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    BSONElement expr,
    const VariablesParseState& vps) {

    uassert(4660801,
            str::stream() << kExpressionName << " cannot be used inside a validator.",
            !expCtx->isParsingCollectionValidator);

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
    Value thisVal = _thisRef->evaluate(root, variables);
    uassert(31225, "'this' must be an object.", thisVal.getType() == BSONType::Object);

    // If the scope does not exist and is created by the following call, then make sure to
    // re-bind emit() and the given function to the new scope.
    ExpressionContext* expCtx = getExpressionContext().get();

    auto jsExec = expCtx->getJsExecWithScope();
    // Inject the native "emit" function to be called from the user-defined map function. This
    // particular Expression/ExpressionContext may be reattached to a new OperationContext (and thus
    // a new JS Scope) when used across getMore operations, so this method will handle that case for
    // us by only injecting if we haven't already.
    jsExec->injectEmitIfNecessary(emitFromJS, &_emitState);

    // Although inefficient to "create" a new function every time we evaluate, this will usually end
    // up being a simple cache lookup. This is needed because the JS Scope may have been recreated
    // on a new thread if the expression is evaluated across getMores.
    auto func = makeJsFunc(expCtx, _funcSource.c_str());

    BSONObj thisBSON = thisVal.getDocument().toBson();
    BSONObj params;
    jsExec->callFunctionWithoutReturn(func, params, thisBSON);

    auto returnValue = Value(std::move(_emitState.emittedObjects));
    _emitState.reset();
    return returnValue;
}
}  // namespace mongo
