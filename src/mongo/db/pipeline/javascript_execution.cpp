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

#include "mongo/db/pipeline/javascript_execution.h"

#include <iostream>

#include "mongo/base/status_with.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {
const auto getExec = OperationContext::declareDecoration<std::unique_ptr<JsExecution>>();
}  // namespace

JsExecution* JsExecution::get(OperationContext* opCtx,
                              const BSONObj& scope,
                              StringData database,
                              bool loadStoredProcedures,
                              boost::optional<int> jsHeapLimitMB) {
    auto& exec = getExec(opCtx);
    if (!exec) {
        exec = std::make_unique<JsExecution>(opCtx, scope, jsHeapLimitMB);
        exec->getScope()->setLocalDB(database);
        if (loadStoredProcedures) {
            exec->getScope()->loadStored(opCtx, true);
        }
        exec->_storedProceduresLoaded = loadStoredProcedures;
    } else {
        uassert(31438,
                "A single operation cannot use both JavaScript aggregation expressions and $where.",
                loadStoredProcedures == exec->_storedProceduresLoaded);
    }
    return exec.get();
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
