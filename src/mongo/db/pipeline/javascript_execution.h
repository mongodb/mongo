// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class OperationContext;

/*
 * This class provides a more sensible interface with JavaScript Scope objects. It helps with
 * boilerplate related to calling JS functions from C++ code, and extracting BSON objects from the
 * JS engine.
 */
class JsExecution {
public:
    /**
     * Create or get a pointer to a JsExecution instance, capable of invoking Javascript functions
     * and reading the return value. If `loadStoredProcedures` is true, this will load all stored
     * procedures from database. The JsExecution* returned is owned by 'opCtx'.
     */
    static JsExecution* get(OperationContext* opCtx,
                            const BSONObj& scope,
                            const DatabaseName& database,
                            bool loadStoredProcedures,
                            boost::optional<int> jsHeapLimitMB);

    /**
     * Gets a pointer to the cached JsExecution instance if it exists, else returns nullptr. This
     * allows the caller to skip passing all the arguments to the more general JsExecution::get()
     * method, which avoids having to potentially construct and create a heap copy of a large scope
     * object that will not be used because a cached JsExecution object already exists.
     */
    static JsExecution* getCached(OperationContext* opCtx, bool loadStoredProcedures);

    /**
     * Construct with a thread-local scope and initialize with the given scope variables.
     */
    JsExecution(OperationContext* opCtx,
                const BSONObj& scopeVars,
                boost::optional<int> jsHeapLimitMB = boost::none)
        : _scope(getGlobalScriptEngine()->newScopeForCurrentThread(jsHeapLimitMB)) {
        _scopeVars = scopeVars.getOwned();
        _scope->init(&_scopeVars);
        _fnCallTimeoutMillis = internalQueryJavaScriptFnTimeoutMillis.load();
        _scope->registerOperation(opCtx);
    }

    ~JsExecution() {
        _scope->unregisterOperation();
    };

    /**
     * Invokes the javascript function given by 'func' with the arguments 'params' and input object
     * 'thisObj'.
     *
     * This method assumes that the desired function to execute does not return a value.
     */
    void callFunctionWithoutReturn(ScriptingFunction func,
                                   const BSONObj& params,
                                   const BSONObj& thisObj);

    /**
     * Invokes the javascript function given by 'func' with the arguments 'params' and input object
     * 'thisObj'.
     *
     * Returns the value returned by the function.
     */
    Value callFunction(ScriptingFunction func, const BSONObj& params, const BSONObj& thisObj);

    /**
     * Creates a function in the owned Scope* if it hasn't been created yet.
     */
    ScriptingFunction createFunction(std::string funcCode) {
        return _scope->createFunction(funcCode.c_str());
    };

    Scope* getScope() {
        return _scope.get();
    }

private:
    BSONObj _scopeVars;
    std::unique_ptr<Scope> _scope;
    bool _storedProceduresLoaded = false;
    int _fnCallTimeoutMillis;
    Value doCallFunction(ScriptingFunction func,
                         const BSONObj& params,
                         const BSONObj& thisObj,
                         bool noReturnVal);
};
}  // namespace mongo
