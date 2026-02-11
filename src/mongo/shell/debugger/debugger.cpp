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

#include <js/CompilationAndEvaluation.h>
#include <js/SourceText.h>

namespace mongo {
namespace mozjs {


DebuggerObject::DebuggerObject(JSContext* cx, JS::HandleObject debugger)
    : _cx(cx), _debugger(cx, debugger) {}

DebuggerObject DebuggerObject::create(JSContext* cx, JS::RootedObject const& global) {

    // Define the Debugger object in the debugger's global
    if (!JS_DefineDebuggerObject(cx, global)) {
        uasserted(ErrorCodes::InternalError, "Failed to define Debugger object");
    }

    // Get Debugger constructor from the debugger's global
    JS::RootedValue debuggerCtor(cx);
    if (!JS_GetProperty(cx, global, "Debugger", &debuggerCtor)) {
        uasserted(ErrorCodes::InternalError, "Failed to get Debugger constructor");
    }

    // Create new Debugger instance in the separate compartment
    JS::RootedObject debugger(cx);
    if (!JS::Construct(cx, debuggerCtor, JS::HandleValueArray::empty(), &debugger)) {
        uasserted(ErrorCodes::InternalError, "Failed to create Debugger instance");
    }

    return DebuggerObject(cx, debugger);
}

Status DebuggerObject::addDebuggee(JS::RootedObject const& global) {
    // Add the global as a debuggee

    // Note: We should be in the debugger's compartment, so we need to wrap global
    JS::RootedObject wrappedGlobal(_cx, global);
    if (!JS_WrapObject(_cx, &wrappedGlobal)) {
        return Status(ErrorCodes::JSInterpreterFailure, "Failed to wrap global");
    }
    JS::RootedValueArray<1> args(_cx);
    args[0].setObject(*wrappedGlobal);

    JS::RootedValue rval(_cx);
    if (!JS_CallFunctionName(_cx, _debugger, "addDebuggee", args, &rval)) {
        return Status(ErrorCodes::JSInterpreterFailure, "Failed to add global as debuggee");
    }

    return Status::OK();
}

bool DebuggerObject::onDebuggerStatementCallback(JSContext* cx, unsigned argc, JS::Value* vp) {
    std::cout << "[WIP] in debugger callback!" << std::endl;
    return true;
};

Status DebuggerObject::setOnDebuggerStatementCallback(JS::RootedObject const& global) {
    Status status = Status::OK();
    if (!(status = registerNativeFunction(_cx,
                                          global,
                                          "__onDebuggerStatementCallback",
                                          DebuggerObject::onDebuggerStatementCallback,
                                          1))
             .isOK()) {
        return status;
    }

    const char* hookCode = R"HOOK(
            (function(frame) {
                // Call C++ callback
                globalThis.__onDebuggerStatementCallback(frame);
                return undefined;
            })
        )HOOK";

    JS::RootedValue onDebuggerStatement(_cx);
    status = compileJSCodeBlock(hookCode, "debugger-hook", &onDebuggerStatement);
    if (!status.isOK()) {
        return status;
    }

    if (!JS_SetProperty(_cx, _debugger, "onDebuggerStatement", onDebuggerStatement)) {
        return Status(ErrorCodes::JSInterpreterFailure, "Failed to set hook");
    }

    return status;
}

Status DebuggerObject::compileJSCodeBlock(const char* code,
                                          const char* name,
                                          JS::MutableHandleValue out) {

    JS::CompileOptions opts(_cx);
    opts.setFileAndLine(name, 1);

    JS::SourceText<mozilla::Utf8Unit> source;
    if (!source.init(_cx, code, strlen(code), JS::SourceOwnership::Borrowed)) {
        return Status(ErrorCodes::JSInterpreterFailure, "Failed to init code source");
    }

    if (!JS::Evaluate(_cx, opts, source, out)) {
        return Status(ErrorCodes::JSInterpreterFailure, "Failed to compile JS code block");
    }

    return Status::OK();
}


Status DebuggerObject::registerNativeFunction(
    JSContext* cx, JS::HandleObject global, const char* name, JSNative func, unsigned argc) {
    JS::RootedFunction jsFunc(cx, JS_NewFunction(cx, func, argc, 0, name));
    if (!jsFunc) {
        return Status(ErrorCodes::JSInterpreterFailure,
                      str::stream() << "Failed to create function: " << name);
    }

    JS::RootedObject jsFuncObj(cx, JS_GetFunctionObject(jsFunc));
    JS::RootedValue jsFuncVal(cx, JS::ObjectValue(*jsFuncObj));
    if (!JS_SetProperty(cx, global, name, jsFuncVal)) {
        return Status(ErrorCodes::JSInterpreterFailure,
                      str::stream() << "Failed to set global property: " << name);
    }

    return Status::OK();
}


Status DebuggerGlobal::init(JSContext* cx) {

    // Get the main global object (the one running user code)
    JS::RootedObject mainGlobal(cx, JS::CurrentGlobalOrNull(cx));
    if (!mainGlobal) {
        return Status(ErrorCodes::JSInterpreterFailure, "No global object");
    }

    // Create a NEW global object for the debugger (separate compartment)
    // Define a JSClass for the debugger global
    static const JSClass debuggerGlobalClass = {
        "DebuggerGlobal",
        JSCLASS_GLOBAL_FLAGS,
        &JS::DefaultGlobalClassOps,
    };

    JS::RealmOptions realmOptions;
    JS::RootedObject debuggerGlobal(
        cx,
        JS_NewGlobalObject(
            cx, &debuggerGlobalClass, nullptr, JS::FireOnNewGlobalHook, realmOptions));

    if (!debuggerGlobal) {
        return Status(ErrorCodes::JSInterpreterFailure, "Failed to create debugger compartment");
    }

    Status status = Status::OK();

    // Create debugger in its own compartment (using nested scope for compartment management)
    {
        // Enter the debugger's compartment
        JSAutoRealm realm(cx, debuggerGlobal);

        DebuggerObject debuggerObject = DebuggerObject::create(cx, debuggerGlobal);
        status = debuggerObject.addDebuggee(mainGlobal);
        if (!status.isOK()) {
            return status;
        }

        // Set up onDebuggerStatement hook
        status = debuggerObject.setOnDebuggerStatementCallback(debuggerGlobal);
        if (!status.isOK()) {
            return status;
        }

    }  // Exit debugger compartment - JSAutoRealm goes out of scope here

    return status;
}

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
