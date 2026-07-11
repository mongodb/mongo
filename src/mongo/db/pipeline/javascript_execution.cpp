// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/javascript_execution.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
const auto getExec = OperationContext::declareDecoration<std::unique_ptr<JsExecution>>();
}  // namespace

JsExecution* JsExecution::get(OperationContext* opCtx,
                              const BSONObj& scope,
                              const DatabaseName& database,
                              bool loadStoredProcedures,
                              boost::optional<int> jsHeapLimitMB) {
    // If a JsExecution object has already been created, return it.
    JsExecution* jsExec = getCached(opCtx, loadStoredProcedures);
    if (jsExec) {
        return jsExec;
    }

    // There is no cached JsExecution object, so create and cache one now.
    auto& exec = getExec(opCtx);
    exec = std::make_unique<JsExecution>(opCtx, scope, jsHeapLimitMB);
    exec->getScope()->setLocalDB(database);
    if (loadStoredProcedures) {
        exec->getScope()->loadStored(opCtx, true);
    }
    exec->_storedProceduresLoaded = loadStoredProcedures;

    return exec.get();
}

JsExecution* JsExecution::getCached(OperationContext* opCtx, bool loadStoredProcedures) {
    auto& exec = getExec(opCtx);
    if (exec) {
        if (loadStoredProcedures == exec->_storedProceduresLoaded) {
            return exec.get();
        }
        tasserted(
            9136200,
            "A single operation cannot use both JavaScript aggregation expressions and $where.");
    }
    return nullptr;
}

Value JsExecution::callFunction(ScriptingFunction func,
                                const BSONObj& params,
                                const BSONObj& thisObj) {
    int err = _scope->invoke(func, &params, &thisObj, _fnCallTimeoutMillis, false);
    uassert(
        31439, str::stream() << "js function failed to execute: " << _scope->getError(), err == 0);

    BSONObjBuilder returnValue;
    _scope->append(returnValue, "", "__returnValue");
    return Value(returnValue.done().firstElement());
}

void JsExecution::callFunctionWithoutReturn(ScriptingFunction func,
                                            const BSONObj& params,
                                            const BSONObj& thisObj) {
    int err = _scope->invoke(func, &params, &thisObj, _fnCallTimeoutMillis, true);
    uassert(
        31470, str::stream() << "js function failed to execute: " << _scope->getError(), err == 0);

    return;
}
}  // namespace mongo
