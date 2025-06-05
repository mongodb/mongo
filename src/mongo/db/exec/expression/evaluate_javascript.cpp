/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/pipeline/make_js_function.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/scripting/engine.h"

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionFunction& expr, const Document& root, Variables* variables) {
    auto jsExec = expr.getExpressionContext()->getJsExecWithScope(expr.getAssignFirstArgToThis());
    auto scope = jsExec->getScope();

    // createFunction is memoized in MozJSImplScope, so it's ok to call this for each
    // eval call.
    ScriptingFunction func = jsExec->getScope()->createFunction(expr.getFuncSource().c_str());
    uassert(31265, "The body function did not evaluate", func);

    auto argValue = expr.getPassedArgs()->evaluate(root, variables);
    uassert(31266, "The args field must be of type array", argValue.getType() == BSONType::array);

    // This logic exists to desugar $where into $expr + $function. In this case set the global obj
    // to this, to handle cases where the $where function references the current document through
    // obj.
    BSONObjBuilder bob;
    if (expr.getAssignFirstArgToThis()) {
        // For defense-in-depth, The $where case will pass a field path expr carrying $$CURRENT as
        // the only element of the array.
        auto args = argValue.getArray();
        uassert(31422,
                "field path $$CURRENT must be the only element in args",
                argValue.getArrayLength() == 1);

        BSONObj thisBSON = args[0].getDocument().toBson();
        scope->setObject("obj", thisBSON);

        return jsExec->callFunction(func, bob.done(), thisBSON);
    }

    int argNum = 0;
    for (const auto& arg : argValue.getArray()) {
        arg.addToBsonObj(&bob, "arg" + std::to_string(argNum++));
    }
    return jsExec->callFunction(func, bob.done(), {});
}

namespace {

// For a given invocation of the user-defined function, this struct holds the results of each
// call to emit().
struct EmitState {
    void emit(Document&& doc) {
        bytesUsed += doc.getApproximateSize();
        uassert(31292,
                str::stream() << "Size of emitted values exceeds the set size limit of "
                              << byteLimit << " bytes",
                bytesUsed < byteLimit);
        emittedObjects.emplace_back(std::move(doc));
    }

    std::vector<Value> emittedObjects;
    int byteLimit;
    int bytesUsed;
};

/**
 * Helper for extracting fields from a BSONObj in one pass. This is a hot path
 * for some map reduce workloads so be careful when changing.
 *
 * 'elts' must be an array of size >= 2.
 */
void extract2Args(const BSONObj& args, BSONElement* elts) {
    const size_t nToExtract = 2;

    auto fail = []() {
        uasserted(31220, "emit takes 2 args");
    };
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

    auto emitState = static_cast<EmitState*>(data);
    uassert(9712400, "Misplaced call to 'emit'", emitState);
    if (elts[0].type() == BSONType::undefined) {
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

Value evaluate(const ExpressionInternalJsEmit& expr, const Document& root, Variables* variables) {
    Value thisVal = expr.getThisRef()->evaluate(root, variables);
    uassert(31225, "'this' must be an object.", thisVal.getType() == BSONType::object);

    // If the scope does not exist and is created by the following call, then make sure to
    // re-bind emit() and the given function to the new scope.
    ExpressionContext* expCtx = expr.getExpressionContext();

    auto jsExec = expCtx->getJsExecWithScope();

    // Inject the native "emit" function to be called from the user-defined map function.
    EmitState emitState{{}, internalQueryMaxJsEmitBytes.load(), 0};
    jsExec->injectEmit(emitFromJS, &emitState);

    // Although inefficient to "create" a new function every time we evaluate, this will usually end
    // up being a simple cache lookup. This is needed because the JS Scope may have been recreated
    // on a new thread if the expression is evaluated across getMores.
    auto func = makeJsFunc(expCtx, expr.getFuncSource().c_str());

    BSONObj thisBSON = thisVal.getDocument().toBson();
    BSONObj params;
    jsExec->callFunctionWithoutReturn(func, params, thisBSON);
    // Invalidate the pointer to the local emitState variable.
    jsExec->injectEmit(emitFromJS, nullptr);

    return Value(std::move(emitState.emittedObjects));
}

}  // namespace exec::expression
}  // namespace mongo
