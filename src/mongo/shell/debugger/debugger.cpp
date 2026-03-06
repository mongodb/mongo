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
#include "mongo/shell/debugger/adapter.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"

#include <map>
#include <set>

#include <jsapi.h>

#include <js/CompilationAndEvaluation.h>
#include <js/Conversions.h>
#include <js/SourceText.h>

namespace mongo {

// Forward declarations for generated JS files
namespace JSFiles {
extern const JSFile onDebuggerStatement;
extern const JSFile onNewScript;
}  // namespace JSFiles
}  // namespace mongo

namespace mongo {
namespace mozjs {
namespace debugger {

using namespace protocol;

// Execution state
static AtomicWord<bool> _paused{false};
static std::string _pausedScript;
static int _pausedLine{0};

AtomicWord<bool> _configurationDone{false};
static stdx::mutex _pauseMutex;
static stdx::condition_variable _pauseCV;

// Evaluation state for debugger REPL
std::string _pendingEval;
std::string _evalResult;
AtomicWord<bool> _hasEvalRequest{false};
AtomicWord<bool> _evalComplete{false};

// Data captured when paused
static std::vector<Scope> _capturedScopes;
static std::map<int, std::vector<Variable>> _capturedVariables;
std::vector<protocol::StackFrame> _capturedStackFrames;

// Debugger state
static std::unique_ptr<DebuggerObject> _debuggerObject;
static std::unique_ptr<JS::PersistentRooted<JSObject*>> _debuggerGlobal;

// Pending breakpoints: map from source URL to set of line numbers
static std::map<std::string, std::set<int>> _pendingBreakpoints;

/**
 *  DebuggerObject
 */

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

    DebuggerFrame frame(cx);
    _pausedScript = frame.getScriptUrl();
    _pausedLine = frame.getLineNumber();

    std::cout << std::endl;
    std::cout << "JSDEBUG> JavaScript execution paused in 'debugger' statement." << std::endl;
    std::cout << "JSDEBUG> Type 'dbcont' to continue" << std::endl;

