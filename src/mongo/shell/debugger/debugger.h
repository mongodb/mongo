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

#pragma once

#include "mongo/base/status.h"

#include <jsapi.h>

namespace mongo {
namespace mozjs {

// Main entrypoint for the shell to initialize when in debug mode
BSONObj initDebuggerGlobal(const BSONObj& args, void* data);


class DebuggerGlobal {
public:
    static Status init(JSContext* cx);
};

/**
 * Facade interface of https://firefox-source-docs.mozilla.org/js/Debugger/Debugger.html
 * and its relevant functionality.
 */
class DebuggerObject {
    JSContext* _cx;
    JS::PersistentRooted<JSObject*> _debugger;

    // Invoked when the JS "debugger" keyword is executed.
    static bool onDebuggerStatementCallback(JSContext* cx, unsigned argc, JS::Value* vp);

    // Helper: Register a native function in the debugger compartment.
    Status registerNativeFunction(
        JSContext* cx, JS::HandleObject global, const char* name, JSNative func, unsigned argc);

    // Helper: Compile a JS Code block and set a reference.
    Status compileJSCodeBlock(const char* code, const char* name, JS::MutableHandleValue out);

public:
    DebuggerObject(JSContext* cx, JS::HandleObject debugger);

    // Create a Debugger instance in the compartment.
    static DebuggerObject create(JSContext* cx, JS::RootedObject const& global);

    // Add a global object for this Debugger instance to debug.
    // https://firefox-source-docs.mozilla.org/js/Debugger/Debugger.html#adddebuggee-global
    Status addDebuggee(JS::RootedObject const& global);

    // Set the "onDebuggerStatement" callback in the compartment
    // https://firefox-source-docs.mozilla.org/js/Debugger/Debugger.html#ondebuggerstatement-frame
    Status setOnDebuggerStatementCallback(JS::RootedObject const& global);
};
}  // namespace mozjs
}  // namespace mongo
