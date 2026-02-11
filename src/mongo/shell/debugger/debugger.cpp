/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/shell/debugger/debugger.h"

#include "mongo/scripting/mozjs/shell/implscope.h"

#include <jsapi.h>

namespace mongo {
namespace mozjs {

class DebuggerGlobal {
public:
    static Status init(JSContext* cx) {

        // Get the main global object (the one running user code)
        JS::RootedObject global(cx, JS::CurrentGlobalOrNull(cx));
        if (!global) {
            return Status(ErrorCodes::JSInterpreterFailure, "No global object");
        }

        // Define the Debugger object on the global if not already present
        if (!JS_DefineDebuggerObject(cx, global)) {
            return Status(ErrorCodes::JSInterpreterFailure, "Failed to define Debugger object");
        }

        // Get Debugger constructor
        JS::RootedValue debuggerCtor(cx);
        if (!JS_GetProperty(cx, global, "Debugger", &debuggerCtor)) {
            return Status(ErrorCodes::JSInterpreterFailure, "Failed to get Debugger constructor");
        }

        // Create new Debugger instance
        JS::RootedObject debugger(cx);
        if (!JS::Construct(cx, debuggerCtor, JS::HandleValueArray::empty(), &debugger)) {
            return Status(ErrorCodes::JSInterpreterFailure, "Failed to create Debugger instance");
        }

        // Add the global as a debuggee
        // Note: We're still in the debugger's compartment, so we need to wrap global
        JS::RootedValueArray<1> args(cx);
        JS::RootedObject wrappedGlobal(cx, global);
        if (!JS_WrapObject(cx, &wrappedGlobal)) {
            return Status(ErrorCodes::JSInterpreterFailure, "Failed to wrap global");
        }
        args[0].setObject(*wrappedGlobal);

        JS::RootedValue rval(cx);
        if (!JS_CallFunctionName(cx, debugger, "addDebuggee", args, &rval)) {
            return Status(ErrorCodes::JSInterpreterFailure, "Failed to add global as debuggee");
        }

        return Status::OK();
    }
};

BSONObj initDebuggerGlobal(const BSONObj& args, void* data) {
    uassert(ErrorCodes::BadValue, "initDebuggerGlobal accepts no arguments", args.nFields() == 0);

    // Get the scope from the data parameter
    auto* mozjsScope = static_cast<mozjs::MozJSImplScope*>(data);
    if (!mozjsScope) {
        uasserted(ErrorCodes::InternalError, "No scope available for debugger initialization");
    }

    // Get JSContext from the scope
    // Note: getJSContextForTest() is appropriate here since the shell is technically
    // a test environment and we need access for the Debugger API
    JSContext* cx = mozjsScope->getJSContextForTest();
    if (!cx) {
        uasserted(ErrorCodes::InternalError, "Failed to get JSContext, cannot initialize debugger");
    }

    // Initialize SpiderMonkey Debugger
    auto status = DebuggerGlobal::init(cx);
    if (!status.isOK()) {
        uasserted(ErrorCodes::InternalError, "Could not initialize MozJS Debugger");
    }

    return BSON("" << true);
}


}  // namespace mozjs
}  // namespace mongo
