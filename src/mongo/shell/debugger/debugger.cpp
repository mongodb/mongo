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
#include <js/Conversions.h>
#include <js/SourceText.h>

namespace mongo {
namespace mozjs {

// Execution state
static AtomicWord<bool> _paused{false};
static std::string _pausedScript;
static int _pausedLine{0};

// Evaluation state for debugger REPL
std::string _pendingEval;
std::string _evalResult;
AtomicWord<bool> _hasEvalRequest{false};
AtomicWord<bool> _evalComplete{false};

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

    return true;  // this is independent of the JS return value
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

Status DebuggerObject::setOnDebuggerStatementCallback(JS::RootedObject const& global) {
    Status status = Status::OK();

    if (!(status = registerNativeFunction(
              _cx, global, "__onDebuggerStatement", DebuggerObject::onDebuggerStatementCallback, 1))
             .isOK()) {
        return status;
    }

    if (!(status = registerNativeFunction(
              _cx, global, "__isPaused", DebuggerObject::isPausedCallback, 0))
             .isOK()) {
        return status;
    }
    if (!(status = registerNativeFunction(
              _cx, global, "__storeEvalResult", DebuggerObject::storeEvalResult, 1))
             .isOK()) {
        return status;
    }
    if (!(status = registerNativeFunction(
              _cx, global, "__hasEvalRequest", DebuggerObject::hasEvalRequest, 0))
             .isOK()) {
        return status;
    }
    if (!(status = registerNativeFunction(
              _cx, global, "__getEvalRequest", DebuggerObject::getEvalRequest, 0))
             .isOK()) {
        return status;
    }

    // Doing more work here in JS makes for 10x fewer LOC in C++
    const char* hookCode = R"HOOK(
            (function(frame) {
                // Store location info so C++ can access it
                globalThis.__pausedLocation = {
                    script: frame.script?.url ?? "unknown",
                    line: frame.script?.getOffsetLocation?.call(frame.script, frame.offset)?.lineNumber ?? 0,
                };
                
                // invoke the C++ callback
                globalThis.__onDebuggerStatement(frame);

                // Spin-wait until the paused flag is cleared
                // This blocks JavaScript execution in this frame
                while (globalThis.__isPaused()) {

                    // Check for pending evaluation requests
                    if (globalThis.__hasEvalRequest()) {
                        // Get the expression to evaluate
                        const expr = globalThis.__getEvalRequest();

                        // Wrap the expression to format the result with tojson in the debuggee context
                        const wrappedExpr = `\
                            (function() {
                                try {
                                    const __result = (${expr});
                                    return tojson(__result);
                                } catch (e) {
                                    // eg. reference errors, assertion failures
                                    return e.name + ": " + e.message;
                                }
                            })()`;
                        // Evaluate in the context of the current frame
                        let result = "";
                        try {
                            output = frame.eval(wrappedExpr);
                            if (output.return) {
                                result = output.return;
                            } else if (output.throw) {
                                // eg, syntax error in eval'ed string
                                const e = output.throw.unsafeDereference(); // unwrap Debugger.Object
                                result = e.name + ": " + e.message;
                            }
                        } catch (e) {
                            // something really unexpected happened, but avoid a crash
                            result = e.name + ": " + e.message;
                        }
                        globalThis.__storeEvalResult(result);
                    }
                }

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

DebuggerFrame::DebuggerFrame(JSContext* cx) : _cx(cx) {}

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


}  // namespace mozjs
}  // namespace mongo
