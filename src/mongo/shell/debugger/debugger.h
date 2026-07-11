// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/debugger/adapter.h"
#include "mongo/shell/debugger/protocol.h"

#include <jsapi.h>

namespace mongo {
namespace mozjs {
namespace debugger {


using namespace protocol;

// Main entrypoint for the shell to initialize when in debug mode
BSONObj initDebuggerGlobal(const BSONObj& args, void* data);


class DebuggerGlobal {
public:
    static Status init(JSContext* cx);
    // Must be called before the JSContext passed to init() is destroyed.
    static void cleanup();
    static void handleStdinThread();
    static void setBreakpoints(SetBreakpointsRequest request);

    static std::string getPausedScript();
    static int getPausedLine();

    static std::vector<Scope> getScopes(int frameId);
    static std::vector<Variable> getVariables(int variablesReference);
    static std::vector<protocol::StackFrame> getStackFrames();

    static std::string evaluate(EvaluateRequest request);
    static std::string setVariable(SetVariableRequest request);
    static void unpause();
};

/**
 * Facade interface of the Debugger Object and its relevant functionality.
 * https://firefox-source-docs.mozilla.org/devtools-user/debugger-api/debugger/index.html
 */
class DebuggerObject {
private:
    JSContext* _cx;
    JS::PersistentRootedObject _debugger;

    // Invoked when the JS "debugger" keyword is executed.
    static bool onDebuggerStatementCallback(JSContext* cx, unsigned argc, JS::Value* vp);

    static bool isPausedCallback(JSContext* cx, unsigned argc, JS::Value* vp);

    static bool storeEvalResult(JSContext* cx, unsigned argc, JS::Value* vp);
    static bool storeScopesCallback(JSContext* cx, unsigned argc, JS::Value* vp);
    static bool storeVariablesCallback(JSContext* cx, unsigned argc, JS::Value* vp);
    static bool storeStackFramesCallback(JSContext* cx, unsigned argc, JS::Value* vp);
    static bool getBreakpointsCallback(JSContext* cx, unsigned argc, JS::Value* vp);
    static bool hasBPUpdateRequestCallback(JSContext* cx, unsigned argc, JS::Value* vp);
    static bool getBPUpdatedUrlsCallback(JSContext* cx, unsigned argc, JS::Value* vp);
    static bool fromInteractiveREPL(JSContext* cx, unsigned argc, JS::Value* vp);

    static bool hasEvalRequest(JSContext* cx, unsigned argc, JS::Value* vp);
    static bool getEvalRequest(JSContext* cx, unsigned argc, JS::Value* vp);

    // Helper: Register a native function in the debugger compartment.
    Status registerNativeFunction(
        JSContext* cx, JS::HandleObject global, const char* name, JSNative func, unsigned argc);

    // Helper: Compile a JS Code block and set a reference.
    Status compileJSCodeBlock(const char* code, const char* name, JS::MutableHandleValue out);
    Status compileJSCodeBlock(JSFile jsfile, JS::MutableHandleValue out);

public:
    DebuggerObject(JSContext* cx, JS::HandleObject debugger) : _cx(cx), _debugger(cx, debugger) {};

    // Create a Debugger instance in the compartment.
    static DebuggerObject create(JSContext* cx, JS::RootedObject const& global);

    // Add a global object for this Debugger instance to debug.
    // https://firefox-source-docs.mozilla.org/devtools-user/debugger-api/debugger/index.html#debugger-api-debugger-add-debuggee
    Status addDebuggee(JS::RootedObject const& global);

    // Set the "onDebuggerStatement" callback in the compartment
    // https://firefox-source-docs.mozilla.org/devtools-user/debugger-api/debugger/index.html
    Status setOnDebuggerStatementCallback(JS::RootedObject const& global);

    // Set the "onNewScript" callback in the compartment
    // https://firefox-source-docs.mozilla.org/devtools-user/debugger-api/debugger/index.html
    Status setOnNewScriptCallback(JS::RootedObject const& global);

    // Evaluate helpers.js to install shared JS utilities (__spinwait, __processScopes, etc.)
    Status setupHelpers(JS::RootedObject const& global);

    // Set breakpoints for the given request
    void setBreakpoints(SetBreakpointsRequest request);

    std::string evaluate(EvaluateRequest request);
    std::string setVariable(SetVariableRequest request);

    static bool onExceptionCallback(JSContext* cx, unsigned argc, JS::Value* vp);
    static bool storeExceptionInfoCallback(JSContext* cx, unsigned argc, JS::Value* vp);
    Status setOnExceptionUnwindCallback(JS::RootedObject const& global);
};

/**
 * Facade interface of Debugger.Frame and its relevant functionality.
 * https://firefox-source-docs.mozilla.org/devtools-user/debugger-api/debugger.frame/index.html
 */
class DebuggerFrame {
    JSContext* _cx;

public:
    DebuggerFrame(JSContext* cx) : _cx(cx) {};

    // Return the script url of the frame instance.
    // Will look something like "jstests/my_test.js".
    std::string getScriptUrl();

    // Return the line number of the frame instance.
    // Uses 1-based indexing, so line 1 is the first line of the file.
    int getLineNumber();
};

/**
 * Facade interface of Debugger.Script and its relevant functionality.
 * https://firefox-source-docs.mozilla.org/devtools-user/debugger-api/debugger.script/index.html
 */
class DebuggerScript {
public:
    // Callback handler to the `setBreakpoint(offset, handler)` method
    static bool breakpointHandler(JSContext* cx, unsigned argc, JS::Value* vp);
};

}  // namespace debugger
}  // namespace mozjs
}  // namespace mongo
