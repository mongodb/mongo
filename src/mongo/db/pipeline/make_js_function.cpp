// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/make_js_function.h"

#include "mongo/db/pipeline/javascript_execution.h"
#include "mongo/util/assert_util.h"

namespace mongo {

// Given a function represented as a string, constructs a JS execution context and attempts to parse
// it as a JS function.
ScriptingFunction makeJsFunc(ExpressionContext* const expCtx, const std::string& func) {
    auto jsExec =
        expCtx->getJsExecWithScope();  // default arg forceLoadOfStoredProcedures is false here.
    ScriptingFunction parsedFunc = jsExec->getScope()->createFunction(func.c_str());
    uassert(
        31247, "The user-defined function failed to parse in the javascript engine", parsedFunc);
    return parsedFunc;
}

}  // namespace mongo