    _paused.store(true);  // cue the thread to prompt the user
    return true;
};

bool DebuggerObject::isPausedCallback(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setBoolean(_paused.load());
    return true;
};

/**
 * Check if there's a pending evaluation request from stdin thread.
 */
bool DebuggerObject::hasEvalRequest(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setBoolean(_hasEvalRequest.load());
    return true;
}

/**
 * Get and clear the pending evaluation request.
 */
bool DebuggerObject::getEvalRequest(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    std::string cmd = _pendingEval;
    _hasEvalRequest.store(false);

    JS::RootedString cmdStr(cx, JS_NewStringCopyZ(cx, cmd.c_str()));
    args.rval().setString(cmdStr);
    return true;
}

/**
 * Get pending breakpoints for a given source URL.
 * Returns an array of line numbers.
 */
bool DebuggerObject::getPendingBreakpointsCallback(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (argc < 1 || !args[0].isString()) {
        args.rval().setUndefined();
        return true;
    }

    JS::RootedString urlStr(cx, args[0].toString());
    JS::UniqueChars urlChars = JS_EncodeStringToUTF8(cx, urlStr);
    if (!urlChars) {
        args.rval().setUndefined();
        return true;
    }

    std::string url(urlChars.get());

    // Look up any pending breakpoints for this URL
    auto it = _pendingBreakpoints.find(url);
    if (it == _pendingBreakpoints.end() || it->second.empty()) {
        // none found
        args.rval().setUndefined();
        return true;
    }

    // Create an array of line numbers
    JS::RootedObject linesArray(cx, JS::NewArrayObject(cx, it->second.size()));
    if (!linesArray) {
        args.rval().setUndefined();
        return true;
    }

    uint32_t index = 0;
    for (int line : it->second) {
        JS::RootedValue lineVal(cx, JS::Int32Value(line));
        if (!JS_SetElement(cx, linesArray, index++, lineVal)) {
            args.rval().setUndefined();
            return true;
        }
    }

    args.rval().setObject(*linesArray);
    return true;
}

/**
 * Store the stringified result of the frame.eval
 */
bool DebuggerObject::storeEvalResult(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (argc < 1 || !args[0].isString()) {
        args.rval().setString(
            JS_NewStringCopyZ(cx, "ERROR: storeEvalResult requires a string argument"));
        return true;
    }

    JS::RootedString returnStr(cx, args[0].toString());
    JS::UniqueChars returnChars = JS_EncodeStringToUTF8(cx, returnStr);
    _evalResult = returnChars ? std::string(returnChars.get()) : "[Failed to convert]";

    _evalComplete.store(true);

    return true;
}

/**
 * Store scope information extracted from the frame.
 * Expects an array of objects with {name, variablesReference, expensive} properties.
 */
bool DebuggerObject::storeScopesCallback(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (argc < 1 || !args[0].isObject()) {
        args.rval().setUndefined();
        return true;
    }

    JS::RootedObject scopesArray(cx, &args[0].toObject());
    bool isArray;
    if (!JS::IsArrayObject(cx, scopesArray, &isArray) || !isArray) {
        args.rval().setUndefined();
        return true;
    }

    uint32_t length;
    if (!JS::GetArrayLength(cx, scopesArray, &length)) {
        args.rval().setUndefined();
        return true;
    }

    // Clear both scopes and variables when called with empty array (initialization)
    // Only clear scopes when called with actual data (to preserve variables we just stored)
    _capturedScopes.clear();
    if (length == 0) {
        _capturedVariables.clear();
    }

    for (uint32_t i = 0; i < length; i++) {
        JS::RootedValue scopeVal(cx);
        if (!JS_GetElement(cx, scopesArray, i, &scopeVal) || !scopeVal.isObject()) {
            continue;
        }

        JS::RootedObject scopeObj(cx, &scopeVal.toObject());

        // Extract name
        JS::RootedValue nameVal(cx);
        std::string name = "unknown";
        if (JS_GetProperty(cx, scopeObj, "name", &nameVal) && nameVal.isString()) {
            JS::RootedString nameStr(cx, nameVal.toString());
            JS::UniqueChars nameChars = JS_EncodeStringToUTF8(cx, nameStr);
            if (nameChars) {
                name = std::string(nameChars.get());
            }
        }

        // Extract variablesReference
        JS::RootedValue refVal(cx);
        int variablesReference = 0;
        if (JS_GetProperty(cx, scopeObj, "variablesReference", &refVal)) {
            int32_t ref;
            if (JS::ToInt32(cx, refVal, &ref)) {
                variablesReference = ref;
            }
        }

        // Extract expensive flag
        JS::RootedValue expensiveVal(cx);
        bool expensive = false;
        if (JS_GetProperty(cx, scopeObj, "expensive", &expensiveVal) && expensiveVal.isBoolean()) {
            expensive = expensiveVal.toBoolean();
        }

        _capturedScopes.emplace_back(name, variablesReference, expensive);
    }

    args.rval().setUndefined();
    return true;
}

/**
 * Store stack frame information.
 * Expects an array of objects with {url, line} properties.
 */
bool DebuggerObject::storeStackFramesCallback(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (argc < 1 || !args[0].isObject()) {
        args.rval().setUndefined();
        return true;
    }

    JS::RootedObject framesArray(cx, &args[0].toObject());
    bool isArray;
    if (!JS::IsArrayObject(cx, framesArray, &isArray) || !isArray) {
        args.rval().setUndefined();
        return true;
    }

    uint32_t length;
    if (!JS::GetArrayLength(cx, framesArray, &length)) {
        args.rval().setUndefined();
        return true;
    }

    _capturedStackFrames.clear();

    for (uint32_t i = 0; i < length; i++) {
        JS::RootedValue frameVal(cx);
        if (!JS_GetElement(cx, framesArray, i, &frameVal) || !frameVal.isObject()) {
            continue;
        }

        JS::RootedObject frameObj(cx, &frameVal.toObject());

        // Extract url
        JS::RootedValue urlVal(cx);
        std::string url;
        if (JS_GetProperty(cx, frameObj, "url", &urlVal) && urlVal.isString()) {
            JS::RootedString urlStr(cx, urlVal.toString());
            JS::UniqueChars urlChars = JS_EncodeStringToUTF8(cx, urlStr);
            url = urlChars ? std::string(urlChars.get()) : "unknown";
        }

        // Extract line number
        JS::RootedValue lineVal(cx);
        int line = 0;
        if (JS_GetProperty(cx, frameObj, "line", &lineVal)) {
            int32_t lineNum;
            if (JS::ToInt32(cx, lineVal, &lineNum)) {
                line = lineNum;
            }
        }

        // Set both the "name" and the "source" to the url, the adapter will modify each
        // appropriately.
        _capturedStackFrames.emplace_back(url, url, line);
    }

    args.rval().setUndefined();
    return true;
}

/**
 * Store variable information for a given scope.
 * Expects variablesReference (int) and an array of {name, value, type, variablesReference}
 * objects.
 */
bool DebuggerObject::storeVariablesCallback(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    if (argc < 2 || !args[1].isObject()) {
        args.rval().setUndefined();
        return true;
    }

    // Get the variablesReference (scope ID)
    int32_t scopeId;
    if (!JS::ToInt32(cx, args[0], &scopeId)) {
        args.rval().setUndefined();
        return true;
    }

    std::vector<Variable> variables;

    JS::RootedObject varsArray(cx, &args[1].toObject());
    bool isArray;
    if (!JS::IsArrayObject(cx, varsArray, &isArray) || !isArray) {
        args.rval().setUndefined();
        return true;
    }

    uint32_t length;
    if (!JS::GetArrayLength(cx, varsArray, &length)) {
        args.rval().setUndefined();
        return true;
    }

    for (uint32_t i = 0; i < length; i++) {
        JS::RootedValue varVal(cx);
        if (!JS_GetElement(cx, varsArray, i, &varVal) || !varVal.isObject()) {
            continue;
        }

        JS::RootedObject varObj(cx, &varVal.toObject());

        // Extract name
        JS::RootedValue nameVal(cx);
        std::string name = "unknown";
        if (JS_GetProperty(cx, varObj, "name", &nameVal) && nameVal.isString()) {
            JS::RootedString nameStr(cx, nameVal.toString());
            JS::UniqueChars nameChars = JS_EncodeStringToUTF8(cx, nameStr);
            if (nameChars) {
                name = std::string(nameChars.get());
            }
        }

        // Extract value
        JS::RootedValue valueVal(cx);
        std::string value = "";
        if (JS_GetProperty(cx, varObj, "value", &valueVal) && valueVal.isString()) {
            JS::RootedString valueStr(cx, valueVal.toString());
            JS::UniqueChars valueChars = JS_EncodeStringToUTF8(cx, valueStr);
            if (valueChars) {
                value = std::string(valueChars.get());
            }
        }

        // Extract type
        JS::RootedValue typeVal(cx);
        std::string type = "unknown";
        if (JS_GetProperty(cx, varObj, "type", &typeVal) && typeVal.isString()) {
            JS::RootedString typeStr(cx, typeVal.toString());
            JS::UniqueChars typeChars = JS_EncodeStringToUTF8(cx, typeStr);
            if (typeChars) {
                type = std::string(typeChars.get());
            }
        }

        // Extract variablesReference
        JS::RootedValue refVal(cx);
        int variablesReference = 0;
        if (JS_GetProperty(cx, varObj, "variablesReference", &refVal)) {
            int32_t ref;
            if (JS::ToInt32(cx, refVal, &ref)) {
                variablesReference = ref;
            }
        }

        variables.emplace_back(name, value, type, variablesReference);
    }

    _capturedVariables[scopeId] = variables;

    args.rval().setUndefined();
    return true;
}

Status DebuggerObject::setOnDebuggerStatementCallback(JS::RootedObject const& global) {
    Status status = Status::OK();

    status = registerNativeFunction(
        _cx, global, "__onDebuggerStatement", DebuggerObject::onDebuggerStatementCallback, 1);
    if (!status.isOK()) {
        return status;
    }

    status = registerNativeFunction(_cx, global, "__isPaused", DebuggerObject::isPausedCallback, 0);
    if (!status.isOK()) {
        return status;
    }

    status = registerNativeFunction(
        _cx, global, "__storeEvalResult", DebuggerObject::storeEvalResult, 1);
    if (!status.isOK()) {
        return status;
    }

    status =
        registerNativeFunction(_cx, global, "__hasEvalRequest", DebuggerObject::hasEvalRequest, 0);
    if (!status.isOK()) {
        return status;
    }

    status =
        registerNativeFunction(_cx, global, "__getEvalRequest", DebuggerObject::getEvalRequest, 0);
    if (!status.isOK()) {
        return status;
    }

    status = registerNativeFunction(
        _cx, global, "__storeScopes", DebuggerObject::storeScopesCallback, 1);
    if (!status.isOK()) {
        return status;
    }

    status = registerNativeFunction(
        _cx, global, "__storeVariables", DebuggerObject::storeVariablesCallback, 2);
    if (!status.isOK()) {
        return status;
    }

    status = registerNativeFunction(
        _cx, global, "__storeStackFrames", DebuggerObject::storeStackFramesCallback, 1);
    if (!status.isOK()) {
        return status;
    }

    JS::RootedValue onDebuggerStatement(_cx);
    status = compileJSCodeBlock(::mongo::JSFiles::onDebuggerStatement, &onDebuggerStatement);
    if (!status.isOK()) {
        return status;
    }

    if (!JS_SetProperty(_cx, _debugger, "onDebuggerStatement", onDebuggerStatement)) {
        return Status(ErrorCodes::JSInterpreterFailure, "Failed to set hook");
    }

    return status;
}

Status DebuggerObject::setOnNewScriptCallback(JS::RootedObject const& global) {
    Status status = Status::OK();

    status = registerNativeFunction(
        _cx, global, "__getPendingBreakpoints", DebuggerObject::getPendingBreakpointsCallback, 1);
    if (!status.isOK()) {
        return status;
    }

    status = registerNativeFunction(
        _cx, global, "__onScriptSetBreakpoint", DebuggerScript::breakpointHandler, 1);
    if (!status.isOK()) {
        return status;
    }

    JS::RootedValue onNewScript(_cx);
    status = compileJSCodeBlock(::mongo::JSFiles::onNewScript, &onNewScript);
    if (!status.isOK()) {
        return status;
    }

    if (!JS_SetProperty(_cx, _debugger, "onNewScript", onNewScript)) {
        return Status(ErrorCodes::JSInterpreterFailure, "Failed to set onNewScript");
    }

    return Status::OK();
}

void DebuggerObject::setBreakpoints(SetBreakpointsRequest request) {

    // Assume that all breakpoints are set before running the shell.
    // This means that the scripts that the breakpoints are in have not been loaded yet.
    // Store these pending breakpoints to register later via debugger.onNewScript.
    _pendingBreakpoints[request.source].clear();
    for (int line : request.lines) {
        _pendingBreakpoints[request.source].insert(line);
    }

    // TODO: Try to apply to already-loaded scripts, where users add more breakpoints as they go
}

Status DebuggerObject::compileJSCodeBlock(JSFile jsfile, JS::MutableHandleValue out) {
    auto code = std::string(toStdStringViewForInterop(jsfile.source));
    auto name = jsfile.name;
    return DebuggerObject::compileJSCodeBlock(code.c_str(), name, out);
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

/**
 *  DebuggerFrame
 */

// Currently this just queries the context for a "__pausedLocation" property of the form {script,
// line}. Relying on JS here drastically reduces LOC to otherwise retrieve those in C++.
std::string DebuggerFrame::getScriptUrl() {
    JS::RootedObject global(_cx, JS::CurrentGlobalOrNull(_cx));

    // Read location info that was set by the JavaScript hook
    JS::RootedValue locationVal(_cx);
    if (JS_GetProperty(_cx, global, "__pausedLocation", &locationVal) && locationVal.isObject()) {
        JS::RootedObject locationObj(_cx, &locationVal.toObject());

        // Get script property
        JS::RootedValue scriptVal(_cx);
        if (JS_GetProperty(_cx, locationObj, "script", &scriptVal) && scriptVal.isString()) {
            JS::RootedString scriptStr(_cx, scriptVal.toString());
            JS::UniqueChars scriptChars = JS_EncodeStringToUTF8(_cx, scriptStr);
            if (scriptChars) {
                return std::string(scriptChars.get());
            }
        }
    }
    return "";
}

// Currently this just queries the context for a "__pausedLocation" property of the form {script,
// line}. Relying on JS here drastically reduces LOC to otherwise retrieve those in C++.
int DebuggerFrame::getLineNumber() {
    JS::RootedObject global(_cx, JS::CurrentGlobalOrNull(_cx));

    // Read location info that was set by the JavaScript hook
    JS::RootedValue locationVal(_cx);
    if (JS_GetProperty(_cx, global, "__pausedLocation", &locationVal) && locationVal.isObject()) {
        JS::RootedObject locationObj(_cx, &locationVal.toObject());

        // Get line property
        JS::RootedValue lineVal(_cx);
        if (JS_GetProperty(_cx, locationObj, "line", &lineVal)) {
            int32_t lineNum;
            if (JS::ToInt32(_cx, lineVal, &lineNum)) {
                return lineNum;
            }
        }
    }

    return 0;
}

/**
 * DebuggerScript
 */

bool DebuggerScript::breakpointHandler(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    // Get current frame information
    DebuggerFrame frame(cx);
    _pausedScript = frame.getScriptUrl();
    _pausedLine = frame.getLineNumber();

    DebugAdapter::sendPause();

    std::unique_lock<std::mutex> lock(_pauseMutex);
    _paused.store(true);
    _pauseCV.wait(lock, [] { return !_paused.load(); });

    // Resumption value of 'undefined': The debuggee should continue execution normally.
    args.rval().setUndefined();

    return true;
}

/**
 * DebuggerGlobal
 */

void DebuggerGlobal::setBreakpoints(SetBreakpointsRequest request) {
    _debuggerObject->setBreakpoints(request);
}

std::string DebuggerGlobal::getPausedScript() {
    return _pausedScript;
}

int DebuggerGlobal::getPausedLine() {
    return _pausedLine;
}

std::vector<Scope> DebuggerGlobal::getScopes(int frameId) {
    return _capturedScopes;
}

std::vector<Variable> DebuggerGlobal::getVariables(int variablesReference) {
    auto it = _capturedVariables.find(variablesReference);
    if (it != _capturedVariables.end()) {
        return it->second;
    }
    return std::vector<Variable>();
}

std::vector<protocol::StackFrame> DebuggerGlobal::getStackFrames() {
    return _capturedStackFrames;
}

void DebuggerGlobal::unpause() {
    _paused.store(false);
    _pauseCV.notify_all();
}

std::unique_ptr<std::thread> _stdinThread;

void DebuggerGlobal::handleStdinThread() {
    // Open /dev/tty to read directly from the terminal, even when stdin is redirected
    FILE* tty_in = fopen("/dev/tty", "r");
    if (!tty_in) {
        std::cerr << "Failed to open /dev/tty for debug input: " << strerror(errno) << std::endl;
        std::cerr << "Debug pause/continue will not work" << std::endl;
        return;
    }

    // Open /dev/tty for writing as well, so prompts appear even when stdout is redirected
    FILE* tty_out = fopen("/dev/tty", "w");
    if (!tty_out) {
        std::cerr << "Failed to open /dev/tty for debug output: " << strerror(errno) << std::endl;
        fclose(tty_in);
        return;
    }

    char buffer[256];
    while (true) {
        if (_paused.load()) {
            if (!_pausedScript.empty()) {
                fprintf(tty_out, "JSDEBUG@%s:%d> ", _pausedScript.c_str(), _pausedLine);
            } else {
                fprintf(tty_out, "JSDEBUG> ");
            }
            fflush(tty_out);
            if (!fgets(buffer, sizeof(buffer), tty_in)) {
                // EOF or error
                break;
            }

            std::string command(buffer);

            // Trim whitespace and newline
            command.erase(0, command.find_first_not_of(" \t\n\r"));
            command.erase(command.find_last_not_of(" \t\n\r") + 1);

            if (command == "dbcont") {
                fprintf(tty_out, "JSDEBUG> Continuing execution...\n");
                fflush(tty_out);
                _paused.store(false);
            } else if (!command.empty()) {
                // Command to be evaluated in the frame
                _pendingEval = command;
                _evalComplete.store(false);
                _hasEvalRequest.store(true);

                // Wait for the evaluation to complete (with timeout)
                auto delay = std::chrono::milliseconds(100);
                int retries = 100;  // total 10 secs
                while (!_evalComplete.load() && retries > 0) {
                    std::this_thread::sleep_for(delay);
                    retries--;
                }

                if (_evalComplete.load()) {
                    fprintf(tty_out, "%s\n", _evalResult.c_str());
                    fflush(tty_out);
                } else {
                    fprintf(tty_out, "ERROR: Evaluation timed out\n");
                    fflush(tty_out);
                    _hasEvalRequest.store(false);
                }
            }
        } else {
            // Not paused, sleep a bit to avoid spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    fclose(tty_in);
    fclose(tty_out);
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
    _debuggerGlobal = std::make_unique<JS::PersistentRooted<JSObject*>>(cx, debuggerGlobal);

    Status status = Status::OK();

    // Create debugger in its own compartment (using nested scope for compartment management)
    {
        // Enter the debugger's compartment
        JSAutoRealm realm(cx, debuggerGlobal);

        // Create and store the debugger object for later use (e.g., setting breakpoints)
        _debuggerObject =
            std::make_unique<DebuggerObject>(DebuggerObject::create(cx, debuggerGlobal));

        status = _debuggerObject->addDebuggee(mainGlobal);
        if (!status.isOK()) {
            return status;
        }

        // Set up onDebuggerStatement hook
        status = _debuggerObject->setOnDebuggerStatementCallback(debuggerGlobal);
        if (!status.isOK()) {
            return status;
        }

        // Set up script.onNewScript hook to activate breakpoints
        status = _debuggerObject->setOnNewScriptCallback(debuggerGlobal);
        if (!status.isOK()) {
            return status;
        }

    }  // Exit debugger compartment - JSAutoRealm goes out of scope here

    status = DebugAdapter::connect();
    if (!status.isOK()) {
        return status;
    }

    // Wait for the debugger to send initial configuration (breakpoints, etc.)
    // before proceeding with script execution
    DebugAdapter::waitForHandshake();

    // Start stdin handling thread for debug commands
    _stdinThread = std::make_unique<std::thread>(handleStdinThread);

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


}  // namespace debugger
}  // namespace mozjs
}  // namespace mongo
