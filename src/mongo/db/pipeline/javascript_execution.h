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

#include "mongo/db/client.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/scripting/engine.h"

namespace mongo {
/*
 * This class provides a more sensible interface with JavaScript Scope objects. It helps with
 * boilerplate related to calling JS functions from C++ code, and extracting BSON objects from the
 * JS engine.
 */
class JsExecution {
public:
    /**
     * Construct with a thread-local scope.
     */
    JsExecution() : _scope(nullptr) {
        _scope.reset(getGlobalScriptEngine()->newScopeForCurrentThread());
    }

    /**
     * Registers and invokes the javascript function given by 'func' with the arguments 'params' and
     * input object 'thisObj'.
     *
     * This method assumes that the desired function to execute does return a value.
     */
    void callFunctionWithoutReturn(ScriptingFunction func,
                                   const BSONObj& params,
                                   const BSONObj& thisObj) {
        _scope->registerOperation(Client::getCurrent()->getOperationContext());
        const auto guard = makeGuard([&] { _scope->unregisterOperation(); });

        _scope->invoke(func, &params, &thisObj, 0, true);
    }

    /**
     * Registers and invokes the javascript function given by 'func' with the arguments 'params' and
     * input object 'thisObj'.
     *
     * Returns the value returned by the function.
     */
    Value callFunction(ScriptingFunction func, const BSONObj& params, const BSONObj& thisObj) {
        _scope->registerOperation(Client::getCurrent()->getOperationContext());
        const auto guard = makeGuard([&] { _scope->unregisterOperation(); });

        _scope->invoke(func, &params, &thisObj, 0, false);
        BSONObjBuilder returnValue;
        _scope->append(returnValue, "", "__returnValue");
        return Value(returnValue.done().firstElement());
    }

    Scope* getScope() {
        return _scope.get();
    }

private:
    std::unique_ptr<Scope> _scope;
};
}  // namespace mongo
